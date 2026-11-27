// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoTorch's portable part (FlashHw.h, TorchCore.h, TorchSpmi.h) against a model of the SPMI arbiter v7 (APID map,
// owner table, EE 0's observer channels, the owner write channels) with the PM8550 flash module and two other peripherals:
//   - level 1: only observer reads of the flash APID, never a write, never another peripheral, never past its 256 bytes;
//   - the refusals (no APID, another owner, not a flash module, the three-channel layout, channels 3/4 in use, a partial survey);
//   - the on sequence: only the ten allowlisted registers of PPID 0x1ee, masked read-modify-write (other bits kept), each read
//     back; 50 mA -> ITARGET 4 on both channels, default clamp 9, 5 mA steps, software strobe, timers off; the hard 400 mA cap;
//   - off; a read-back mismatch and a denied write (both: off attempted, the state off, a cool-down);
//   - the guard: the time limit, the boot test, the module's thermal/current/voltage/short bits, the SoC and battery limits,
//     one unreadable poll tolerated and two not, the enables found off.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoPower torch_host.cpp -o /tmp/torch_host && /tmp/torch_host
#include "TorchSpmi.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

using namespace pianotorch;
using pianopower::Window;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// --------------------------------------------------------------------------------------------------------- the arbiter model
struct Arbiter {
  struct Apid {
    uint16_t ppid;
    uint32_t owner;
  };
  std::vector<Apid> apids;
  std::map<uint32_t, uint8_t> pmic;               // ppid << 8 | reg
  std::map<uint32_t, bool> ignoresWrites;          // a register that keeps its value (a read-back mismatch)
  std::map<uint8_t, bool> unreadableRegs;
  std::map<int, uint32_t> obsvStatus, obsvD0, obsvD1;
  std::map<int, uint32_t> chnlStatus, chnlData;
  std::vector<std::tuple<uint16_t, uint8_t, unsigned>> reads;   // ppid, reg, bytes
  std::vector<std::tuple<uint16_t, uint8_t, uint8_t>> writes;   // ppid, reg, value
  unsigned failReads = 0;                                       // the next N observer reads fail (status error)
  unsigned writeAttempts = 0, failWriteAt = 0;
  uint32_t failedWriteStatus = 0;
  bool applyFailedWrite = false;

  uint8_t &r(uint16_t ppid, uint8_t reg) { return pmic[uint32_t(ppid) << 8 | reg]; }
  uint8_t flash(uint8_t reg) { return r(kPpid, reg); }

  uint32_t rd(Window w, uint32_t off)
  {
    if (w == Window::core) {
      if (off == 0) return 0x70020000;
      if (off == 4) return uint32_t(apids.size());
      if (off >= 0x2000 && off < 0x2000 + 4 * apids.size()) return uint32_t(apids[(off - 0x2000) / 4].ppid) << 8;
      return 0;
    }
    if (w == Window::cnfg) return off / 4 < apids.size() ? apids[off / 4].owner : 0;
    const int apid = int(off / 0x20);
    switch (off % 0x20) {
      case 0x08: return obsvStatus[apid];
      case 0x18: return obsvD0[apid];
      case 0x1c: return obsvD1[apid];
    }
    return 0;
  }
  void wr(Window w, uint32_t off, uint32_t value)
  {
    CHECK(w == Window::obsv);
    if (w != Window::obsv || off % 0x20 != 0) return;
    const int apid = int(off / 0x20);
    CHECK((value >> 27) == 1);   // an extended read: the observer channel never writes
    const uint8_t reg = uint8_t(value >> 4);
    const unsigned bytes = (value & 7) + 1;
    const uint16_t ppid = apids[apid].ppid;
    reads.emplace_back(ppid, reg, bytes);
    CHECK(unsigned(reg) + bytes <= 0x100);
    if (failReads || unreadableRegs[reg]) {
      if (failReads) --failReads;
      obsvStatus[apid] = 0x3;
      return;
    }
    uint32_t lo = 0, hi = 0;
    for (unsigned b = 0; b < bytes; ++b) (b < 4 ? lo : hi) |= uint32_t(r(ppid, uint8_t(reg + b))) << (8 * (b & 3));
    obsvD0[apid] = lo;
    obsvD1[apid] = hi;
    obsvStatus[apid] = 1;
  }
  // the owner channel of one APID (offsets into its 0x20 bytes)
  uint32_t chRd(int apid, uint32_t off) { return off == kChnlStatus ? chnlStatus[apid] : 0; }
  void chWr(int apid, uint32_t off, uint32_t value)
  {
    if (off == kChnlWdata0) {
      chnlData[apid] = value;
      return;
    }
    CHECK(off == kChnlCmd);
    CHECK((value >> 27) == kOpExtWriteL && (value & 7) == 0);
    const uint8_t reg = uint8_t(value >> 4);
    const uint16_t ppid = apids[apid].ppid;
    const bool fail = ++writeAttempts == failWriteAt;
    if (apids[apid].owner != 0) {
      chnlStatus[apid] = 0x5;   // done + denied
      return;
    }
    if (!fail || applyFailedWrite) {
      writes.emplace_back(ppid, reg, uint8_t(chnlData[apid]));
      if (!ignoresWrites[uint32_t(ppid) << 8 | reg]) r(ppid, reg) = uint8_t(chnlData[apid]);
    }
    chnlStatus[apid] = fail ? failedWriteStatus : 1;
  }
};

static Arbiter *gArb;
static uint32_t ioRd(void *, Window w, uint32_t off) { return gArb->rd(w, off); }
static void ioWr(void *, Window w, uint32_t off, uint32_t v) { gArb->wr(w, off, v); }
static void ioDelay(void *, unsigned) {}
static int gChApid;
static uint32_t chRd(void *, uint32_t off) { return gArb->chRd(gChApid, off); }
static void chWr(void *, uint32_t off, uint32_t v) { gArb->chWr(gChApid, off, v); }
static const pianopower::Io kIo = {nullptr, ioRd, ioWr, ioDelay};
static const ChnlIo kChnl = {nullptr, chRd, chWr, ioDelay};

// Piano's PMICs as far as the test needs: the PON (SID 0), a PM8550 GPIO, the flash module (four channels, off, reset values 0).
static void piano(Arbiter &a, uint32_t flashOwner = 0)
{
  a = Arbiter();
  a.apids = {{0x013, 0}, {0x18d, 0}, {kPpid, flashOwner}};
  a.r(kPpid, kRegRevision) = 0x02;
  a.r(kPpid, kRegType) = kTypeFlash;
  a.r(kPpid, kRegSubtype) = kSubtype4ch;
  a.r(kPpid, kRegThermThrsh1) = 0x03;
  a.r(kPpid, kRegThermThrsh2) = 0x01;
  a.r(0x013, 0x10) = 0x80;
  gArb = &a;
}

struct Port {
  SpmiPort port;
  Bus bus;
  explicit Port(bool writes)
  {
    const pianopower::Arbiter arb = pianopower::spmiArbiter(kIo);
    const pianopower::Apid ap = pianopower::spmiFindApid(kIo, arb, kPpid);
    port.io = &kIo;
    port.apid = ap.apid;
    gChApid = ap.apid;
    port.chnl = writes ? &kChnl : nullptr;
    bus = portBus(port);
  }
};

static bool inAllowlist(uint8_t reg)
{
  for (const Writable &w : kWritable)
    if (w.reg == reg) return true;
  return false;
}

// Every read is of the flash APID, at most 8 bytes, inside its 256 bytes; every write is to an allowlisted register of it.
static bool onlyFlash(const Arbiter &a)
{
  for (const auto &[ppid, reg, bytes] : a.reads)
    if (ppid != kPpid || unsigned(reg) + bytes > 0x100 || bytes > 8) return false;
  for (const auto &[ppid, reg, value] : a.writes)
    if (ppid != kPpid || !inAllowlist(reg)) return false;
  return true;
}

static void testProbeOptIn()
{
  constexpr const char *key = "piano-torch-probe";
  CHECK(!bootSwitchEnabled(nullptr, key) && !bootSwitchEnabled(" \t", key));
  CHECK(!bootSwitchEnabled("piano-torch=2 piano-torch-test=8", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe-spare=1 piano-torch-flash=1", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=2", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=11", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=18446744073709551617", key));
  CHECK(bootSwitchEnabled("piano-torch=2\tpiano-torch-probe=1  ", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=1 piano-torch-probe=0", key));
  CHECK(!bootSwitchEnabled("piano-torch-probe=1 piano-torch-probe", key));
  CHECK(bootSwitchEnabled("piano-torch-probe=0 piano-torch-probe=1", key));
  constexpr const char *ladder = "piano-torch-ladder";
  CHECK(!bootSwitchEnabled("piano-torch=0 piano-torch-max-ma=400", ladder));
  CHECK(!bootSwitchEnabled("piano-torch=2 piano-torch-ladder=0", ladder));
  CHECK(bootSwitchEnabled("piano-torch=2 piano-torch-max-ma=400 piano-torch-ladder=1", ladder));
}

static void testConstants()
{
  CHECK(kPpid == 0x1ee);
  CHECK(itargetForMa(50) == 4 && itargetForMa(100) == 9 && itargetForMa(400) == 39 && itargetForMa(10) == 0 && itargetForMa(5) == 0);
  CHECK(itargetForMa(45) == 3 && maForItarget(3) == 40);   // rounds down, never up
  CHECK(kDefaultLevel == 0 && kDefaultOnMa == 400 && kDefaultMaxMa == 400 && kHardMaxMa == 400);
  CHECK(clampMa(500, 100) == 100 && clampMa(1, 100) == 10 && clampMa(80, 500) == 80 && clampMa(500, 500) == 400);
  CHECK(boundedRequest(0, 100) == 0 && boundedRequest(50, 100) == 50 && boundedRequest(1ull << 32, 100) == 100);
  CHECK(boundedRequest(UINT64_MAX, 1800) == 1800);
  CHECK(clampForMa(100) == 9 && clampForMa(60) == 5 && clampForMa(400) == 39);
  CHECK(!writable(kRegThermThrsh1, 0x07) && !writable(kRegThermThrsh2, 0x07) && !writable(0xd0, 0xff) && !writable(0x3d, 0xff));
  CHECK(!writable(kRegChanEn, 0x10) && !writable(kRegIres, 0x10) && !writable(kRegModuleEn, 0x7f) && !writable(kRegChanEn, 0));
  // the spare channels are writable too (the probe's one-boot channel test), never a bit above the four channels
  CHECK(writable(kRegChanEn, 0x0c) && writable(kRegIres, 0x04) && writable(kRegItarget0 + 2, 0x7f) && writable(kRegStrobe0 + 3, 0x7f));
  CHECK(writable(kRegTimer0 + 3, 0xff) && writable(kRegChanEn, 0x03) && writable(kRegModuleEn, 0x80) && writable(kRegTorchClamp, 0x7f));
  const Faults f = decodeFaults(0x04, 0x20, 0x00, kPianoChannelMask);
  CHECK(f.shortCircuit && f.thermal && !f.current && !f.voltage && !f.timeout && !f.open);
  const Faults o = decodeFaults(0x02, 0, 0, kPianoChannelMask);
  CHECK(o.open && !o.shortCircuit && o.any());   // channel 0's open bit (the no-light signature)
  CHECK(!decodeFaults(0x10, 0, 0x10, kPianoChannelMask).any());   // channel 2's bits are not piano's
  CHECK(std::strcmp(strobeName(0x01), "software, level, active high") == 0);
  char faults[80];
  formatFaults(decodeFaults(0x06, 0x01, 0, kPianoChannelMask), faults, sizeof faults);
  CHECK(std::strcmp(faults, "SHORT OPEN LOW-SUPPLY") == 0);
  formatFaults(Faults(), faults, sizeof faults);
  CHECK(std::strcmp(faults, "none") == 0);
}

static void testAllowlist()
{
  uint8_t masks[256] = {};
  for (unsigned ch = 0; ch < 4; ++ch) {
    masks[kRegTimer0 + ch] = 0xff;
    masks[kRegItarget0 + ch] = 0x7f;
    masks[kRegStrobe0 + ch] = 0x7f;
  }
  masks[kRegModuleEn] = 0x80;
  masks[kRegIres] = 0x0f;
  masks[kRegChanEn] = 0x0f;
  masks[kRegTorchClamp] = 0x7f;
  unsigned registers = 0;
  for (unsigned reg = 0; reg < 256; ++reg) {
    registers += masks[reg] != 0;
    for (unsigned mask = 0; mask < 256; ++mask)
      CHECK(writable(uint8_t(reg), uint8_t(mask)) == (mask && !(mask & ~masks[reg])));
  }
  CHECK(registers == 16 && sizeof kWritable / sizeof kWritable[0] == 16);
}

static void testRequestBoundaries()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Torch t(p.bus, GuardConfig(), kDefaultMaxMa);
  const struct { uint64_t request; unsigned current; } cases[] = {
    {0, 0}, {1, 10}, {9, 10}, {10, 10}, {11, 10}, {99, 90}, {100, 100}, {200, 200}, {399, 390}, {400, 400}, {401, 400},
    {UINT32_MAX, 400}, {1ull << 32, 400}, {(1ull << 32) + 1, 400}, {UINT64_MAX, 400},
  };
  for (const auto &[raw, current] : cases) {
    const auto o = t.request(0, boundedRequest(raw, kHardMaxMa), boundedRequest(1ull << 32, t.config().maxS), {});
    CHECK(o.steps.ok && o.lit == (current != 0) && t.ma() == current);
    CHECK(t.maxMa() == 400 && !(a.flash(kRegChanEn) & ~kPianoChannelMask));
    if (current) {
      const uint8_t target = uint8_t(current / 10 - 1);
      CHECK(a.flash(kRegItarget0) == target && a.flash(kRegItarget0 + 1) == target && a.flash(kRegTorchClamp) == 39);
      CHECK(a.flash(kRegChanEn) == kPianoChannelMask && (a.flash(kRegModuleEn) & kModuleEnBit));
      CHECK(t.deadlineMs() == 1800000 && t.turnOff(1, Reason::user).steps.ok);
    }
    CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  }
}

static void testSurvey()
{
  Arbiter a;
  piano(a);
  Port p(false);
  CHECK(p.port.apid == 2);
  const Survey s = survey(p.bus);
  CHECK(s.identity && s.flash && s.fourChannel && s.complete && s.failures == 0);
  CHECK(s.revision == 0x02 && !s.lit() && s.reg[kRegThermThrsh1] == 0x03);
  CHECK(a.writes.empty() && onlyFlash(a));
  CHECK(writeRefusal(s, p.port.apid, 0) == nullptr);
  CHECK(writeRefusal(s, -1, 0) != nullptr && writeRefusal(s, 2, 2) != nullptr);
  // level 1 has no write path at all: put() refuses before any bus access
  Controller c(p.bus);
  StepResult r;
  CHECK(!c.put(r, kRegChanEn, 0x03, 0x03) && r.refused == 1 && a.writes.empty());

  // not a flash module: only the identity read
  piano(a);
  a.r(kPpid, kRegType) = 0x51;
  Port q(false);
  const Survey t = survey(q.bus);
  CHECK(t.identity && !t.flash && t.reads == 1 && writeRefusal(t, 2, 0) != nullptr);
  // three channels: read and logged, never written
  piano(a);
  a.r(kPpid, kRegSubtype) = kSubtype3chPm8150;
  Port q3(false);
  const Survey u = survey(q3.bus);
  CHECK(u.flash && !u.fourChannel && writeRefusal(u, 2, 0) != nullptr);
  // someone else's channel on
  piano(a);
  a.r(kPpid, kRegChanEn) = 0x04;
  Port q4(false);
  CHECK(writeRefusal(survey(q4.bus), 2, 0) != nullptr);
  CHECK(writeRefusal(survey(q4.bus), 2, 0, 0x0c) == nullptr);   // the probe's spare pair includes it
  // a read that fails: incomplete, refused
  piano(a);
  Port q5(false);
  a.failReads = 0;
  Survey v = survey(q5.bus);
  CHECK(v.complete);
  a.reads.clear();
  a.failReads = 0;
  // fail the second range read
  struct Once {
    static bool read(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes)
    {
      if (reg == 0x10) return false;
      return portRead(ctx, reg, value, bytes);
    }
  };
  Bus flaky = q5.bus;
  flaky.read = Once::read;
  v = survey(flaky);
  CHECK(!v.complete && v.failures == 1 && writeRefusal(v, 2, 0) != nullptr);
  const Survey partial = survey(flaky, true);
  CHECK(!partial.complete && partial.valid[0x0f] && !partial.valid[0x10] && !partial.valid[0xff]);
}

static void testOnOff()
{
  Arbiter a;
  piano(a);
  a.r(kPpid, kRegIres) = 0x04;        // channel 3's bit: kept
  a.r(kPpid, kRegModuleEn) = 0x01;    // a low bit: kept
  a.r(kPpid, kRegItarget0) = 0x80;    // bit 7: kept
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, kDefaultMaxMa);
  Temperatures temps;
  char line[120];
  t.describe(0, line, sizeof line);
  CHECK(std::strcmp(line, "off") == 0);
  Torch::Outcome o = t.request(1000, kDefaultOnMa, 0, temps);
  CHECK(o.lit && o.steps.ok && o.steps.writes == 12 && o.steps.readbacks == 12 && t.ma() == 400 && t.itarget() == 39);
  CHECK(a.flash(kRegModuleEn) == 0x81 && a.flash(kRegChanEn) == 0x03 && a.flash(kRegIres) == 0x07);
  CHECK(a.flash(kRegItarget0) == 0xa7 && a.flash(kRegItarget0 + 1) == 39 && a.flash(kRegTorchClamp) == 39);
  CHECK(a.flash(kRegStrobe0) == 0x01 && a.flash(kRegStrobe0 + 1) == 0x01 && a.flash(kRegTimer0) == 0 && a.flash(kRegTimer0 + 1) == 0);
  CHECK(onlyFlash(a));
  CHECK(t.deadlineMs() == 1000 + 300 * 1000);
  t.describe(1000, line, sizeof line);
  CHECK(std::strcmp(line, "on 400 mA (2 x 200 mA, itarget 39), off in 300 s") == 0);

  a.writes.clear();
  o = t.request(2000, 0, 0, temps);
  CHECK(!o.lit && o.steps.ok && o.steps.writes == 4 && !t.lit() && t.lastReason() == Reason::user);
  CHECK(a.flash(kRegModuleEn) == 0x01 && a.flash(kRegChanEn) == 0 && a.flash(kRegItarget0) == 0x80 && a.flash(kRegItarget0 + 1) == 0);
  t.describe(2000, line, sizeof line);
  CHECK(std::strcmp(line, "off (asked)") == 0);

  // clamped to the configured default cap (400 mA) and lower custom caps; the seconds to maxS
  o = t.request(3000, 900, 99999, temps);
  CHECK(o.lit && t.ma() == 400 && t.itarget() == 39 && t.deadlineMs() == 3000 + 1800 * 1000);
  Torch ubuntuCap(p.bus, g, kUbuntuTorchMaxMa);
  o = ubuntuCap.request(3500, 400, 10, temps);
  CHECK(o.lit && ubuntuCap.ma() == 100 && a.flash(kRegItarget0 + 1) == 9 && a.flash(kRegTorchClamp) == 9);
  ubuntuCap.request(3600, 0, 0, temps);
  Torch low(p.bus, g, 60);
  o = low.request(4000, 100, 10, temps);
  CHECK(o.lit && low.ma() == 60 && a.flash(kRegItarget0 + 1) == 5 && a.flash(kRegTorchClamp) == 5);
  low.request(4500, 0, 0, temps);
  Torch oversized(p.bus, g, 500);
  o = oversized.request(5000, 500, 10, temps);
  CHECK(o.lit && oversized.maxMa() == 400 && oversized.ma() == 400 && oversized.itarget() == 39);
  CHECK((a.flash(kRegItarget0) & kItargetMask) == 39 && a.flash(kRegItarget0 + 1) == 39 && a.flash(kRegTorchClamp) == 39);
  oversized.request(6000, 0, 0, temps);
  Controller c(p.bus);
  const StepResult capped = c.on(60, 60);
  CHECK(capped.ok && a.flash(kRegItarget0 + 1) == 39 && a.flash(kRegTorchClamp) == 39);
  c.off();
  const StepResult lowerClamp = c.on(77, 5);
  CHECK(lowerClamp.ok && a.flash(kRegItarget0 + 1) == 5 && a.flash(kRegTorchClamp) == 5);
  c.off();

  // the probe's spare-pair window: channels 3+4 through the same sequence, their registers only, the piano pair's kept
  a.writes.clear();
  a.r(kPpid, kRegIres) = 0x03;      // the piano pair's bits stand
  CHECK(!c.on(4, 9, 0x0c).ok && a.writes.empty());
  CHECK(!c.flashPulse(1, 8, 0x0c).ok && a.writes.empty());
  CHECK(!c.off(0x0c).ok && a.writes.empty());
  StepResult unauthorized;
  CHECK(!c.put(unauthorized, kRegIres, 0x04, 0x04) && a.writes.empty());
  CHECK(!c.put(unauthorized, kRegTimer0 + 3, 0xff, 0) && a.writes.empty());
  c.setBus(p.bus, true);
  const StepResult spare = c.on(4, 9, 0x0c);
  CHECK(spare.ok && spare.writes == 12 && spare.readbacks == 12);
  CHECK(a.flash(kRegChanEn) == 0x0c && a.flash(kRegIres) == 0x0f);
  CHECK(a.flash(kRegItarget0 + 2) == 4 && a.flash(kRegItarget0 + 3) == 4 && (a.flash(kRegItarget0) & 0x7f) == 0);
  CHECK(a.flash(kRegStrobe0 + 2) == 0x01 && a.flash(kRegStrobe0 + 3) == 0x01 && a.flash(kRegTimer0 + 2) == 0);
  const StepResult spareOff = c.off(0x0c);
  CHECK(spareOff.ok && a.flash(kRegChanEn) == 0 && a.flash(kRegItarget0 + 2) == 0);
  CHECK(onlyFlash(a));
  // The optional T4 diagnostic pulse uses FLASH's 12.5 mA resolution and a
  // bounded safety timer, then the ordinary off sequence restores a safe state.
  a.writes.clear();
  const StepResult pulse = c.flashPulse(77, 8);
  CHECK(pulse.ok && pulse.writes == 11 && pulse.readbacks == 11);
  CHECK((a.flash(kRegIres) & kPianoChannelMask) == 0 && a.flash(kRegTimer0) == (kTimerEnable | 8) &&
        a.flash(kRegTimer0 + 1) == (kTimerEnable | 8));
  CHECK((a.flash(kRegItarget0) & kItargetMask) == 1 && (a.flash(kRegStrobe0) & kStrobeMask) == kStrobeSoftwareLevelHigh &&
        (a.flash(kRegChanEn) & kPianoChannelMask) == kPianoChannelMask && (a.flash(kRegModuleEn) & kModuleEnBit) == kModuleEnBit);
  CHECK((a.flash(kRegItarget0 + 1) & kItargetMask) == 1);   // 2 x 25 mA at the FLASH step size
  const StepResult pulseOff = c.off();
  CHECK(pulseOff.ok && a.flash(kRegChanEn) == 0 && (a.flash(kRegModuleEn) & kModuleEnBit) == 0);
  a.writes.clear();
  CHECK(!c.flashPulse(3, 0).ok && a.writes.empty());
  CHECK(!c.flashPulse(3, 9).ok && a.writes.empty());
  CHECK(!c.flashPulse(3, 0x80).ok && a.writes.empty());
  // a mask above the four channels is refused before any write
  a.writes.clear();
  const StepResult wild = c.on(4, 9, 0x10);
  CHECK(!wild.ok && wild.refused == 1 && a.writes.empty());

  // The controller also accepts a four-channel mask. The cap applies to its combined current.
  constexpr uint8_t allChannels = kPianoChannelMask | kOtherChannelMask;
  CHECK(c.on(77, 77, allChannels).ok && a.flash(kRegTorchClamp) == 19);
  for (unsigned ch = 0; ch < kChannels4ch; ++ch)
    CHECK((a.flash(kRegItarget0 + ch) & kItargetMask) == 19);   // 4 x 100 mA in torch mode
  CHECK(c.off(allChannels).ok);
  CHECK(c.flashPulse(77, 8, allChannels).ok);
  for (unsigned ch = 0; ch < kChannels4ch; ++ch)
    CHECK((a.flash(kRegItarget0 + ch) & kItargetMask) == 0);   // 4 x 12.5 mA in FLASH mode
  CHECK(c.off(allChannels).ok);

  // An earlier spare-pair shutdown can disable the module while leaving a
  // channel bit set. Neither activation may turn the module back on then.
  a.r(kPpid, kRegChanEn) = 0x04;
  a.writes.clear();
  const StepResult staleTorch = c.on(9, 9);
  CHECK(!staleTorch.ok && staleTorch.failedReg == kRegChanEn && a.writes.empty());
  a.r(kPpid, kRegChanEn) = 0x04;
  const StepResult staleFlash = c.flashPulse(3, 8);
  CHECK(!staleFlash.ok && staleFlash.failedReg == kRegChanEn && a.writes.empty());
}

static void testLadder()
{
  for (const unsigned cap : {kUbuntuTorchMaxMa, kDefaultMaxMa}) {
    Arbiter a;
    piano(a);
    Port p(true);
    Torch t(p.bus, GuardConfig(), cap);
    uint64_t start = 0;
    unsigned completed = 0;
    for (const unsigned ma : kLadderLevelsMa) {
      if (ma > t.maxMa()) break;
      CHECK(ma == (completed + 1) * 100);
      const auto on = t.request(start, ma, kLadderWindowSeconds, {});
      CHECK(on.lit && on.ma == ma && on.steps.ok && on.steps.writes == 12 && on.steps.readbacks == 12);
      CHECK(on.live.ok && on.live.lit(kPianoChannelMask));
      t.setTestMode(true);
      const unsigned target = ma / 10 - 1;
      CHECK(a.flash(kRegItarget0) == target && a.flash(kRegItarget0 + 1) == target);
      CHECK(a.flash(kRegTorchClamp) == cap / 10 - 1 && a.flash(kRegIres) == kPianoChannelMask);
      CHECK(t.deadlineMs() == start + 10000);
      for (unsigned elapsed = 0; elapsed < 10000; elapsed += 500)
        CHECK(t.poll(start + elapsed, {}, /*ladder=*/true).lit);
      const auto off = t.poll(start + 10000, {}, /*ladder=*/true);
      CHECK(!off.lit && off.reason == Reason::test && off.steps.ok && off.steps.writes == 4 && off.steps.readbacks == 4);
      for (unsigned elapsed = 0; elapsed <= 10000; elapsed += 500)
        CHECK(ladderDarkFault(t.controller().live(), {}, t.config()) == Reason::none);
      CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0 && !t.needsOff() && onlyFlash(a));
      start += 20000;
      ++completed;
    }
    CHECK(completed == cap / 100);
  }
}

static void testLadderReadFailures()
{
  const struct { uint8_t reg, bits; Reason reason; } cases[] = {
    {kRegIntRtSts, 0, Reason::statusUnreadable},
    {kRegIntRtSts, kRtFault, Reason::pmicFault},
    {kRegStatus3, 0x01, Reason::pmicTimer},
  };
  for (const bool dark : {false, true}) {
    for (const auto &[reg, bits, reason] : cases) {
      Arbiter a;
      piano(a);
      Port p(true);
      Torch t(p.bus, GuardConfig(), 400);
      CHECK(t.request(0, 100, kLadderWindowSeconds, {}).lit);
      CHECK(t.poll(10000, {}, /*ladder=*/true).steps.readbacks == 4);
      CHECK(t.request(20000, 200, kLadderWindowSeconds, {}).lit);
      if (dark) CHECK(t.poll(30000, {}, /*ladder=*/true).steps.readbacks == 4);
      const uint64_t sample = dark ? 30500 : 20500;
      if (!bits) a.unreadableRegs[reg] = true;
      else a.r(kPpid, reg) = bits;
      Torch::Outcome off;
      if (dark) {
        const auto fault = ladderDarkFault(t.controller().live(), {}, t.config());
        CHECK(fault == reason);
        off = t.turnOff(sample, fault);
      } else {
        off = t.poll(sample, {}, /*ladder=*/true);
      }
      CHECK(!off.lit && off.reason == reason && off.steps.ok && off.steps.writes == 4 && off.steps.readbacks == 4);
      CHECK(!t.needsOff() && a.flash(kRegModuleEn) == 0 && a.flash(kRegChanEn) == 0 && onlyFlash(a));
    }
  }
}

static void testFailures()
{
  Arbiter a;
  piano(a);
  a.ignoresWrites[uint32_t(kPpid) << 8 | (kRegItarget0 + 1)] = true;
  a.r(kPpid, kRegItarget0 + 1) = 0x11;
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, 100);
  Temperatures temps;
  Torch::Outcome o = t.request(1000, 50, 0, temps);
  CHECK(!o.lit && !t.lit() && o.reason == Reason::writeFailed && o.steps.why && o.steps.failedReg == kRegItarget0 + 1);
  CHECK(o.steps.failedWant == 4 && o.steps.failedGot == 0x11);
  CHECK(a.flash(kRegModuleEn) == 0 && a.flash(kRegChanEn) == 0);   // never enabled, and off ran
  const uint64_t cooldown = t.coolUntilMs();
  o = t.request(1000 + 59000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::writeFailed && t.needsOff() && t.coolUntilMs() == cooldown);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);   // still retrying OFF during cooldown
  a.ignoresWrites.clear();
  t.poll(1000 + 59500, temps);
  o = t.request(1000 + 60000, 50, 0, temps);
  CHECK(o.lit);
  t.request(70000, 0, 0, temps);

  // the arbiter denies (another owner): the first write fails, nothing changes
  piano(a, 3);
  Port q(true);
  Torch u(q.bus, g, 100);
  o = u.request(1000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::writeFailed && a.writes.empty() && q.port.writeFailures >= 1 && (q.port.lastWriteStatus & 0xe));

  // a fault the module reports right after on: off again
  piano(a);
  a.r(kPpid, kRegStatus2) = kSts2Otst1;
  Port r(true);
  Torch w(r.bus, g, 100);
  o = w.request(1000, 50, 0, temps);
  CHECK(!o.lit && o.reason == Reason::pmicThermal && a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
}

static void testGuard()
{
  Arbiter a;
  piano(a);
  Port p(true);
  GuardConfig g;
  Torch t(p.bus, g, 100);
  Temperatures temps;
  // the time limit
  CHECK(t.request(0, 50, 5, temps).lit);
  CHECK(t.poll(4000, temps).lit);
  Torch::Outcome o = t.poll(5000, temps);
  CHECK(!o.lit && o.reason == Reason::timeout && a.flash(kRegChanEn) == 0);
  // the boot test names itself
  CHECK(t.request(6000, 50, 3, temps).lit);
  t.setTestMode(true);
  o = t.poll(9000, temps);
  CHECK(!o.lit && o.reason == Reason::test);
  // the module's own bits, while on
  CHECK(t.request(10000, 50, 0, temps).lit);
  a.r(kPpid, kRegStatus2) = kSts2BobIlimS1;
  o = t.poll(11000, temps);
  CHECK(!o.lit && o.reason == Reason::pmicCurrent);
  a.r(kPpid, kRegStatus2) = 0;
  CHECK(!t.request(12000, 50, 0, temps).lit);   // cooling down
  CHECK(t.request(71000, 50, 0, temps).lit);
  a.r(kPpid, kRegStatus1) = 0x01;   // channel 0 short
  o = t.poll(72000, temps);
  CHECK(!o.lit && o.reason == Reason::shortCircuit);
  a.r(kPpid, kRegStatus1) = 0;
  // temperatures: refused when hot, off when it gets hot
  temps.socValid = true;
  temps.socDeci = 900;
  CHECK(t.request(200000, 50, 0, temps).reason == Reason::socHot && !t.lit());
  temps.socDeci = 500;
  CHECK(t.request(200000, 50, 0, temps).lit);
  temps.batteryValid = true;
  temps.batteryCenti = 4600;
  o = t.poll(201000, temps);
  CHECK(!o.lit && o.reason == Reason::batteryHot);
  temps = Temperatures();
  // one unreadable poll is tolerated, two are not
  CHECK(t.request(300000, 50, 0, temps).lit);
  a.failReads = 1;
  CHECK(t.poll(301000, temps).lit);
  CHECK(t.poll(302000, temps).lit);
  // spmiRead retries a failed command four times (SpmiObserver.h), so a single failed command no longer makes a poll unreadable:
  // from here every read fails, the first unreadable poll is tolerated, the second is not
  a.failReads = 1000;
  CHECK(t.poll(303000, temps).lit);
  o = t.poll(304000, temps);
  CHECK(!o.lit && o.reason == Reason::statusUnreadable);
  a.failReads = 0;
  CHECK(t.poll(305000, temps).steps.ok && !t.needsOff());
  // someone (or the module) turned the channels off
  CHECK(t.request(400000, 50, 0, temps).lit);
  a.r(kPpid, kRegChanEn) = 0x01;
  o = t.poll(401000, temps);
  CHECK(!o.lit && o.reason == Reason::lostState);
  // sleep and shutdown: off with their reason
  CHECK(t.request(500000, 50, 0, temps).lit);
  o = t.turnOff(500500, Reason::sleep);
  CHECK(!o.lit && t.lastReason() == Reason::sleep && a.flash(kRegModuleEn) == 0);
  // the open bit: register-perfect but no light — the guard reports it instead of calling the torch fine
  CHECK(t.request(600000, 50, 0, temps).lit);
  a.r(kPpid, kRegStatus1) = 0x20;   // channel 3's (index 2's) open bit is not piano's; nothing happens
  CHECK(t.poll(601000, temps).lit);
  a.r(kPpid, kRegStatus1) = 0x02;   // channel 0 open
  o = t.poll(602000, temps);
  CHECK(!o.lit && o.reason == Reason::channelOpen && a.flash(kRegChanEn) == 0);
  CHECK(!t.request(610000, 50, 0, temps).lit);   // it cools down like a fault
  a.r(kPpid, kRegStatus1) = 0;
  CHECK(onlyFlash(a));
}

static void testGuardRegressions()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Torch t(p.bus, GuardConfig(), kHardMaxMa);
  Temperatures temps;

  CHECK(t.request(0, 50, 10, temps).lit);
  a.failReads = 1000;
  temps.batteryValid = true;
  temps.batteryCenti = 4500;
  CHECK(t.poll(500, temps).reason == Reason::batteryHot);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);

  a.failReads = 0;
  temps = {};
  CHECK(t.poll(1000, temps).steps.ok && !t.needsOff());
  CHECK(t.request(61000, 50, 10, temps).lit);
  a.r(kPpid, kRegIntRtSts) = kRtFault;
  CHECK(!t.poll(61500, temps).lit);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);

  piano(a);
  Torch retry(p.bus, GuardConfig(), kHardMaxMa);
  CHECK(retry.request(0, 50, 1, temps).lit);
  const uint32_t enable = uint32_t(kPpid) << 8 | kRegChanEn;
  a.ignoresWrites[enable] = true;
  CHECK(!retry.poll(1000, temps).steps.ok && a.flash(kRegModuleEn) == 0);
  a.ignoresWrites.erase(enable);
  retry.poll(1500, temps);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
}

static void testPartialGuardSample()
{
  for (uint8_t lost : {kRegChanEn, kRegModuleEn}) {
    Arbiter a;
    piano(a);
    Port p(true);
    Torch t(p.bus, GuardConfig(), 100);
    CHECK(t.request(0, 100, 10, {}).lit);
    a.unreadableRegs[kRegStatus1] = true;
    a.r(kPpid, lost) = lost == kRegChanEn ? 1 : 0;
    const auto trip = t.poll(1, {});
    CHECK(trip.sampled && !trip.live.ok && trip.reason == Reason::lostState && !trip.lit && trip.steps.ok);
    CHECK(trip.steps.writes == 4 && a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  }
}

static void testActivationSpmiFaults()
{
  // The final channel-enable command may reach the PMIC even if SPMI times out.
  for (unsigned step = 1; step <= 12; ++step) {
    for (uint32_t status : {0u, 0x3u}) {
      Arbiter a;
      piano(a);
      Port p(true);
      Torch t(p.bus, GuardConfig(), 100);
      a.failWriteAt = step;
      a.failedWriteStatus = status;
      a.applyFailedWrite = status == 0;
      const auto on = t.request(0, 100, 10, {});
      CHECK(!on.lit && !on.steps.ok && on.reason == Reason::writeFailed && p.port.writeFailures == 1);
      CHECK(on.steps.writes == step - 1 && on.cleanedUp && on.cleanup.ok && on.cleanup.readbacks == 4);
      CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
      CHECK(a.flash(kRegItarget0) == 0 && a.flash(kRegItarget0 + 1) == 0);
      CHECK(!t.needsOff() && t.coolUntilMs() == 60000 && onlyFlash(a));
    }
  }
}

static void testTeardownRetry()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Torch t(p.bus, GuardConfig(), 100);
  CHECK(t.request(0, 100, 10, {}).lit);
  a.apids[gChApid].owner = 1;   // Neither synchronous OFF attempt can reach the PMIC.
  const auto sleep = t.quiesce(1, Reason::sleep);
  CHECK(!sleep.steps.ok && !t.operational() && t.needsOff() && p.port.writeFailures == 8);
  CHECK(a.flash(kRegChanEn) == kPianoChannelMask && a.flash(kRegModuleEn) == kModuleEnBit);
  char state[128];
  t.describe(1, state, sizeof state);
  CHECK(std::strcmp(state, "off unconfirmed (system sleep)") == 0);
  a.apids[gChApid].owner = 0;
  CHECK(!t.request(2, 100, 10, {}).lit && p.port.writeFailures == 8);
  t.resume();
  // An ON request arriving before the worker's next poll must service the pending OFF.
  const auto retry = t.request(3, 100, 10, {});
  CHECK(!retry.lit && retry.reason == Reason::writeFailed && !t.needsOff());
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  CHECK(t.request(4, 100, 10, {}).lit);
  a.failWriteAt = a.writeAttempts + 1;
  a.failedWriteStatus = 0;
  const auto shutdown = t.quiesce(5, Reason::shutdown);
  CHECK(shutdown.steps.ok && shutdown.steps.writes == 7 && shutdown.steps.readbacks == 7);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0 && !t.needsOff());
  t.resume();
  CHECK(!t.operational() && !t.request(60001, 100, 10, {}).lit);
}

static void testBootOff()
{
  for (bool asleep : {false, true}) {
    Arbiter a;
    piano(a);
    a.r(kPpid, kRegModuleEn) = kModuleEnBit;
    a.r(kPpid, kRegChanEn) = kPianoChannelMask;
    Port p(true);
    Torch t;
    if (asleep) t.quiesce(0, Reason::sleep);
    t.configure(p.bus, GuardConfig(), 100);
    const auto boot = t.checkBootState(1, survey(p.bus));
    if (asleep) {
      CHECK(!t.operational() && t.needsOff() && a.writes.empty());
      t.resume();
      const auto retry = t.poll(2, {});
      CHECK(retry.steps.ok && retry.steps.readbacks == 4 && retry.reason == Reason::bootFoundOn);
    } else {
      CHECK(boot.steps.ok && boot.steps.readbacks == 4 && boot.reason == Reason::bootFoundOn);
    }
    CHECK(!t.lit() && !t.needsOff() && a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  }
}

static void testProbeReadback()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Controller c(p.bus);
  a.unreadableRegs[kRegStatus1] = true;
  a.r(kPpid, kRegIntRtSts) = kRtRampUpDone;
  a.r(kPpid, kRegModuleEn) = kModuleEnBit;
  a.r(kPpid, kRegChanEn) = kPianoChannelMask;
  const Controller::Live partial = c.live();
  CHECK(!partial.ok && !partial.statusOk && partial.rtOk && partial.moduleOk && partial.channelsOk);
  CHECK(partial.rt == kRtRampUpDone && partial.lit(kPianoChannelMask));
  a.r(kPpid, kRegChanEn) = kPianoChannelMask | 0x04;
  CHECK(!c.live().lit(kPianoChannelMask));
}

static void testProbeSafety()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Torch t;
  constexpr uint8_t pianoPair = kPianoChannelMask, sparePair = kOtherChannelMask;
  Temperatures cool, hot;
  hot.socValid = true;
  hot.socDeci = 850;
  t.configure(p.bus, GuardConfig(), 400, true);
  CHECK(t.requestProbe(0, 400, 15000, hot, sparePair, false).reason == Reason::socHot && a.writes.empty());
  CHECK(t.requestProbe(0, 50, 0, hot, pianoPair, true).reason == Reason::socHot && a.writes.empty());
  a.r(kPpid, kRegStatus2) = kSts2VphLow;
  CHECK(t.requestProbe(0, 400, 15000, cool, sparePair, false).reason == Reason::pmicVoltage && a.writes.empty());
  a.r(kPpid, kRegStatus2) = 0;

  // Only the selected pair's open bits trip its window. An open on A can still be followed by B.
  a.r(kPpid, kRegStatus1) = 0x02;
  auto open = t.requestProbe(0, 100, 15000, cool, pianoPair, false);
  CHECK(!open.lit && open.reason == Reason::channelOpen && open.sampled && open.live.chanEn == pianoPair);
  CHECK(open.cleanedUp && open.cleanup.ok && a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  const size_t beforePulse = a.writes.size();
  CHECK(t.requestProbe(500, 50, 0, cool, pianoPair, true).reason == Reason::channelOpen && a.writes.size() == beforePulse);
  const auto spare = t.requestProbe(1000, 9999, 15000, cool, sparePair, false);
  CHECK(spare.lit && spare.ma == 400 && spare.live.chanEn == sparePair && spare.steps.readbacks == 12);
  CHECK(a.flash(kRegItarget0 + 2) == 39 && a.flash(kRegItarget0 + 3) == 39 && a.flash(kRegTorchClamp) == 39);
  const auto asleep = t.quiesce(1100, Reason::sleep);
  CHECK(asleep.reason == Reason::sleep && asleep.steps.ok && asleep.steps.writes == 4);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0 && a.flash(kRegItarget0 + 2) == 0);
  const size_t afterOff = a.writes.size();
  CHECK(!t.request(62000, 100, 0, cool).lit && a.writes.size() == afterOff);
  t.resume();
  CHECK(t.requestProbe(62000, 100, 15000, cool, sparePair, false).reason == Reason::sleep && a.writes.size() == afterOff);
  a.r(kPpid, kRegStatus1) = 0;
  CHECK(t.request(62000, 100, 0, cool).lit);   // a new runtime request may run after wake
  CHECK(t.quiesce(62001, Reason::shutdown).steps.ok);
  t.resume();
  CHECK(!t.operational() && !t.request(70000, 50, 0, cool).lit);
  CHECK(onlyFlash(a));
}

static void testProbePulse()
{
  Arbiter a;
  piano(a);
  Port p(true);
  Temperatures t;
  Torch low(p.bus, GuardConfig(), 40);
  CHECK(!low.requestProbe(0, 100, 0, t, kPianoChannelMask, true).lit && a.writes.empty());
  Torch pulse(p.bus, GuardConfig(), 400);
  const auto factory = pulse.requestProbe(0, 500, 15000, t, kPianoChannelMask, false);
  CHECK(factory.lit && factory.ma == 400 && factory.steps.writes == 12 && factory.steps.readbacks == 12);
  CHECK(a.flash(kRegItarget0) == 39 && a.flash(kRegItarget0 + 1) == 39 && a.flash(kRegTorchClamp) == 39);
  CHECK(pulse.poll(15000, t).steps.readbacks == 4);
  const uint64_t start = 16000;
  const auto started = pulse.requestProbe(start, 9999, 0, t, kPianoChannelMask, true);
  CHECK(started.lit && started.ma == 50 && started.sampled && started.live.chanEn == kPianoChannelMask);
  CHECK(pulse.deadlineMs() == start + 80 && (a.flash(kRegItarget0) & kItargetMask) == 1);
  CHECK(pulse.poll(start + 79, t).lit);
  a.r(kPpid, kRegStatus3) = 0x05;   // both timers expired
  a.r(kPpid, kRegChanEn) = 0;
  const auto ended = pulse.poll(start + 80, t);
  CHECK(!ended.lit && ended.reason == Reason::test && ended.sampled && ended.live.status3 == 0x05);
  CHECK(ended.steps.ok && ended.steps.writes == 4 && a.flash(kRegModuleEn) == 0);
  CHECK(pulse.requestProbe(start + 500, 50, 0, t, kPianoChannelMask, true).lit);
  t.batteryValid = true;
  t.batteryCenti = 4500;
  const auto tripped = pulse.poll(start + 520, t);
  CHECK(!tripped.lit && tripped.reason == Reason::batteryHot && tripped.steps.writes == 4);
  CHECK(a.flash(kRegChanEn) == 0 && a.flash(kRegModuleEn) == 0);
  t = {};
  CHECK(pulse.requestProbe(start + 62000, 50, 0, t, kPianoChannelMask, true).lit);
  CHECK(pulse.quiesce(start + 62001, Reason::shutdown).steps.ok && a.flash(kRegModuleEn) == 0 && a.flash(kRegChanEn) == 0);
  CHECK(onlyFlash(a));
}

int main()
{
  testProbeOptIn();
  testConstants();
  testAllowlist();
  testRequestBoundaries();
  testSurvey();
  testOnOff();
  testLadder();
  testLadderReadFailures();
  testFailures();
  testGuard();
  testGuardRegressions();
  testPartialGuardSample();
  testActivationSpmiFaults();
  testTeardownRetry();
  testBootOff();
  testProbeReadback();
  testProbeSafety();
  testProbePulse();
  if (failures) {
    std::printf("torch_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("torch_host: all passed\n");
  return 0;
}
