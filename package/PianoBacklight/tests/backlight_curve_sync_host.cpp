// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -working-directory "${backlight_objects:=$(mktemp -d "${TMPDIR:-/tmp}/backlight-curve.XXXXXXXX")}" -I"$PWD/../../include" -I"$PWD/../../PianoSensors" -c "$PWD"/../../PianoSensors/Sensors{Policy,Fixed,Orient}.c "$PWD"/../../PianoAuto/Auto{Light,Stream}.c && trap 'rm -r "$backlight_objects"' EXIT && clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../include -I../../PianoSensors backlight_curve_sync_host.cpp "$backlight_objects"/*.o -o /tmp/backlight_curve_sync_host && /tmp/backlight_curve_sync_host

#include "BitI2c.h"
#include "Ktz8866Chip.h"
#include "NativeBrightness.h"
#include "PianoBacklightClient.h"
extern "C" {
#include "../../PianoAuto/AutoLight.h"
}

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace pianobacklight;
using pianoauto::Ambient;
using pianoauto::AmbientVersion;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); \
} } while (false)

namespace {

constexpr uint16_t kFloor = 921, kSdrMax = 1952;
constexpr uint32_t kMaxMilliNits = 600000, kLastLux = 11165;
constexpr uint32_t kReferenceCurve = 1;
constexpr char kSource[] = "host test";

uint32_t fixedFromMilliNits(uint32_t milli)
{
  return uint32_t((uint64_t(milli) << 16) / 1000);
}

uint32_t targetAtLux(uint32_t lux, uint16_t floor = kFloor)
{
  return ps_als_target_permille(PS_ALS_REFERENCE, floor, lux);
}

uint16_t codeForTarget(uint32_t target, uint16_t floor = kFloor)
{
  return std::min(Ambient::codeForPermille(target, floor), kSdrMax);
}

void testMap()
{
  CHECK(std::size(pa_sdr_points) == 613);
  CHECK(kSdrMax == PA_SDR_MAX_CODE && kFullScale == 2047 && kSdrMax < kFullScale);
  CHECK(pa_sdr_points[0][0] == 0 && pa_sdr_points[0][1] == 0);
  CHECK(pa_sdr_points[1][0] == 8 && pa_sdr_points[1][1] == 178);
  const auto &last = pa_sdr_points[std::size(pa_sdr_points) - 1];
  CHECK(last[0] == 2047 && last[1] == kSdrMax);
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < std::size(pa_sdr_points); ++i) {
    const auto &point = pa_sdr_points[i];
    CHECK(pa_sdr_code_at_dbv(point[0]) == point[1]);
    if (i) {
      CHECK(point[0] > pa_sdr_points[i - 1][0]);
      CHECK(point[1] >= pa_sdr_points[i - 1][1]);
    }
    for (uint16_t word : point) {
      for (uint8_t byte : {uint8_t(word), uint8_t(word >> 8)}) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
          crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
      }
    }
  }
  CHECK(~crc == 0x9673290eu && ~crc == PA_SDR_MAP_CRC);

  // Forward conversions are nondecreasing. The captured DBV map includes plateaus.
  for (uint16_t dbv = 1; dbv <= PA_SDR_MAX_DBV; ++dbv) {
    const uint16_t code = pa_sdr_code_at_dbv(dbv);
    CHECK(code >= pa_sdr_code_at_dbv(dbv - 1) && code <= kSdrMax);
    if (dbv >= PA_SDR_MIN_DBV) CHECK(pa_sdr_code(pa_sdr_fixed_for_code(code)) == code);
  }
  uint16_t previousDbv = PA_SDR_MIN_DBV, previousCode = PA_SDR_MIN_CODE;
  for (uint32_t milli = 0; milli <= kMaxMilliNits; ++milli) {
    const uint32_t fixed = fixedFromMilliNits(milli);
    const uint16_t dbv = pa_sdr_dbv(fixed), code = pa_sdr_code(fixed);
    CHECK(dbv >= previousDbv && dbv <= previousDbv + 1);
    CHECK(code >= previousCode && code <= kSdrMax);
    previousDbv = dbv;
    previousCode = code;
  }
  CHECK(previousDbv == 2047 && previousCode == kSdrMax);

  const NativeBrightness native(kFloor, kSdrMax, true);
  // The inverse assigns a distinct 16.16 and millinit value to every lit code.
  for (uint16_t code = kFloor + 1; code <= kSdrMax; ++code) {
    CHECK(pa_sdr_fixed_for_code(code) > pa_sdr_fixed_for_code(code - 1));
    CHECK(native.milliNits(code) > native.milliNits(code - 1));
  }
  uint16_t code = 0, dbv = 0;
  CHECK(native.decode(native.minFixed(), code, &dbv) && code == kFloor);
  CHECK(native.decode(native.maxFixed(), code, &dbv) && code == kSdrMax && dbv == 2047);
  CHECK(native.milliNits(kSdrMax) == kMaxMilliNits);
  const NativeBrightness legacy(kFloor);
  CHECK(legacy.decode(legacy.maxFixed(), code) && code == 2047);
}

void testLuxCurve()
{
  constexpr std::array anchors{
    std::array{0u, 3500u}, std::array{1u, 4500u}, std::array{2u, 6000u}, std::array{4u, 15000u},
    std::array{6u, 21000u}, std::array{8u, 27000u}, std::array{10u, 36000u}, std::array{15u, 52000u},
    std::array{20u, 71000u}, std::array{25u, 87000u}, std::array{30u, 97000u}, std::array{82u, 101000u},
    std::array{771u, 178000u}, std::array{1202u, 236000u}, std::array{2058u, 343000u},
    std::array{5100u, 465000u}, std::array{8900u, 560000u}, std::array{kLastLux, kMaxMilliNits},
  };
  CHECK(ps_als_end_lux(PS_ALS_REFERENCE) == kLastLux);
  CHECK(targetAtLux(6) == targetAtLux(0));
  CHECK(codeForTarget(targetAtLux(8)) > kFloor);
  for (const uint16_t floor : {uint16_t(PA_SDR_MIN_CODE), kFloor}) {
    const NativeBrightness native(floor, kSdrMax, true);
    const uint32_t dark = targetAtLux(0, floor);
    const uint16_t darkCode = codeForTarget(dark, floor);
    CHECK(darkCode == (floor == kFloor ? kFloor : 250));
    uint32_t previous = 0;
    for (const auto &[lux, milli] : anchors) {
      const uint32_t target = targetAtLux(lux, floor);
      CHECK(target == pa_sdr_permille(std::max(floor, pa_sdr_code(fixedFromMilliNits(milli)))));
      if (target == dark && floor == kFloor) CHECK(target == previous || lux == 0);
      else CHECK(target > previous);
      previous = target;
    }
    uint32_t lastTarget = dark, lastMilli = native.milliNits(darkCode);
    for (uint32_t lux = 1; lux <= kLastLux; ++lux) {
      const uint32_t target = targetAtLux(lux, floor);
      const uint16_t code = codeForTarget(target, floor);
      const uint32_t milli = native.milliNits(code);
      CHECK(target >= lastTarget && code >= darkCode && code <= kSdrMax);
      CHECK(milli >= lastMilli && milli <= kMaxMilliNits);
      lastTarget = target;
      lastMilli = milli;
    }
    CHECK(lastMilli == kMaxMilliNits && targetAtLux(UINT32_MAX, floor) == lastTarget);
  }
}

void testLuxSamples()
{
  PasConfig config;
  pas_defaults(&config);
  PalLight light;
  pal_init(&light, &config);
  CHECK(pal_select_curve(&light, PS_ALS_REFERENCE, 1, kFloor));
  pal_conditions(&light, 1, 1, 1, 0);
  CHECK(pas_action(&light.stream, 0) == PAS_ACT_ENABLE);
  pas_sent(&light.stream, PAS_ACT_ENABLE, 0);
  pas_response(&light.stream, 1, 1);
  for (const float lux : {std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(), -1.0f}) {
    CHECK(pal_event(&light, std::bit_cast<uint32_t>(lux), 2) == -1);
    CHECK(!light.sample_valid && !light.samples && light.stream.waiting);
  }
  CHECK(light.invalid == 3);
  CHECK(pal_event(&light, std::bit_cast<uint32_t>(0.0f), 3) == int32_t(targetAtLux(0)));
  CHECK(light.sample_valid && light.samples == 1 && light.lux == 0);

  // A finite lux sample above the curve's range saturates at its last knot.
  const uint32_t bright = std::bit_cast<uint32_t>(std::numeric_limits<float>::max());
  pal_event(&light, bright, 1003);
  CHECK(light.sample_valid && light.lux == kLastLux && light.als.goal == targetAtLux(kLastLux));
}

enum class Operation { read, write, done, dark, touchPark, panelSleep };

struct Event {
  Operation op;
  unsigned ic = 0;
  uint8_t reg = 0, value = 0;
  I2c result = I2c::ok;
  bool operator==(const Event &) const = default;
};

class Pair final : public Ics {
 public:
  Pair()
  {
    for (unsigned ic = 0; ic < kIcs; ++ic) {
      auto &chip = chips_[ic];
      backlighttest::resetRegs(chip.reg);
      chip.reg[kRegCfg1] = 0x53;
      chip.reg[kRegIfs] = 0xd9;
      chip.reg[kRegBrtLsb] = kFloor & 7;
      chip.reg[kRegBrtMsb] = kFloor >> 3;
      chip.reg[kRegEn] = 0x7f;
      chip.reg[kRegPwmLsb] = 0xff;
      chip.reg[kRegPwmMsb] = 0x07;
      buses_[ic].configure(chip.lines([](void *, unsigned) {}), 5, 2000);
    }
  }

  void nackWrites(unsigned ic, uint8_t reg, uint8_t value, unsigned count)
  {
    nack_ = {ic, reg, count};
    nackValue_ = value;
  }
  void corruptRead(unsigned ic, uint8_t reg, unsigned skip = 0) { corrupt_ = {ic, reg, skip + 1}; }

  bool read(unsigned ic, uint8_t reg, uint8_t *value) override
  {
    const I2c result = buses_[ic].writeRead(kAddress, &reg, 1, value, 1);
    if (result == I2c::ok && corrupt_.matches(ic, reg) && --corrupt_.remaining == 0)
      *value ^= reg == kRegEn ? 0x40 : 0x01;
    trace.push_back({Operation::read, ic, reg, result == I2c::ok ? *value : uint8_t(0), result});
    return result == I2c::ok;
  }

  bool write(unsigned ic, uint8_t reg, uint8_t value) override
  {
    CHECK(reg == kRegBrtLsb || reg == kRegBrtMsb || reg == kRegEn);
    if (reg == kRegEn) CHECK(value == 0x3f || value == 0x7f);
    if (reg == kRegBrtLsb) CHECK(value <= 7);
    if (nack_.matches(ic, reg) && value == nackValue_) {
      --nack_.remaining;
      chips_[ic].nackWriteReg = reg;
    }
    const uint8_t bytes[] = {reg, value};
    const I2c result = buses_[ic].write(kAddress, bytes, std::size(bytes));
    trace.push_back({Operation::write, ic, reg, value, result});
    return result == I2c::ok;
  }

  void done() override
  {
    for (const auto &chip : chips_) CHECK(chip.sda() && chip.scl());
    trace.push_back({Operation::done});
  }
  void log(const char *) override {}
  uint64_t nowMs() override { return 0; }

  uint8_t reg(unsigned ic, uint8_t address) const { return chips_[ic].reg[address]; }
  uint16_t code(unsigned ic) const { return codeOf(reg(ic, kRegBrtLsb), reg(ic, kRegBrtMsb)); }
  uint16_t pwm(unsigned ic) const { return uint16_t(reg(ic, kRegPwmMsb) << 8 | reg(ic, kRegPwmLsb)); }

  std::vector<Event> trace;

 private:
  struct Fault {
    unsigned ic = 0;
    uint8_t reg = 0;
    unsigned remaining = 0;
    bool matches(unsigned index, uint8_t address) const { return remaining && ic == index && reg == address; }
  };

  backlighttest::Chip chips_[kIcs];
  BitI2c buses_[kIcs];
  Fault nack_, corrupt_;
  uint8_t nackValue_ = 0;
};

struct Rig {
  Pair bus;
  Controller ctl{bus};
  const NativeBrightness native{kFloor, kSdrMax, true};
  uint32_t revision = native.initialRevision();
  uint64_t taken = 0;

  explicit Rig(unsigned errorLimit = Config{}.errorLimit)
  {
    Config config;
    config.floor = kFloor;
    config.bootPercent = 0;
    config.android = config.autoAllowed = true;
    config.errorLimit = errorLimit;
    ctl.configure(config);
    CHECK(ctl.survey() && ctl.start());
    CHECK(ctl.floor() == kFloor && ctl.ceiling() == kSdrMax && ctl.exponential());
    checkLit(kFloor);
    bus.trace.clear();
  }

  // Only the IOKit worker's edge and version bookkeeping is modeled here.
  void display(bool on)
  {
    const bool wasLit = ctl.wantLit();
    ctl.display(on, 1, uint32_t(on));
    if (wasLit != ctl.wantLit()) {
      ++revision;
      taken = 0;
      ctl.invalidateAmbient(false);
    }
  }

  // The versioned registry snapshot route in poll() and applyAmbient().
  bool ambient(uint64_t target, uint32_t lux, AmbientVersion version, bool valid = true)
  {
    if ((valid && target > 1000) || !version.newer(revision, kReferenceCurve, taken)) return false;
    taken = version.seq;
    if (!valid) {
      ctl.invalidateAmbient(false);
      return false;
    }
    return ctl.wantLit() && ctl.ambientState().requested() && !ctl.autoFault()
        && ctl.ambient(int32_t(target), lux, kSource);
  }

  bool lux(uint32_t value, uint64_t seq)
  {
    return ambient(targetAtLux(value), value, {seq, revision, kReferenceCurve});
  }

  // The bounded worker reply and the copy thread's dependent callouts.
  bool panelOff()
  {
    display(false);
    for (unsigned retry = 0; ctl.pending() && retry < 2; ++retry) ctl.retry();
    if (!ctl.bothDark()) return false;
    bus.trace.push_back({Operation::dark});
    bus.trace.push_back({Operation::touchPark});
    bus.trace.push_back({Operation::panelSleep});
    return true;
  }

  void checkRegisters(uint16_t code, uint8_t enable) const
  {
    for (unsigned ic = 0; ic < kIcs; ++ic) {
      CHECK(bus.code(ic) == code && bus.reg(ic, kRegEn) == enable && bus.pwm(ic) == 0x07ff);
    }
  }

  void checkLit(uint16_t code) const
  {
    checkRegisters(code, 0x7f);
    CHECK(ctl.bothLit() && !ctl.pending() && ctl.pwmQualified());
    uint16_t reported = 0;
    CHECK(native.applied(ctl, reported) && reported == code);
  }

  void checkDark(uint16_t code) const
  {
    checkRegisters(code, 0x3f);
    CHECK(!ctl.wantLit() && ctl.bothDark() && !ctl.pending());
    uint16_t reported = 0;
    CHECK(!native.applied(ctl, reported) && reported == 0);
  }
};

void expectCode(std::vector<Event> &events, unsigned ic, uint16_t code)
{
  const uint8_t low = uint8_t(code & 7), high = uint8_t(code >> 3);
  events.push_back({Operation::write, ic, kRegBrtLsb, low});
  events.push_back({Operation::write, ic, kRegBrtMsb, high});
  events.push_back({Operation::read, ic, kRegBrtLsb, low});
  events.push_back({Operation::read, ic, kRegBrtMsb, high});
}

void expectEnable(std::vector<Event> &events, unsigned ic, bool on)
{
  const uint8_t before = on ? 0x3f : 0x7f, after = on ? 0x7f : 0x3f;
  events.push_back({Operation::read, ic, kRegEn, before});
  events.push_back({Operation::write, ic, kRegEn, after});
  events.push_back({Operation::read, ic, kRegEn, after});
}

void checkLitTrace(const std::vector<Event> &trace, const std::vector<Event> &changes)
{
  CHECK(trace.size() > changes.size() && std::equal(changes.begin(), changes.end(), trace.begin()));
  // Full-duty qualification follows the register updates and never makes stores.
  CHECK(trace.back().op == Operation::done);
  for (size_t i = changes.size(); i + 1 < trace.size(); ++i) CHECK(trace[i].op == Operation::read);
  for (unsigned ic = 0; ic < kIcs; ++ic) {
    const Event lsb{Operation::read, ic, kRegPwmLsb, 0xff}, msb{Operation::read, ic, kRegPwmMsb, 0x07};
    CHECK(std::count(trace.begin(), trace.end(), lsb) == 2);
    CHECK(std::count(trace.begin(), trace.end(), msb) == 2);
  }
}

void testControlsAndInvalidInput()
{
  Rig rig;
  rig.display(true);
  CHECK(rig.lux(0, 1) && rig.ctl.autoActive());
  rig.checkLit(kFloor);
  CHECK(rig.lux(82, 2));
  rig.checkLit(codeForTarget(targetAtLux(82)));
  CHECK(rig.lux(kLastLux, 3));
  rig.checkLit(kSdrMax);
  CHECK(rig.lux(0, 4));
  rig.checkLit(kFloor);

  rig.bus.trace.clear();
  uint16_t code = 123, dbv = 123;
  for (const double nits : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                           -1.0, -0.0, 0.0, std::numeric_limits<double>::denorm_min()}) {
    const int32_t fixed = nitsFixedFromDouble(std::bit_cast<uint64_t>(nits));
    CHECK(fixed == 0 && !rig.native.decode(uint64_t(fixed), code, &dbv));
  }
  const int32_t huge = nitsFixedFromDouble(std::bit_cast<uint64_t>(std::numeric_limits<double>::max()));
  CHECK(huge == INT32_MAX);
  for (const uint64_t fixed : {uint64_t(huge), uint64_t(rig.native.maxFixed()) + 1,
                               uint64_t(rig.native.minFixed()) - 1, uint64_t(-1)}) {
    CHECK(!rig.native.decode(fixed, code, &dbv));
  }
  CHECK(code == 123 && dbv == 123);
  for (const uint64_t target : {uint64_t(1001), uint64_t(-2), uint64_t(-1)}) {
    CHECK(!rig.ambient(target, 82, {5, rig.revision, kReferenceCurve}));
    CHECK(!rig.ctl.ambientSnapshot(5, 1, uint32_t(target), 82));
  }
  for (uint16_t invalid : {uint16_t(kFloor - 1), uint16_t(kSdrMax + 1), kFullScale, uint16_t(UINT16_MAX)})
    rig.ctl.manualCode(invalid, kSource);
  CHECK(rig.bus.trace.empty() && rig.ctl.autoActive() && rig.ctl.ambientState().lastLux() == 0);
  CHECK(rig.taken == 4);

  const int32_t swap = nitsFixedFromDouble(std::bit_cast<uint64_t>(100.0));
  CHECK(swap == 100 << 16 && rig.native.decode(uint64_t(swap), code, &dbv));
  CHECK(dbv == pa_sdr_dbv(uint32_t(swap)) && code == pa_sdr_code_at_dbv(dbv));
  CHECK(rig.ctl.userLevel(code, kSource) && rig.ctl.autoActive());
  rig.checkLit(code);
  CHECK(rig.lux(82, 5) && rig.ctl.code() > code);
  rig.ctl.manualCode(code, kNativeRequest);
  CHECK(!rig.ctl.ambientState().requested() && !rig.ctl.autoActive());
  rig.checkLit(code);
  rig.bus.trace.clear();
  CHECK(!rig.lux(kLastLux, 6));
  CHECK(rig.ctl.ambient(1000, kLastLux, kSource));   // Direct call also leaves the manual level in force.
  CHECK(rig.bus.trace.empty());
  rig.checkLit(code);

  KeyWatch keys;
  int step = keys.consumer(kUsageBrightnessUp);
  CHECK(step == 1);
  rig.ctl.step(step, kSource);
  const uint16_t brighter = rig.ctl.code();
  CHECK(brighter > code);
  rig.checkLit(brighter);
  CHECK(keys.consumer(kUsageBrightnessUp) == 0 && keys.consumer(0) == 0);
  step = keys.consumer(kUsageBrightnessDown);
  CHECK(step == -1);
  rig.ctl.step(step, kSource);
  CHECK(rig.ctl.code() < brighter);
  rig.checkLit(rig.ctl.code());
  CHECK(rig.ctl.automatic(true, kSource) && !rig.ctl.autoActive());
  CHECK(rig.lux(0, 7) && rig.ctl.autoActive());

  // A negative stream notification invalidates ALS but leaves the last verified level.
  rig.bus.trace.clear();
  const uint16_t held = rig.ctl.code();
  CHECK(!rig.ambient(0, 0, {8, rig.revision, kReferenceCurve}, false));
  CHECK(!rig.ctl.ambientState().valid() && !rig.ctl.autoActive() && rig.bus.trace.empty());
  rig.checkLit(held);

  // The direct ambient callout bounds an oversized positive target and treats a negative as a stream fault.
  CHECK(rig.ctl.ambient(INT32_MAX, kLastLux, kSource));
  rig.checkLit(kSdrMax);
  rig.bus.trace.clear();
  CHECK(rig.ctl.ambient(INT32_MIN, 0, kSource));
  CHECK(rig.ctl.autoFault() && !rig.ctl.autoActive() && rig.bus.trace.empty());
  rig.checkLit(kSdrMax);
}

void testPairRecovery()
{
  constexpr unsigned limit = 3;
  for (unsigned ic = 0; ic < kIcs; ++ic) {
    Rig rig(limit);
    rig.display(true);
    rig.bus.trace.clear();
    rig.ctl.manualCode(1300, kSource);
    std::vector<Event> changes;
    expectCode(changes, 0, 1300);
    expectCode(changes, 1, 1300);
    checkLitTrace(rig.bus.trace, changes);
    rig.checkLit(1300);

    rig.bus.corruptRead(ic, kRegBrtLsb);
    rig.ctl.manualCode(1601, kSource);
    CHECK(rig.ctl.pending() && rig.ctl.bothLit() && !rig.ctl.pwmQualified());
    CHECK(rig.ctl.errors() == 1 && rig.ctl.ic(ic).errors == 1 && rig.ctl.ic(1 - ic).errors == 0);
    rig.checkRegisters(1300, 0x7f);
    uint16_t applied = 0;
    CHECK(!rig.native.applied(rig.ctl, applied) && applied == 0);
    rig.ctl.retry();
    rig.checkLit(1601);
    CHECK(rig.ctl.errors() == 1);

    rig.bus.trace.clear();
    rig.bus.nackWrites(ic, kRegBrtMsb, uint8_t(1803 >> 3), limit);
    rig.ctl.manualCode(1803, kSource);
    for (unsigned failures = 1; failures <= limit; ++failures) {
      CHECK(rig.ctl.errors() == 1 + failures && rig.ctl.ic(ic).errors == 1 + failures);
      CHECK(rig.ctl.controlling() == (failures < limit));
      rig.checkRegisters(1601, 0x7f);
      rig.ctl.retry();
    }
    const Event nack{Operation::write, ic, kRegBrtMsb, uint8_t(1803 >> 3), I2c::nackData};
    CHECK(std::count(rig.bus.trace.begin(), rig.bus.trace.end(), nack) == limit);
    CHECK(rig.ctl.pending() && rig.ctl.errors() == 1 + limit);
    rig.bus.trace.clear();
    rig.ctl.retry();
    CHECK(rig.bus.trace.empty() && !rig.ctl.controlling());
    CHECK(rig.panelOff());   // Brightness lockout still permits display-off blanking.
    rig.checkDark(1601);
  }
}

void testDisplayCycle()
{
  Rig rig;
  CHECK(!rig.panelOff());
  CHECK(rig.bus.trace.empty());   // Pre-first-on splash stays lit, so M5 cannot park touch.
  rig.checkLit(kFloor);
  rig.display(true);
  CHECK(rig.lux(1202, 1));
  const uint16_t retained = codeForTarget(targetAtLux(1202));
  rig.checkLit(retained);
  const AmbientVersion stale{2, rig.revision, kReferenceCurve};

  rig.bus.trace.clear();
  CHECK(rig.panelOff());
  std::vector<Event> off;
  expectEnable(off, 0, false);
  expectEnable(off, 1, false);
  off.insert(off.end(), {{Operation::done}, {Operation::dark}, {Operation::touchPark}, {Operation::panelSleep}});
  CHECK(rig.bus.trace == off);
  rig.checkDark(retained);
  CHECK(!rig.ctl.ambientState().valid());
  rig.bus.trace.clear();
  CHECK(!rig.ambient(targetAtLux(kLastLux), kLastLux, stale));
  CHECK(!rig.lux(kLastLux, 1));
  CHECK(!rig.ctl.ambient(1000, kLastLux, kSource));
  CHECK(rig.bus.trace.empty());
  rig.checkDark(retained);

  rig.display(true);
  std::vector<Event> on;
  expectEnable(on, 0, true);
  expectEnable(on, 1, true);
  checkLitTrace(rig.bus.trace, on);
  rig.checkLit(retained);
  rig.bus.trace.clear();
  CHECK(!rig.ctl.ambientState().valid() && !rig.ambient(targetAtLux(kLastLux), kLastLux, stale));
  CHECK(!rig.ambient(targetAtLux(kLastLux), kLastLux, {2, rig.revision, 0}));
  CHECK(rig.bus.trace.empty());
  CHECK(rig.lux(0, 1) && rig.ctl.autoActive());
  rig.checkLit(kFloor);
  CHECK(!rig.lux(kLastLux, 1));

  CHECK(rig.panelOff());
  rig.bus.trace.clear();
  rig.ctl.manualCode(1601, kNativeRequest);
  rig.checkDark(kFloor);
  CHECK(rig.bus.trace.size() == 1 && rig.bus.trace[0].op == Operation::done);
  rig.bus.trace.clear();
  rig.display(true);
  on.clear();
  expectCode(on, 0, 1601);
  expectCode(on, 1, 1601);
  expectEnable(on, 0, true);
  expectEnable(on, 1, true);
  checkLitTrace(rig.bus.trace, on);
  rig.checkLit(1601);
}

void testBlankAndWakeFaults()
{
  for (unsigned ic = 0; ic < kIcs; ++ic) {
    Rig rig;
    rig.display(true);
    rig.bus.trace.clear();
    // An acknowledged OFF write with an ON readback still needs confirmation.
    rig.bus.corruptRead(ic, kRegEn, 1);
    CHECK(rig.panelOff() && rig.ctl.errors() == 1);
    rig.checkDark(kFloor);
    const Event confirmed{Operation::read, ic, kRegEn, 0x3f};
    const auto dark = std::find(rig.bus.trace.begin(), rig.bus.trace.end(), Event{Operation::dark});
    CHECK(dark != rig.bus.trace.end() && std::count(rig.bus.trace.begin(), dark, confirmed) == 1);

    // The other IC may NACK wake. The first is blanked again before a retry.
    rig.bus.nackWrites(1, kRegEn, 0x7f, 1);
    rig.display(true);
    CHECK(rig.ctl.bothDark() && rig.ctl.wantLit() && rig.ctl.pending() && rig.ctl.errors() == 2);
    rig.checkRegisters(kFloor, 0x3f);
    rig.ctl.retry();
    rig.checkLit(kFloor);

    rig.bus.trace.clear();
    rig.bus.nackWrites(ic, kRegEn, 0x3f, 3);
    CHECK(!rig.panelOff());
    const Event nack{Operation::write, ic, kRegEn, 0x3f, I2c::nackData};
    CHECK(std::count(rig.bus.trace.begin(), rig.bus.trace.end(), nack) == 3);
    CHECK(!rig.ctl.bothDark() && rig.ctl.pending() && rig.ctl.errors() == 5);
    CHECK(rig.bus.reg(ic, kRegEn) == 0x7f && rig.bus.reg(1 - ic, kRegEn) == 0x3f);
    for (const Event &event : rig.bus.trace)
      CHECK(event.op == Operation::read || event.op == Operation::write || event.op == Operation::done);
    CHECK(rig.panelOff() && rig.ctl.errors() == 5);
    rig.checkDark(kFloor);
  }
}

}  // namespace

int main()
{
  testMap();
  testLuxCurve();
  testLuxSamples();
  testControlsAndInvalidInput();
  testPairRecovery();
  testDisplayCycle();
  testBlankAndWakeFaults();
  std::puts("backlight_curve_sync_host: all passed");
}
