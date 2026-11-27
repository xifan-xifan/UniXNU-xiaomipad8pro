// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace pianodpu::flip {

constexpr uint32_t kSurfaceWriteCombined = 0x400;   // IOSurface's requested mode

template <class BootArg>
bool readCleanSkip(uint32_t flipLevel, BootArg bootArg)
{
  return flipLevel == 4 && bootArg("piano-dpu-clean-skip", 0) == 1;
}

enum class CleanReason : unsigned {
  eligible,
  forced,
  handoff,
  unknownMode,
  mode,
  attributes,
  backing,
  initialClean,
  count
};

inline const char *cleanReasonName(CleanReason reason)
{
  constexpr const char *names[] = {"eligible", "forced", "handoff", "unknown-mode", "mode", "attrs", "backing", "init-clean"};
  static_assert(sizeof names / sizeof names[0] == unsigned(CleanReason::count));
  return names[unsigned(reason)];
}

struct CleanPages {
  unsigned wc = 0, other = 0, unknown = 0;
  bool allWriteCombined() const { return wc && !other && !unknown; }
};

// Inspect the entire wired allocation, including pages outside this flip's pixel range.
template<class Page, class Attribute>
CleanPages cachePages(unsigned count, Page page, Attribute attribute, unsigned mask, unsigned writeCombined)
{
  CleanPages result;
  for (unsigned i = 0; i < count; ++i) {
    unsigned value = 0;
    if (!attribute(page(i), value)) ++result.unknown;
    else if ((value & mask) == writeCombined) ++result.wc;
    else ++result.other;
  }
  return result;
}

inline CleanReason cleanEligibility(uint32_t mode, const CleanPages &pages)
{
  if (!mode) return CleanReason::unknownMode;
  if (mode != kSurfaceWriteCombined) return CleanReason::mode;
  return pages.allWriteCombined() ? CleanReason::eligible : CleanReason::attributes;
}

// One epoch per retained, wired slot. A whole-backing clean covers any later valid subrange.
struct CleanEpoch {
  bool initialized = false;

  enum class Action { initialized, skipped, fallback, refused };
  struct Result {
    Action action;
    CleanReason reason;
  };

  template<class Clean, class Verify, class Barrier>
  Result prepare(CleanReason reason, Clean clean, Verify verify, Barrier barrier)
  {
    if (reason == CleanReason::eligible) {
      if (initialized) {
        barrier();
        return {Action::skipped, reason};
      }
      if (clean(true)) {
        reason = verify();
        if (reason == CleanReason::eligible) {
          initialized = true;
          return {Action::initialized, reason};
        }
      } else {
        reason = CleanReason::initialClean;
      }
    }
    initialized = false;
    return {clean(false) ? Action::fallback : Action::refused, reason};
  }
};

}  // namespace pianodpu::flip
