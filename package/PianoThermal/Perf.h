// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: the stock-level CPU policy of piano-cpufreq=3 (2026-11-22, the owner's "performance first" priority), selected at
// run time by machdep.piano_cpufreq_perf (boot default piano-cpufreq-perf). Portable; host-tested by tests/thermal_host.cpp.
//
// What the stock Android of this tablet does (numbers as behaviour; docs/drivers/thermal.md section 14):
//   - a virtual skin temperature: 0.500 battery + 0.131 quiet_therm - 0.009 charger1_therm + 0.089 wifi_therm + 0.232 cpu_therm
//     (the msm thermistor) + 1.6 C, polled every 2 s;
//   - per cluster a step table on that temperature (the "normal" scene): from 15 C cpu0-5 at most 2745.6 MHz and cpu6-7 at most
//     3072 MHz, lower steps as the skin warms (37 C: 2400 / 1958.4 ...; 48 C: 748.8, 44 C: 1017.6), each step with its own clear
//     point (hysteresis);
//   - the UI boosts (app launch / animation hints): cpu0-5 at 2745.6 and cpu6-7 at 4089.6 MHz for 0.4-1.5 s, allowed until the skin
//     reaches 48 C.
// Here: virtualSkin() over the PMIC ADC's thermistors (PianoThermalAdc), SkinCap the step table with its hysteresis, Burst the boost:
// macOS gives the kernel no launch/animation hint, so a burst opens at the ONSET of a load (the governor wanted more than its
// ceiling on this tick but not on the tick before), lasts at most windowMs, and the next one needs another onset and gapMs since the
// last end: a sustained load stays at the step table's cap, as on Android without hints.
#ifndef PIANO_THERMAL_PERF_H
#define PIANO_THERMAL_PERF_H
#include <stdint.h>

namespace pianothermal::perf {

// The virtual sensor's inputs, in the stock order.
enum SkinInput : unsigned { kSkinBattery = 0, kSkinQuiet, kSkinCharger, kSkinWifi, kSkinMsm, kSkinInputs };
constexpr int32_t kSkinWeight[kSkinInputs] = {500, 131, -9, 89, 232};   // per mille of the stock weight_sum 1000 (they add to 943)
constexpr int32_t kSkinWeightSum = 1000, kSkinWeightAll = 943;
constexpr int32_t kSkinOffsetMilli = 1600;
constexpr const char *kSkinChannel[kSkinInputs] = {"batt", "quiet", "charger", "wifi", "msm"};   // AdcChannels.h names
constexpr uint32_t kSkinMask = (1u << kSkinInputs) - 1;

struct Skin {
  bool valid = false;
  int32_t milliC = 0;
  uint32_t used = 0;   // bit i: input i was in the sum
};

// The stock sum: sum(weight x reading) / 1000 + 1.6 C. Omit the charger input only when the DSP is known to be driving its
// pin. Scale the remaining inputs to the stock weight in that case.
inline Skin virtualSkin(const int32_t milliC[kSkinInputs], uint32_t validMask, bool chargerPinHeld = false)
{
  Skin s;
  const uint32_t required = chargerPinHeld ? kSkinMask & ~(1u << kSkinCharger) : kSkinMask;
  if ((validMask & required) != required) return s;
  int64_t sum = 0, w = 0;
  for (unsigned i = 0; i < kSkinInputs; ++i) {
    if (!(required & (1u << i))) continue;
    sum += int64_t(kSkinWeight[i]) * milliC[i];
    w += kSkinWeight[i];
    s.used |= 1u << i;
  }
  if (w <= 0) return Skin();
  s.milliC = int32_t(sum * kSkinWeightAll / (w * kSkinWeightSum)) + kSkinOffsetMilli;
  s.valid = true;
  return s;
}

struct Step {
  int32_t trigMilli, clrMilli;
  uint32_t khz;
};
// The normal scene's tables (trigger, clear, the cap at and above the trigger).
constexpr Step kCluster0[] = {{15000, 13000, 2745600}, {35000, 32000, 2745600}, {37000, 35000, 2400000}, {39000, 37000, 1996800},
                              {41000, 39000, 1555200}, {43000, 41000, 1555200}, {44000, 43000, 1555200}, {45000, 44000, 1363200},
                              {46000, 45000, 960000},  {48000, 46000, 748800}};
constexpr Step kCluster1[] = {{15000, 13000, 3072000}, {34000, 32000, 2649600}, {37000, 35000, 1958400}, {39000, 38000, 1958400},
                              {39500, 39000, 1689600}, {40000, 39500, 1401600}, {44000, 42000, 1017600}};
constexpr unsigned kCluster0Steps = sizeof kCluster0 / sizeof kCluster0[0], kCluster1Steps = sizeof kCluster1 / sizeof kCluster1[0];
inline uint32_t skinFaultCap(unsigned cluster) { return cluster ? kCluster1[kCluster1Steps - 1].khz : kCluster0[kCluster0Steps - 1].khz; }

// One cluster's step: -1 below the first trigger (no cap), else the index of the step in force. Up as far as the triggers allow,
// down only under the clear point of the step in force. A sensor fault applies the lowest cap without discarding hysteresis.
class SkinCap {
 public:
  uint32_t update(const Step *t, unsigned n, bool valid, int32_t milliC)
  {
    if (!n) return 0;
    if (step_ >= int(n)) reset();
    if (!valid) return t[n - 1].khz;
    while (step_ + 1 < int(n) && milliC >= t[step_ + 1].trigMilli) ++step_;
    while (step_ >= 0 && milliC < t[step_].clrMilli) --step_;
    return step_ < 0 ? 0 : t[step_].khz;
  }
  int step() const { return step_; }
  void reset() { step_ = -1; }

 private:
  int step_ = -1;
};

struct BurstConfig {
  uint32_t khz = 0;              // the burst's cap (0: no burst)
  uint32_t windowMs = 1500;      // the longest burst (the stock first-launch boost)
  uint32_t gapMs = 500;          // from the end of one burst to the next onset that may open one
  int32_t limitMilli = 48000;    // no burst from this skin temperature up (the stock boost limit)
};

class Burst {
 public:
  // One load decision at nowMs: wantsMore = the governor wanted more than its ceiling without the burst (its last decision);
  // allowed = the profile allows bursts (Balanced) and the cluster governs. True while the burst is open.
  bool tick(const BurstConfig &c, uint64_t nowMs, bool wantsMore, bool allowed, bool skinValid, int32_t skinMilli)
  {
    const bool onset = wantsMore && !wanted_;
    wanted_ = wantsMore;
    const bool eligible = allowed && skinValid && skinMilli < c.limitMilli && c.khz && c.windowMs;
    if (open_ && (!eligible || nowMs - startMs_ >= c.windowMs || !wantsMore)) close(nowMs);
    if (!open_ && onset && eligible && (!ended_ || nowMs - endMs_ >= c.gapMs)) {
      open_ = true;
      startMs_ = nowMs;
      ++windows_;
    }
    return open_;
  }
  bool open() const { return open_; }
  uint32_t windows() const { return windows_; }
  uint64_t openMs(uint64_t nowMs) const { return totalMs_ + (open_ ? nowMs - startMs_ : 0); }

 private:
  void close(uint64_t nowMs)
  {
    open_ = false;
    ended_ = true;
    endMs_ = nowMs;
    totalMs_ += nowMs - startMs_;
  }
  bool open_ = false, wanted_ = false, ended_ = false;
  uint64_t startMs_ = 0, endMs_ = 0, totalMs_ = 0;
  uint32_t windows_ = 0;
};

// The perf cap handed to the Governor (Governor::setRuntimeCaps): the step table's, raised to the burst's while one is open; 0 (no
// cap) only when the skin is below the first trigger and no burst is open.
inline uint32_t perfCapKhz(uint32_t skinKhz, bool burstOpen, uint32_t burstKhz)
{
  if (!burstOpen || !burstKhz) return skinKhz;
  if (!skinKhz) return 0;
  return burstKhz > skinKhz ? burstKhz : skinKhz;
}

// Load decisions between downs for a down hold of downMs at tickMs per decision (at least 1).
inline uint32_t downTicksFor(uint32_t downMs, uint32_t tickMs)
{
  const uint32_t n = tickMs ? (downMs + tickMs - 1) / tickMs : 1;
  return n ? n : 1;
}

// ------------------------------------------------------------------------------------------------- the launch and input boosts
// (owner 2026-11-22 01:02: short boosts as on Android; CPU-PERF.md section 7). The stock perf HAL's hints on this SoC
// (perfboostsconfig.xml, Target sun): first app launch 0x1081 = cpu0-5 min = max 2745.6 MHz, cpu6-7 min = max 4089.6 MHz for
// 1.5 s, sched_boost 1, GPU min_pwrlevel 9 (443 MHz) .. max_pwrlevel 2 (967 MHz), kgsl touch_wake; next launch 0x1082 the same
// for 0.4 s; animation 0x1083 the CPU pin for 0.4 s; scroll / drag 0x1080 / 0x1087 sched_boost 2 and WALT adaptive floors
// (0.6-0.8 / 1.0-1.3 GHz) while the gesture lasts. macOS gives the kernel no such hints, so the events are our own: an exec of an
// app bundle's executable (a kauth file-operation listener) for the launch, and any HID report our input kexts send
// (PianoPerfInput.h) for the input boost.

// The executable of an app bundle: ".app/Contents/MacOS/" in the path, and not an XPC service or a framework's helper.
inline bool isAppLaunchPath(const char *p)
{
  if (!p) return false;
  auto has = [p](const char *needle) {
    for (const char *s = p; *s; ++s) {
      const char *a = s, *b = needle;
      while (*a && *b && *a == *b) ++a, ++b;
      if (!*b) return true;
    }
    return false;
  };
  return has(".app/Contents/MacOS/") && !has("/XPCServices/") && !has(".framework/");
}

// The short name of the app (the bundle's last path component without ".app") into out, for the log.
inline void appName(const char *p, char *out, unsigned size)
{
  if (!size) return;
  out[0] = 0;
  if (!p) return;
  const char *end = nullptr;
  for (const char *s = p; *s; ++s)
    if (s[0] == '.' && s[1] == 'a' && s[2] == 'p' && s[3] == 'p' && s[4] == '/') end = s;
  if (!end) return;
  const char *start = end;
  while (start > p && start[-1] != '/') --start;
  unsigned n = 0;
  while (start < end && n + 1 < size) out[n++] = *start++;
  out[n] = 0;
}

enum class BoostKind : uint8_t { none = 0, input = 1, launch = 2 };
inline const char *boostName(BoostKind k) { return k == BoostKind::launch ? "launch" : k == BoostKind::input ? "input" : "none"; }

// Which boost holds at nowNs: a launch within launchMs of its exec, else an input event within inputMs of the last one. An event
// stamped a little after nowNs (written by another thread after this one read the clock) counts as now.
inline BoostKind boostAt(uint64_t nowNs, uint64_t launchNs, uint32_t launchMs, uint64_t inputNs, uint32_t inputMs)
{
  auto within = [nowNs](uint64_t at, uint32_t ms) {
    if (!at || !ms) return false;
    if (at >= nowNs) return true;
    return nowNs - at < uint64_t(ms) * 1000000ull;
  };
  if (within(launchNs, launchMs)) return BoostKind::launch;
  if (within(inputNs, inputMs)) return BoostKind::input;
  return BoostKind::none;
}

// High Power (Profile::performance, macOS Energy Mode): the perf cap is the cluster's top level (piano-cpufreq-hpm0/1, default the
// tables' tops 3532.8 / 4320 MHz), over the firmware's sustained level unless the firmware refused such a level this boot; no skin
// table, no onset burst (the ceiling is already the top), the boosts as in Balanced; the hard caps, the power cap and the thermal
// limit (piano-cpufreq-hot) stay on top in Governor::ceilingFor.
struct HighPowerCap {
  uint32_t khz;
  bool aboveSustained;
};
inline HighPowerCap highPowerCap(uint32_t topKhz, bool aboveRefused) { return HighPowerCap{topKhz, !aboveRefused}; }

// ------------------------------------------------------------------------------------------------- the full table (fullopp)
// (owner 2026-11-22 22:43: Android's full CPU table and thermal policy, as the GPU's fullopp line). piano-cpufreq-fullopp=1, at run
// time machdep.piano_cpufreq_fullopp (0/1, the same-boot A/B against the policy above). Numbers as behaviour, from the stock
// Android of this tablet (docs/drivers/thermal.md section 15):
//   - the table: the firmware's 16 + 16 levels (NCC0 384..3532.8, NCC1 1017.6..4320 MHz; 4204.8 / 4320 above NCC1's sustained
//     4089.6), all of them allowed; the thermal daemon's scene decides how much of it a given temperature gets;
//   - the scenes (the CPU rows of the stock thermal daemon's scene files): "normal", the everyday one, is the step table above
//     (kCluster0 / kCluster1, boosts until 48 C); "nolimits" has one step per cluster, 51 C (clear 50 C) -> 748.8 / 1017.6 MHz,
//     boosts until 51 C, i.e. the whole table below 51 C. macOS's Energy Mode picks the scene here: High Power = nolimits,
//     Automatic and Low Power = normal (Low Power keeps the Power Saver caps on top);
//   - in every scene the battery-percentage limit (reverse: at or under the trigger): <= 3 % -> 1785.6 / 1401.6 MHz, <= 1 % ->
//     1363.2 / 1401.6 MHz, cleared at 4 % / 2 %;
//   - the kernel's silicon rule: the cpu zones' passive trip 95 C with 8 C hysteresis, step_wise on the cluster's cpufreq cooling
//     (PianoThermal.cpp: piano-cpufreq-hot=95 and the release point piano-cpufreq-fullopp-release, 87 C); no temperature gate on
//     bursts and boosts beyond the thermal limit itself (piano-cpufreq-fullopp-gate=0).
enum class Scene : uint8_t { normal = 0, nolimits = 1 };
constexpr Step kNolimits0[] = {{51000, 50000, 748800}};
constexpr Step kNolimits1[] = {{51000, 50000, 1017600}};
struct SceneTable {
  const Step *steps[2];
  unsigned count[2];
  int32_t boostLimitMilli;   // no burst or boost from this virtual skin temperature up (the scene's boost_limit)
  const char *name;
};
inline SceneTable sceneTable(Scene s)
{
  if (s == Scene::nolimits) return SceneTable{{kNolimits0, kNolimits1}, {1, 1}, 51000, "nolimits"};
  return SceneTable{{kCluster0, kCluster1}, {kCluster0Steps, kCluster1Steps}, 48000, "normal"};
}
inline Scene sceneFor(bool highPower) { return highPower ? Scene::nolimits : Scene::normal; }

// The battery-percentage limit: level 0 none, 1 (<= 3 %), 2 (<= 1 %); up at the trigger, down only at the clear point.
// soc < 0 (no reading) keeps the level.
constexpr int32_t kSocTrig[3] = {0, 3, 1}, kSocClr[3] = {0, 4, 2};
constexpr uint32_t kSocKhz[3][2] = {{0, 0}, {1785600, 1401600}, {1363200, 1401600}};
class SocLimit {
 public:
  unsigned update(int32_t soc)
  {
    if (soc < 0) return level_;
    while (level_ < 2 && soc <= kSocTrig[level_ + 1]) ++level_;
    while (level_ > 0 && soc >= kSocClr[level_]) --level_;
    return level_;
  }
  uint32_t khz(unsigned cluster) const { return cluster < 2 ? kSocKhz[level_][cluster] : 0; }
  unsigned level() const { return level_; }

 private:
  unsigned level_ = 0;
};

// The cap of the full table's scene for one cluster: the scene step's (0: none), the battery limit's on top (the lower one wins).
inline uint32_t fullCapKhz(uint32_t sceneKhz, uint32_t socKhz)
{
  if (!socKhz) return sceneKhz;
  if (!sceneKhz) return socKhz;
  return socKhz < sceneKhz ? socKhz : sceneKhz;
}

// WALT-like demand (piano-cpufreq-walt, machdep.piano_cpufreq_walt; the stock governor is walt, its behaviour from its public
// design): per load window a core's work is its busy share times the cluster's frequency in that window (frequency-invariant,
// kHz-equivalent), a history of kWaltHist windows is kept, and the demand the governor sees is the larger of the latest window and
// the history's mean (WALT's default window statistic, "max of recent and average"). The cluster's demand is the max over its cores;
// the governor then wants demand / target (80 %: the 1.25 headroom), hispeed at >= 90 % busy, ups at once, downs held.
constexpr unsigned kWaltHist = 5;
inline uint32_t workKhz(uint32_t busyPermille, uint32_t khz) { return uint32_t(uint64_t(khz) * busyPermille / 1000u); }
struct WaltCore {
  uint32_t hist[kWaltHist] = {};
  uint8_t n = 0, at = 0;
  uint32_t push(uint32_t work)
  {
    hist[at] = work;
    at = uint8_t((at + 1) % kWaltHist);
    if (n < kWaltHist) ++n;
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; ++i) sum += hist[i];
    const uint32_t mean = uint32_t(sum / n);
    return work > mean ? work : mean;
  }
  void reset() { n = at = 0; }
};

}  // namespace pianothermal::perf

#endif
