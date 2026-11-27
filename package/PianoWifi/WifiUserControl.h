// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// User intent shared by the Apple80211 request threads and the radio's one
// worker. A stop survives a quick off/on or disconnect/join until the worker
// finishes the old station's teardown. Only the worker acknowledges stops.
#pragma once
#include <stdint.h>

namespace pianowifi {
// Serialized with the profile store. Native control is sticky across radio
// restarts, while every accepted SET20 grants exactly one station attempt.
struct WifiJoinRequest {
  static constexpr uint64_t kTimeoutMs = 9500; // before IO80211's ~10 s poll ends
  bool nativeOwner = false;
  uint32_t generation = 0;
  uint32_t radioRestart = 0;
  uint64_t deadline = 0;
  bool consumed = false;
  uint32_t terminal = 2;

  void accept(uint32_t gen, uint64_t now, uint32_t restart) {
    nativeOwner = true;
    generation = gen;
    radioRestart = restart;
    deadline = now + kTimeoutMs;
    consumed = false;
    terminal = 0;
  }
  void cancel() { nativeOwner = true; terminal = 2; }
  uint32_t result(uint64_t now) const { return !terminal && now >= deadline ? 9u : terminal; }
  bool canStart(uint64_t now) const { return !nativeOwner || (!consumed && !result(now)); }
  void consume() { if (nativeOwner) consumed = true; }
  bool finish(uint32_t gen, uint32_t status, uint64_t now, uint32_t restart) {
    if (!gen || gen != generation || terminal) return false;
    terminal = now >= deadline ? 9u : status;
    // A request queued during a restart belongs to the new pass once it authorizes there.
    if (terminal == 1) radioRestart = restart;
    return terminal != 0;
  }
  uint32_t currentReceipt(uint64_t now, uint32_t restart, uint32_t stationGeneration,
                          bool stationActive, bool stationAuthorized) const {
    if (!nativeOwner || radioRestart != restart) return 0;
    const uint32_t status = result(now);
    const bool sameStation = consumed && stationGeneration == generation;
    if (status == 0 && (!consumed || (sameStation && stationActive))) return generation;
    if (status == 1 && sameStation && stationAuthorized) return generation;
    return 0;
  }
};

struct WifiUserControl;
// SET19 and SET20 carry no user-origin bit. Keep boot safety until a root-
// controlled arm explicitly enables physical OFF. Neither startup ON nor
// automatic joins can establish that the user requested a power change.
struct WifiBootPower {
  uint32_t armed = 0;
  bool enabled = true; // configured once, before publishing the driver
  void arm() { __atomic_store_n(&armed, 1u, __ATOMIC_RELEASE); }
  bool isArmed() const { return __atomic_load_n(&armed, __ATOMIC_ACQUIRE) != 0; }
  bool suppressOff() const { return enabled && !isArmed(); }
  bool setPower(WifiUserControl &owner, bool on) const;
};

struct WifiUserControl {
  enum : uint32_t { kPower = 1, kJoin = 2, kCycle = 4, kSerial = 8 };
  uint32_t word = kPower | kJoin;
  uint32_t applied = 0;

  uint32_t read() const { return __atomic_load_n(&word, __ATOMIC_ACQUIRE); }
  static bool power(uint32_t w) { return (w & kPower) != 0; }
  // GET19 stays ON until the worker confirms physical OFF.
  static bool reportedPower(uint32_t w) { return (w & (kPower | kCycle)) != 0; }
  static uint32_t serial(uint32_t w) { return w >> 3; }
  static bool joinRequested(uint32_t w) { return (w & (kPower | kJoin | kCycle)) == (kPower | kJoin); }
  bool pending(uint32_t w) const { return serial(w) != __atomic_load_n(&applied, __ATOMIC_ACQUIRE); }
  bool allowed(uint32_t w) const { return joinRequested(w) && !pending(w); }
  static bool radioStop(uint32_t w) { return !power(w) || (w & kCycle); }
  bool radioStop() const { return radioStop(read()); }
  static bool canReportJoinReceipt(uint32_t w, bool radioUsable, bool stopRequested) {
    return joinRequested(w) && radioUsable && !stopRequested;
  }
  bool radioOffConfirmed(uint32_t request) const {
    const uint32_t current = read();
    return serial(current) == serial(request) && !(current & kCycle) && !pending(request);
  }

  uint32_t setPower(bool on) {
    uint32_t old = read(), next;
    do {
      // ON can reopen the station gate after a sleep-preparation SET22.
      // Native callers still need a fresh association request.
      if (power(old) == on && (!on || (old & kJoin))) return old;
      next = on ? (old | kPower | kJoin) : ((old + kSerial) & ~(kPower | kJoin)) | kCycle;
    } while (!__atomic_compare_exchange_n(&word, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return next;
  }
  uint32_t disconnect() {
    uint32_t old = read(), next;
    do {
      if (!(old & kJoin)) return old;
      // A pending physical cycle already tears down the station. Keep its
      // serial so a sleep-preparation leave cannot invalidate its OFF ack.
      next = ((old & kCycle) ? old : old + kSerial) & ~kJoin;
    } while (!__atomic_compare_exchange_n(&word, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return next;
  }
  bool join() {
    uint32_t old = read();
    do {
      if (!power(old)) return false;
    } while (!__atomic_compare_exchange_n(&word, &old, old | kJoin, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return true;
  }
  void stopped(uint32_t w) { __atomic_store_n(&applied, serial(w), __ATOMIC_RELEASE); }
  // Called after WLAN_EN is low and DMA is disabled. A concurrent new off
  // remains pending. ON can change the flags of the same cycle.
  void radioOff(uint32_t w) {
    uint32_t current = read();
    do {
      if (serial(current) != serial(w)) return;
    } while (!__atomic_compare_exchange_n(&word, &current, current & ~kCycle, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    stopped(w);
  }
};
inline bool WifiBootPower::setPower(WifiUserControl &owner, bool on) const
{
  if (!on && suppressOff()) return false;
  owner.setPower(on);
  return true;
}
}
