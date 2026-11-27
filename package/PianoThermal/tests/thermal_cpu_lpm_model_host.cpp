// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fsanitize=float-cast-overflow -fno-sanitize-recover=all -I../../include -I../../PianoA830/adreno830/platform/sm8750 thermal_cpu_lpm_model_host.cpp -o /tmp/thermal_cpu_lpm_model_host && /tmp/thermal_cpu_lpm_model_host
// Offline B13 coverage. The Android qualification session adds a cool dwell, paired rollback, and
// LPM restoration around the real selector and governor. Those session rules are test-only.
// The capped fixture uses the explicit cap pair from docs/drivers/android-reference-controls.md.
// Hardware qualification of any unverified high-OPP thermal profile remains HOLD.

#include "../BoardTemps.h"
#include "../Cluster.h"
#include "../Scmi.h"
#include "../../PianoA830/adreno830/A830R3bThermal.h"
#include "../../PianoPolicy/Policy.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>

using namespace pianothermal;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
  std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, __func__, #__VA_ARGS__); \
  std::exit(1); \
} } while (0)

using Frequencies = std::array<uint32_t, 2>;
using DieReadings = std::array<ThermalInput, 2>;
constexpr Frequencies kBase{2400000, 2438400}, kNext{2745600, 2649600};
constexpr Frequencies kSaver{1785600, 1958400}, kSkinFault{748800, 1017600}, kMinimum{384000, 1017600};
constexpr uint32_t kLevels[2][16] = {
    {384000, 556800, 748800, 960000, 1152000, 1363200, 1555200, 1785600, 1996800, 2227200, 2400000, 2745600,
     2918400, 3072000, 3321600, 3532800},
    {1017600, 1209600, 1401600, 1689600, 1958400, 2246400, 2438400, 2649600, 2841600, 3072000, 3283200,
     3513600, 3801600, 4089600, 4204800, 4320000},
};
constexpr unsigned kSustained[] = {15, 13};
constexpr PowerSource kAc = PowerSource::external;
constexpr ClusterTrips kTrips{900, 850};
constexpr uint64_t kBoardAtUs = 1000000;

struct EnergyRoute {
  pianopolicy::Config config = {.act = true, .lpmOnly = true};
  pianopolicy::Engine engine{config};
  pianopolicy::Inputs inputs = {.power = pianopolicy::Power::external, .thermalProfiles = kProfileMax};
  Profile profile = Profile::bySource;
  unsigned changes = 0;

  void notifications(bool lpm, bool hpm)
  {
    inputs.lpm = lpm;
    inputs.hpm = hpm;
    const auto actions = engine.step(inputs, changes);
    for (unsigned i = 0; i < actions.n; ++i) {
      const auto &action = actions.a[i];
      if (action.kind != pianopolicy::Kind::thermalProfile) continue;
      CHECK(profileValid(action.value));
      profile = Profile(action.value);
      engine.profileDone(action.value, pianopolicy::Result::taken);
      ++changes;
    }
  }

  void powermode(unsigned value) { notifications(value == 1, value == 2); }
};

// TSENS is integer on the wire. Reject invalid synthetic input before converting to its register format.
static bool inRange(double value, int32_t min, int32_t max)
{
  return value >= min && value <= max;
}

struct DieRegisters {
  Monitor monitor;
  uint32_t words[kControllers][kMaxSensors]{};
  bool mapped[kControllers] = {true, true, true, true};
  uint64_t captureId[2]{};

  DieRegisters() { all(600); }

  static uint32_t word(int32_t deci) { return kStatusValid | encodeThreshold(deci); }

  void all(int32_t deci)
  {
    for (auto &controller : words)
      for (auto &status : controller) status = word(deci);
  }

  // The host feed can identify replay by capture ID. Equal temperatures with new IDs are normal.
  void capture(unsigned domain, double deci, uint64_t id)
  {
    const bool fresh = id > captureId[domain] && inRange(deci, kPlausibleMinDeci, kPlausibleMaxDeci);
    words[domain][kMap[domain].cpuFirst] = fresh ? word(int32_t(deci)) : 0;
    captureId[domain] = id;
  }

  DieReadings poll()
  {
    monitor.poll({
        this,
        [](void *ctx, unsigned c, uint32_t reg) { return static_cast<DieRegisters *>(ctx)->words[c][(reg - kTmStatus) / 4]; },
        [](void *ctx, unsigned c) { return static_cast<DieRegisters *>(ctx)->mapped[c]; },
    });
    return {clusterThermal(monitor, 0, kTrips, false), clusterThermal(monitor, 1, kTrips, false)};
  }
};

static adc::Snapshot boardSnapshot(int32_t milliC = 30000)
{
  adc::Snapshot snapshot;
  snapshot.state = PIANO_BOARD_TEMPS_RUNNING;
  snapshot.rounds = 1;
  snapshot.periodMs = 5000;
  for (unsigned i = 0; i < adc::kChannelCount; ++i) {
    snapshot.milliC[i] = milliC;
    snapshot.atUs[i] = kBoardAtUs;
    snapshot.validMask |= 1u << i;
  }
  return snapshot;
}

static piano_skin_temps boardSkin(const adc::Snapshot &snapshot, uint64_t nowUs = kBoardAtUs,
                                 uint32_t freshPeriods = adc::kFreshPeriods)
{
  piano_skin_temps skin{};
  skin.version = PIANO_SKIN_TEMPS_VERSION;
  CHECK(adc::fillSkinTemps(snapshot, nowUs, &skin, freshPeriods));
  return skin;
}

struct CpuLadder {
  Governor governor[2];
  perf::SkinCap skinCap[2];
  perf::Scene scene = perf::Scene::normal;
  Frequencies skinKhz{};

  explicit CpuLadder(bool baseCaps = false)
  {
    for (unsigned d = 0; d < 2; ++d) {
      GovernorConfig config;
      if (baseCaps) config.capKhz = capKhz(d ? 2438 : 2400);
      config.batteryKhz = capKhz(d ? 1958 : 1785);
      config.externalKhz = capKhz(d ? 4320 : 3532);
      governor[d].init(CpuMode::govern, config, kLevels[d], std::size(kLevels[d]), kSustained[d], 0);
    }
  }

  // Full-OPP skin scenes without simulated load bursts.
  void skin(Profile profile, const piano_skin_temps &sample)
  {
    const auto nextScene = perf::sceneFor(profile == Profile::performance);
    const auto table = perf::sceneTable(nextScene);
    for (unsigned d = 0; d < 2; ++d) {
      if (scene != nextScene) skinCap[d].reset();
      skinKhz[d] = skinCap[d].update(table.steps[d], table.count[d], sample.virtual_valid, sample.virtual_milli_c);
      const uint32_t cap = skinKhz[d] ? skinKhz[d] : kLevels[d][15];
      governor[d].setRuntimeCaps(false, 0, cap, true);
    }
    scene = nextScene;
  }

  void thermal(const DieReadings &die, uint64_t now)
  {
    for (unsigned d = 0; d < 2; ++d) governor[d].thermal(die[d], now);
  }

  Frequencies ceilings(PowerSource power) const
  {
    return {governor[0].khz(governor[0].ceiling(power)), governor[1].khz(governor[1].ceiling(power))};
  }
};

static void energyModes()
{
  EnergyRoute route;
  CpuLadder cpu;
  DieRegisters die;
  const auto skin = boardSkin(boardSnapshot(40000));  // virtual skin 39.32 C
  const auto mode = [&](unsigned value) {
    route.powermode(value);
    cpu.skin(route.profile, skin);
    return cpu.ceilings(profilePower(kAc, route.profile));
  };
  CHECK(mode(0) == Frequencies{1996800, 1958400} && route.profile == Profile::balanced && !route.inputs.lpm);
  CHECK(mode(1) == kSaver && route.profile == Profile::saver && route.inputs.lpm);
  CHECK(mode(2) == Frequencies{3532800, 4320000} && route.profile == Profile::performance && !route.inputs.lpm);
  CHECK(cpu.scene == perf::Scene::nolimits && route.changes == 3);

  for (auto &g : cpu.governor) g.setBoostFloor(4320000);
  die.all(900);
  cpu.thermal(die.poll(), 100);
  CHECK(cpu.ceilings(kAc) == Frequencies{3321600, 4204800});
  CHECK(cpu.governor[1].khz(cpu.governor[1].enforce(kAc)) == 4204800);
  CHECK(mode(2) == Frequencies{3321600, 4204800} && route.changes == 3);

  route.notifications(true, true);  // powerd notification overlap during a mode change
  cpu.skin(route.profile, skin);
  CHECK(route.profile == Profile::saver && cpu.scene == perf::Scene::normal);
  CHECK(cpu.ceilings(profilePower(kAc, route.profile)) == kSaver);
  CHECK(mode(0) == Frequencies{1996800, 1958400} && !route.inputs.lpm);
  CHECK(mode(2) == Frequencies{3321600, 4204800});  // routing never resets the thermal limit
  die.all(1150);
  cpu.thermal(die.poll(), 110);
  CHECK(cpu.ceilings(kAc) == kMinimum);
}

static void skinBands()
{
  A830R3bThermal gpu;
  auto skin = boardSkin(boardSnapshot());
  // Drive the real GPU guard with precise boundaries at its virtual-skin input.
  const auto capAt = [&](int32_t milliC) {
    skin.virtual_milli_c = milliC;
    gpu.skinTick(&skin);
    CHECK(gpu.skinValid);
    return gpu.skinCap;
  };
  gpu.skinTable = 1;
  CHECK(capAt(34999) == 14 && capAt(35000) == 13 && capAt(44999) == 13 && capAt(45000) == 12);
  CHECK(capAt(43000) == 12 && capAt(44999) == 12 && capAt(42999) == 13 && capAt(44999) == 13);
  CHECK(capAt(33000) == 13 && capAt(32999) == 14);
  gpu.skinTable = 0;
  CHECK(capAt(24999) == 13 && capAt(22999) == 14);  // the mobile latch tracks samples while docked
  CHECK(capAt(24999) == 14 && capAt(25000) == 13 && capAt(45000) == 9);
  CHECK(capAt(42000) == 9 && capAt(44999) == 9 && capAt(41999) == 13);
  CHECK(capAt(23000) == 13 && capAt(22999) == 14);
  gpu.skinTable = 1;
  CHECK(capAt(55000) == 6);
  gpu.skinTick(nullptr);
  gpu.skinTable = 0;
  gpu.skinTick(nullptr);
  CHECK(!gpu.skinValid && gpu.skinCap == 6);
  CHECK(capAt(30000) == 13);

  CpuLadder cpu;
  const auto cpuCaps = [&](Profile profile, int32_t milliC) {
    skin.virtual_milli_c = milliC;
    cpu.skin(profile, skin);
    return cpu.ceilings(profilePower(kAc, profile));
  };
  constexpr auto normal = Profile::balanced, high = Profile::performance;
  CHECK(cpuCaps(normal, 36999) == kNext);
  CHECK(cpuCaps(normal, 37000) == Frequencies{2400000, 1958400});
  CHECK(cpuCaps(normal, 35000) == Frequencies{2400000, 1958400});
  CHECK(cpuCaps(normal, 34999) == kNext && cpuCaps(normal, 36999) == kNext);
  CHECK(cpuCaps(high, 51000) == kSkinFault && cpuCaps(high, 50000) == kSkinFault);
  CHECK(cpuCaps(high, 49999) == Frequencies{3532800, 4320000});
}

static void dieFaults()
{
  enum class Fault { invalid, sentinel, belowRange, aboveRange, unmapped, nan, replay };
  for (unsigned d = 0; d < 2; ++d) {
    for (auto fault : {Fault::invalid, Fault::sentinel, Fault::belowRange, Fault::aboveRange,
                       Fault::unmapped, Fault::nan, Fault::replay}) {
      DieRegisters die;
      CpuLadder cpu;
      cpu.skin(Profile::performance, boardSkin(boardSnapshot()));
      die.capture(d, 600, 1);
      cpu.thermal(die.poll(), 10);
      CHECK(cpu.ceilings(kAc) == Frequencies{3532800, 4320000});
      auto &reg = die.words[d][kMap[d].cpuFirst];
      switch (fault) {
        case Fault::invalid: reg &= ~kStatusValid; break;
        case Fault::sentinel: reg = DieRegisters::word(-2048); break;  // signed register sentinel
        case Fault::belowRange: reg = DieRegisters::word(-401); break;
        case Fault::aboveRange: reg = DieRegisters::word(1501); break;
        case Fault::unmapped: die.mapped[d] = false; break;
        case Fault::nan: die.capture(d, std::numeric_limits<double>::quiet_NaN(), 2); break;
        case Fault::replay: die.capture(d, 600, 1); break;  // replayed cool sample
      }
      const auto evidence = die.poll();
      CHECK(!evidence[d].valid && !evidence[d].cut && evidence[1 - d].valid);
      CHECK(die.monitor.usable(d, kMap[d].cpuFirst) == die.mapped[d]);  // cache is diagnostic only
      cpu.thermal(evidence, 20);
      CHECK(cpu.ceilings(kAc)[d] == kMinimum[d] && cpu.governor[1 - d].limit() == 15);
      for (unsigned i = 0; i < kStalePolls; ++i) cpu.thermal(die.poll(), 30 + i * 10);
      CHECK(!die.monitor.usable(d, kMap[d].cpuFirst) && cpu.ceilings(kAc)[d] == kMinimum[d]);

      die.mapped[d] = true;
      die.all(850);
      cpu.thermal(die.poll(), 2000);  // equality at release never starts recovery
      CHECK(cpu.governor[d].limit() == 0);
      die.all(849);
      die.capture(d, 849, 3);
      cpu.thermal(die.poll(), 3000);
      die.capture(d, 849, 4);
      cpu.thermal(die.poll(), 3999);
      CHECK(die.monitor.fresh(d, kMap[d].cpuFirst) && cpu.governor[d].limit() == 0);
      cpu.thermal(die.poll(), 4000);
      CHECK(cpu.governor[d].limit() == 1 && cpu.ceilings(kAc)[d] == kLevels[d][1]);
    }
  }
}

static void skinFaults()
{
  constexpr unsigned quiet = adc::channelIndex("quiet");
  enum class Fault { missing, minSentinel, maxSentinel, belowRange, aboveRange, nan, frozen };
  for (auto fault : {Fault::missing, Fault::minSentinel, Fault::maxSentinel, Fault::belowRange,
                     Fault::aboveRange, Fault::nan, Fault::frozen}) {
    auto snapshot = boardSnapshot();
    uint64_t now = kBoardAtUs;
    switch (fault) {
      case Fault::missing: snapshot.validMask &= ~(1u << quiet); break;
      case Fault::minSentinel: snapshot.milliC[quiet] = INT32_MIN; break;
      case Fault::maxSentinel: snapshot.milliC[quiet] = INT32_MAX; break;
      case Fault::belowRange: snapshot.milliC[quiet] = adc::kMinMilliC - 1; break;
      case Fault::aboveRange: snapshot.milliC[quiet] = adc::kMaxMilliC + 1; break;
      case Fault::nan: {  // There is no NaN representation in the ADC snapshot ABI.
        const double value = std::numeric_limits<double>::quiet_NaN();
        if (inRange(value, adc::kMinMilliC, adc::kMaxMilliC)) snapshot.milliC[quiet] = int32_t(value);
        else snapshot.validMask &= ~(1u << quiet);
        break;
      }
      case Fault::frozen:  // All channels except quiet have a new timestamp. Quiet's frozen value ages out.
        now += (uint64_t(snapshot.periodMs) * adc::kFreshPeriods + 1) * 1000;
        for (unsigned i = 0; i < adc::kChannelCount; ++i)
          if (i != quiet) snapshot.atUs[i] = now;
        CHECK(boardSkin(snapshot, now - 1000).virtual_valid == 0);  // future-dated peers are unusable
        snapshot.atUs[quiet] += 1000;
        CHECK(boardSkin(snapshot, now).virtual_valid);
        snapshot.atUs[quiet] -= 1000;
        break;
    }
    const auto faultSkin = boardSkin(snapshot, now);
    CHECK(!faultSkin.virtual_valid);
    A830R3bThermal gpu;
    gpu.skinTable = 1;
    const auto gpuFault = boardSkin(snapshot, now, 2);
    gpu.skinTick(&gpuFault);
    CHECK(!gpu.skinValid && gpu.skinCap == 9);
    snapshot = boardSnapshot();
    for (auto &at : snapshot.atUs) at = now;
    const auto recovered = boardSkin(snapshot, now);
    for (const Profile p : {Profile::saver, Profile::balanced, Profile::performance}) {
      CpuLadder cpu;
      const auto power = profilePower(kAc, p);
      cpu.skin(p, boardSkin(boardSnapshot()));
      CHECK(cpu.ceilings(power) != kSkinFault);
      cpu.skin(p, faultSkin);
      CHECK(cpu.ceilings(power) == kSkinFault);
      perf::Burst burst;
      CHECK(!burst.tick({.khz = kNext[0]}, 10, true, true, faultSkin.virtual_valid, faultSkin.virtual_milli_c));
      cpu.skin(p, recovered);
      CHECK(cpu.ceilings(power) != kSkinFault);
    }
    gpu.skinTick(&recovered);
    CHECK(gpu.skinValid && gpu.skinCap == 14);
  }
  // Exercise the signed weighted sum at the valid edges as well as rejecting INT32 extremes above.
  const auto cold = boardSkin(boardSnapshot(adc::kMinMilliC));
  const auto hot = boardSkin(boardSnapshot(adc::kMaxMilliC));
  CHECK(cold.virtual_valid && cold.virtual_milli_c == -36120);
  CHECK(hot.virtual_valid && hot.virtual_milli_c == 119475);
}

// A two-domain SCMI endpoint. Keys deliberately differ from sorted ladder offsets.
struct ScmiFirmware {
  uint32_t words[scmi::kShmemBytes / 4]{};
  scmi::Domain domains[2]{};
  uint32_t actualKey[2]{}, deniedKey[2]{}, successfulSets[2]{};
  uint64_t now = 0;
  scmi::Channel channel;

  ScmiFirmware()
  {
    words[scmi::kChanStatus / 4] = scmi::kStatusFree;
    for (unsigned d = 0; d < 2; ++d) {
      auto &dom = domains[d];
      dom.indexing = dom.canSet = true;
      dom.sustained = kSustained[d];
      for (unsigned i = 0; i < std::size(kLevels[d]); ++i)
        scmi::addLevel(dom, {100 * (d + 1) + 2 * i, 0, kLevels[d][i], 0, 0});
      actualKey[d] = dom.levels[scmi::fastestAtMost(dom, kBase[d])].key;
    }
    channel.io = {
        this,
        [](void *ctx, uint32_t reg) { return static_cast<ScmiFirmware *>(ctx)->words[reg / 4]; },
        [](void *ctx, uint32_t reg, uint32_t val) { static_cast<ScmiFirmware *>(ctx)->words[reg / 4] = val; },
        [](void *ctx) { static_cast<ScmiFirmware *>(ctx)->reply(); },
        [](void *ctx) { return static_cast<ScmiFirmware *>(ctx)->now; },
        [](void *ctx, uint32_t us) { static_cast<ScmiFirmware *>(ctx)->now += us; },
    };
  }

  void reply()
  {
    const auto header = scmi::decodeHeader(words[scmi::kHeader / 4]);
    const unsigned d = words[scmi::kPayload / 4];
    CHECK(d < 2 && header.protocol == scmi::kProtocolPerf);
    auto &value = words[scmi::kPayload / 4 + 1];
    auto &length = words[scmi::kLength / 4];
    uint32_t status = 0;
    if (header.message == scmi::kLevelGet) {
      CHECK(length == 8);
      value = actualKey[d];
      length = 12;
    } else {
      CHECK(header.message == scmi::kLevelSet && length == 12);
      CHECK(scmi::indexOfKey(domains[d], value) >= 0);
      if (value == deniedKey[d]) status = uint32_t(-3);
      else {
        actualKey[d] = value;
        ++successfulSets[d];
      }
      length = 8;
    }
    words[scmi::kPayload / 4] = status;
    words[scmi::kChanStatus / 4] = scmi::kStatusFree;
  }

  unsigned actual(unsigned d)
  {
    int index = -1;
    CHECK(scmi::levelGet(channel, domains[d], d, index) == 0 && index >= 0);
    return unsigned(index);
  }

  Frequencies frequencies() { return {domains[0].levels[actual(0)].khz, domains[1].levels[actual(1)].khz}; }
};

struct AndroidSession {
  static constexpr uint64_t kCoolDwellMs = 1000;
  CpuLadder cpu{true};
  ScmiFirmware fw;
  EnergyRoute route;
  bool requested = false, active = false;
  unsigned savedMode = 0;
  int32_t refusal = 0;
  std::optional<uint64_t> coolSince;

  explicit AndroidSession(unsigned mode)
  {
    route.powermode(mode);
    for (unsigned d = 0; d < 2; ++d) cpu.governor[d].setLevel(fw.actual(d));
  }

  void begin()
  {
    savedMode = route.inputs.lpm ? 1 : route.inputs.hpm ? 2 : 0;
    route.powermode(0);  // A host qualification session temporarily leaves Saver to exercise the opt-in.
    requested = true;
  }

  int32_t apply(unsigned d, unsigned want)
  {
    auto &g = cpu.governor[d];
    const unsigned last = fw.actual(d);
    if (want == last) return 0;
    const int32_t status = scmi::levelSet(fw.channel, fw.domains[d], d, want);
    g.setLevel(status ? last : want);
    return status;
  }

  void stop()
  {
    requested = active = false;
    coolSince.reset();
    route.powermode(savedMode);
    const auto power = profilePower(kAc, route.profile);
    for (unsigned d = 0; d < 2; ++d) {
      auto &g = cpu.governor[d];
      g.selectAndroidCap(0);
      // A refused upward vote leaves the downward path available in this fixture.
      CHECK(apply(d, g.enforce(power)) == 0);
    }
  }

  void poll(uint64_t now, const DieReadings &die, const piano_skin_temps &skin, bool bootBoost = false)
  {
    cpu.skin(route.profile, skin);
    cpu.thermal(die, now);
    const bool usable = die[0].valid && die[1].valid && !die[0].cut && !die[1].cut;
    bool domains = true;
    for (unsigned d = 0; d < 2; ++d) {
      const auto &dom = fw.domains[d];
      domains &= dom.canSet && dom.levels[scmi::fastestAtMost(dom, kBase[d])].khz == kBase[d] &&
                 dom.levels[scmi::fastestAtMost(dom, kNext[d])].khz == kNext[d];
    }
    const AndroidCpuInput input{androidCpuArmed(CpuMode::govern, 2400, 2438), requested, domains, usable,
                                bootBoost, false, kAc, route.profile, androidCpuLpmValue(32, route.inputs.lpm)};
    const auto selection = androidCpuSelection(input);
    const bool eligible = selection.enabled && skin.virtual_valid;
    const bool skinAllowsNext = (!cpu.skinKhz[0] || cpu.skinKhz[0] >= kNext[0]) &&
                                (!cpu.skinKhz[1] || cpu.skinKhz[1] >= kNext[1]);
    const bool cool = eligible && die[0].releaseExcessDeci < 0 && die[1].releaseExcessDeci < 0 &&
                      skinAllowsNext;
    if (!cool) coolSince.reset();
    else if (!coolSince) coolSince = now;
    active = cool && now - *coolSince >= kCoolDwellMs;
    for (unsigned d = 0; d < 2; ++d) {
      auto &g = cpu.governor[d];
      g.selectAndroidCap(active ? kAndroidNextKhz[d] : 0);
      const unsigned want = g.tick({1000, selection.power, 4320000});
      if (const int32_t status = apply(d, want)) {
        refusal = status;
        stop();
        break;
      }
    }
  }
};

static void androidSafety()
{
  DieRegisters die;
  const auto cool = boardSkin(boardSnapshot());
  const auto warm = boardSkin(boardSnapshot(40000));
  AndroidSession session(0);
  CHECK(kAndroidBaseKhz[0] == kBase[0] && kAndroidBaseKhz[1] == kBase[1]);
  CHECK(kAndroidNextKhz[0] == kNext[0] && kAndroidNextKhz[1] == kNext[1]);
  for (uint64_t now : {0, 1000, 2000}) {
    session.poll(now, die.poll(), cool);
    CHECK(!session.active && session.cpu.ceilings(kAc) == kBase && session.fw.frequencies() == kBase);
  }
  session.begin();
  session.fw.domains[1].canSet = false;
  session.poll(2100, die.poll(), cool);
  session.poll(3100, die.poll(), cool);
  CHECK(!session.active && session.fw.frequencies() == kBase);
  session.fw.domains[1].canSet = true;
  session.poll(3200, die.poll(), cool, true);
  CHECK(!session.active && session.fw.frequencies() == kBase);
  session.poll(3990, die.poll(), warm);
  CHECK(!session.active && !session.coolSince);
  session.poll(4000, die.poll(), cool);
  die.all(850);  // A sample at the release point interrupts the cool dwell.
  session.poll(4999, die.poll(), cool);
  CHECK(!session.active && session.fw.frequencies() == kBase);
  die.all(600);
  session.poll(5000, die.poll(), cool);
  session.poll(5999, die.poll(), cool);
  CHECK(!session.active && session.fw.frequencies() == kBase);
  session.poll(6000, die.poll(), cool);
  CHECK(session.active && session.fw.frequencies() == kNext);
  session.poll(6010, die.poll(), warm);
  CHECK(session.requested && !session.active && session.fw.frequencies() == Frequencies{1996800, 1958400});
  session.poll(6020, die.poll(), cool);
  CHECK(!session.active && session.fw.frequencies() == kBase);
  session.poll(7020, die.poll(), cool);
  CHECK(session.active && session.fw.frequencies() == kNext);

  die.all(900);
  for (unsigned step = 1; step <= 9; ++step) {
    const uint64_t now = 7020 + step * 10;
    session.poll(now, die.poll(), cool);
    for (const auto &g : session.cpu.governor) CHECK(g.limit() == 15 - step);
    session.poll(now + 9, die.poll(), cool);
    for (const auto &g : session.cpu.governor) CHECK(g.limit() == 15 - step);
  }
  CHECK(session.requested && !session.active && session.fw.frequencies() == Frequencies{1555200, 2438400});
  session.stop();
  CHECK(!session.active && !session.route.inputs.lpm && session.fw.frequencies() == Frequencies{1555200, 2438400});

  die.all(600);
  AndroidSession normal(0);
  normal.begin();
  normal.poll(100, die.poll(), cool);
  normal.poll(1100, die.poll(), cool);
  CHECK(normal.fw.frequencies() == kNext);
  normal.stop();
  CHECK(!normal.requested && !normal.route.inputs.lpm && normal.fw.frequencies() == kBase);

  normal.begin();
  normal.poll(2000, die.poll(), cool);
  normal.poll(3000, die.poll(), cool);
  CHECK(normal.active && normal.fw.frequencies() == kNext);
  die.words[0][kMap[0].cpuFirst] = 0;
  normal.poll(3010, die.poll(), cool);
  CHECK(!normal.active && normal.fw.frequencies() == Frequencies{kMinimum[0], kBase[1]});
  die.all(600);

  for (bool deny : {false, true}) {
    AndroidSession saver(1);
    CHECK(saver.route.inputs.lpm);
    saver.begin();
    CHECK(!saver.route.inputs.lpm);
    if (deny) saver.fw.deniedKey[1] = saver.fw.domains[1].levels[saver.cpu.governor[1].fastestAtMost(kNext[1])].key;
    saver.poll(100, die.poll(), cool);
    saver.poll(1100, die.poll(), cool);
    if (deny) {
      CHECK(saver.refusal == -3 && saver.fw.channel.lastError == 0 && saver.fw.channel.failures == 1);
      CHECK(saver.fw.successfulSets[0] == 2 && saver.fw.successfulSets[1] == 1);  // first cluster rose, then rolled back
    } else {
      CHECK(saver.fw.frequencies() == kNext);
      saver.stop();
    }
    CHECK(!saver.requested && !saver.active && saver.route.inputs.lpm && saver.route.profile == Profile::saver);
    CHECK(saver.cpu.ceilings(kAc) == kBase && saver.fw.frequencies() == kSaver);
    saver.poll(1110, die.poll(), cool);
    CHECK(saver.fw.frequencies() == kSaver);
  }
}

int main()
{
  energyModes();
  skinBands();
  dieFaults();
  skinFaults();
  androidSafety();
  std::puts("thermal_cpu_lpm_model_host: all passed (unverified high-OPP profiles HOLD)");
}
