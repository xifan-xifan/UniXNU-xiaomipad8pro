// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -I../../include thermal_shutdown_stress_host.cpp -o /tmp/thermal_shutdown_stress_host && /tmp/thermal_shutdown_stress_host
// Virtual clock only. The production TSENS, ADC snapshot, governor and policy code run against
// synthetic inputs. The shutdown sink records a halt request so cooldown can still be tested.

#include "../BoardTemps.h"
#include "../Cluster.h"
#include "../Policy.h"
#include "../../PianoPolicy/Policy.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>

using namespace pianothermal;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
  std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, __func__, #__VA_ARGS__); \
  std::exit(1); \
} } while (0)

using Pair = std::array<uint32_t, 2>;
constexpr uint32_t kTickMs = 5, kRiseMs = 100, kAdcPeriodMs = 5000;
constexpr int32_t kHotDeci = 950, kReleaseDeci = 870;
constexpr Pair kBase{2400000, 2438400}, kTop{3532800, 4320000}, kSaver{1785600, 1958400};
constexpr Pair kFloor{384000, 1017600}, kSkinFault{748800, 1017600};
constexpr uint32_t kLevels[2][16] = {
    {384000, 556800, 748800, 960000, 1152000, 1363200, 1555200, 1785600, 1996800, 2227200, 2400000, 2745600,
     2918400, 3072000, 3321600, 3532800},
    {1017600, 1209600, 1401600, 1689600, 1958400, 2246400, 2438400, 2649600, 2841600, 3072000, 3283200,
     3513600, 3801600, 4089600, 4204800, 4320000},
};
constexpr unsigned kTopLevel = std::size(kLevels[0]) - 1;

struct DieFeed {
  struct Sample {
    uint32_t status = kStatusValid | 300u;
    uint64_t atMs = 0;
  };

  Sample sensor[kControllers][kMaxSensors];
  uint64_t nowMs = 0;

  void publish(int32_t deci, uint64_t atMs)
  {
    for (auto &controller : sensor)
      for (auto &sample : controller) sample = {kStatusValid | encodeThreshold(deci), atMs};
  }

  TsensIo io(uint64_t now)
  {
    nowMs = now;
    return {
        this,
        [](void *ctx, unsigned c, uint32_t off) {
          const auto &feed = *static_cast<DieFeed *>(ctx);
          const Sample &sample = feed.sensor[c][(off - kTmStatus) / 4];
          // A capture timestamp is host metadata. A missing or replayed capture supplies no VALID word.
          return sample.atMs == feed.nowMs ? sample.status : 0u;
        },
        [](void *, unsigned) { return true; },
    };
  }
};

struct Modes {
  unsigned energy = 2;  // Automatic, Low Power, High Power
  bool lpm = false;     // independent notification, including overlap with High Power
  bool ac = true;
};

struct Reading {
  PolicyEvent protection;
  piano_skin_temps skin;
  Pair limits{}, caps{};
  Profile profile = Profile::bySource;
  unsigned profileWrites = 0;
};

struct Bench {
  uint64_t nowMs = 20000;
  unsigned ticks = 0, halts = 0;
  DieFeed die;
  Monitor monitor;
  Policy policy;
  adc::Snapshot board;
  Governor cpu[2];
  perf::SkinCap skinBand[2];
  perf::Scene scene = perf::Scene::normal;
  pianopolicy::Engine route;
  Profile profile = Profile::bySource;

  explicit Bench(bool baseCaps = false, bool lpmOnly = false)
      : route(pianopolicy::Config{.act = true, .lpmOnly = lpmOnly})
  {
    for (unsigned d = 0; d < 2; ++d) {
      GovernorConfig config;
      config.capKhz = baseCaps ? capKhz(d ? 2438 : 2400) : 0;
      config.batteryKhz = capKhz(d ? 1958 : 1785);
      config.externalKhz = capKhz(d ? 4320 : 3532);
      config.stepMs = kTickMs;
      config.riseMs = kRiseMs;
      cpu[d].init(CpuMode::govern, config, kLevels[d], std::size(kLevels[d]), d ? 13 : 15, kTopLevel);
    }
    publishBoard(30000);
  }

  uint64_t nowUs() const { return nowMs * 1000; }

  void publishBoard(int32_t milliC)
  {
    board.state = PIANO_BOARD_TEMPS_RUNNING;
    board.periodMs = kAdcPeriodMs;
    ++board.rounds;
    board.validMask = (1u << adc::kChannelCount) - 1;
    for (unsigned i = 0; i < adc::kChannelCount; ++i) {
      board.milliC[i] = milliC;
      board.atUs[i] = nowUs();
    }
  }

  Reading tick(Modes modes = {})
  {
    monitor.poll(die.io(nowMs));
    Reading r{.protection = policy.update(monitor, nowMs), .skin = {}};
    // Level 2 with shutdown enabled. Keep the one-shot request latched like protect().
    if (r.protection.after == State::critical && !halts) ++halts;

    const pianopolicy::Inputs inputs{
        .power = modes.ac ? pianopolicy::Power::external : pianopolicy::Power::battery,
        .lpm = modes.energy == 1 || modes.lpm, .hpm = modes.energy == 2, .thermalProfiles = kProfileMax,
    };
    const auto actions = route.step(inputs, nowMs);
    for (unsigned i = 0; i < actions.n; ++i) {
      const auto &action = actions.a[i];
      if (action.kind != pianopolicy::Kind::thermalProfile) continue;
      CHECK(profileValid(action.value));
      profile = Profile(action.value);
      route.profileDone(action.value, pianopolicy::Result::taken);
      ++r.profileWrites;
    }
    r.profile = profile;
    r.skin.version = PIANO_SKIN_TEMPS_VERSION;
    CHECK(adc::fillSkinTemps(board, nowUs(), &r.skin, adc::kFreshPeriods));

    // The steady-demand full-OPP path, with the scene and thermal cap applied to both domains.
    const auto power = profilePower(modes.ac ? PowerSource::external : PowerSource::battery, profile);
    const auto nextScene = perf::sceneFor(profile == Profile::performance);
    const auto table = perf::sceneTable(nextScene);
    for (unsigned d = 0; d < 2; ++d) {
      auto &g = cpu[d];
      g.thermal(clusterThermal(monitor, d, {kHotDeci, kReleaseDeci}, r.protection.after == State::critical), nowMs);
      if (nextScene != scene) skinBand[d].reset();
      const uint32_t skinCap = skinBand[d].update(table.steps[d], table.count[d], r.skin.virtual_valid, r.skin.virtual_milli_c);
      g.setRuntimeCaps(false, 0, skinCap ? skinCap : capKhz(d ? 4320 : 3532), true);
      if (!r.skin.virtual_valid) g.setRuntimeCaps(false, 0, perf::fullCapKhz(g.perfKhz(), perf::skinFaultCap(d)));
      r.limits[d] = g.limit();
      r.caps[d] = g.khz(g.ceiling(power));
      CHECK(g.khz(g.tick({1000, power, UINT32_MAX})) == r.caps[d]);
    }
    scene = nextScene;
    ++ticks;
    nowMs += kTickMs;
    return r;
  }

  Reading step(int32_t deci, Modes modes = {})
  {
    die.publish(deci, nowMs);
    return tick(modes);
  }
};

static void rampAndCooldown()
{
  Bench b;
  for (int32_t t = 300; t < kHotDeci; ++t) {
    const auto r = b.step(t);
    CHECK(r.caps == kTop && r.limits == Pair{kTopLevel, kTopLevel});
    CHECK(r.protection.after == (t >= 850 ? State::warm : State::normal) && !b.halts);
  }
  for (int32_t t = kHotDeci; t <= 1050; ++t) {
    const auto r = b.step(t);
    const unsigned limit = kTopLevel - std::min(kTopLevel, unsigned(t - kHotDeci + 1));
    CHECK(r.limits == Pair{limit, limit} && r.caps == Pair{kLevels[0][limit], kLevels[1][limit]});
    CHECK(r.protection.after == (t == 1050 ? State::passive : State::warm) && !b.halts);
    CHECK(pollPeriodMs(pollTier(b.monitor, {kHotDeci, kReleaseDeci}, limit < kTopLevel, false, 700),
                       1000, 250, kTickMs) == kTickMs);
  }
  for (unsigned i = 0; i < 120; ++i) {
    const auto r = b.step(1050);
    CHECK(r.caps == kFloor && r.protection.gpuLimited() && !r.protection.criticalPending && !b.halts);
  }
  for (int32_t t = 1049; t >= kReleaseDeci; --t) {
    const auto r = b.step(t);
    CHECK(r.caps == kFloor && r.protection.gpuLimited() == (t >= 1000));
  }
  // A short dip followed by equality at the release point must restart the dwell.
  for (unsigned i = 0; i < kRiseMs / kTickMs; ++i) CHECK(b.step(869).caps == kFloor);
  CHECK(b.step(870).caps == kFloor);
  for (int32_t t = kReleaseDeci - 1; t >= 400; --t) {
    const auto r = b.step(t);
    const unsigned limit = std::min(kTopLevel, unsigned(kReleaseDeci - 1 - t) * kTickMs / kRiseMs);
    CHECK(r.limits == Pair{limit, limit} && r.caps == Pair{kLevels[0][limit], kLevels[1][limit]});
    CHECK(r.protection.after == (t >= 800 ? State::warm : State::normal));
  }
  CHECK(b.ticks >= 1000 && !b.halts);
}

static void shutdownAndHysteresis()
{
  Bench b;
  // One non-CPU zone decides shutdown and, once critical, forces both CPU domains to their floors.
  const auto gpu = [&](int32_t deci) {
    b.die.publish(400, b.nowMs);
    b.die.sensor[2][8].status = kStatusValid | encodeThreshold(deci);
    return b.tick();
  };
  CHECK(gpu(1149).protection.after == State::passive && !b.halts);
  const auto holdShortOfTrip = [&] {
    for (unsigned i = 0; i < 1000 / kTickMs; ++i) {
      const auto r = gpu(1150);
      CHECK(r.protection.criticalPending && r.protection.after == State::passive && r.caps == kTop && !b.halts);
    }
  };
  holdShortOfTrip();
  CHECK(!gpu(1149).protection.criticalPending && !b.halts);
  holdShortOfTrip();
  b.die.publish(400, b.nowMs);
  b.die.sensor[2][8].status = 0;
  const auto missing = b.tick();
  CHECK(missing.protection.incomplete && !missing.protection.criticalPending && missing.protection.after == State::passive);
  CHECK(!b.halts);
  holdShortOfTrip();
  const auto trip = gpu(1150);
  CHECK(trip.protection.after == State::critical && !trip.protection.criticalPending && trip.caps == kFloor && b.halts == 1);
  CHECK(trip.protection.controller == 2 && trip.protection.sensor == 8 && trip.protection.deci == 1150);
  for (int32_t t = 1149; t >= 1100; --t) {
    const auto r = gpu(t);
    CHECK(r.protection.after == State::critical && r.protection.gpuLimited() && r.caps == kFloor && b.halts == 1);
  }
  const auto blind = b.tick();  // Every TSENS capture is now stale.
  CHECK(blind.protection.incomplete && blind.protection.after == State::critical && blind.caps == kFloor);
  CHECK(gpu(1099).protection.after == State::passive && b.halts == 1);
  CHECK(gpu(1000).protection.gpuLimited());
  const auto cool = gpu(999);
  CHECK(!cool.protection.gpuLimited() && cool.caps == kFloor && b.halts == 1);
}

static void dieFaults()
{
  enum class Fault { nan, busSentinel, temperatureSentinel, stuckCapture, futureCapture };
  for (unsigned c = 0; c <= kControllers; ++c) {
    const bool all = c == kControllers;
    for (unsigned s = 0; s < (all ? 1u : kMap[c].count); ++s) {
      for (const auto fault : {Fault::nan, Fault::busSentinel, Fault::temperatureSentinel, Fault::stuckCapture, Fault::futureCapture}) {
        Bench b;
        CHECK(b.step(1050).protection.after == State::passive);
        const auto inject = [&](DieFeed::Sample &sample, unsigned i) {
          switch (fault) {
            case Fault::nan: {
              const double value = std::numeric_limits<double>::quiet_NaN();
              // A nonfinite host input maps to a cleared VALID bit at the integer sensor boundary.
              sample.status = value >= -400 && value <= 1500 ? kStatusValid | encodeThreshold(int32_t(value)) : 0u;
              break;
            }
            case Fault::busSentinel: sample.status = UINT32_MAX; break;
            case Fault::temperatureSentinel: sample.status = kStatusValid | encodeThreshold(-2048); break;
            case Fault::stuckCapture:
              sample = {kStatusValid | 1050u, sample.atMs - uint64_t(i + 1) * kTickMs};
              break;
            case Fault::futureCapture: sample.atMs += kTickMs; break;
          }
        };
        for (unsigned i = 0; i <= kStalePolls; ++i) {
          b.die.publish(400, b.nowMs);
          if (all) {
            for (auto &controller : b.die.sensor)
              for (auto &sample : controller) inject(sample, i);
          } else {
            inject(b.die.sensor[c][s], i);
          }
          const auto r = b.tick();
          CHECK(r.protection.incomplete && r.protection.gpuLimited() && r.protection.after == State::passive);
          CHECK(!r.protection.criticalPending && !b.halts);
          for (unsigned d = 0; d < 2; ++d) {
            const bool affected = all || (c == d && s >= kMap[d].cpuFirst && s <= kMap[d].cpuLast);
            CHECK(r.caps[d] == (affected ? kFloor[d] : kLevels[d][kTopLevel - 1]));
          }
        }
        CHECK(!b.monitor.usable(all ? 0 : c, s));
        const auto recovered = b.step(400);
        CHECK(!recovered.protection.incomplete && !recovered.protection.gpuLimited());
        for (unsigned d = 0; d < 2; ++d) {
          const bool affected = all || (c == d && s >= kMap[d].cpuFirst && s <= kMap[d].cpuLast);
          if (affected) CHECK(recovered.caps[d] == kFloor[d]);
        }
      }
    }
  }
  Bench b;
  CHECK(b.step(-1).protection.deci == -1 && b.monitor.allFresh());  // -0.1 C is a legitimate reading.
  CHECK(!b.step(400).protection.gpuLimited());
  b.die.publish(400, b.nowMs);
  b.die.sensor[1][kMap[1].cpuFirst].status = UINT32_MAX;
  const auto coldFault = b.tick();
  CHECK(coldFault.protection.after == State::normal && coldFault.protection.gpuLimited());
  CHECK(coldFault.caps == Pair{kTop[0], kFloor[1]});
}

static void adcFaults()
{
  enum class Fault { nan, minSentinel, maxSentinel, frozen, zeroStamp, futureStamp, missing };
  uint32_t skinChannels = 0;
  for (const char *name : perf::kSkinChannel) skinChannels |= 1u << adc::channelIndex(name);
  for (unsigned input = 0; input <= adc::kChannelCount; ++input) {
    const bool all = input == adc::kChannelCount;
    const bool affectsSkin = all || (skinChannels & (1u << input));
    for (const auto fault : {Fault::nan, Fault::minSentinel, Fault::maxSentinel, Fault::frozen,
                             Fault::zeroStamp, Fault::futureStamp, Fault::missing}) {
      Bench b;
      const Modes automatic{0, false, true};
      b.publishBoard(45000);  // 44.035 C virtual skin: 1555.2 / 1017.6 MHz bands
      CHECK(b.step(400, automatic).caps == Pair{1555200, 1017600});
      const uint64_t lastGoodUs = b.nowUs() - 15000000;  // Three 5 s periods before the first read.
      for (unsigned i = 0; i < 16; ++i) {
        for (unsigned ch = 0; ch < adc::kChannelCount; ++ch) {
          if (!all && ch != input) continue;
          const uint32_t bit = 1u << ch;
          switch (fault) {
            case Fault::nan: {
              const double value = std::numeric_limits<double>::quiet_NaN();
              if (value >= adc::kMinMilliC && value <= adc::kMaxMilliC) b.board.milliC[ch] = int32_t(value);
              else b.board.validMask &= ~bit;
              break;
            }
            case Fault::minSentinel: b.board.milliC[ch] = INT32_MIN; break;
            case Fault::maxSentinel: b.board.milliC[ch] = INT32_MAX; break;
            case Fault::frozen: b.board.atUs[ch] = lastGoodUs; break;
            case Fault::zeroStamp: b.board.atUs[ch] = 0; break;
            case Fault::futureStamp: b.board.atUs[ch] = b.nowUs() + 1; break;
            case Fault::missing: b.board.validMask &= ~bit; break;
          }
        }
        const auto r = b.step(400, automatic);
        const bool valid = !affectsSkin || (fault == Fault::frozen && i == 0);
        CHECK(bool(r.skin.virtual_valid) == valid && r.caps == (valid ? Pair{1555200, 1017600} : kSkinFault));
        CHECK(b.skinBand[0].step() == 6 && b.skinBand[1].step() == 6);
      }
      b.publishBoard(43000);  // 42.149 C: below the trigger, still inside cluster 1's hold band
      CHECK(b.step(400, automatic).caps == Pair{1555200, 1017600});
      b.publishBoard(42800);  // 41.960 C: below the 42 C clear point
      CHECK(b.step(400, automatic).caps == Pair{1555200, 1401600});
    }
  }
}

static Modes packet(unsigned id)
{
  return {id % 3u, bool(id / 3 % 2), bool(id / 6)};
}

static void energyAndPowerTransitions()
{
  for (const bool base : {false, true}) {
    for (const bool lpmOnly : {false, true}) {
      Bench b(base, lpmOnly), reversed(base, lpmOnly);
      const auto verify = [&](Bench &model, unsigned id, int32_t deci, unsigned limit, bool fault) {
        const Modes modes = packet(id);
        const bool low = modes.energy == 1 || modes.lpm;
        const bool high = modes.energy == 2 && !low;
        const Profile expected = low ? Profile::saver : high ? Profile::performance : lpmOnly ? Profile::balanced : Profile::bySource;
        const auto before = model.profile;
        if (model.ticks % (kAdcPeriodMs / kTickMs) == 0) model.publishBoard(30000);
        if (fault) model.board.validMask = 0;
        const auto r = model.step(deci, modes);
        CHECK(r.profile == expected && r.profileWrites == unsigned(before != expected));
        const Pair profileCaps = low || (!high && !modes.ac && !lpmOnly) ? kSaver : high ? kTop : Pair{2745600, 3072000};
        CHECK(r.limits == Pair{limit, limit} && bool(r.skin.virtual_valid) == !fault);
        for (unsigned d = 0; d < 2; ++d) {
          const uint32_t cap = std::min({profileCaps[d], base ? kBase[d] : kTop[d], kLevels[d][limit],
                                         fault ? kSkinFault[d] : kTop[d]});
          CHECK(r.caps[d] == cap);
        }
        return r.caps;
      };
      for (unsigned i = 0; i < 2400; ++i) {
        const unsigned id = (i * 5 + i / 12) % 12;  // Each block visits all combinations of Energy Mode, LPM and source.
        const bool hot = i >= 1200;
        const unsigned limit = hot ? kTopLevel - std::min(i - 1199, kTopLevel) : kTopLevel;
        verify(b, id, hot ? 950 : 400, limit, false);
        verify(reversed, 11 - id, hot ? 950 : 400, limit, false);
      }
      for (unsigned i = 0; i < 12; ++i) verify(b, i, 950, 0, true);
      b.publishBoard(30000);
      for (unsigned i = 0; i <= kTopLevel * (kRiseMs / kTickMs); ++i) {
        const unsigned limit = i * kTickMs / kRiseMs;
        const auto cap = verify(b, 8, 400, limit, false);
        CHECK(cap == verify(reversed, 8, 400, limit, false));
      }
      for (unsigned i = 0; i < 12; ++i) CHECK(verify(b, i, 400, kTopLevel, false) == verify(reversed, i, 400, kTopLevel, false));
      for (unsigned i = 0; i < 12; ++i) verify(b, i, 400, kTopLevel, true);
      const auto blind = b.tick();  // Leave both feeds in fault on the next tick.
      CHECK(blind.protection.incomplete && !blind.skin.virtual_valid && blind.caps == kFloor);
    }
  }
}

static void scalingBoundaries()
{
  for (const uint32_t mhz : {0u, 300u, 1785u, 1958u, 2400u, 2438u, 3532u, 4320u, 5000u}) {
    const uint64_t expected = mhz ? (uint64_t(mhz) + 1) * 1000 - 1 : 0;
    CHECK(capKhz(mhz) == expected);
  }
  for (const auto &table : kLevels)
    for (const uint32_t khz : table)
      for (uint32_t busy = 0; busy <= 1000; ++busy)
        CHECK(perf::workKhz(busy, khz) == uint64_t(khz) * busy / 1000);

  Bench b;
  for (unsigned mask = 0; mask <= perf::kSkinMask; ++mask) {
    b.publishBoard(0);
    int64_t expected = 0;
    constexpr int weights[] = {500, 131, -9, 89, 232};
    for (unsigned i = 0; i < perf::kSkinInputs; ++i) {
      const int32_t milli = mask & (1u << i) ? 125000 : -40000;
      b.board.milliC[adc::channelIndex(perf::kSkinChannel[i])] = milli;
      expected += int64_t(milli) * weights[i];
    }
    const auto r = b.step(mask & 1 ? -400 : 1500);
    CHECK(r.skin.virtual_valid && r.skin.virtual_milli_c == expected / 1000 + 1600);
  }
}

int main()
{
  rampAndCooldown();
  shutdownAndHysteresis();
  dieFaults();
  adcFaults();
  energyAndPowerTransitions();
  scalingBoundaries();
  std::puts("thermal_shutdown_stress_host: ramp, shutdown, sensor faults, Energy Mode and scaling passed");
}
