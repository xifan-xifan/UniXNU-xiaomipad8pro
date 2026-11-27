// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTorch's portable part (docs/drivers/torch.md section 4): the SPMI write channel, the level 1 survey of the PM8550 flash
// module, the only writes level 2 makes (an allowlist of registers of that one peripheral, each written read-modify-write
// under its mask and read back), the torch on/off sequences, and the guard that turns the torch off (time limit, the module's own
// fault and over-temperature bits, the SoC and battery temperatures other drivers publish, a lost or unreadable state).
// No I/O of its own: reads and writes go through a Bus the kext (PianoTorch.cpp) or the host test (tests/torch_host.cpp) gives.
#ifndef PIANO_TORCH_CORE_H
#define PIANO_TORCH_CORE_H
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#include <string.h>
#endif
#include "FlashHw.h"
#include "../include/PianoSpmi.h"   // the SPMI lock and the transactions every kext shares

namespace pianotorch {

// A diagnostic switch needs an explicit =1 token. The last mention of the switch wins.
inline bool bootSwitchEnabled(const char *args, const char *name)
{
  if (!args) return false;
  const size_t keyLen = strlen(name);
  bool enabled = false;
  while (*args) {
    while (*args == ' ' || *args == '\t') ++args;
    const char *end = args;
    while (*end && *end != ' ' && *end != '\t') ++end;
    const size_t len = size_t(end - args);
    if (len >= keyLen && memcmp(args, name, keyLen) == 0 && (len == keyLen || args[keyLen] == '='))
      enabled = len == keyLen + 2 && args[keyLen + 1] == '1';
    args = end;
  }
  return enabled;
}

// ------------------------------------------------------------------------------------------------- the SPMI write channel
// The write channel of one APID: its 0x20 bytes at chnls + 0x1000 * APID (the kext maps that one page and nothing else of the
// channel window). rd/wr take an offset into the channel.
struct ChnlIo {
  void *ctx;
  uint32_t (*rd)(void *ctx, uint32_t offset);
  void (*wr)(void *ctx, uint32_t offset, uint32_t value);
  void (*delayUs)(void *ctx, unsigned us);
};

enum class WriteResult : uint8_t { ok, error, timeout };

// One register write of one byte (spmi-pmic-arb v7: the data first, then the command; status bit 0 done, bits 1-3 failure): the
// transaction is package/include/PianoSpmi.h's, under the SPMI lock every kext shares.
inline WriteResult spmiWriteByte(const ChnlIo &io, uint8_t reg, uint8_t value, uint32_t *status = nullptr)
{
  return WriteResult(uint8_t(pianospmi::write([&](uint32_t off) { return io.rd(io.ctx, off); },
                                              [&](uint32_t off, uint32_t v) { io.wr(io.ctx, off, v); },
                                              [&](unsigned us) { io.delayUs(io.ctx, us); }, reg, &value, 1, status)));
}

// ------------------------------------------------------------------------------------------------------------- the bus
// read: bytes (1..8) from reg over the observer channel; write: one byte over the owner channel (null at level 1).
struct Bus {
  void *ctx = nullptr;
  bool (*read)(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes) = nullptr;
  bool (*write)(void *ctx, uint8_t reg, uint8_t value) = nullptr;
};

// ------------------------------------------------------------------------------------------------------- level 1: survey
// The byte ranges level 1 reads (8 bytes a read): the peripheral's whole 256 bytes. The first T2 boots read only the ranges the
// sources name; the rest (0x20-0x37, 0x50-0x77, 0x80-0xe7) had never been read when the torch lit no light with every named
// register correct, so the survey now covers everything the peripheral decodes (a 256-byte region; reads only).
struct Range {
  uint8_t start, bytes;
};
constexpr Range kSurveyRanges[] = {
    {0x00, 32}, {0x20, 32}, {0x40, 32}, {0x60, 32}, {0x80, 32}, {0xa0, 32}, {0xc0, 32}, {0xe0, 32},
};

struct Survey {
  bool identity = false;     // TYPE/SUBTYPE/REVISION answered
  bool flash = false;        // TYPE = 0x18
  bool fourChannel = false;  // SUBTYPE = 0x07: the layout FlashHw.h describes
  bool complete = false;     // every range answered
  uint8_t revision = 0, type = 0, subtype = 0;
  uint8_t reg[256] = {};
  bool valid[256] = {};
  unsigned reads = 0, failures = 0;

  // The four-channel decode (meaningful with fourChannel).
  bool moduleEn() const { return reg[kRegModuleEn] & kModuleEnBit; }
  uint8_t chanEn() const { return reg[kRegChanEn] & 0x0f; }
  bool lit() const { return moduleEn() && (chanEn() & kPianoChannelMask); }
  ChannelView channel(unsigned ch) const
  {
    ChannelView v;
    v.itarget = reg[kRegItarget0 + ch] & kItargetMask;
    v.strobe = reg[kRegStrobe0 + ch] & kStrobeMask;
    v.timer = reg[kRegTimer0 + ch];
    v.torchSteps = reg[kRegIres] & (1u << ch);
    v.on = chanEn() & (1u << ch);
    return v;
  }
  Faults faults() const { return decodeFaults(reg[kRegStatus1], reg[kRegStatus2], reg[kRegStatus3], kPianoChannelMask); }
};

inline Survey survey(const Bus &bus, bool stopOnFailure = false)
{
  Survey s;
  uint8_t id[8] = {};
  ++s.reads;
  if (!bus.read(bus.ctx, 0x00, id, 8)) {
    ++s.failures;
    return s;
  }
  s.identity = true;
  s.revision = id[kRegRevision];
  s.type = id[kRegType];
  s.subtype = id[kRegSubtype];
  s.flash = s.type == kTypeFlash;
  s.fourChannel = s.flash && s.subtype == kSubtype4ch;
  if (!s.flash) return s;   // not the flash module: nothing more is read
  s.complete = true;
  for (const Range &r : kSurveyRanges) {
    for (unsigned at = r.start; at < unsigned(r.start) + r.bytes; at += 8) {
      uint8_t b[8] = {};
      ++s.reads;
      if (!bus.read(bus.ctx, uint8_t(at), b, 8)) {
        ++s.failures;
        s.complete = false;
        if (stopOnFailure) return s;
        continue;
      }
      for (unsigned i = 0; i < 8; ++i) {
        s.reg[at + i] = b[i];
        s.valid[at + i] = true;
      }
    }
  }
  return s;
}

// Why level 2 must not write, or nullptr. ownerEe: the owner of the flash APID (cnfg); the arbiter drops another EE's writes.
// channels: the channels this driver drives (kPianoChannelMask normally; the probe may name another pair).
inline const char *writeRefusal(const Survey &s, int apid, uint32_t ownerEe, uint8_t channels = kPianoChannelMask)
{
  if (apid < 0) return "the flash module's PPID 0x1ee is not in the SPMI arbiter's APID map";
  if (ownerEe != 0) return "the flash APID belongs to another execution environment (owner EE is not 0): the arbiter refuses our writes";
  if (!s.identity) return "the module did not answer the identity read";
  if (!s.flash) return "TYPE is not 0x18: not a flash LED module";
  if (!s.fourChannel) return "SUBTYPE is not 0x07: not the four-channel layout this driver knows";
  if (!s.complete) return "part of the survey did not read: the state is not known";
  if (s.chanEn() & uint8_t(~channels) & 0x0f) return "a channel outside this driver's set is enabled: someone else drives this module";
  return nullptr;
}

// ----------------------------------------------------------------------------------------------- level 2: the writes
// Every register level 2 may write and the bits it may change in it. Nothing else is ever written (Controller::put refuses).
// Channels 3 and 4 (bits 2 and 3) are the factory's spare channels, not wired to piano's lamp on this board per the factory
// tree; their registers are in the allowlist only so the probe (piano-torch-probe) can drive them once, to settle which
// channels the rear LED really sits on — a boot with the probe off never writes them.
struct Writable {
  uint8_t reg, mask;
};
constexpr Writable kWritable[] = {
    {kRegTimer0 + 0, 0xff},  {kRegTimer0 + 1, 0xff},  {kRegTimer0 + 2, 0xff},  {kRegTimer0 + 3, 0xff},
    {kRegItarget0 + 0, kItargetMask}, {kRegItarget0 + 1, kItargetMask}, {kRegItarget0 + 2, kItargetMask},
    {kRegItarget0 + 3, kItargetMask}, {kRegModuleEn, kModuleEnBit}, {kRegIres, 0x0f},
    {kRegStrobe0 + 0, kStrobeMask}, {kRegStrobe0 + 1, kStrobeMask}, {kRegStrobe0 + 2, kStrobeMask},
    {kRegStrobe0 + 3, kStrobeMask}, {kRegChanEn, 0x0f}, {kRegTorchClamp, kClampMask},
};
inline bool writable(uint8_t reg, uint8_t mask)
{
  if (!mask) return false;
  for (const Writable &w : kWritable)
    if (w.reg == reg) return (mask & ~w.mask) == 0;
  return false;
}

// The faults as one word list for a log line ("none" when there is none).
inline void formatFaults(const Faults &f, char *out, unsigned size)
{
  if (!f.any()) {
    snprintf(out, size, "none");
    return;
  }
  unsigned at = 0;
  const char *parts[] = {"SHORT", "OPEN", "TIMER-EXPIRED", "OVER-TEMPERATURE", "CURRENT-LIMIT", "LOW-SUPPLY"};
  const bool have[] = {f.shortCircuit, f.open, f.timeout, f.thermal, f.current, f.voltage};
  for (unsigned i = 0; i < 6; ++i) {
    if (!have[i] || at + 1 >= size) continue;
    if (at) out[at++] = ' ';
    at += unsigned(snprintf(out + at, size - at, "%s", parts[i]));
    if (at >= size) at = size - 1;
  }
  out[size - 1] = 0;
}

constexpr unsigned kDefaultLevel = 0;
constexpr unsigned kDefaultOnMa = kFactoryTorchMaxMa;   // 2 x 200 mA, configured lit setting (OWNER-0801)
constexpr unsigned kDefaultMaxMa = kFactoryTorchMaxMa;  // 2 x 200 mA, ITARGET and TORCH_CLAMP 39 (OWNER-0801)
constexpr unsigned kHardMaxMa = kFactoryTorchMaxMa;     // 2 x 200 mA, ITARGET and TORCH_CLAMP 39
constexpr unsigned kLadderLevelsMa[] = {100, 200, 300, 400};
constexpr unsigned kLadderWindowSeconds = 10, kLadderWindowMs = kLadderWindowSeconds * 1000;
constexpr uint8_t kProbeFlashItarget = 1, kProbeFlashTicks = 8;
constexpr unsigned kProbeFlashMs = kProbeFlashTicks * kTimerStepMs;
constexpr unsigned kProbeFlashMa = (kProbeFlashItarget + 1) * kPianoChannels * kFlashStepUa / 1000;
constexpr unsigned kProbeMaxSeconds = 60, kProbePollMs = 500, kPulsePollMs = 20;
static_assert(kProbeFlashMa == 50 && kProbeFlashMa <= kHardMaxMa && kProbeFlashTicks == 8 && kProbeFlashMs == 80,
              "bounded T4 pulse");
inline unsigned clampMa(unsigned ma, unsigned maxMa)
{
  if (maxMa > kHardMaxMa) maxMa = kHardMaxMa;
  if (maxMa < kTorchMinMa) maxMa = kTorchMinMa;
  return ma < kTorchMinMa ? kTorchMinMa : ma > maxMa ? maxMa : ma;
}

// Clamp at the ABI width so oversized arguments cannot wrap to OFF or to the default timeout.
constexpr unsigned boundedRequest(uint64_t value, unsigned limit)
{
  return value > limit ? limit : unsigned(value);
}

struct StepResult {
  bool ok = true;
  unsigned writes = 0, readbacks = 0, refused = 0;
  uint8_t failedReg = 0, failedWant = 0, failedGot = 0;
  const char *why = nullptr;   // the first failure
};

class Controller {
 public:
  Controller() = default;
  explicit Controller(const Bus &bus, bool allowSpare = false) { setBus(bus, allowSpare); }
  void setBus(const Bus &bus, bool allowSpare)
  {
    bus_ = bus;
    channels_ = allowSpare ? kPianoChannelMask | kOtherChannelMask : kPianoChannelMask;
  }

  // The survey's bytes as the last known values (a masked write whose read fails merges into these, so off() still writes).
  void seed(const Survey &s)
  {
    for (unsigned i = 0; i < 256; ++i)
      if (s.valid[i]) remember(uint8_t(i), s.reg[i]);
  }

  // A masked write: read (unless the mask is the whole byte), merge, write and compare the read-back.
  bool put(StepResult &r, uint8_t reg, uint8_t mask, uint8_t value)
  {
    return putMasked(r, reg, mask, value, false);
  }

 private:
  bool putMasked(StepResult &r, uint8_t reg, uint8_t mask, uint8_t value, bool emergencyOff)
  {
    if (!writable(reg, mask) || (value & ~mask) || !bus_.write || !authorized(reg, mask)) {
      ++r.refused;
      return fail(r, reg, value, 0, "a write outside the allowlist was refused");
    }
    uint8_t old = 0;
    if (mask != 0xff) {
      if (bus_.read(bus_.ctx, reg, &old, 1)) remember(reg, old);
      else if (emergencyOff && known_[reg]) old = cache_[reg];
      else return fail(r, reg, value, 0, "the read before a masked write failed");
    }
    const uint8_t next = uint8_t((old & ~mask) | value);
    if (!bus_.write(bus_.ctx, reg, next)) return fail(r, reg, value, 0, "the SPMI write failed (status error or timeout)");
    ++r.writes;
    remember(reg, next);
    uint8_t back = 0;
    if (!bus_.read(bus_.ctx, reg, &back, 1)) return fail(r, reg, value, 0, "the read-back failed");
    ++r.readbacks;
    remember(reg, back);
    if ((back & mask) != value) return fail(r, reg, value, uint8_t(back & mask), "the read-back differs from the value written");
    return true;
  }

 public:
  // Torch on at itarget over the channels of mask (one bit a channel, piano's pair by default), with Linux's torch settings:
  // software strobe, 5 mA steps, no safety timer, the clamp set for maxMa. Stops at the first failure; the caller runs off().
  StepResult on(uint8_t itarget, uint8_t clamp, uint8_t mask = kPianoChannelMask)
  {
    StepResult r;
    if (!allowChannels(r, mask)) return r;
    if (!channelsAvailable(r, mask)) return r;
    const uint8_t maxItarget = maxItargetForMask(mask, kTorchStepUa, kHardMaxMa);
    if (clamp > maxItarget) clamp = maxItarget;
    if (itarget > clamp) itarget = clamp;
    bool ok = put(r, kRegChanEn, mask, 0) && put(r, kRegModuleEn, kModuleEnBit, 0);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegStrobe0 + ch), kStrobeMask, kStrobeSoftwareLevelHigh);
    ok = ok && put(r, kRegTorchClamp, kClampMask, clamp);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegItarget0 + ch), kItargetMask, itarget);
    ok = ok && put(r, kRegIres, mask, mask);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegTimer0 + ch), 0xff, 0);
    ok = ok && put(r, kRegModuleEn, kModuleEnBit, kModuleEnBit) && put(r, kRegChanEn, mask, mask);
    return r;
  }

  // A bounded diagnostic FLASH pulse for T4.  FLASH uses the module's 12.5 mA
  // resolution (IRESOLUTION cleared) and its safety timer; it is deliberately
  // separate from the ordinary torch path so a probe can prove the timed mode
  // without leaving a level-triggered torch behind.  The caller always follows
  // this with off(), even though the module's timer should have disabled it.
  StepResult flashPulse(uint8_t itarget, uint8_t timerTicks, uint8_t mask = kPianoChannelMask)
  {
    StepResult r;
    if (!timerTicks || timerTicks > kProbeFlashTicks) {
      ++r.refused;
      fail(r, kRegTimer0, timerTicks, 0, "the FLASH pulse arguments are outside the bounded diagnostic range");
      return r;
    }
    if (!allowChannels(r, mask)) return r;
    if (!channelsAvailable(r, mask)) return r;
    const uint8_t maxItarget = maxItargetForMask(mask, kFlashStepUa, kProbeFlashMa);
    if (itarget > maxItarget) itarget = maxItarget;
    bool ok = put(r, kRegChanEn, mask, 0) && put(r, kRegModuleEn, kModuleEnBit, 0);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegStrobe0 + ch), kStrobeMask, kStrobeSoftwareLevelHigh);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegItarget0 + ch), kItargetMask, itarget);
    // Clear IRESOLUTION for the selected channels: the flash path is 12.5 mA steps.
    ok = ok && put(r, kRegIres, mask, 0);
    for (unsigned ch = 0; ok && ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) ok = put(r, uint8_t(kRegTimer0 + ch), 0xff, uint8_t(kTimerEnable | timerTicks));
    ok = ok && put(r, kRegModuleEn, kModuleEnBit, kModuleEnBit) && put(r, kRegChanEn, mask, mask);
    return r;
  }

  // Torch off: channels, module, current to the lowest step. Every step is tried even after a failure.
  StepResult off(uint8_t mask = kPianoChannelMask)
  {
    StepResult r;
    if (!allowChannels(r, mask)) return r;
    putMasked(r, kRegChanEn, mask, 0, true);
    putMasked(r, kRegModuleEn, kModuleEnBit, 0, true);
    for (unsigned ch = 0; ch < kChannels4ch; ++ch)
      if (mask & (1u << ch)) putMasked(r, uint8_t(kRegItarget0 + ch), kItargetMask, 0, true);
    return r;
  }

  // The live state while on: the three status bytes, the real-time fault byte, the enables, over the observer channel.
  struct Live {
    bool ok = false;
    bool statusOk = false, rtOk = false, moduleOk = false, channelsOk = false;
    uint8_t status1 = 0, status2 = 0, status3 = 0, rt = 0, moduleEn = 0, chanEn = 0;
    bool lit(uint8_t mask) const { return (moduleEn & kModuleEnBit) && (chanEn & 0x0f) == mask; }
  };
  Live live() const
  {
    Live l;
    uint8_t st[4] = {};   // 0x06..0x09: STATUS1, STATUS2, (0x08), STATUS3
    l.statusOk = bus_.read(bus_.ctx, kRegStatus1, st, 4);
    l.rtOk = bus_.read(bus_.ctx, kRegIntRtSts, &l.rt, 1);
    l.moduleOk = bus_.read(bus_.ctx, kRegModuleEn, &l.moduleEn, 1);
    l.channelsOk = bus_.read(bus_.ctx, kRegChanEn, &l.chanEn, 1);
    l.ok = l.statusOk && l.rtOk && l.moduleOk && l.channelsOk;
    l.status1 = st[0];
    l.status2 = st[1];
    l.status3 = st[3];
    return l;
  }

 private:
  bool authorized(uint8_t reg, uint8_t mask) const
  {
    if (reg == kRegChanEn || reg == kRegIres) return (mask & ~channels_) == 0;
    constexpr uint8_t bases[] = {kRegTimer0, kRegItarget0, kRegStrobe0};
    for (const uint8_t base : bases) {
      if (reg >= base && reg < base + kChannels4ch) return channels_ & (1u << (reg - base));
    }
    return true;
  }

  bool allowChannels(StepResult &r, uint8_t mask) const
  {
    if (mask && !(mask & ~channels_)) return true;
    ++r.refused;
    return fail(r, kRegChanEn, mask, 0, "the channel mask is outside the authorized set");
  }

  bool channelsAvailable(StepResult &r, uint8_t mask)
  {
    uint8_t enabled = 0;
    if (!bus_.read(bus_.ctx, kRegChanEn, &enabled, 1))
      return fail(r, kRegChanEn, 0, 0, "the channel enables could not be read before activation");
    const uint8_t unexpected = enabled & 0x0f & ~mask;
    if (unexpected)
      return fail(r, kRegChanEn, 0, unexpected, "a channel outside the requested set is already enabled");
    return true;
  }

  static uint8_t maxItargetForMask(uint8_t mask, unsigned stepUa, unsigned maxMa)
  {
    unsigned channels = 0;
    for (unsigned ch = 0; ch < kChannels4ch; ++ch) channels += (mask >> ch) & 1u;
    // Keep the pair's per-channel ceiling even when a single channel is selected.
    if (channels < kPianoChannels) channels = kPianoChannels;
    return uint8_t(maxMa * 1000 / (channels * stepUa) - 1);
  }

  void remember(uint8_t reg, uint8_t value)
  {
    cache_[reg] = value;
    known_[reg] = true;
  }
  static bool fail(StepResult &r, uint8_t reg, uint8_t want, uint8_t got, const char *why)
  {
    if (r.ok) {
      r.ok = false;
      r.failedReg = reg;
      r.failedWant = want;
      r.failedGot = got;
      r.why = why;
    }
    return false;
  }
  Bus bus_;
  uint8_t channels_ = kPianoChannelMask;
  uint8_t cache_[256] = {};
  bool known_[256] = {};
};

// ----------------------------------------------------------------------------------------------------------- the guard
enum class Reason : uint8_t {
  none, user, timeout, test, bootFoundOn, pmicThermal, pmicCurrent, pmicVoltage, shortCircuit, channelOpen, socHot, batteryHot,
  statusUnreadable, lostState, writeFailed, pmicFault, sleep, shutdown, pmicTimer
};
inline const char *reasonName(Reason r)
{
  switch (r) {
    case Reason::none: return "none";
    case Reason::user: return "asked";
    case Reason::timeout: return "the time limit";
    case Reason::test: return "the boot test is over";
    case Reason::bootFoundOn: return "found on when the driver started";
    case Reason::pmicThermal: return "the module's over-temperature bits";
    case Reason::pmicCurrent: return "the module's current-limit bits";
    case Reason::pmicVoltage: return "the module's low-supply bit";
    case Reason::shortCircuit: return "a channel's short-circuit bit";
    case Reason::channelOpen: return "a channel's open-circuit bit (the LED, its supply, or the wiring is absent: register-perfect but no light)";
    case Reason::socHot: return "the SoC temperature (PianoThermal)";
    case Reason::batteryHot: return "the battery temperature (PianoPower)";
    case Reason::statusUnreadable: return "the status could not be read";
    case Reason::lostState: return "the module or channel enables differ from the requested state";
    case Reason::writeFailed: return "a write or its read-back failed";
    case Reason::pmicFault: return "the PMIC real-time fault bit (open, short, or regulator not ready)";
    case Reason::sleep: return "system sleep";
    case Reason::shutdown: return "shutdown or restart";
    case Reason::pmicTimer: return "the PMIC safety timer expired during the ladder";
  }
  return "?";
}

struct GuardConfig {
  uint32_t defaultS = 300;        // piano-torch-timeout: the auto-off of a request without its own time
  uint32_t maxS = 1800;           // no request lasts longer
  int32_t socMaxDeci = 850;       // piano-torch-soc-max (C): the hottest TSENS controller (PianoThermal's warm level)
  int32_t batteryMaxCenti = 4500; // piano-torch-batt-max (C): the gauges' battery temperature
  uint32_t coolDownMs = 60000;    // after a thermal or fault off, no new on for this long
};

// The temperatures other drivers publish (absent when that driver is not running: the time limit and the module's own bits
// still apply).
struct Temperatures {
  bool socValid = false, batteryValid = false, flashValid = false;
  int32_t socDeci = 0, batteryCenti = 0, flashDeci = 0;
};

// Why an on request is refused before any write (the temperatures), or Reason::none.
inline Reason refuseOn(const Temperatures &t, const GuardConfig &c)
{
  if (t.socValid && t.socDeci >= c.socMaxDeci) return Reason::socHot;
  if (t.batteryValid && t.batteryCenti >= c.batteryMaxCenti) return Reason::batteryHot;
  return Reason::none;
}

inline Reason supplyFault(uint8_t status2)
{
  if (status2 & kSts2Thermal) return Reason::pmicThermal;
  if (status2 & kSts2Current) return Reason::pmicCurrent;
  if (status2 & kSts2Voltage) return Reason::pmicVoltage;
  return Reason::none;
}

// A known safety trip takes precedence over unreadable fields and pair-specific open faults.
inline Reason judge(const Controller::Live &l, unsigned &unreadable, const Temperatures &t, const GuardConfig &c,
                    uint8_t mask = kPianoChannelMask)
{
  const Reason hot = refuseOn(t, c);
  if (hot != Reason::none) return hot;
  if (l.statusOk) {
    const Reason supply = supplyFault(l.status2);
    if (supply != Reason::none) return supply;
    const Faults f = decodeFaults(l.status1, l.status2, l.status3, mask);
    if (f.shortCircuit) return Reason::shortCircuit;
    if (f.open && l.ok) return Reason::channelOpen;
  }
  if (l.rtOk && (l.rt & kRtFault)) return Reason::pmicFault;
  // STATUS3 timer expiry is diagnostic. Either enable read can establish lost state.
  if ((l.moduleOk && !(l.moduleEn & kModuleEnBit)) || (l.channelsOk && (l.chanEn & 0x0f) != mask))
    return Reason::lostState;
  if (!l.ok) return ++unreadable >= 2 ? Reason::statusUnreadable : Reason::none;
  unreadable = 0;
  return Reason::none;
}

inline Reason ladderDarkFault(const Controller::Live &l, const Temperatures &t, const GuardConfig &c)
{
  const Reason hot = refuseOn(t, c);
  if (hot != Reason::none) return hot;
  if (l.statusOk) {
    const Reason supply = supplyFault(l.status2);
    if (supply != Reason::none) return supply;
    const Faults f = decodeFaults(l.status1, l.status2, l.status3, kPianoChannelMask);
    if (f.shortCircuit) return Reason::shortCircuit;
    if (f.open) return Reason::channelOpen;
    if (f.timeout) return Reason::pmicTimer;
  }
  if (l.rtOk && (l.rt & kRtFault)) return Reason::pmicFault;
  if ((l.moduleOk && (l.moduleEn & kModuleEnBit)) || (l.channelsOk && (l.chanEn & 0x0f))) return Reason::lostState;
  return l.ok ? Reason::none : Reason::statusUnreadable;
}

inline bool coolsDown(Reason r)
{
  return r == Reason::pmicThermal || r == Reason::pmicCurrent || r == Reason::pmicVoltage || r == Reason::shortCircuit ||
         r == Reason::channelOpen || r == Reason::pmicFault || r == Reason::pmicTimer ||
         r == Reason::socHot || r == Reason::batteryHot || r == Reason::writeFailed;
}

// ------------------------------------------------------------------------------------------------- the torch's state
// One owner (the kext's thread, under its lock) calls these. ma 0 = off.
class Torch {
 public:
  Torch() = default;
  Torch(const Bus &bus, const GuardConfig &c, unsigned maxMa) { configure(bus, c, maxMa); }
  void configure(const Bus &bus, const GuardConfig &c, unsigned maxMa, bool allowSpare = false)
  {
    ctl_.setBus(bus, allowSpare);
    allowSpare_ = allowSpare;
    cfg_ = c;
    maxMa_ = clampMa(maxMa, kHardMaxMa);
  }

  struct Outcome {
    bool lit = false;           // the torch is on after this
    unsigned ma = 0;
    Reason reason = Reason::none;   // why it is off (or why an on was refused)
    StepResult steps;           // the writes of this action
    bool cleanedUp = false;
    StepResult cleanup;         // explicit off after a failed activation
    bool sampled = false;
    Controller::Live live;      // the last sample taken before any cleanup
  };

  Outcome checkBootState(uint64_t nowMs, const Survey &s)
  {
    ctl_.seed(s);
    if (!s.moduleEn() && !s.chanEn()) return {};
    if (operational()) return turnOff(nowMs, Reason::bootFoundOn);
    // Startup may finish surveying while the power callback has control blocked.
    mask_ = kPianoChannelMask;
    lastReason_ = Reason::bootFoundOn;
    return {};
  }

  // On at ma (clamped) for seconds (0: the default; at most maxS), or off when ma is 0.
  Outcome request(uint64_t nowMs, unsigned ma, unsigned seconds, const Temperatures &t, Reason offReason = Reason::user)
  {
    if (!ma) return turnOff(nowMs, offReason);
    if (!seconds) seconds = cfg_.defaultS;
    if (seconds > cfg_.maxS) seconds = cfg_.maxS;
    return activate(nowMs, ma, seconds * 1000, t, kPianoChannelMask, false, false);
  }

  Outcome requestProbe(uint64_t nowMs, unsigned ma, unsigned durationMs, const Temperatures &t, uint8_t mask, bool flash)
  {
    Outcome o;
    if (diagnosticAbort_ != Reason::none) {
      o.reason = diagnosticAbort_;
      return o;
    }
    if ((mask != kPianoChannelMask && !(allowSpare_ && mask == kOtherChannelMask)) ||
        (flash && (mask != kPianoChannelMask || maxMa_ < kProbeFlashMa))) {
      o.reason = Reason::writeFailed;
      return o;
    }
    return activate(nowMs, flash ? kProbeFlashMa : ma, flash ? kProbeFlashMs : boundedRequest(durationMs, kProbeMaxSeconds * 1000),
                    t, mask, true, flash);
  }

 private:
  Outcome activate(uint64_t nowMs, unsigned ma, unsigned durationMs, const Temperatures &t, uint8_t mask,
                   bool diagnostic, bool flash)
  {
    Outcome o;
    const Reason hot = refuseOn(t, cfg_);
    if (hot != Reason::none && lit_) return turnOff(nowMs, hot);
    // A confirmed open on the factory pair can leave the alternate pair worth testing. All module-wide faults stop T4.
    const bool alternate = diagnostic && mask == kOtherChannelMask && lastMask_ == kPianoChannelMask &&
                           coolReason_ == Reason::channelOpen;
    Reason refused = hot;
    if (powerBlock_ != Reason::none) refused = powerBlock_;
    else if (needsOff()) {
      // A stream of ON requests must not starve the OFF retry.
      o = poll(nowMs, t);
      refused = Reason::writeFailed;
    } else if (nowMs < coolUntilMs_ && !alternate) refused = coolReason_;
    if (refused != Reason::none) {
      o.reason = refused;
      o.lit = lit_;
      o.ma = ma_;
      return o;
    }
    if (diagnostic) {
      o.live = ctl_.live();
      o.sampled = true;
      o.reason = o.live.ok ? supplyFault(o.live.status2) : Reason::statusUnreadable;
      if (o.reason != Reason::none) return o;
    }
    ma = clampMa(ma, maxMa_);
    const uint8_t itarget = flash ? kProbeFlashItarget : itargetForMa(ma);
    mask_ = mask;
    o.steps = flash ? ctl_.flashPulse(itarget, kProbeFlashTicks, mask) : ctl_.on(itarget, clampForMa(maxMa_), mask);
    unsigned unreadable = 0;
    Reason bad = Reason::writeFailed;
    if (o.steps.ok) {
      o.live = ctl_.live();
      o.sampled = true;
      bad = judge(o.live, unreadable, t, cfg_, mask);
      if (bad == Reason::none && !o.live.ok) bad = Reason::statusUnreadable;
    } else {
      o.live = {};
      o.sampled = false;
    }
    if (bad != Reason::none) {
      o.cleanup = turnOff(nowMs, bad).steps;
      o.cleanedUp = true;
      o.reason = bad;
      return o;
    }
    lit_ = true;
    ma_ = flash ? kProbeFlashMa : maForItarget(itarget);
    itarget_ = itarget;
    unreadable_ = 0;
    deadlineMs_ = nowMs + durationMs;
    testMode_ = diagnostic;
    flash_ = flash;
    o.lit = true;
    o.ma = ma_;
    return o;
  }

 public:
  // Check the time limit and guards. The ladder additionally stops on the first unreadable sample or a timer fault.
  Outcome poll(uint64_t nowMs, const Temperatures &t, bool ladder = false)
  {
    Outcome o;
    if (powerBlock_ != Reason::none || !mask_) return o;
    if (!lit_) {
      // Retrying the writes keeps the original fault's cooldown.
      const uint64_t until = coolUntilMs_;
      o = turnOff(nowMs, lastReason_);
      coolUntilMs_ = until;
      return o;
    }
    if (nowMs >= deadlineMs_) {
      // On the timed pulse only, sample the timer/enable result immediately before explicit off.
      if (flash_) {
        o.live = ctl_.live();
        o.sampled = true;
      }
      o.reason = testMode_ ? Reason::test : Reason::timeout;
      o.steps = turnOff(nowMs, o.reason).steps;
      return o;
    }
    o.live = ctl_.live();
    o.sampled = true;
    Reason bad = judge(o.live, unreadable_, t, cfg_, mask_);
    if (bad == Reason::none && ladder) {
      if (!o.live.ok) bad = Reason::statusUnreadable;
      else if (decodeFaults(o.live.status1, o.live.status2, o.live.status3, mask_).timeout) bad = Reason::pmicTimer;
    }
    if (bad != Reason::none) {
      o.steps = turnOff(nowMs, bad).steps;
      o.reason = bad;
      return o;
    }
    o.lit = true;
    o.ma = ma_;
    return o;
  }

  Outcome turnOff(uint64_t nowMs, Reason why)
  {
    Outcome o;
    if (!mask_) mask_ = kPianoChannelMask;
    lastMask_ = mask_;
    o.steps = ctl_.off(mask_);
    if (!o.steps.ok) {
      const StepResult first = o.steps;
      o.steps = ctl_.off(mask_);
      o.steps.writes += first.writes;
      o.steps.readbacks += first.readbacks;
    }
    if (o.steps.ok) mask_ = 0;
    settleOff(nowMs, why == Reason::user && !o.steps.ok ? Reason::writeFailed : why);
    o.reason = lastReason_;
    return o;
  }

  // Called under the same lock as both request paths. A wake never resumes an interrupted boot test.
  Outcome quiesce(uint64_t nowMs, Reason why)
  {
    if (powerBlock_ != Reason::shutdown) powerBlock_ = why;
    diagnosticAbort_ = powerBlock_;
    return mask_ ? turnOff(nowMs, powerBlock_) : Outcome();
  }
  void resume() { if (powerBlock_ == Reason::sleep) powerBlock_ = Reason::none; }
  bool operational() const { return powerBlock_ == Reason::none; }
  Reason diagnosticAbort() const { return diagnosticAbort_; }
  bool needsOff() const { return mask_ && !lit_; }
  void setTestMode(bool on) { testMode_ = on; }
  bool lit() const { return lit_; }
  unsigned ma() const { return lit_ ? ma_ : 0; }
  uint8_t itarget() const { return itarget_; }
  unsigned maxMa() const { return maxMa_; }
  Reason lastReason() const { return lastReason_; }
  uint64_t deadlineMs() const { return deadlineMs_; }
  uint64_t coolUntilMs() const { return coolUntilMs_; }
  const GuardConfig &config() const { return cfg_; }
  Controller &controller() { return ctl_; }

  // One line for the piano-torch-state property and the log.
  void describe(uint64_t nowMs, char *out, unsigned size) const
  {
    if (lit_) {
      const uint64_t left = deadlineMs_ > nowMs ? (deadlineMs_ - nowMs + 999) / 1000 : 0;
      snprintf(out, size, "on %u mA (2 x %u mA, itarget %u), off in %llu s", ma_, ma_ / 2, itarget_, (unsigned long long)left);
    } else if (needsOff()) {
      snprintf(out, size, "off unconfirmed (%s)", reasonName(lastReason_));
    } else if (lastReason_ == Reason::none) {
      snprintf(out, size, "off");
    } else {
      snprintf(out, size, "off (%s)", reasonName(lastReason_));
    }
  }

 private:
  void settleOff(uint64_t nowMs, Reason why)
  {
    lit_ = false;
    ma_ = 0;
    lastReason_ = why;
    testMode_ = false;
    flash_ = false;
    if (coolsDown(why)) {
      coolReason_ = why;
      coolUntilMs_ = nowMs + cfg_.coolDownMs;
    }
  }

  Controller ctl_;
  GuardConfig cfg_;
  unsigned maxMa_ = kDefaultMaxMa;
  bool lit_ = false, testMode_ = false, flash_ = false, allowSpare_ = false;
  unsigned ma_ = 0, unreadable_ = 0;
  uint8_t itarget_ = 0, mask_ = 0, lastMask_ = 0;
  uint64_t deadlineMs_ = 0, coolUntilMs_ = 0;
  Reason lastReason_ = Reason::none, coolReason_ = Reason::none;
  Reason powerBlock_ = Reason::none, diagnosticAbort_ = Reason::none;
};

}  // namespace pianotorch
#endif
