// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -working-directory "${stress_objects:=$(mktemp -d "${TMPDIR:-/tmp}/backlight-stress.XXXXXXXX")}" -I"$PWD/../../include" -c "$PWD"/../../PianoSensors/Sensors{Policy,Fixed,Orient}.c && trap 'rm -r "$stress_objects"' EXIT && clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../include -I../../PianoSensors backlight_dual_ic_fault_stress_host.cpp "$stress_objects"/*.o -o /tmp/backlight_dual_ic_fault_stress_host && /tmp/backlight_dual_ic_fault_stress_host

#include "Ktz8866Chip.h"
#include "NativeBrightness.h"
#include "SensorsPolicy.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

using namespace pianobacklight;
using pianoauto::AmbientVersion;

namespace {

struct Profile {
  uint16_t floor, ceiling;
  bool android;
};

constexpr Profile kCandidate{921, 1952, true};
constexpr Profile kAndroidDaily{178, 1952, true};
constexpr Profile kLegacyDaily{178, 2047, false};
constexpr unsigned kCycles = 512; // per profile
constexpr char kSwap[] = "PianoSwapBrightnessV1";
constexpr uint8_t kOff = 0x3f, kOn = 0x7f;
unsigned cycle;
const char *phase = "setup";

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: cycle %u (%s): %s\n", __FILE__, __LINE__, cycle, phase, #condition); \
  std::exit(1); \
} } while (false)

enum class Fault { writeNack, readbackNack, staleReadback };

// Two real BitI2c instances over the shared bit-level chip model. The active
// code changes on an ACKed MSB write, even if the following readback fails.
class Pair final : public Ics {
 public:
  explicit Pair(Profile profile) : profile_(profile)
  {
    for (unsigned ic = 0; ic < kIcs; ++ic) {
      auto &chip = chips_[ic];
      backlighttest::resetRegs(chip.reg);
      chip.reg[kRegCfg1] = kQualifiedPwmCfg1;
      chip.reg[kRegIfs] = kQualifiedPwmIfs;
      chip.reg[kRegPwmLsb] = 0xff;
      chip.reg[kRegPwmMsb] = 7;
      chip.reg[kRegEn] = ic == 0 ? kOn : kOff;
      active_[ic] = uint16_t(profile.floor + 71 + 83 * ic);
      chip.reg[kRegBrtLsb] = uint8_t(active_[ic] & 7);
      chip.reg[kRegBrtMsb] = uint8_t(active_[ic] >> 3);
      buses_[ic].configure(chip.lines([](void *, unsigned) {}), 5, 2000);
    }
  }

  void inject(Fault kind, unsigned ic, uint8_t reg, uint8_t value, unsigned count = 1)
  {
    CHECK(!fault_.remaining && !fault_.readback);
    fault_ = {kind, ic, reg, value, count};
  }

  bool read(unsigned ic, uint8_t reg, uint8_t *value) override
  {
    ++transactions;
    auto &chip = chips_[ic];
    const bool inject = fault_.readback && fault_.ic == ic && fault_.reg == reg;
    if (inject && fault_.kind == Fault::readbackNack) chip.address = kAddress + 1;
    const auto result = buses_[ic].writeRead(kAddress, &reg, 1, value, 1);
    chip.address = kAddress;
    if (inject) {
      fault_.readback = false;
      --fault_.remaining;
      ++faultHits[ic];
      if (fault_.kind == Fault::readbackNack) {
        CHECK(result == I2c::nackAddress);
        return false;
      }
      CHECK(result == I2c::ok && *value != fault_.before);
      *value = fault_.before;
    }
    CHECK(result == I2c::ok);
    if (reg == kRegEn) darkRead_[ic] = *value == kOff && chip.reg[reg] == kOff;
    return true;
  }

  bool write(unsigned ic, uint8_t reg, uint8_t value) override
  {
    ++transactions;
    auto &chip = chips_[ic];
    CHECK(writable(reg));
    if (reg == kRegBrtLsb) CHECK(value <= 7);
    if (reg == kRegEn) {
      CHECK(value == kOff || value == kOn);
      darkRead_[ic] = false;
      if (value == kOn) {
        ++enableAttempts;
        CHECK(active_[0] == active_[1]);
        CHECK(active_[0] >= profile_.floor && active_[0] <= profile_.ceiling);
      }
    }
    const bool inject = fault_.remaining && fault_.ic == ic && fault_.reg == reg && fault_.value == value;
    if (inject && fault_.kind == Fault::writeNack) chip.nackWriteReg = reg;
    const uint8_t before = chip.reg[reg], bytes[] = {reg, value};
    const auto result = buses_[ic].write(kAddress, bytes, sizeof bytes);
    if (inject && fault_.kind == Fault::writeNack) {
      CHECK(result == I2c::nackData && chip.reg[reg] == before);
      --fault_.remaining;
      ++faultHits[ic];
      return false;
    }
    CHECK(result == I2c::ok && chip.reg[reg] == value);
    if (reg == kRegBrtMsb) {
      active_[ic] = registerCode(ic);
      CHECK(active_[ic] >= profile_.floor && active_[ic] <= profile_.ceiling);
    }
    if (inject) {
      fault_.readback = true;
      fault_.before = before;
    }
    return true;
  }

  void done() override
  {
    for (auto &chip : chips_) {
      CHECK(chip.sda() && chip.scl());
      chip.writes.clear();
    }
  }
  void log(const char *) override {}
  uint64_t nowMs() override { return now; }
  uint16_t registerCode(unsigned ic) const
  {
    return uint16_t(chips_[ic].reg[kRegBrtMsb] * 8 + chips_[ic].reg[kRegBrtLsb]);
  }
  uint16_t activeCode(unsigned ic) const { return active_[ic]; }
  uint8_t enable(unsigned ic) const { return chips_[ic].reg[kRegEn]; }
  bool confirmedDark() const { return darkRead_[0] && darkRead_[1] && enable(0) == kOff && enable(1) == kOff; }

  uint64_t now = 0;
  unsigned transactions = 0, enableAttempts = 0;
  std::array<unsigned, kIcs> faultHits{};

 private:
  Profile profile_;
  backlighttest::Chip chips_[kIcs];
  BitI2c buses_[kIcs];
  uint16_t active_[kIcs]{};
  bool darkRead_[kIcs]{};
  struct Injection {
    Fault kind{};
    unsigned ic = 0;
    uint8_t reg = 0, value = 0;
    unsigned remaining = 0;
    bool readback = false;
    uint8_t before = 0;
  } fault_;
};

uint32_t alsTarget(Profile profile, uint32_t lux)
{
  return ps_als_target_permille(profile.android ? PS_ALS_REFERENCE : PS_ALS_LEGACY, profile.floor, lux);
}

int curveCode(Profile profile, uint32_t lux)
{
  return std::clamp(int((uint64_t(alsTarget(profile, lux)) * 2047 + 500) / 1000),
                    int(profile.floor), int(profile.ceiling));
}

// The kext's dispatch and the M5 gate are modeled here. Decode, intent,
// ambient version checks, and register transactions use the live portable code.
class Rig {
 public:
  explicit Rig(Profile profile)
    : profile(profile), bus(profile), native(profile.floor, profile.ceiling, profile.android),
      revision(native.initialRevision())
  {
    Config config;
    config.floor = profile.floor;
    config.bootPercent = 0;
    config.android = config.autoAllowed = profile.android;
    ctl.configure(config);
    CHECK(ctl.survey() && ctl.start());
    CHECK(ctl.floor() == profile.floor && ctl.ceiling() == profile.ceiling && ctl.exponential());
    CHECK(ctl.ic(0).foundCode != ctl.ic(1).foundCode && ctl.ic(0).foundLit != ctl.ic(1).foundLit);
    checkLit(uint16_t(profile.floor + 71));
    display(true);
    if (profile.android) lease.set(kManagedMaxS, bus.now);
  }

  void display(bool on)
  {
    const bool wasLit = ctl.wantLit();
    if (on && !displayOn_) intent.reset(bus.now);
    displayOn_ = on;
    ctl.display(on, 1, uint32_t(on));
    if (wasLit != ctl.wantLit()) restartAmbient();
  }

  void systemSleep(bool asleep)
  {
    if (ctl.asleep() == asleep) return;
    if (!asleep) intent.reset(bus.now);
    ctl.sleep(asleep);
    restartAmbient();
  }

  bool panelOff()
  {
    display(false);
    for (unsigned retry = 0; ctl.pending() && retry < 2; ++retry) ctl.retry();
    if (!ctl.bothDark()) return false;
    CHECK(bus.confirmedDark());
    ++m5Sleeps;
    return true;
  }

  uint32_t swapFixed(double nits) const
  {
    const int32_t asked = nitsFixedFromDouble(std::bit_cast<uint64_t>(nits));
    return asked <= 0 ? 0 : std::clamp(uint32_t(asked), native.minFixed(), native.maxFixed());
  }

  bool swap(double nits)
  {
    const uint32_t fixed = swapFixed(nits);
    if (!fixed) return false;
    const auto choice = streamChoice(intent.classify(fixed, bus.now), owns(), lease.active(bus.now));
    if (choice == StreamChoice::follow) return false;
    const uint16_t code = decode(fixed);
    if (choice == StreamChoice::automaticTrim) CHECK(ctl.userLevel(code, kSwap));
    else ctl.manualCode(code, kSwap);
    return true;
  }

  uint16_t slider(double nits)
  {
    const uint32_t fixed = swapFixed(nits);
    const uint16_t code = decode(fixed);
    if (owns()) {
      const unsigned transactions = bus.transactions;
      CHECK(!swap(nits) && bus.transactions == transactions);
      // The lease lets piano-brightness-sync report the user's SDR level.
      intent.moved(fixed);
      CHECK(ctl.userLevel(code, "piano-brightness-sync"));
    } else {
      CHECK(swap(nits));
    }
    CHECK(ctl.code() == code);
    return code;
  }

  uint16_t decode(uint32_t fixed) const
  {
    uint16_t code = 0;
    CHECK(native.decode(fixed, code));
    CHECK(code >= profile.floor && code <= profile.ceiling);
    return code;
  }

  bool ambient(uint32_t lux, AmbientVersion version)
  {
    if (revision) {
      if (!version.newer(revision, 1, taken_)) return false;
      taken_ = version.seq;
      if (!owns() || !ctl.wantLit()) return false;
    }
    return ctl.ambient(int32_t(alsTarget(profile, lux)), lux, "PianoSensors");
  }

  AmbientVersion nextAmbient() const { return {taken_ + 1, revision, 1}; }
  void checkAmbient(uint32_t lux, uint16_t expected)
  {
    CHECK(ambient(lux, nextAmbient()) == profile.android);
    CHECK(ctl.autoActive() == profile.android);
    checkLit(expected);
  }

  void checkPair(uint16_t code, uint8_t enable) const
  {
    for (unsigned ic = 0; ic < kIcs; ++ic) {
      CHECK(bus.enable(ic) == enable);
      CHECK(bus.activeCode(ic) == code && bus.registerCode(ic) == code);
    }
  }

  void checkLit(uint16_t code) const
  {
    checkPair(code, kOn);
    uint16_t reported = 0;
    CHECK(ctl.bothLit() && !ctl.pending() && ctl.pwmQualified());
    CHECK(native.applied(ctl, reported) && reported == code);
  }

  void checkUnapplied() const
  {
    uint16_t reported = 0;
    CHECK(ctl.pending() && !native.applied(ctl, reported));
  }

  Profile profile;
  Pair bus;
  Controller ctl{bus};
  NativeBrightness native;
  NativeIntent intent;
  Lease lease;
  uint32_t revision;
  unsigned m5Sleeps = 0;

 private:
  bool owns() const { return ctl.ambientState().requested() && !ctl.autoFault(); }
  void restartAmbient()
  {
    if (!revision) return;
    ++revision;
    taken_ = 0;
    ctl.invalidateAmbient(false);
  }
  bool displayOn_ = false;
  uint64_t taken_ = 0;
};

void testStaleWakeReadback()
{
  phase = "stale BL_EN readback after a successful wake write";
  for (unsigned ic = 0; ic < kIcs; ++ic) {
    Rig rig(kCandidate);
    const uint16_t code = rig.ctl.code();
    CHECK(rig.panelOff());
    rig.bus.inject(Fault::staleReadback, ic, kRegEn, kOn);
    rig.display(true);
    CHECK(rig.bus.faultHits[ic] == 1 && rig.ctl.errors() == 1);
    rig.checkUnapplied();
    rig.checkPair(code, kOff);
    CHECK(rig.bus.confirmedDark() && rig.ctl.bothDark());
    CHECK(rig.panelOff()); // An interrupted wake can still safely go back to M5.
    rig.display(true);
    rig.checkLit(code);
  }
}

void testCurveAndSlider()
{
  phase = "ALS and slider sweeps";
  CHECK(!Config{}.android && !Config{}.autoAllowed && Config{}.floor == 128);
  for (const Profile profile : {kAndroidDaily, kCandidate}) {
    const NativeBrightness native(profile.floor, profile.ceiling, true);
    const int dark = profile.floor == kCandidate.floor ? 921 : 250;
    CHECK(curveCode(profile, 0) == dark);
    CHECK(curveCode(profile, 6) == (dark == 921 ? dark : 847));
    CHECK(curveCode(profile, 8) > dark && curveCode(profile, 100) > curveCode(profile, 8));
    int previousCode = dark;
    uint32_t previousMilli = native.milliNits(uint16_t(dark));
    for (uint32_t lux = 0; lux <= 100000; ++lux) {
      const int code = curveCode(profile, lux);
      const uint32_t milli = native.milliNits(uint16_t(code));
      CHECK(code >= previousCode && code <= 1952 && alsTarget(profile, lux) <= 1000);
      CHECK(milli >= previousMilli && milli >= 2000 && milli <= 600000);
      if (lux >= 11165) CHECK(code == 1952 && milli == 600000);
      previousCode = code;
      previousMilli = milli;
    }
    CHECK(curveCode(profile, UINT32_MAX) == previousCode);
  }

  for (const Profile profile : {kLegacyDaily, kAndroidDaily, kCandidate}) {
    Rig rig(profile);
    uint16_t previous = profile.floor;
    uint32_t previousMilli = 0;
    // An unleased, settled stream can also report the user's move directly.
    rig.lease.set(0, rig.bus.now);
    if (profile.android) {
      rig.checkAmbient(0, uint16_t(curveCode(profile, 0)));
      CHECK(!rig.swap(0.25));
      rig.bus.now += NativeIntent::kSettleMs;
      CHECK(rig.swap(800.0) && rig.ctl.autoActive());
      rig.checkLit(profile.ceiling);
      CHECK(rig.ctl.automatic(false, kSwap));
    }
    for (unsigned quarterNits = 1; quarterNits <= 3200; ++quarterNits) {
      const uint16_t code = rig.slider(double(quarterNits) / 4);
      const uint32_t milli = rig.native.milliNits(code);
      CHECK(code >= previous && milli >= previousMilli);
      CHECK(milli > 0 && milli <= (profile.android ? 600000u : 500000u));
      if (quarterNits == 1) CHECK(code == profile.floor);
      rig.checkLit(code);
      previous = code;
      previousMilli = milli;
    }
    CHECK(previous == profile.ceiling);
  }
}

void testBoundedBlankAndWake()
{
  for (unsigned ic = 0; ic < kIcs; ++ic) {
    phase = "one IC refuses to blank within the M5 retry budget";
    Rig rig(kCandidate);
    const uint16_t code = rig.ctl.code();
    rig.bus.inject(Fault::writeNack, ic, kRegEn, kOff, 3);
    CHECK(!rig.panelOff() && rig.m5Sleeps == 0);
    CHECK(rig.bus.faultHits[ic] == 3 && rig.bus.faultHits[1 - ic] == 0);
    CHECK(rig.bus.enable(ic) == kOn && rig.bus.enable(1 - ic) == kOff);
    CHECK(!rig.ctl.bothDark());
    rig.checkUnapplied();
    CHECK(rig.panelOff() && rig.m5Sleeps == 1);
    rig.checkPair(code, kOff);

    phase = "one IC refuses to enable through brightness lockout";
    const unsigned limit = Config{}.errorLimit;
    rig.bus.inject(Fault::writeNack, ic, kRegEn, kOn, limit);
    rig.display(true);
    for (unsigned i = 1; i <= limit; ++i) {
      CHECK(rig.bus.faultHits[ic] == 3 + i && rig.ctl.controlling() == (i < limit));
      rig.checkUnapplied();
      rig.checkPair(code, kOff);
      CHECK(rig.ctl.bothDark() && rig.bus.confirmedDark());
      rig.ctl.retry();
    }
    CHECK(rig.ctl.errors() == limit + 3 && rig.bus.faultHits[1 - ic] == 0);
    CHECK(rig.panelOff() && rig.m5Sleeps == 2);
  }
}

// Pick a level that will change the MSB even at a clamped endpoint.
double oppositeLevel(Profile profile, uint16_t code)
{
  return code <= (profile.floor + profile.ceiling) / 2 ? std::numeric_limits<double>::max() : 0.25;
}

void stress(Profile profile)
{
  Rig rig(profile);
  std::mt19937 rng(0x8866 + profile.floor);
  int trim = 0;
  constexpr std::array faults{Fault::writeNack, Fault::readbackNack, Fault::staleReadback};
  for (cycle = 0; cycle < kCycles; ++cycle) {
    rig.bus.now += 4;
    if (profile.android && cycle % 128 == 0) rig.lease.set(kManagedMaxS, rig.bus.now);
    const unsigned ic = cycle % kIcs;
    const auto kind = faults[(cycle / kIcs) % faults.size()];
    const uint32_t dimLux = rng() % 101, midLux = 101 + rng() % (11165 - 101);
    const uint32_t brightLux = 11165 + rng() % (100001 - 11165);
    const uint32_t lux = cycle % 4 < 2 ? dimLux : brightLux;
    const uint32_t nextLux = cycle % 5 == 0 ? midLux : lux == dimLux ? brightLux : dimLux;

    phase = "ALS and slider while lit";
    const int curve = curveCode(profile, lux);
    const uint16_t ambientCode = profile.android
        ? uint16_t(std::clamp(curve + trim, int(profile.floor), int(profile.ceiling))) : rig.ctl.code();
    rig.checkAmbient(lux, ambientCode);
    if (profile.android) {
      const uint32_t echoes = rig.intent.echoes();
      const unsigned transactions = rig.bus.transactions;
      const double echo = double(rig.native.midFixed()) / 65536;
      rig.intent.expect(rig.swapFixed(echo), rig.bus.now);
      CHECK(!rig.swap(echo) && rig.intent.echoes() == echoes + 1 && rig.bus.transactions == transactions);
    }
    const uint16_t old = rig.slider(2.0 + double(rng() % 9600) / 16);
    rig.checkLit(old);
    const double nextLevel = oppositeLevel(profile, old);
    const uint16_t wanted = rig.decode(rig.swapFixed(nextLevel));
    const auto hits = rig.bus.faultHits;
    const uint32_t errors = rig.ctl.errors();
    rig.bus.inject(kind, ic, kRegBrtMsb, uint8_t(wanted >> 3));
    CHECK(rig.slider(nextLevel) == wanted);
    rig.checkUnapplied();
    rig.checkPair(old, kOn);
    CHECK(rig.bus.faultHits[ic] == hits[ic] + 1 && rig.bus.faultHits[1 - ic] == hits[1 - ic]);
    CHECK(rig.ctl.errors() == errors + 1);
    rig.bus.now += 1000; // periodic retry
    rig.ctl.retry();
    rig.checkLit(wanted);
    trim = int(wanted) - curve;
    const int nextCurve = curveCode(profile, nextLux);
    const uint16_t retained = profile.android
        ? uint16_t(std::clamp(nextCurve + trim, int(profile.floor), int(profile.ceiling))) : wanted;
    rig.checkAmbient(nextLux, retained);

    phase = "confirmed dark before M5 and slider while asleep";
    const AmbientVersion stale = rig.nextAmbient();
    const auto blankFault = faults[(cycle / kIcs + 1) % faults.size()];
    const unsigned blankRetries = blankFault == Fault::writeNack ? 2 : 1;
    rig.bus.inject(blankFault, 1 - ic, kRegEn, kOff, blankRetries);
    CHECK(rig.panelOff());
    CHECK(rig.bus.faultHits[1 - ic] == hits[1 - ic] + blankRetries);
    CHECK(rig.ctl.errors() == errors + 1 + blankRetries);
    rig.checkPair(retained, kOff);
    const bool system = cycle % 8 < 2;
    if (system) rig.systemSleep(true);
    const unsigned transactions = rig.bus.transactions;
    CHECK(!rig.ambient(nextLux, stale) && !rig.ambient(lux, rig.nextAmbient()));
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
      CHECK(!rig.swap(invalid) && rig.ctl.code() == retained);
    }
    if (cycle % 3 == 0) rig.slider(100.0);
    const uint16_t wakeCode = rig.slider(oppositeLevel(profile, retained));
    if (profile.android) CHECK(rig.ctl.ambientState().pendingLevel() == wakeCode);
    CHECK(rig.bus.transactions == transactions);
    rig.checkPair(retained, kOff);
    if (system) {
      rig.display(true);
      rig.checkPair(retained, kOff);
    }

    phase = "asymmetric wake fault and recovery";
    const bool codeFault = (cycle / (kIcs * faults.size())) % 2 == 0;
    const unsigned onAttempts = rig.bus.enableAttempts;
    rig.bus.inject(kind, ic, codeFault ? kRegBrtMsb : kRegEn, codeFault ? uint8_t(wakeCode >> 3) : kOn);
    if (system) rig.systemSleep(false);
    else rig.display(true);
    rig.checkUnapplied();
    CHECK(rig.ctl.errors() == errors + 2 + blankRetries && rig.bus.faultHits[ic] == hits[ic] + 2);
    CHECK(rig.ctl.bothDark() && rig.bus.confirmedDark());
    if (codeFault) {
      CHECK(rig.bus.enableAttempts == onAttempts);
      CHECK(rig.bus.activeCode(ic) == (kind == Fault::writeNack ? retained : wakeCode));
      CHECK(rig.bus.activeCode(1 - ic) == (ic == 0 ? retained : wakeCode));
    } else {
      rig.checkPair(wakeCode, kOff);
    }
    rig.bus.now += 1000;
    rig.ctl.retry();
    rig.checkLit(wakeCode);

    phase = "post-wake ALS revision";
    const unsigned afterWake = rig.bus.transactions;
    CHECK(!rig.ambient(nextLux, stale) && rig.bus.transactions == afterWake);
    const AmbientVersion fresh = rig.nextAmbient();
    rig.checkAmbient(lux, wakeCode); // First target takes the trim from the sleeping slider move.
    trim = int(wakeCode) - curve;
    const unsigned afterFresh = rig.bus.transactions;
    CHECK(!rig.ambient(nextLux, fresh) && rig.bus.transactions == afterFresh);
  }
  CHECK(rig.m5Sleeps == kCycles);
}

} // namespace

int main()
{
  testStaleWakeReadback();
  testCurveAndSlider();
  testBoundedBlankAndWake();
  for (const Profile profile : {kCandidate, kAndroidDaily, kLegacyDaily}) stress(profile);
  std::printf("backlight_dual_ic_fault_stress_host: %u cycles passed\n", 3 * kCycles);
}
