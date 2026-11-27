// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline board integration test for docs/drivers/gpu-dvfs-thermal.md.
// cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -c ../../adreno830/platform/sm8750/Sm8750Tsens.c -o "$(dirname /tmp/test_gpu_dvfs_thermal)/test_gpu_dvfs_tsens.o" && c++ -std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -DADRENO830_HOST_TEST -DADRENO830_MACOS -I.. -Ihost-include -I../../adreno830 -I../../adreno830/tests -I../../adreno830/platform/sm8750 -I../../../include test_gpu_dvfs_thermal.cpp ../../adreno830/A830Dcvs.cpp ../../adreno830/A830PowerState.cpp ../../adreno830/A830Recovery.cpp "$(dirname /tmp/test_gpu_dvfs_thermal)/test_gpu_dvfs_tsens.o" -o /tmp/test_gpu_dvfs_thermal && /tmp/test_gpu_dvfs_thermal
#include "A830PowerFakes.h"
#include "A830Qualification.h"
#include "A830QualificationTop.h"
#include "PianoGpuCeiling.h"
#include "PianoGpuInputs.h"
#include "../../../PianoPolicy/Policy.h"
#include "../../../PianoThermal/BoardTemps.h"
#include <algorithm>
#include <iterator>

namespace {

using pianothermal::adc::channelIndex;
using pianothermal::adc::Snapshot;

constexpr Adreno830Level bin2[] = PIANO_GPU_BIN2_LEVELS;
constexpr uint32_t ddr[] = PIANO_GPU_DDR_KBPS;
constexpr A830DcvsTable table{bin2, std::size(bin2), ddr, std::size(ddr)};
constexpr uint32_t expectedMhz[] = {160, 222, 342, 389, 443, 525, 607, 660, 734, 832, 900, 967, 1050, 1100};
static_assert(std::size(bin2) == std::size(expectedMhz));
constexpr uint64_t ms = 1'000'000;
constexpr int adcChannels[] = {channelIndex("batt"), channelIndex("quiet"), channelIndex("charger"),
                               channelIndex("wifi"), channelIndex("msm"), channelIndex("xo")};
static_assert(std::size(adcChannels) == PIANO_SKIN_COUNT);

A830PowerState power;
A830Recovery recovery;
A830Dcvs dcvs;
A830Qualification guard;
pianogpu::CeilingSlots slots;
unsigned unsafeExits;
unsigned lowVotes;

void boot(uint32_t maxMhz = 1100, uint8_t skinTable = 1)
{
  adreno830_dcvs_set_ceiling(0);
  resetAll();
  power = {};
  recovery = {};
  dcvs = {};
  guard = {};
  slots = {};
  unsafeExits = lowVotes = 0;
  adreno830_profile_value = A830ProfileAnnibale;
  gQueuePower = &power;
  CHECK(power.init(A830PowerConfig{}, gIo, gPowerEnv, gPowerOps) == kIOReturnSuccess);
  A830RecoveryOps recoveryOps = gRecoveryOps;
  recoveryOps.qualificationLowVote = [](void *, bool) {
    CHECK(gEnv.locked && power.qualificationBlocked() && power.qualificationEffectiveCap() == 1);
    ++lowVotes;
    return kIOReturnSuccess;
  };
  CHECK(recovery.init(A830RecoveryConfig{}, gIo, gPowerEnv, gPowerOps, recoveryOps, &power) == kIOReturnSuccess);
  power.start(1, 2);
  guard.init(&power, &recovery, true, [](void *, A830ExitOrigin origin) {
    CHECK(origin == A830ExitOrigin::Raw105);
    ++unsafeExits;
  });
  guard.fullopp();
  guard.daily();
  CHECK(guard.setTop(14));
  guard.thermal().skinTable = skinTable;

  A830DcvsConfig config;
  config.qualificationR3b = config.qualification607 = config.kgsl = config.acd = true;
  config.maxGiven = true;
  config.maxLevel = 14;
  config.maxMhz = maxMhz;
  config.minMhz = 222;
  config.initialLevel = 1;
  config.windowMs = 10;
  config.idleMs = 80;
  config.sustainKhz = 0;
  A830DcvsOps ops = gDcvsOps;
  ops.qualificationService = [](void *, uint64_t now) { guard.checkLease(now); };
  ops.qualificationThermal = [](void *) { return guard.thermalSnapshot(); };
  CHECK(dcvs.init(config, table, gIo, gPowerEnv, ops, &power, &recovery, nullptr) == kIOReturnSuccess);
  CHECK(dcvs.start() == kIOReturnSuccess);
}

struct Sensors {
  Snapshot adc;
  piano_skin_temps skin{};
  uint32_t gpuValid = 0xff;
  int16_t die = 706;

  void refreshAdc(int32_t skinMilli, int32_t xoMilli = 40000)
  {
    adc.state = PIANO_BOARD_TEMPS_RUNNING;
    adc.periodMs = 5000;
    ++adc.rounds;
    // Equal synthetic thermistor values give the requested proxy through the documented weights and integer rounding.
    const int32_t ntc = ((skinMilli - 1600) * 1000 + 942) / 943;
    for (const int channel : adcChannels) {
      adc.validMask |= 1u << channel;
      adc.atUs[channel] = gEnv.now / 1000;
      adc.milliC[channel] = ntc;
    }
    adc.milliC[channelIndex("xo")] = xoMilli;
  }

  void refreshSkin()
  {
    skin.version = PIANO_SKIN_TEMPS_VERSION;
    CHECK(pianothermal::adc::fillSkinTemps(adc, gEnv.now / 1000, &skin));
  }

  void heartbeat()
  {
    const Sm8750TsensIo tsens{this, [](void *context, uint32_t offset) -> uint32_t {
      const auto &s = *static_cast<Sensors *>(context);
      const unsigned zone = (offset - Sm8750TsensStatusBase) / Sm8750TsensStatusStride - Sm8750TsensGpuFirst;
      CHECK(zone < Sm8750TsensGpuSensors);
      const int16_t deci = zone == 7 ? s.die : s.die - 20;
      return (s.gpuValid & (1u << zone) ? uint32_t(Sm8750TsensValidBit) : 0u) |
             (uint32_t(deci) & Sm8750TsensTempMask);
    }};
    Sm8750TsensSample gpu{};
    Sm8750TsensReadGpu(&tsens, &gpu);
    piano_gpu_temps board{};
    board.version = PIANO_GPU_TEMPS_VERSION;
    const bool ok = pianothermal::adc::fillGpuTemps(adc, gEnv.now / 1000, &board);
    guard.tick(gEnv.now, gpu, board, ok, false, &skin);
  }

  void tick(unsigned elapsedMs = 100)
  {
    gEnv.now += elapsedMs * ms;
    heartbeat();
    service(elapsedMs);
  }

  void setSkin(int32_t skinMilli, int32_t xoMilli = 40000)
  {
    refreshAdc(skinMilli, xoMilli);
    refreshSkin();
    CHECK(skin.virtual_valid && skin.virtual_milli_c == skinMilli);
    tick();
  }

  static void service(unsigned elapsedMs)
  {
    gHw.busyTicks += uint64_t(A830_XO_HZ) * elapsedMs / 1000;
    dcvs.noteSubmit();
    dcvs.serviceTick(gEnv.now);
  }

  // Wait for the two auto-arm rounds, then let the cool die ladder climb from level 3 to 14.
  void warmup(int32_t skinMilli)
  {
    const uint64_t start = gEnv.now;
    for (unsigned i = 0; i <= 280; ++i) {
      gEnv.now = start + i * 100 * ms;
      if (i % 50 == 0) refreshAdc(skinMilli);
      if (i % 10 == 0) refreshSkin();
      heartbeat();
      service(100);
      if (i < 50) CHECK(!guard.high() && dcvs.ceiling() <= 3);
      if (i == 50) CHECK(guard.high());
    }
    CHECK(guard.high() && guard.thermal().step == 14 && !guard.reason());
  }
};

void setCeiling(uintptr_t client, uint32_t khz)
{
  bool changed;
  CHECK(slots.set(client, khz, &changed));
  if (changed) adreno830_dcvs_set_ceiling(slots.effective);
}

void expectVote(uint8_t level, uint32_t khz)
{
  CHECK(dcvs.ceiling() == level && dcvs.level() == level && power.votedLevel() == level);
  CHECK(dcvs.khz(level) == khz && !gOps.votes.empty() && gOps.votes.back().first == level);
}

void testLadder()
{
  boot();
  for (uint8_t level = 1; level <= table.count; ++level) {
    const uint32_t khz = expectedMhz[level - 1] * 1000;
    CHECK(dcvs.khz(level) == khz && Adreno830FulloppMhz[level] == expectedMhz[level - 1]);
    CHECK(dcvs.levelAtLeast(khz) == level && dcvs.levelAtMost(khz) == level);
    CHECK(dcvs.levelAtMost(khz - 1) == (level == 1 ? 1 : level - 1));
    CHECK(dcvs.levelAtLeast(khz + 1) == (level == table.count ? level : level + 1));
    if (level > 1) CHECK(dcvs.khz(level - 1) < khz);
  }
  CHECK(dcvs.levelAtLeast(0) == 1 && dcvs.levelAtMost(UINT32_MAX) == 14);
  for (uint8_t invalid : {0, 15, 16, 255}) {
    CHECK(!dcvs.khz(invalid) && !dcvs.busOf(invalid));
    CHECK(!guard.setTop(invalid) && guard.top() == 14);
  }
  Adreno830Board board{};
  board.flags = 1u << Adreno830KnobDcvsMax;
  board.qualification_r3b = board.qualification_fullopp = 1;
  board.knobs.dcvs_max_mhz = 1100;
  board.knobs.maxlevel = 14;
  CHECK(adreno830_qualification_top(&board) == 14);
  for (uint32_t invalid : {0u, 15u, 256u + 14, UINT32_MAX}) {
    board.knobs.maxlevel = invalid;
    CHECK(adreno830_qualification_top(&board) == 0);
  }
  A830DcvsConfig config;
  for (uint8_t invalid : {0, 15, 255}) {
    config.initialLevel = invalid;
    A830Dcvs rejected;
    CHECK(rejected.init(config, table, gIo, gPowerEnv, gDcvsOps, &power, nullptr, nullptr) == kIOReturnBadArgument);
  }

  // An unrestricted, cool guard lets DCVS visit every OPP via consecutive cap requests.
  Sensors sensors;
  sensors.warmup(22000);
  expectVote(14, 1100000);
  for (uint8_t level = 13; level >= 1; --level) {
    setCeiling(2, expectedMhz[level - 1] * 1000);
    sensors.tick();
    expectVote(level, expectedMhz[level - 1] * 1000);
  }
  for (uint8_t level = 2; level <= 14; ++level) {
    setCeiling(2, expectedMhz[level - 1] * 1000);
    sensors.tick();
    expectVote(level, expectedMhz[level - 1] * 1000);
  }

  // Exercise the 1050 MHz devfreq limit as an explicit frequency cap. The board table still has level 14.
  boot(1050);
  sensors = {};
  sensors.warmup(22000);
  expectVote(13, 1050000);
}

void expectThermalVote(uint8_t level)
{
  CHECK(uint8_t(guard.thermalSnapshot()) == level);
  expectVote(level, expectedMhz[level - 1] * 1000);
}

void testSkinBands()
{
  struct Band {int32_t enter, release; uint8_t level;};
  const Band docked[] = {{35000, 33000, 13}, {45000, 43000, 12}, {47000, 45000, 11}, {49000, 47000, 10},
                         {51000, 49000, 9}, {53000, 51000, 7}, {55000, 53000, 6}};
  const Band handheld[] = {{25000, 23000, 13}, {45000, 42000, 9}};
  for (uint8_t selector : {0, 1}) {
    boot(1100, selector);
    Sensors sensors;
    sensors.warmup(22000);
    const Band *bands = selector ? docked : handheld;
    const unsigned count = selector ? std::size(docked) : std::size(handheld);
    for (unsigned i = 0; i < count; ++i) {
      const Band &band = bands[i];
      const uint8_t previous = i ? bands[i - 1].level : 14;
      sensors.setSkin(band.enter - 1);
      expectThermalVote(previous);
      sensors.setSkin(band.enter);
      expectThermalVote(band.level);
      if (selector) CHECK(guard.thermal().band == i + 1);
      sensors.setSkin(band.enter - 1);
      expectThermalVote(band.level);
      sensors.setSkin(band.release);
      expectThermalVote(band.level);
      sensors.setSkin(band.release - 1);
      expectThermalVote(previous);
      sensors.setSkin(band.enter);
      expectThermalVote(band.level);
    }
    sensors.setSkin(22000);
    expectThermalVote(14);
    sensors.setSkin(22000, 77999);
    expectThermalVote(14);
    sensors.setSkin(22000, 78000);
    expectThermalVote(9);
    sensors.setSkin(22000, 70000);
    expectThermalVote(9);
    sensors.setSkin(22000, 69999);
    expectThermalVote(14);
  }
}

void testSkinFaults()
{
  boot();
  Sensors sensors;
  sensors.warmup(22000);
  expectThermalVote(14);
  const uint32_t wifi = 1u << channelIndex("wifi");
  const uint32_t charger = 1u << channelIndex("charger");

  // A stale skin thermistor expires after two ADC periods while the guard's msm/quiet input stays healthy.
  sensors.adc.atUs[channelIndex("batt")] = gEnv.now / 1000 - 10'000'000;
  sensors.refreshSkin();
  CHECK(sensors.skin.virtual_valid);
  sensors.tick(1);
  sensors.refreshSkin();
  CHECK(!sensors.skin.virtual_valid);
  sensors.tick();
  expectThermalVote(9);
  CHECK(guard.high() && guard.thermal().cause == A830R3bThermal::CauseSkinUnknown);

  // A partial snapshot and an out-of-range XO both prevent a return to full operation.
  sensors.refreshAdc(22000);
  sensors.adc.validMask &= ~wifi;
  sensors.refreshSkin();
  sensors.tick();
  expectThermalVote(9);
  sensors.setSkin(22000, 125001);
  CHECK(!guard.thermal().skinValid);
  expectThermalVote(9);
  sensors.refreshAdc(22000);
  sensors.adc.validMask &= ~charger;
  sensors.refreshSkin();
  sensors.tick();
  expectThermalVote(9);
  sensors.adc.pinHeldMask = charger;
  sensors.refreshSkin();
  CHECK(sensors.skin.virtual_valid && !((sensors.skin.virtual_used >> PIANO_SKIN_CHARGER) & 1u));
  sensors.tick();
  expectThermalVote(14);

  // Unknown skin retains the previous lower ceiling across table changes until a fresh reading can release it.
  sensors.setSkin(55000);
  expectThermalVote(6);
  sensors.adc.disabledMask = wifi;
  guard.thermal().skinTable = 0;
  sensors.refreshSkin();
  sensors.tick();
  expectThermalVote(6);
  CHECK(guard.thermal().band == 7 && guard.thermal().skin45 && guard.high());
  guard.thermal().skinTable = 1;
  sensors.tick();
  expectThermalVote(6);
  sensors.adc.disabledMask = 0;
  sensors.setSkin(53000);
  expectThermalVote(6);
  sensors.setSkin(52999);
  expectThermalVote(7);
}

void testAdvancingAdcRounds()
{
  boot();
  Sensors sensors;
  sensors.setSkin(22000);
  const Snapshot first = sensors.adc;
  for (unsigned i = 0; i < 49; ++i) sensors.tick();
  CHECK(!guard.high() && guard.thermal().entryRounds == 1);

  sensors.refreshAdc(22000);
  const Snapshot second = sensors.adc;
  sensors.adc.rounds = first.rounds;
  sensors.refreshSkin();
  sensors.tick();
  CHECK(guard.thermal().healthy && !guard.high() && dcvs.ceiling() == 3);

  for (const char *channel : {"msm", "quiet"}) {
    sensors.adc = second;
    const int i = channelIndex(channel);
    sensors.adc.atUs[i] = first.atUs[i];
    sensors.refreshSkin();
    sensors.tick();
    CHECK(guard.thermal().healthy && !guard.high() && dcvs.ceiling() == 3);
  }

  sensors.adc = second;
  sensors.refreshSkin();
  sensors.tick();
  CHECK(guard.thermal().healthy && guard.ready() && guard.high());
}

void expectHardFallback(Sensors &sensors, A830QualReason reason, unsigned exits = 0)
{
  sensors.tick();
  CHECK(!guard.high() && power.qualificationLatched() && guard.reason() == reason);
  CHECK(unsafeExits == exits && lowVotes == 1 && recovery.qualificationSafeLow());
  CHECK(power.votedLevel() == 1 && power.votedBus() == 2 && dcvs.level() == 1 && dcvs.ceiling() == 1);
  sensors.die = 706;
  sensors.gpuValid = 0xff;
  sensors.setSkin(22000);
  sensors.setSkin(22000);
  CHECK(!guard.high() && guard.ready() && !guard.arm() && guard.reason() == reason);
  CHECK(dcvs.level() <= 3 && dcvs.ceiling() <= 3 && lowVotes == 1 && unsafeExits == exits);
}

void testHardFaults()
{
  for (uint32_t valid : {0xfeu, 0u}) {
    boot();
    Sensors sensors;
    sensors.warmup(22000);
    sensors.gpuValid = valid;
    expectHardFallback(sensors, A830QualSensor);
  }

  boot();
  Sensors sensors;
  sensors.warmup(22000);
  sensors.die = -401;
  expectHardFallback(sensors, A830QualSensor);

  for (const char *channel : {"msm", "quiet"}) {
    boot();
    sensors = {};
    sensors.warmup(22000);
    const int sensor = channelIndex(channel);
    sensors.adc.atUs[sensor] = (gEnv.now + 100 * ms) / 1000 - 10'000'000;
    sensors.tick();
    CHECK(guard.high());
    expectHardFallback(sensors, A830QualSensor);

    boot();
    sensors = {};
    sensors.warmup(22000);
    sensors.adc.milliC[sensor] = 125001;
    expectHardFallback(sensors, A830QualSensor);

    boot();
    sensors = {};
    sensors.warmup(22000);
    sensors.adc.validMask &= ~(1u << sensor);
    expectHardFallback(sensors, A830QualSensor);
  }

  boot();
  sensors = {};
  sensors.warmup(22000);
  // A late heartbeat expires the permit even with otherwise fresh readings.
  gEnv.now += 151 * ms;
  expectHardFallback(sensors, A830QualSensor);

  for (int16_t die : {1000, 1049, 1050}) {
    boot();
    sensors = {};
    sensors.warmup(22000);
    sensors.die = die;
    if (die == 1050) {
      sensors.gpuValid = 0x80;  // a single VALID hotspot still invokes the independent unsafe exit
      sensors.adc.validMask = 0;
    }
    expectHardFallback(sensors, A830QualGpuHot, die == 1050 ? 1 : 0);
  }

  boot();
  sensors = {};
  sensors.warmup(22000);
  sensors.setSkin(22000, 89999);
  expectThermalVote(9);
  sensors.refreshAdc(22000, 90000);
  sensors.refreshSkin();
  expectHardFallback(sensors, A830QualBoardHot);
}

void testFilteredDieStep()
{
  boot();
  Sensors sensors;
  sensors.warmup(22000);
  sensors.die = 999;
  for (unsigned i = 0; i < 10; ++i) sensors.tick();
  expectThermalVote(12);
  sensors.die = 949;
  sensors.tick();
  CHECK(guard.thermal().raw < 950 && guard.thermal().filter16 >= 950 * 16);
  CHECK(guard.thermal().downs == 3 && guard.high());
  expectThermalVote(11);
}

pianopolicy::Config energyConfig()
{
  pianopolicy::Config config;
  config.act = config.lpmOnly = config.gpuNeeded = true;
  return config;
}

void applyEnergyMode(pianopolicy::Engine &engine, pianopolicy::Inputs inputs, uint32_t profile, uint32_t khz,
                     bool profileChanged = true)
{
  using namespace pianopolicy;
  inputs.gpuClients = pianogpu::kCeilingClients;
  bool profileAction = false;
  const bool slotChanged = engine.gpuWritten() != khz;
  const Actions actions = engine.step(inputs, gEnv.now / ms);
  CHECK(actions.n == unsigned(profileChanged) + unsigned(slotChanged));
  for (unsigned i = 0; i < actions.n; ++i) {
    const Action &action = actions.a[i];
    if (action.kind == Kind::thermalProfile) {
      CHECK(!profileAction && action.value == profile);
      profileAction = true;
      engine.profileDone(action.value, Result::taken);
    } else if (action.kind == Kind::gpuCeiling) {
      CHECK(action.client == pianogpu::kCeilingPolicy && action.value == khz);
      setCeiling(action.client, action.value);
      CHECK(engine.gpuDone(action.value, Result::taken, inputs.thermal, action.client).n == 0);
    } else {
      CHECK(false);
    }
  }
  CHECK(profileAction == profileChanged);
  CHECK(engine.gpuOwnSlot() && engine.gpuWritten() == khz && slots.khz[pianogpu::kCeilingPolicy] == khz);
}

void testEnergyModes()
{
  using namespace pianopolicy;
  for (const Power source : {Power::external, Power::battery}) {
    boot();
    Sensors sensors;
    sensors.warmup(45800);
    Engine engine(energyConfig());
    Inputs input;
    input.power = source;
    input.thermalProfiles = kThermalPerformance;
    applyEnergyMode(engine, input, kThermalBalanced, 0);
    expectThermalVote(12);
    input.lpm = true;
    applyEnergyMode(engine, input, kThermalSaver, 342000);
    sensors.tick();
    expectVote(3, 342000);
    CHECK(uint8_t(guard.thermalSnapshot()) == 12 && slots.holder() == pianogpu::kCeilingPolicy);

    // Thermal slot 0 can lower the cap even during Low Power. High Power and Balanced leave that slot in force.
    input.thermal = Thermal::passive;
    setCeiling(pianogpu::kCeilingThermal, 160000);
    sensors.tick();
    expectVote(1, 160000);
    input.hpm = true;
    applyEnergyMode(engine, input, kThermalSaver, 342000, false);
    sensors.tick();
    expectVote(1, 160000);
    input.lpm = false;
    applyEnergyMode(engine, input, kThermalPerformance, 0);
    sensors.tick();
    expectVote(1, 160000);
    input.hpm = false;
    applyEnergyMode(engine, input, kThermalBalanced, 0);
    sensors.tick();
    expectVote(1, 160000);
    CHECK(slots.holder() == pianogpu::kCeilingThermal);
    setCeiling(pianogpu::kCeilingThermal, 0);
    sensors.tick();
    expectThermalVote(12);

    input.thermal = Thermal::normal;
    input.hpm = true;
    applyEnergyMode(engine, input, kThermalPerformance, 0);
    input.power = source == Power::external ? Power::battery : Power::external;
    applyEnergyMode(engine, input, kThermalPerformance, 0, false);
    sensors.tick();
    CHECK(guard.thermal().skinTable == 1);
    expectThermalVote(12);
    guard.thermal().skinTable = 0;
    sensors.tick();
    expectThermalVote(9);
    input.hpm = false;
    applyEnergyMode(engine, input, kThermalBalanced, 0);
    sensors.tick();
    expectThermalVote(9);
  }
}

void testSustain()
{
  using namespace pianopolicy;
  boot();
  Sensors sensors;
  sensors.warmup(41200);
  Engine engine(energyConfig());
  Inputs input;
  input.hpm = true;
  input.thermalProfiles = kThermalPerformance;
  applyEnergyMode(engine, input, kThermalPerformance, 0);
  expectThermalVote(13);

  // Interpolate the documented endpoints over 600 s of sustained demand. ADC conversions arrive every 5 s,
  // the skin API is read every 1 s, the guard runs every 100 ms, and DCVS services 10 ms windows in between.
  constexpr unsigned durationMs = 600'000;
  const uint64_t start = gEnv.now;
  for (unsigned elapsedMs = 0; elapsedMs <= durationMs; elapsedMs += 10) {
    gEnv.now = start + elapsedMs * ms;
    if (elapsedMs % 5000 == 0) sensors.refreshAdc(41200 + int32_t(elapsedMs * 4600ull / durationMs));
    if (elapsedMs % 1000 == 0) sensors.refreshSkin();
    if (elapsedMs % 100 == 0) {
      sensors.die = 706 + int16_t(elapsedMs * 135 / durationMs);
      sensors.heartbeat();
    }
    if (elapsedMs) Sensors::service(10);
    // The linear proxy first reaches 45 C at the 500 s ADC round.
    const uint8_t expected = elapsedMs < 500'000 ? 13 : 12;
    expectThermalVote(expected);
    CHECK(guard.high() && guard.thermal().healthy && guard.thermal().skinValid && !guard.reason());
    CHECK(guard.thermal().step == 14 && guard.thermal().downs == 0 && guard.thermal().band == (expected == 13 ? 1 : 2));
    if (elapsedMs == 0) CHECK(guard.thermal().skinMilli == 41200 && guard.thermal().raw == 706);
  }
  CHECK(gEnv.now - start == durationMs * ms && guard.thermal().skinMilli == 45800 && guard.thermal().raw == 841);
  CHECK(guard.thermal().cause == A830R3bThermal::CauseSkinBand);
  CHECK(!lowVotes && !unsafeExits && !dcvs.stats().voteFailures && !recovery.stats().recoveries);

  // Extend beyond the measured envelope. The skin crossing binds immediately and a return to 45.8 C retains it.
  sensors.setSkin(47000);
  expectThermalVote(11);
  sensors.setSkin(45800);
  expectThermalVote(11);
  sensors.setSkin(44999);
  expectThermalVote(12);

  sensors.die = 949;
  sensors.tick();
  CHECK(guard.thermal().step == 14 && !guard.thermal().downs);
  sensors.die = 950;
  sensors.tick();
  CHECK(guard.thermal().step == 13 && guard.thermal().downs == 1);
  expectThermalVote(12);
  for (uint8_t step = 13; step >= 3; --step) {
    for (unsigned tick = 1; tick <= 5; ++tick) {
      sensors.tick();
      const uint8_t expected = tick == 5 && step > 3 ? step - 1 : step;
      CHECK(guard.high() && guard.thermal().step == expected);
      const uint8_t cap = std::min<uint8_t>(12, expected);
      expectThermalVote(cap);
    }
  }
  CHECK(guard.thermal().downs == 11 && guard.thermal().cause == A830R3bThermal::CauseLadder);

  // Recovery needs filtered die at or below 90 C and two continuously cool seconds with valid skin.
  sensors.die = 901;
  for (unsigned i = 0; i < 30; ++i) {
    sensors.tick();
    expectThermalVote(3);
    CHECK(!guard.thermal().coolSince);
  }
  sensors.die = 899;
  sensors.refreshAdc(44999);
  sensors.adc.validMask &= ~(1u << channelIndex("wifi"));
  sensors.refreshSkin();
  for (unsigned i = 0; i < 30; ++i) {
    sensors.tick();
    expectThermalVote(3);
    CHECK(!guard.thermal().coolSince);
  }
  CHECK(guard.thermal().filter16 <= 900 * 16);
  sensors.setSkin(44999);
  CHECK(guard.thermal().coolSince == gEnv.now);
  expectThermalVote(3);
  const unsigned ups = guard.thermal().ups;
  for (unsigned i = 1; i <= 20; ++i) {
    sensors.tick();
    expectThermalVote(i == 20 ? 4 : 3);
  }
  CHECK(guard.high() && guard.thermal().ups == ups + 1 && !guard.reason());
}

}  // namespace

int main()
{
  testLadder();
  testSkinBands();
  testSkinFaults();
  testAdvancingAdcRounds();
  testHardFaults();
  testFilteredDieStep();
  testEnergyModes();
  testSustain();
  std::printf("PASS: GPU DVFS and thermal policy (%u checks)\n", gChecks);
}
