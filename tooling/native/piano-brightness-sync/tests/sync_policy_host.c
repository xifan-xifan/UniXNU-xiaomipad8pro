// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host tests of SyncPolicy.h:
//   cc -std=c11 -Wall -Wextra -Werror sync_policy_host.c -o /tmp/sync_policy_host && /tmp/sync_policy_host
#include "../SyncPolicy.h"
#include <assert.h>
#include <stdio.h>

#define A(s, t, panel, cb) pbs_decide(&(s), (t), (panel), 1, 1, 1, (cb))   // automatic owns the lit panel

int main(void)
{
  struct pbs_state s;
  pbs_init(&s);
  // The installed boot of 2026-11-21 11:42: CoreBrightness restored its 600-nit preference, the panel at the automatic 26.16 nits.
  assert(A(s, 1000, 26160, 600000) == PBS_WAIT && s.expected == 600000);   // the restore is the baseline, not a move
  assert(A(s, 1500, 26160, 600000) == PBS_WAIT);
  assert(A(s, 2000, 26160, 600000) == PBS_SYNC);                            // settled 1 s: give CoreBrightness 26.16 nits
  assert(s.synced_level == 26160 && s.repeats == 1);
  pbs_synced(&s, 26159);                                                    // read back right after the set
  assert(A(s, 3500, 26160, 26159) == PBS_IDLE && s.repeats == 0);           // taken, and not a move
  // The room darkens: the automatic ramp moves the level every 250 ms; nothing until it rests.
  for (uint32_t t = 4000, level = 23160; level > 5000; t += 250, level -= 3000) assert(A(s, t, level, 26159) == PBS_WAIT);
  assert(A(s, 6000, 5225, 26159) == PBS_WAIT);
  assert(A(s, 6900, 5225, 26159) == PBS_WAIT);
  assert(A(s, 7000, 5225, 26159) == PBS_SYNC);
  pbs_synced(&s, -1);                                                       // no read-back: the level we gave
  assert(s.expected == 5225);
  // Within 3 % or 0.2 nit: in step (CoreBrightness's own rounding).
  assert(pbs_matches(5225, 5225) && pbs_matches(5400, 5225) && !pbs_matches(5500, 5225));
  assert(pbs_matches(2293, 2400) && pbs_matches(600000, 590000) && !pbs_matches(580000, 600000));
  assert(A(s, 9000, 5225, 5300) == PBS_IDLE);
  // EDR ramps (installed 14:07:31: the swap at 450 / 460.8 nits) leave CoreBrightness's own level alone: nothing to report.
  struct pbs_state e;
  pbs_init(&e);
  assert(A(e, 0, 2293, 431949) == PBS_WAIT);
  assert(A(e, 1000, 2293, 431949) == PBS_SYNC);
  pbs_synced(&e, 2293);
  for (uint64_t t = 1100; t < 3000; t += 17) assert(A(e, t, 2293, 2293) == PBS_IDLE);
  // The user's move while automatic owns the panel (slider2's desk-login keys, the owner's 12:11 slide): reported once.
  assert(A(e, 4000, 2293, 182304) == PBS_MOVED && e.expected == 182304);
  // The kernel leaves automatic mode: the user's level is followed, never synced (the slider owns the panel).
  assert(pbs_decide(&e, 4100, 182300, 0, 0, 1, 182304) == PBS_IDLE);
  assert(pbs_decide(&e, 9000, 182300, 0, 0, 1, 431949) == PBS_IDLE && e.expected == 431949);
  // A wake gives automatic mode back: the level is seen afresh, then synced; the user's old level is no move.
  assert(pbs_decide(&e, 10000, 2293, 1, 0, 1, 431949) == PBS_IDLE);         // requested, no target yet
  assert(A(e, 10500, 2293, 431949) == PBS_WAIT);
  assert(A(e, 11500, 2293, 431949) == PBS_SYNC);
  // 2026-11-22 (AUTO-BRIGHT.md): the kernel keeps automatic mode after a reported move (the curve's trim). The owner's 14:47:32
  // case: CoreBrightness at the synced 91.575 nits, Low Power Mode's eco factor takes it to 64.103 (a move), the panel follows it;
  // in step, nothing to do; the room changes (the sensor covered): the new automatic level is synced; eco off is a move again.
  struct pbs_state o;
  pbs_init(&o);
  assert(A(o, 0, 91722, 91575) == PBS_WAIT && A(o, 1000, 91722, 91575) == PBS_IDLE);   // the baseline, in step
  assert(A(o, 2000, 91722, 64103) == PBS_MOVED && o.expected == 64103);
  assert(A(o, 2100, 64250, 64103) == PBS_WAIT);                                         // the panel took the trim
  assert(A(o, 3100, 64250, 64103) == PBS_IDLE);                                         // in step: no sync, no move
  assert(A(o, 5000, 30100, 64103) == PBS_WAIT && A(o, 6000, 30100, 64103) == PBS_SYNC);  // a darker room: synced
  pbs_synced(&o, 30080);
  assert(A(o, 7000, 30100, 30080) == PBS_IDLE);
  assert(A(o, 8000, 30100, 43000) == PBS_MOVED);                                        // eco off: CoreBrightness / 0.7
  // A dark panel is never touched, and the level CoreBrightness has meanwhile is the new baseline.
  struct pbs_state u;
  pbs_init(&u);
  assert(pbs_decide(&u, 1000, 2293, 1, 1, 0, 600000) == PBS_IDLE);
  assert(pbs_decide(&u, 9000, 2293, 1, 1, 0, 500000) == PBS_IDLE && u.expected == 500000);
  assert(pbs_decide(&u, 9000, 0, 1, 1, 1, 400000) == PBS_IDLE && u.expected == 400000);
  // corebrightnessd not answering, then restarting with its (synced) preference: a new baseline, never a move.
  struct pbs_state k;
  pbs_init(&k);
  assert(A(k, 0, 2293, -1) == PBS_IDLE);
  assert(A(k, 500, 2293, 2400) == PBS_WAIT && k.expected == 2400);
  assert(A(k, 1500, 2293, 2400) == PBS_IDLE);
  assert(A(k, 2000, 2293, -1) == PBS_IDLE && k.expected == -1);
  assert(A(k, 2500, 2293, 431949) == PBS_WAIT && k.expected == 431949);    // a restart's restore
  // A level CoreBrightness never takes: 2 s apart, three tries, then a minute's back-off; a new level tries again at once.
  struct pbs_state r;
  pbs_init(&r);
  assert(A(r, 0, 100000, 600000) == PBS_WAIT);
  assert(A(r, 1000, 100000, 600000) == PBS_SYNC);
  pbs_synced(&r, 600000);                                                   // it kept its level
  assert(A(r, 2000, 100000, 600000) == PBS_WAIT);                           // inside the 2 s gap
  assert(A(r, 3000, 100000, 600000) == PBS_SYNC);
  pbs_synced(&r, 600000);
  assert(A(r, 5000, 100000, 600000) == PBS_SYNC && r.repeats == 3);
  pbs_synced(&r, 600000);
  assert(A(r, 7000, 100000, 600000) == PBS_IDLE);                           // backing off
  assert(A(r, 40000, 100000, 600000) == PBS_IDLE);
  assert(A(r, 67001, 100000, 600000) == PBS_SYNC);                          // after the minute: once more
  pbs_synced(&r, 600000);
  struct pbs_state n = r;
  n.backoff_until = 0;
  assert(A(n, 70000, 90000, 600000) == PBS_WAIT);
  assert(A(n, 71000, 90000, 600000) == PBS_SYNC && n.repeats == 1);
  // 16.16 for piano-backlight-sync / -moved: 26.16 nits, the 2.293 floor, 600 nits.
  assert(pbs_fixed(26160) == 1714422 && pbs_fixed(2293) == 150274 && pbs_fixed(600000) == 39321600);
  puts("sync policy: restore baseline, settle, automatic ramp, tolerance, EDR, the user's move, automatic kept after a move, manual, "
       "wake, dark, restart, back-off and 16.16 passed");
  return 0;
}
