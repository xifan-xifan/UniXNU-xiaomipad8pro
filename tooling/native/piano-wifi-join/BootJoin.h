// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// One boot transaction. Transport readiness may repeat; an association never does.
#pragma once
#include <stdint.h>
struct piano_boot_join_transport {
  void *context;
  int (*ready)(void *);
  uint32_t (*receipt)(void *);
  int (*associate)(void *);
  void (*pause)(void *);
};
enum { PianoBootJoinSkipped = 4 };

static int piano_boot_join(struct piano_boot_join_transport *t, unsigned limit)
{
  int ready = 0;
  for (unsigned n = 0; n < limit; ++n) {
    ready = t->ready(t->context);
    if (ready) break;
    t->pause(t->context);
  }
  if (ready < 0) return PianoBootJoinSkipped;
  if (!ready) return 1;
  const uint32_t before = t->receipt(t->context);
  const int joined = t->associate(t->context);
  if (joined < 0) return PianoBootJoinSkipped;
  if (!joined) return 2;
  // A successful Apple API return alone is insufficient: require a new SET20.
  for (unsigned n = 0; n < 30; ++n) {
    const uint32_t after = t->receipt(t->context);
    if (after && after != before) return 0;
    t->pause(t->context);
  }
  return 3;
}
