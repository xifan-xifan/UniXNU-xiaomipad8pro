// SPDX-License-Identifier: BSD-2-Clause-Patent
// c++ -std=c++20 -Wall -Wextra -Werror -I .. dpu_clean_host.cpp -o /tmp/dpu_clean_host && /tmp/dpu_clean_host
#include "DpuClean.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace pianodpu::flip;
using Action = CleanEpoch::Action;

int main()
{
  constexpr unsigned mask = 7, wc = 3;
  std::array<unsigned, 3> attrs{wc, wc | 8, wc};
  const auto inspect = [&](unsigned count) {
    return cachePages(count, [](unsigned i) { return i; },
                      [&](unsigned i, unsigned &attr) { attr = attrs[i]; return attr != 0; }, mask, wc);
  };
  assert(inspect(3).wc == 3);
  assert(cleanEligibility(0, inspect(3)) == CleanReason::unknownMode);
  for (auto mode : {0x100u, 0x300u, 0x700u, 0x401u, UINT32_MAX})
    assert(cleanEligibility(mode, inspect(3)) == CleanReason::mode);
  assert(cleanEligibility(kSurfaceWriteCombined, inspect(0)) == CleanReason::attributes);
  attrs.back() = 1;
  assert(inspect(3).other == 1 && cleanEligibility(kSurfaceWriteCombined, inspect(3)) == CleanReason::attributes);
  attrs.back() = 0;
  assert(inspect(3).unknown == 1 && cleanEligibility(kSurfaceWriteCombined, inspect(3)) == CleanReason::attributes);
  attrs.back() = wc;

  CleanEpoch epoch;
  enum Event { fullBacking, scanout, check, dsb };
  std::vector<Event> events;
  bool failInit = false, failFallback = false;
  const auto verify = [&] {
    events.push_back(check);
    return cleanEligibility(kSurfaceWriteCombined, inspect(3));
  };
  const auto clean = [&](bool init) {
    events.push_back(init ? fullBacking : scanout);
    assert(!init || !epoch.initialized);
    return init ? !failInit : !failFallback;
  };
  const auto prepare = [&](CleanReason reason) {
    events.clear();
    return epoch.prepare(reason, clean, verify, [&] { events.push_back(dsb); });
  };
  assert(prepare(CleanReason::eligible).action == Action::initialized && epoch.initialized);
  assert((events == std::vector<Event>{fullBacking, check}));
  assert(prepare(CleanReason::eligible).action == Action::skipped && events == std::vector<Event>{dsb});
  const auto forced = prepare(CleanReason::forced);
  assert(forced.action == Action::fallback && forced.reason == CleanReason::forced && !epoch.initialized);
  assert(events == std::vector<Event>{scanout});
  assert(prepare(CleanReason::eligible).action == Action::initialized && epoch.initialized);
  assert((events == std::vector<Event>{fullBacking, check}));
  assert(prepare(CleanReason::mode).action == Action::fallback && !epoch.initialized);
  assert(events == std::vector<Event>{scanout});

  attrs.back() = 1;   // final check observes an attribute change during initialization
  const auto changed = prepare(CleanReason::eligible);
  assert(changed.action == Action::fallback && changed.reason == CleanReason::attributes && !epoch.initialized);
  assert((events == std::vector<Event>{fullBacking, check, scanout}));
  attrs.back() = wc;
  failInit = true;
  assert(prepare(CleanReason::eligible).reason == CleanReason::initialClean && !epoch.initialized);
  assert((events == std::vector<Event>{fullBacking, scanout}));
  failFallback = true;
  assert(prepare(CleanReason::eligible).action == Action::refused && !epoch.initialized);
  assert(prepare(CleanReason::handoff).action == Action::refused && events == std::vector<Event>{scanout});
  failInit = failFallback = false;
  assert(prepare(CleanReason::eligible).action == Action::initialized);
  std::puts("PianoDpu conditional clean host tests: all passed");
}
