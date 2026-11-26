// SPDX-License-Identifier: BSD-2-Clause-Patent
// A simulated cover for unattended tests (private/host-checks/20261124-keys-lid/ANALYSIS.md): machdep.piano_keyboard_hall_sim,
// root writes, refused unless the boot argument piano-keyboard-hall-sim=1. 0 closed, 1 open, -1 the sensor again. While set, the
// simulated level is what PianoKeyboard accepts (published piano-keyboard-hall with its message, so PianoPolicy and the clamshell
// see a cover closing or opening exactly as from gpio80); real samples are ignored (still logged with hall tracing). A system wake
// gives the cover back to the sensor: the kernel's cover-open wake reads the real gpio80, so a simulated close never wakes, and a
// close left set would put the system straight back to sleep after a timer or power key wake.
// What only a physical close adds: the magnet, gpio80's level and edge latch, and the kernel's cover-open wake (k27 0062, armed only
// when gpio80 reads closed at the suspend's entry).
// Portable (no kernel headers): PianoKeyboard.cpp's pollHall uses step(), tests/keyboard_host.cpp drives it.
#ifndef PIANO_KEYBOARD_HALL_SIM_H
#define PIANO_KEYBOARD_HALL_SIM_H
#include <stdint.h>

namespace pianokeyboard {

enum class HallSimSet : uint8_t { ok, off, invalid, noSensor };

inline const char *hallSimSetName(HallSimSet r)
{
  return r == HallSimSet::ok ? "ok" : r == HallSimSet::off ? "refused: piano-keyboard-hall-sim=0"
       : r == HallSimSet::noSensor ? "refused: no hall reading (piano-keyboard-hall=0 or gpio80 unmapped)" : "refused: not -1, 0 or 1";
}

// What the serving thread does with the hall at one pass.
struct HallSimStep {
  int accept = -1;          // the level to accept now (acceptHall), -1 nothing
  bool readReal = false;    // the simulation just ended: read gpio80 now, accept it if it differs
  bool skipReal = false;    // a simulation holds the level: this pass's real sample is ignored
  bool changed = false;     // the requested state differs from the applied one (a line is logged)
};

struct HallSim {
  int requested = -1;   // what the sysctl asked (-1 the sensor), written by the sysctl, read by the serving thread
  int applied = -1;     // what the serving thread last applied
  uint32_t sets = 0, refused = 0, wakeClears = 0;

  HallSimSet set(bool enabled, bool sensor, int64_t value)
  {
    HallSimSet r = HallSimSet::ok;
    if (!enabled) r = HallSimSet::off;
    else if (!sensor) r = HallSimSet::noSensor;
    else if (value < -1 || value > 1) r = HallSimSet::invalid;
    if (r != HallSimSet::ok) {
      ++refused;
      return r;
    }
    requested = int(value);
    ++sets;
    return r;
  }

  // One pass of pollHall: HALL the accepted level (0 closed, 1 open).
  HallSimStep step(int hall)
  {
    HallSimStep s;
    const int want = requested;
    s.changed = want != applied;
    if (s.changed) {
      if (want >= 0) {
        if (want != hall) s.accept = want;
      } else {
        s.readReal = true;
      }
      applied = want;
    }
    s.skipReal = applied >= 0;
    return s;
  }

  // A system wake: the sensor owns the cover again (true when a simulation was in force).
  bool clearAtWake()
  {
    const bool was = requested >= 0 || applied >= 0;
    if (was) ++wakeClears;
    requested = applied = -1;
    return was;
  }
};

}  // namespace pianokeyboard
#endif
