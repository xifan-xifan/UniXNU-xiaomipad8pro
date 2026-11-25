// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-brightness-sync's decision (host-tested: tests/sync_policy_host.c). CoreBrightness never reads the panel back: its level
// starts from its BrightnessLevelNits preference and changes only by its own commits, so while PianoBacklight's automatic mode
// owns the panel the Control Center slider shows a stale value (2026-11-21, SLIDER-INIT.md).
//
// Round 2 (slider2 and the installed 14:06 boot): WindowServer's swap brightness is CoreBrightness's SDR level times the EDR headroom,
// so EDR ramps (and CoreBrightness's restore) look like slider moves to the kernel, and both ended automatic mode. CoreBrightness's
// own SDR level (DisplayNitsKey) does not move with EDR and only moves at a restore (the daemon's baseline: never a move), at our
// syncs (re-read right after) and when the user moves it. So the daemon holds PianoBacklight's lease and decides:
//   PBS_MOVED  CoreBrightness's level left the one we expect while automatic mode owns the panel: the user's move (report it)
//   PBS_SYNC   the automatic level has settled and CoreBrightness is more than 3 % (and 0.2 nit) away: give it the panel's level
#ifndef PIANO_BRIGHTNESS_SYNC_POLICY_H
#define PIANO_BRIGHTNESS_SYNC_POLICY_H
#include <stdint.h>

enum {
  PBS_SETTLE_MS = 1000,    // the panel's level unchanged this long (the automatic ramp has settled)
  PBS_GAP_MS = 2000,       // at least this long between two syncs
  PBS_REPEATS = 3,         // syncs of one level CoreBrightness did not take before backing off
  PBS_BACKOFF_MS = 60000,
};
enum pbs_action { PBS_IDLE = 0, PBS_WAIT = 1, PBS_SYNC = 2, PBS_MOVED = 3 };   // nothing / ask again in about a second / sync / report

struct pbs_state {
  uint32_t level;           // the panel's level as last seen (millinits), 0 none
  uint64_t level_since;     // when it took that value (ms)
  uint64_t last_sync;       // the last sync (ms), 0 none
  uint32_t synced_level;    // the level it gave
  uint32_t repeats;         // syncs of synced_level since CoreBrightness last matched the panel
  uint64_t backoff_until;
  int64_t expected;         // CoreBrightness's level as we expect it (millinits), -1: not known yet (the next answer is the baseline)
};

static inline void pbs_init(struct pbs_state *s)
{
  *s = (struct pbs_state){0};
  s->expected = -1;
}

// Two levels in millinits within 3 % of the second, or 0.2 nit.
static inline int pbs_matches(uint32_t a, uint32_t b)
{
  const uint32_t d = a > b ? a - b : b - a;
  return d <= 200 || (uint64_t)d * 100 <= (uint64_t)b * 3;
}

// now: ms; panel: PianoBacklight's applied level (IODisplayParameters BrightnessMilliNits value, millinits); owned: its
// piano-backlight-auto-requested (the kernel drops the stream then); active: piano-backlight-auto-active (a target applied); lit:
// piano-backlight-lit; corebrightness: DisplayNitsKey in millinits, < 0 unknown (corebrightnessd not answering).
static inline enum pbs_action pbs_decide(struct pbs_state *s, uint64_t now, uint32_t panel, int owned, int active, int lit,
                                         int64_t corebrightness)
{
  if (corebrightness < 0 || corebrightness > 0xffffffffll) {   // not answering: the next answer is a new baseline
    s->expected = -1;
    s->level = 0;
    return PBS_IDLE;
  }
  if (s->expected < 0) s->expected = corebrightness;   // its restore (or a restart's): what it had before we looked
  if (!owned || !lit || !panel) {   // the user's level (the stream owns the panel) or dark: follow it, never touch it
    s->expected = corebrightness;
    s->level = 0;
    return PBS_IDLE;
  }
  if (!pbs_matches((uint32_t)corebrightness, (uint32_t)s->expected)) {   // automatic owns the panel, and the user moved the slider
    s->expected = corebrightness;
    s->level = 0;
    return PBS_MOVED;
  }
  if (!active) return PBS_IDLE;   // requested, no target applied yet (its auto-active change wakes the daemon)
  if (panel != s->level) {
    s->level = panel;
    s->level_since = now;
    return PBS_WAIT;
  }
  if (now - s->level_since < PBS_SETTLE_MS) return PBS_WAIT;
  if (pbs_matches((uint32_t)corebrightness, panel)) {
    s->repeats = 0;
    return PBS_IDLE;
  }
  if (now < s->backoff_until) return PBS_IDLE;
  if (s->last_sync && now - s->last_sync < PBS_GAP_MS) return PBS_WAIT;
  if (panel != s->synced_level) {
    s->repeats = 0;
  } else if (s->repeats >= PBS_REPEATS) {
    s->repeats = 0;
    s->backoff_until = now + PBS_BACKOFF_MS;
    return PBS_IDLE;
  }
  ++s->repeats;
  s->synced_level = panel;
  s->last_sync = now;
  return PBS_SYNC;
}

// After a sync: CoreBrightness's level as read right after the set (its own rounding), else the level we gave.
static inline void pbs_synced(struct pbs_state *s, int64_t corebrightness_after)
{
  s->expected = corebrightness_after >= 0 ? corebrightness_after : (int64_t)s->synced_level;
}

// millinits as PianoBacklight's signed 16.16 nits (piano-backlight-sync, piano-backlight-moved)
static inline int64_t pbs_fixed(uint32_t millinits) { return ((int64_t)millinits * 65536 + 500) / 1000; }

#endif
