// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU's Android-curve preset (gpu/throttle1, owner 2026-11-24 05:45: the full Android curve is the kext default, not only the ESP's
// arguments). With piano-gpu >= 2, piano-gpu-android-curve != 0 (default 1) and NONE of the preset's arguments on the command line,
// each preset argument reads as the installed v13.1y12c line gives it (the R3b daily guard with Android's bin-2 curve, floor 222 MHz,
// top 1100 MHz, ACD, the KGSL-style governor). One given argument switches the whole preset off, so a test image that names its own
// GPU knobs (the 607 qualification, a sweep) gets exactly those, never a mix. Readers: PianoA830's board (its boot-argument getter, the
// start() lines) and PianoPolicy (whether Power Saver's 342 MHz ceiling has a higher GPU maximum to act on). Portable; host test:
// include/tests/gpu_preset_host.cpp.
#pragma once
#include <stdint.h>
#include <string.h>
namespace pianogpu {
struct PresetArg {
  const char *name;
  uint32_t value;
};
inline constexpr PresetArg kAndroidCurve[] = {
    {"piano-gpu-r3b", 1},        {"piano-gpu-daily", 1},     {"piano-gpu-fullopp", 1},     {"piano-gpu-governor", 2},
    {"piano-gpu-profile", 1},    {"piano-gpu-speedbin", 1},  {"piano-gpu-dcvs", 1},        {"piano-gpu-maxlevel", 14},
    {"piano-gpu-dcvs-max", 1100}, {"piano-gpu-dcvs-min", 222}, {"piano-gpu-acd", 1},        {"piano-gpu-hfi", 0},
    {"piano-gpu-rails", 1},      {"piano-gpu-ddr", 11},      {"piano-gpu-ddr-floor", 11}};
inline constexpr unsigned kAndroidCurveCount = sizeof kAndroidCurve / sizeof kAndroidCurve[0];
using ArgGetter = bool (*)(void *cookie, const char *name, uint32_t *value);
// The preset's entry for a piano-gpu-* name (nullptr: not a preset argument).
inline const PresetArg *presetEntry(const char *name)
{
  for (const PresetArg &a : kAndroidCurve)
    if (!strcmp(a.name, name)) return &a;
  return nullptr;
}
// True when the preset applies: the GPU brought up (piano-gpu >= 2), not switched off, and no preset argument given.
inline bool presetActive(ArgGetter get, void *cookie, uint32_t gpuLevel)
{
  if (gpuLevel < 2 || !get) return false;
  uint32_t v = 0;
  if (get(cookie, "piano-gpu-android-curve", &v) && v == 0) return false;
  for (const PresetArg &a : kAndroidCurve)
    if (get(cookie, a.name, &v)) return false;
  return true;
}
// One argument with the preset: the command line's value, else the preset's (when active), else the fallback.
inline uint32_t presetArg(ArgGetter get, void *cookie, uint32_t gpuLevel, const char *name, uint32_t fallback, bool *given = nullptr)
{
  uint32_t v = fallback;
  if (given) *given = false;
  if (get && get(cookie, name, &v)) {
    if (given) *given = true;
    return v;
  }
  if (const PresetArg *a = presetEntry(name))
    if (presetActive(get, cookie, gpuLevel)) {
      if (given) *given = true;
      return a->value;
    }
  return fallback;
}
}  // namespace pianogpu
