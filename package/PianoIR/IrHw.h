// SPDX-License-Identifier: BSD-2-Clause-Patent
// The piano IR emitter's hardware. Sources are pinned in UPSTREAM.
//   the emitter is the MOSI line of QUP2 serial engine 7 ("SE15", spi@89c000), GPIO29, active high, no supply of its own
//   (sm8750-xiaomi-piano-desktop.dts: spi15 / irled@0 "xiaomi,piano-ir-spi", 1.92 MHz; Project-SunUEFI 766ab7ca35d1's dtso);
//   GPIO28 MISO, 30 CLK, 31 CS; function 1 = qup2_se7 (pinctrl-sm8750.c), 6 mA, no bias; idle: GPIO29 a GPIO driven low;
//   GCC (gcc-sm8750.c): the QUP2 vote word 0x52010 (bit 0 core, 1 s-ahb, 2 m-ahb, 3 core2x, 17 s7), CBCRs 0x23298 m-ahb and
//   0x2329c s-ahb (hardware-gated), 0x232a0 core, 0x232b4 core2x, 0x1e8a8 s7; the s7 RCG 0x1e8b8 (a DFS RCG, parent map 0:
//   0 XO 19.2 MHz, 1 GPLL0 600 MHz, 6 GPLL0 even 300 MHz);
//   QUP2 wrapper geniqup@8c0000, its SMMU stream 0x423 (SE DMA; GPI DMA would be 0x436, not used); apps SMMU 0x15000000.
#ifndef PIANO_IR_HW_H
#define PIANO_IR_HW_H
#include <stdint.h>

namespace pianoir {

constexpr uint64_t kGcc = 0x100000, kGccBytes = 0x60000;
constexpr uint64_t kSe15 = 0x0089c000, kSe15Bytes = 0x4000;
constexpr uint64_t kQup2 = 0x008c0000, kQup2Bytes = 0x2000;
constexpr uint64_t kTlmm = 0x0f100000, kTlmmStride = 0x1000;
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kQup2Stream = 0x423;

constexpr uint32_t kGccQup2Vote = 0x52010;
enum : uint32_t { kVoteCore = 1u << 0, kVoteSAhb = 1u << 1, kVoteMAhb = 1u << 2, kVoteCore2x = 1u << 3, kVoteS7 = 1u << 17 };
constexpr uint32_t kVoteBits = kVoteCore | kVoteSAhb | kVoteMAhb | kVoteCore2x | kVoteS7;
constexpr uint32_t kGccVoteTag = 0x49525831;   // "IRX1": PianoIR's tag in PianoA830's GCC lock word

struct VoteResult {
  uint32_t before, after, waits;
  bool locked;
};

template <class Read, class Write, class TryLock, class Unlock, class Wait>
VoteResult addClockVotes(Read read, Write write, TryLock tryLock, Unlock unlock, Wait wait)
{
  VoteResult result = {};
  result.before = result.after = read(kGccQup2Vote);
  if ((result.before & kVoteBits) == kVoteBits) return result;
  for (; result.waits < 5000; ++result.waits) {
    if (tryLock()) {
      result.locked = true;
      result.before = read(kGccQup2Vote);
      if ((result.before & kVoteBits) != kVoteBits) write(kGccQup2Vote, result.before | kVoteBits);
      result.after = read(kGccQup2Vote);
      unlock();
      return result;
    }
    wait();
  }
  result.after = read(kGccQup2Vote);
  return result;
}

struct Branch {
  const char *name;
  uint32_t cbcr;
};
constexpr Branch kBranches[5] = {{"m-ahb", 0x23298}, {"s-ahb", 0x2329c}, {"core", 0x232a0}, {"core2x", 0x232b4}, {"s7", 0x1e8a8}};
constexpr uint32_t kCbcrClkOff = 1u << 31, kCbcrHwCtl = 1u << 1;
// A branch runs when CLK_OFF is clear, or when it is hardware-gated (CLK_OFF may read 1 while the bus idles).
inline bool branchOn(uint32_t cbcr) { return (cbcr & kCbcrHwCtl) || !(cbcr & kCbcrClkOff); }

// The s7 RCG: CMD +0 (bit 31 root off), CFG +4 (4:0 half divider 2d-1, 10:8 source, 13:12 M/N mode), M +8, N +0xc (~(N-M)),
// DFS: CMD_DFSR +0x14 (bit 0 on), PERF(l) +0x1c+4l (CFG's layout), PERF_M(l) +0x5c+4l, PERF_N(l) +0x9c+4l, eight levels.
constexpr uint32_t kS7Rcg = 0x1e8b8;
constexpr uint32_t kRcgCfg = 0x4, kRcgM = 0x8, kRcgN = 0xc, kRcgDfsCmd = 0x14, kRcgDfsPerf = 0x1c, kRcgDfsM = 0x5c, kRcgDfsN = 0x9c;
constexpr unsigned kRcgDfsLevels = 8;
constexpr uint32_t kRcgParentHz[8] = {19200000, 600000000, 0, 0, 0, 0, 300000000, 0};

// The rate of one CFG/M/~(N-M) set; 0 when the source is not known or M/N is degenerate.
inline uint32_t rcgRate(uint32_t cfg, uint32_t m, uint32_t notNMinusM)
{
  uint64_t hz = kRcgParentHz[(cfg >> 8) & 7];
  const uint32_t div = cfg & 0x1f, mode = (cfg >> 12) & 3;
  if (div) hz = hz * 2 / (div + 1);
  if (mode) {
    const uint32_t mm = m & 0xffff, n = ((~notNMinusM) & 0xffff) + mm;
    if (!mm || !n || mm > n) return 0;
    hz = hz * mm / n;
  }
  return uint32_t(hz);
}

struct S7Clock {
  uint32_t cmd = 0, dfs = 0;
  bool dfsOn = false;
  unsigned levels = 0;                        // 8 with DFS, else 1 (the current rate as level 0)
  uint32_t cfg[kRcgDfsLevels] = {}, m[kRcgDfsLevels] = {}, n[kRcgDfsLevels] = {}, hz[kRcgDfsLevels] = {};
};
// rd(offset) reads a GCC register. Read only.
template <class Read>
S7Clock s7Clock(Read rd)
{
  S7Clock c;
  c.cmd = rd(kS7Rcg);
  c.dfs = rd(kS7Rcg + kRcgDfsCmd);
  c.dfsOn = c.dfs & 1;
  c.levels = c.dfsOn ? kRcgDfsLevels : 1;
  for (unsigned l = 0; l < c.levels; ++l) {
    c.cfg[l] = rd(kS7Rcg + (c.dfsOn ? kRcgDfsPerf + 4 * l : kRcgCfg));
    c.m[l] = rd(kS7Rcg + (c.dfsOn ? kRcgDfsM + 4 * l : kRcgM));
    c.n[l] = rd(kS7Rcg + (c.dfsOn ? kRcgDfsN + 4 * l : kRcgN));
    c.hz[l] = rcgRate(c.cfg[l], c.m[l], c.n[l]);
  }
  return c;
}

// ------------------------------------------------------------------------------------------------------------- the pins (TLMM)
// Pin n at kTlmm + n * 0x1000: CTL (pull 1:0, function 5:2, drive 8:6 (mA / 2 - 1), output enable 9, eGPIO present 11 /
// ours 12), IO (bit 0 the pad's level, bit 1 the output value).
enum PinIndex : unsigned { kMiso, kMosi, kClk, kCs, kPins };
constexpr unsigned kGpio[kPins] = {28, 29, 30, 31};
constexpr uint32_t kCtl = 0x0, kIo = 0x4;
constexpr uint32_t kFunctionGpio = 0, kFunctionQup = 1, kDrive2mA = 0, kDrive6mA = 2, kNoPull = 0;
constexpr uint32_t kOe = 1u << 9, kEgpioPresent = 1u << 11, kEgpioEnable = 1u << 12;
inline uint32_t pinCtl(uint32_t found, uint32_t function, uint32_t drive, uint32_t pull, bool output)
{
  return (found & ~(0x3ffu | kEgpioEnable)) | (found & kEgpioPresent ? kEgpioEnable : 0) | pull | function << 2 | drive << 6 |
         (output ? kOe : 0);
}
inline uint32_t pinFunction(uint32_t ctl) { return (ctl >> 2) & 0xf; }
inline uint32_t pinDriveMa(uint32_t ctl) { return (((ctl >> 6) & 7) + 1) * 2; }
inline bool pinOutput(uint32_t ctl) { return ctl & kOe; }
// An idle input or a parked output must read low at the pad.
inline bool mosiDark(uint32_t ctl, uint32_t io)
{
  return pinFunction(ctl) == kFunctionGpio && !(io & 1) && (!pinOutput(ctl) || !(io & 2)) &&
         (!(ctl & kEgpioPresent) || (ctl & kEgpioEnable));
}

template <class Read, class Write>
bool parkEmitter(uint32_t found, Read read, Write write)
{
  write(kIo, 0);  // Set the latch low before enabling the GPIO output.
  write(kCtl, pinCtl(found, kFunctionGpio, kDrive2mA, kNoPull, true));
  const uint32_t ctl = read(kCtl), io = read(kIo);
  return pinOutput(ctl) && mosiDark(ctl, io);
}

}  // namespace pianoir
#endif
