// SPDX-License-Identifier: BSD-2-Clause-Patent
// A simulated key press for unattended tests (private/host-checks/20261124-keys-lid/ANALYSIS.md): machdep.piano_input_sim, root
// writes, refused unless the boot argument piano-input-sim=1. The simulated keys are OR'ed into the decoded PMIC sample, so the
// press goes through everything a real one does after the register read: the debounce, PowerKey.h (held back for PianoPolicy,
// a short press asked of it on the release, a long press reported as the HID Power usage), the HID report and its event driver.
// What only a real press adds: the PMK8550 PON INT_RT_STS bit 7 / bit 6 or PM8550 GPIO6 read itself, and, while suspended, the
// kernel's own power key wake (k27 0062 piano_wake_poll and its wait for the release).
//
// The value written: bits 0-15 the press length in ms (kSimMinMs..kSimMaxMs), bits 16-18 the keys (HidReports.h bits; 0 means the
// power key). Portable (no kernel headers): PianoInput.cpp holds it under a lock, tests/buttons_host.cpp drives it.
#ifndef PIANO_INPUT_KEY_SIM_H
#define PIANO_INPUT_KEY_SIM_H
#include <stdint.h>
#include "HidReports.h"

namespace pianoinput {

constexpr uint32_t kSimMinMs = 50, kSimMaxMs = 10000;

enum class SimResult : uint8_t { ok, off, busy, invalid };

inline const char *simResultName(SimResult r)
{
  return r == SimResult::ok ? "ok" : r == SimResult::off ? "refused: piano-input-sim=0" : r == SimResult::busy ? "refused: a simulated press is in progress"
                                                                                                              : "refused: not a valid press";
}

// The written value decoded: true with the keys and the length, false for anything else (negative, too short or long, other bits).
inline bool decodeSim(int64_t value, uint8_t *keys, uint32_t *ms)
{
  if (value <= 0 || value > 0x7ffff) return false;
  const uint32_t length = uint32_t(value) & 0xffff, bits = (uint32_t(value) >> 16) & 7;
  if (length < kSimMinMs || length > kSimMaxMs) return false;
  *keys = bits ? uint8_t(bits) : kKeyPower;
  *ms = length;
  return true;
}

struct KeySim {
  uint8_t keys = 0;          // the keys held while active
  uint64_t until = 0;        // ms, the poll clock's
  uint32_t presses = 0, refused = 0;

  bool active(uint64_t nowMs) const { return keys && nowMs < until; }

  // A write: one press at a time; refused without the boot argument.
  SimResult arm(bool enabled, int64_t value, uint64_t nowMs)
  {
    uint8_t k = 0;
    uint32_t ms = 0;
    SimResult r = SimResult::ok;
    if (!enabled) r = SimResult::off;
    else if (!decodeSim(value, &k, &ms)) r = SimResult::invalid;
    else if (active(nowMs)) r = SimResult::busy;
    if (r != SimResult::ok) {
      ++refused;
      return r;
    }
    keys = k;
    until = nowMs + ms;
    ++presses;
    return r;
  }

  // The keys to add to the sample taken at nowMs (0 once the press has run out).
  uint8_t held(uint64_t nowMs)
  {
    if (!keys) return 0;
    if (nowMs >= until) {
      keys = 0;
      return 0;
    }
    return keys;
  }

  // A system sleep ends a simulated press (the release is then the next sample's).
  void reset() { keys = 0; until = 0; }
};

}  // namespace pianoinput
#endif
