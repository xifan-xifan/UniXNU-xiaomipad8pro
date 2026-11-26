// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoGpuAndroidPreset.h (gpu/throttle1): the Android-curve preset applies only with piano-gpu >= 2, no
// piano-gpu-android-curve=0 and none of its arguments given; one given argument switches the whole preset off (no mix).
#include "PianoGpuAndroidPreset.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
static unsigned gChecks, gFails;
#define CHECK(x) do { ++gChecks; if (!(x)) { ++gFails; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
using Args = std::map<std::string, uint32_t>;
static bool get(void *cookie, const char *name, uint32_t *value)
{
  const Args &a = *static_cast<const Args *>(cookie);
  auto it = a.find(name);
  if (it == a.end()) return false;
  *value = it->second;
  return true;
}
int main()
{
  using namespace pianogpu;
  Args none;
  CHECK(presetActive(get, &none, 2) && !presetActive(get, &none, 1) && !presetActive(get, &none, 0));
  CHECK(presetArg(get, &none, 2, "piano-gpu-fullopp", 0) == 1 && presetArg(get, &none, 2, "piano-gpu-maxlevel", 0) == 14 &&
        presetArg(get, &none, 2, "piano-gpu-dcvs-max", 342) == 1100 && presetArg(get, &none, 2, "piano-gpu-dcvs-min", 160) == 222 &&
        presetArg(get, &none, 2, "piano-gpu-governor", 0) == 2 && presetArg(get, &none, 2, "piano-gpu-hfi", 7) == 0);
  bool given = false;
  CHECK(presetArg(get, &none, 2, "piano-gpu-dcvs-max", 342, &given) == 1100 && given);
  CHECK(presetArg(get, &none, 1, "piano-gpu-dcvs-max", 342, &given) == 342 && !given);   // survey only: no preset
  CHECK(presetArg(get, &none, 2, "piano-gpu-boost", 5, &given) == 5 && !given);         // not a preset argument
  // the installed y12c line names every argument itself: the same values, the preset inactive
  Args y12c;
  for (const PresetArg &a : kAndroidCurve) y12c[a.name] = a.value;
  CHECK(!presetActive(get, &y12c, 2) && presetArg(get, &y12c, 2, "piano-gpu-maxlevel", 0) == 14);
  // a 607 qualification image: its own two knobs, never fullopp or 1100 from the preset
  Args q607 = {{"piano-gpu-maxlevel", 7}, {"piano-gpu-dcvs-max", 607}};
  CHECK(!presetActive(get, &q607, 2) && presetArg(get, &q607, 2, "piano-gpu-fullopp", 0) == 0 &&
        presetArg(get, &q607, 2, "piano-gpu-dcvs-max", 342) == 607 && presetArg(get, &q607, 2, "piano-gpu-r3b", 0) == 0);
  // the switch
  Args off = {{"piano-gpu-android-curve", 0}};
  CHECK(!presetActive(get, &off, 2) && presetArg(get, &off, 2, "piano-gpu-fullopp", 0) == 0);
  Args on = {{"piano-gpu-android-curve", 1}};
  CHECK(presetActive(get, &on, 2));
  // every preset name is unique and a piano-gpu-* argument
  for (unsigned i = 0; i < kAndroidCurveCount; ++i) {
    CHECK(!std::strncmp(kAndroidCurve[i].name, "piano-gpu-", 10) && presetEntry(kAndroidCurve[i].name) == &kAndroidCurve[i]);
  }
  CHECK(kAndroidCurveCount == 15 && !presetEntry("piano-gpu-android-curve") && !presetActive(nullptr, nullptr, 2));
  if (gFails) return 1;
  std::printf("gpu_preset_host: all passed (%u checks)\n", gChecks);
  return 0;
}
