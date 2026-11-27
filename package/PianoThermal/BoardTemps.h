// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6: the snapshot of a round that the ADC thread publishes, and package/include/PianoBoardTemps.h's answer built from
// it (which channel is which role, validity, age). Portable: the kext (PianoThermalAdc.cpp, which adds the sequence count) and
// tests/adc_host.cpp include it. docs/drivers/thermal.md section 12.
#ifndef PIANO_THERMAL_BOARD_TEMPS_H
#define PIANO_THERMAL_BOARD_TEMPS_H
#include <stdint.h>
#include "PianoBoardTemps.h"
#include "Perf.h"
#include "PmicAdc.h"

namespace pianothermal::adc {

constexpr uint32_t kFreshPeriods = 3;   // a reading counts for three periods (a round may be late by a slow conversion or two)

struct Snapshot {
  uint32_t state = PIANO_BOARD_TEMPS_OFF, rounds = 0, periodMs = 0, validMask = 0, disabledMask = 0;
  uint32_t pinHeldMask = 0;
  int32_t milliC[kChannelCount] = {};
  uint64_t atUs[kChannelCount] = {};
};

inline Snapshot snapshotOf(const Board &b, uint32_t periodMs)
{
  Snapshot s;
  s.state = b.adc.stopped != Err::none ? PIANO_BOARD_TEMPS_STOPPED : b.rounds ? PIANO_BOARD_TEMPS_RUNNING : PIANO_BOARD_TEMPS_STARTING;
  s.rounds = b.rounds;
  s.periodMs = periodMs;
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &c = b.ch[i];
    if (c.valid) {
      s.validMask |= 1u << i;
      s.milliC[i] = c.last.milliC;
      s.atUs[i] = c.atUs;
    } else if (c.enabled && c.pinHeld && c.pin.err == Err::none) {
      s.pinHeldMask |= 1u << i;
      s.atUs[i] = c.atUs;
    }
    if (!c.enabled) s.disabledMask |= 1u << i;
  }
  return s;
}

inline uint64_t sampleAgeMs(const Snapshot &s, unsigned i, uint64_t nowUs)
{
  return s.atUs[i] && nowUs >= s.atUs[i] ? (nowUs - s.atUs[i]) / 1000 : UINT64_MAX;
}

inline bool usableSample(const Snapshot &s, unsigned i, uint64_t ageMs, uint32_t freshPeriods)
{
  const uint32_t bit = 1u << i;
  return s.state == PIANO_BOARD_TEMPS_RUNNING && s.periodMs && (s.validMask & bit) && !(s.disabledMask & bit) &&
         s.milliC[i] >= kMinMilliC && s.milliC[i] <= kMaxMilliC && ageMs <= uint64_t(s.periodMs) * freshPeriods;
}

// The answer for the charging roles (PIANO_BOARD_TEMP_BATTERY.. in Role order). Returns the valid count, -1 for a bad argument.
inline int fillBoardTemps(const Snapshot &s, uint64_t nowUs, piano_board_temps *t)
{
  if (!t || t->version != PIANO_BOARD_TEMPS_VERSION) return -1;
  t->state = s.state;
  t->valid = t->disabled = 0;
  t->period_ms = s.periodMs;
  t->rounds = s.rounds;
  int n = 0;
  for (unsigned r = 0; r < PIANO_BOARD_TEMP_COUNT; ++r) {
    t->milli_c[r] = 0;
    t->age_ms[r] = 0;
    const int channel = roleIndex(Role(r));
    if (channel < 0) continue;
    const unsigned i = unsigned(channel);
    if (s.disabledMask & (1u << i)) t->disabled |= 1u << r;
    const uint64_t ageMs = sampleAgeMs(s, i, nowUs);
    if (!usableSample(s, i, ageMs, kFreshPeriods)) continue;
    t->valid |= 1u << r;
    t->milli_c[r] = s.milliC[i];
    t->age_ms[r] = uint32_t(ageMs);
    ++n;
  }
  return n;
}

inline bool fillGpuTemps(const Snapshot &s, uint64_t nowUs, piano_gpu_temps *t)
{
  if (!t || t->version != PIANO_GPU_TEMPS_VERSION) return false;
  t->state = s.state; t->valid = t->disabled = 0; t->period_ms = s.periodMs; t->rounds = s.rounds;
  const int channels[2] = {channelIndex("msm"), channelIndex("quiet")};
  for (unsigned r = 0; r < 2; ++r) {
    t->milli_c[r] = 0; t->age_ms[r] = 0; t->sample_us[r] = 0;
    const int channel = channels[r];
    if (channel < 0) continue;
    const unsigned i = unsigned(channel);
    if (s.disabledMask & (1u << i)) t->disabled |= 1u << r;
    if (!(s.validMask & (1u << i)) || s.state != PIANO_BOARD_TEMPS_RUNNING) continue;
    const uint64_t age = sampleAgeMs(s, i, nowUs);
    t->sample_us[r] = s.atUs[i]; t->age_ms[r] = age > UINT32_MAX ? UINT32_MAX : uint32_t(age);
    t->milli_c[r] = s.milliC[i];
    if (usableSample(s, i, age, 2)) t->valid |= 1u << r;
  }
  return true;
}

// gpu/fullopp1: the virtual skin sensor's inputs and the XO thermistor (PIANO_SKIN_TEMPS_VERSION), the same freshness rule as
// fillGpuTemps (a reading counts for two periods).
inline bool fillSkinTemps(const Snapshot &s, uint64_t nowUs, piano_skin_temps *t, uint32_t freshPeriods = 2)
{
  if (!t || t->version != PIANO_SKIN_TEMPS_VERSION) return false;
  t->state = s.state; t->valid = t->disabled = 0; t->period_ms = s.periodMs; t->rounds = s.rounds;
  const int channels[PIANO_SKIN_COUNT] = {channelIndex("batt"), channelIndex("quiet"), channelIndex("charger"), channelIndex("wifi"),
                                          channelIndex("msm"), channelIndex("xo")};
  for (unsigned r = 0; r < PIANO_SKIN_COUNT; ++r) {
    t->milli_c[r] = 0; t->age_ms[r] = 0; t->sample_us[r] = 0;
    const int channel = channels[r];
    if (channel < 0) continue;
    const unsigned i = unsigned(channel);
    if (s.disabledMask & (1u << i)) t->disabled |= 1u << r;
    if (!(s.validMask & (1u << i)) || s.state != PIANO_BOARD_TEMPS_RUNNING) continue;
    const uint64_t age = sampleAgeMs(s, i, nowUs);
    t->sample_us[r] = s.atUs[i]; t->age_ms[r] = age > UINT32_MAX ? UINT32_MAX : uint32_t(age);
    t->milli_c[r] = s.milliC[i];
    if (usableSample(s, i, age, freshPeriods)) t->valid |= 1u << r;
  }
  const unsigned charger = unsigned(channels[PIANO_SKIN_CHARGER]);
  const bool pinHeld = (s.pinHeldMask & ~s.disabledMask & (1u << charger)) &&
                       sampleAgeMs(s, charger, nowUs) <= uint64_t(s.periodMs) * freshPeriods;
  const perf::Skin skin = perf::virtualSkin(t->milli_c, t->valid, pinHeld);
  t->virtual_milli_c = skin.milliC;
  t->virtual_valid = skin.valid;
  t->virtual_used = skin.used;
  return true;
}

static_assert(int(Role::battery) == PIANO_BOARD_TEMP_BATTERY && int(Role::charger) == PIANO_BOARD_TEMP_CHARGER &&
                  int(Role::connector) == PIANO_BOARD_TEMP_CONNECTOR && int(Role::connector2) == PIANO_BOARD_TEMP_CONNECTOR2,
              "the API's indices are the roles'");
static_assert(int(PIANO_SKIN_BATTERY) == int(perf::kSkinBattery) && int(PIANO_SKIN_QUIET) == int(perf::kSkinQuiet) &&
                  int(PIANO_SKIN_CHARGER) == int(perf::kSkinCharger) && int(PIANO_SKIN_WIFI) == int(perf::kSkinWifi) &&
                  int(PIANO_SKIN_MSM) == int(perf::kSkinMsm),
              "the skin API's first five are the virtual sensor's inputs in its order");

}  // namespace pianothermal::adc
#endif
