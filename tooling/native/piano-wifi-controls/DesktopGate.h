// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#pragma once
#include <stdint.h>
struct piano_wifi_desktop_gate { unsigned stable; uint32_t prior; };
static inline int piano_wifi_desktop_ready(struct piano_wifi_desktop_gate *g, uint32_t uid,
                                          int desktop, int published, int on)
{
  g->stable = desktop && published && on ? (uid == g->prior ? g->stable + 1 : 1) : 0;
  g->prior = uid;
  return g->stable >= 3;
}
