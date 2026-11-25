// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "../DesktopGate.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
  struct piano_wifi_desktop_gate g = {0};
  // Cold boot, no console, absent SC publication, or raw OFF never arms.
  for (int i = 0; i < 30; ++i) assert(!piano_wifi_desktop_ready(&g, 0, 0, 1, 1));
  for (int i = 0; i < 30; ++i) assert(!piano_wifi_desktop_ready(&g, 501, 1, 0, 1));
  for (int i = 0; i < 30; ++i) assert(!piano_wifi_desktop_ready(&g, 501, 1, 1, 0));
  assert(!piano_wifi_desktop_ready(&g, 501, 1, 1, 1));
  assert(!piano_wifi_desktop_ready(&g, 501, 1, 1, 1));
  assert(!piano_wifi_desktop_ready(&g, 502, 1, 1, 1)); // switch resets stability
  assert(!piano_wifi_desktop_ready(&g, 502, 1, 1, 1));
  assert(piano_wifi_desktop_ready(&g, 502, 1, 1, 1));
  assert(!piano_wifi_desktop_ready(&g, 502, 0, 1, 1)); // logout breaks readiness
  assert(!piano_wifi_desktop_ready(&g, 502, 1, 1, 1));
  assert(!piano_wifi_desktop_ready(&g, 502, 1, 1, 1));
  assert(piano_wifi_desktop_ready(&g, 502, 1, 1, 1));
  puts("desktop gate: cold boot, publication, raw OFF, UID switch, logout and handoff passed");
}
