// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. user_control_host.cpp -o /tmp/user_control_host && /tmp/user_control_host
#include "WifiUserControl.h"
#include "WifiApple80211.h"
#include <cstdio>
using namespace pianowifi;
using namespace pianowifi::a80211;
static unsigned failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void radioCycles()
{
  WifiUserControl owner;
  uint32_t previousOff = owner.read();
  for (unsigned cycle = 1; cycle <= 4; ++cycle) {
    const uint32_t off = owner.setPower(false);
    CHECK(owner.serial(off) == owner.serial(previousOff) + 1);
    CHECK(owner.setPower(false) == off && owner.radioStop() && !owner.join());
    CHECK(!owner.radioOffConfirmed(off));
    if (cycle > 1) {
      owner.radioOff(previousOff);
      CHECK(!owner.radioOffConfirmed(previousOff) && !owner.radioOffConfirmed(off));
      CHECK(owner.pending(off) && owner.radioStop());
    }

    // The worker captures OFF before checking the physical pins. ON can
    // arrive before the readback completes without changing this serial.
    const bool quickOn = cycle % 2 != 0;
    if (quickOn) {
      const uint32_t on = owner.setPower(true);
      CHECK(owner.serial(on) == owner.serial(off) && owner.setPower(true) == on);
      CHECK(owner.radioStop() && !owner.allowed(on) && !owner.radioOffConfirmed(off));
    }
    owner.radioOff(off);
    CHECK(owner.radioOffConfirmed(off) && !owner.pending(owner.read()));
    CHECK(owner.power(owner.read()) == quickOn && owner.radioStop() == !quickOn);
    if (cycle > 1) {
      owner.radioOff(previousOff);
      CHECK(owner.radioOffConfirmed(off) && !owner.radioOffConfirmed(previousOff));
    }
    CHECK(owner.serial(owner.setPower(true)) == owner.serial(off));
    CHECK(owner.allowed(owner.read()) && !owner.radioStop());
    previousOff = off;
  }

  // The three flag bits leave a 29-bit serial that wraps independently.
  owner.word = (UINT32_MAX & ~7u) | owner.kPower | owner.kJoin;
  owner.applied = owner.serial(owner.read());
  const uint32_t last = owner.setPower(false);
  CHECK(owner.serial(last) == 0 && owner.pending(last));
  owner.radioOff(last);
  CHECK(owner.radioOffConfirmed(last));
}

static void joinReceipt()
{
  constexpr uint64_t accepted = 100;
  constexpr uint64_t deadline = accepted + WifiJoinRequest::kTimeoutMs;
  WifiUserControl owner;
  WifiJoinRequest request;
  uint32_t restart = 0, stationGeneration = 0;
  bool radioUsable = true, stopRequested = false, active = false, authorized = false;
  auto readReceipt = [&](uint64_t now) {
    const uint32_t user = owner.read();
    if (!owner.canReportJoinReceipt(user, radioUsable, stopRequested)) return 0u;
    return request.currentReceipt(now, restart, stationGeneration, active, authorized && owner.allowed(user));
  };

  CHECK(!readReceipt(accepted));
  request.accept(1, accepted, restart);
  CHECK(readReceipt(accepted) == 1 && readReceipt(deadline - 1) == 1);
  CHECK(!readReceipt(deadline) && !readReceipt(deadline + 1));
  request.consume();
  active = true;
  CHECK(!readReceipt(deadline - 1)); // previous station still visible
  stationGeneration = 1;
  CHECK(readReceipt(deadline - 1) == 1 && !readReceipt(deadline));
  active = false;
  CHECK(!readReceipt(deadline - 1)); // link loss during the attempt
  active = true;
  CHECK(request.finish(1, 1, deadline, restart) && request.result(deadline) == 9);
  authorized = true;
  CHECK(!readReceipt(deadline + 1)); // late authorization cannot revive a timed-out attempt

  request.accept(2, deadline, restart);
  stationGeneration = 2;
  request.consume();
  CHECK(!request.finish(1, 1, deadline + 1, restart)); // superseded completion
  CHECK(request.finish(2, 1, deadline + 1, restart));
  CHECK(readReceipt(deadline * 3) == 2); // live through the original pending deadline
  authorized = false;
  CHECK(!readReceipt(deadline * 3) && request.generation == 2); // same-pass link loss
  authorized = true;
  stopRequested = true; // parked sleep, even before station and radio state have caught up
  CHECK(!readReceipt(deadline * 3));
  radioUsable = false;
  stopRequested = false;
  CHECK(!readReceipt(deadline * 3)); // wake starts a new radio pass before level 7 is ready
  ++restart;
  radioUsable = true;
  CHECK(!readReceipt(deadline * 3) && request.generation == 2);

  request.accept(3, deadline * 3, restart);
  CHECK(readReceipt(deadline * 3) == 3);
  ++restart;
  CHECK(!readReceipt(deadline * 3)); // a queued request can authorize on a later pass
  stationGeneration = 3;
  request.consume();
  CHECK(request.finish(3, 1, deadline * 3, restart));
  CHECK(readReceipt(deadline * 3) == 3);
  const uint32_t off = owner.setPower(false);
  CHECK(!readReceipt(deadline * 3)); // old station may still say it is authorized
  request.cancel(); // SET19 OFF cancels under the same lock as the power change
  owner.setPower(true);
  CHECK(!readReceipt(deadline * 3));
  owner.radioOff(off);
  CHECK(owner.allowed(owner.read()) && !readReceipt(deadline * 3) && request.generation == 3);

  request.accept(4, deadline * 3, restart);
  CHECK(readReceipt(deadline * 3) == 4);
  const uint32_t leave = owner.disconnect(); // SET22 retains ON while canceling
  CHECK(owner.reportedPower(leave) && !readReceipt(deadline * 3));
  request.cancel();
  CHECK(!readReceipt(deadline * 3) && request.generation == 4);
  CHECK(owner.join() && !owner.allowed(owner.read()));
  request.accept(5, deadline * 3, restart);
  CHECK(readReceipt(deadline * 3) == 5); // fresh SET20 queued behind old teardown
  owner.stopped(leave);
  stationGeneration = 5;
  request.consume();
  CHECK(request.finish(5, 2, deadline * 3, restart));
  CHECK(!readReceipt(deadline * 3));
}

static void sleepDuringPowerCycle()
{
  WifiUserControl owner;
  WifiJoinRequest request;
  request.accept(1, 0, 0);
  request.consume();
  CHECK(request.finish(1, 1, 1, 0));
  const uint32_t off = owner.setPower(false);
  request.cancel();
  owner.setPower(true);
  const uint32_t sleepLeave = owner.disconnect();
  CHECK(owner.power(sleepLeave) && owner.radioStop() && !owner.allowed(sleepLeave));
  // The sleep-preparation leave shares the pending physical cycle. The
  // worker can acknowledge the original OFF after verifying the pins.
  owner.radioOff(off);
  CHECK(owner.radioOffConfirmed(off) && !owner.radioStop() && !owner.allowed(owner.read()));
  CHECK(!request.canStart(2) && !request.currentReceipt(2, 1, 1, true, true));
  owner.setPower(true);
  CHECK(owner.allowed(owner.read()) && !request.canStart(2));
  request.accept(2, 3, 1);
  CHECK(request.canStart(3));
}

int main()
{
  radioCycles();
  joinReceipt();
  sleepDuringPowerCycle();
  WifiUserControl u;
  CHECK(u.allowed(u.read()));
  // Sleep preparation can disconnect without changing logical radio power.
  // Wake can reapply ON. It cannot bypass teardown or grant a native join.
  const uint32_t sleepLeave = u.disconnect();
  CHECK(WifiUserControl::power(u.read()) && !u.allowed(u.read()));
  u.setPower(true);
  CHECK(u.pending(sleepLeave) && !u.allowed(u.read()));
  u.stopped(sleepLeave);
  CHECK(u.allowed(u.read()));
  const uint32_t completedLeave = u.disconnect();
  u.stopped(completedLeave);
  CHECK(!u.allowed(u.read())); // ordinary leave stays idle until a command
  const uint32_t wakeOn = u.setPower(true);
  CHECK(WifiUserControl::serial(wakeOn) == WifiUserControl::serial(completedLeave));
  CHECK(u.allowed(wakeOn) && !u.radioStop());
  CHECK(u.setPower(true) == wakeOn); // repeated ON has no further effect
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    const uint32_t leave = u.disconnect();
    CHECK(!u.allowed(u.read()) && u.pending(leave) && WifiUserControl::power(leave));
    CHECK(u.disconnect() == leave); // repeat leave is idempotent
    CHECK(u.join());               // quick new join cannot skip the old teardown
    CHECK(!u.allowed(u.read()));
    u.stopped(leave);
    CHECK(u.allowed(u.read()));
    const uint32_t off = u.setPower(false);
    CHECK(!u.join() && !u.allowed(u.read()) && u.radioStop());
    CHECK(!u.radioOffConfirmed(off));
    CHECK(u.setPower(false) == off);
    u.setPower(true);              // quick off/on still physically cycles
    CHECK(u.radioStop() && !u.allowed(u.read()));
    u.stopped(off);                // station down alone is insufficient
    CHECK(u.radioStop() && !u.allowed(u.read()) && !u.radioOffConfirmed(off));
    u.radioOff(u.read());          // DMA quiesced and WLAN_EN low
    CHECK(!u.radioStop() && u.allowed(u.read()) && u.radioOffConfirmed(off));
  }
  // No-profile/failure park: there is no station left, so the idle worker
  // acknowledges the leave before using the queued SET20 as a restart cue.
  const uint32_t idleLeave = u.disconnect();
  CHECK(u.join() && !u.allowed(u.read()));
  u.stopped(idleLeave);
  CHECK(u.allowed(u.read()));
  // SET20 is one attempt per generation. Neither ON, retained profiles, nor
  // a late authorization can start or resurrect that attempt.
  WifiJoinRequest request;
  CHECK(request.canStart(100)); // RAM-only profile mode
  request.cancel();
  CHECK(request.nativeOwner && !request.canStart(100) && request.result(100) == 2);
  request.accept(10, 100, 0);
  CHECK(request.canStart(100) && request.result(100) == 0);
  request.consume();
  CHECK(!request.canStart(101) && request.result(9599) == 0);
  CHECK(request.result(9600) == 9 && request.finish(10, 1, 9600, 0));
  CHECK(request.result(9601) == 9 && !request.canStart(9601));
  request.accept(11, 9700, 0);
  CHECK(!request.finish(10, 1, 9700, 0) && request.result(9700) == 0);
  request.consume();
  CHECK(request.finish(11, 1, 9701, 0) && request.result(9701) == 1);
  CHECK(!request.canStart(20000)); // wake/link loss cannot replay it
  request.cancel();             // SET22, or physical OFF
  CHECK(request.result(20001) == 2 && !request.canStart(20001));
  CHECK(!request.finish(11, 1, 20002, 0) && request.result(20002) == 2);
  request.accept(12, 20003, 0);
  CHECK(request.canStart(20003) && request.result(20003) == 0);
  request.consume();
  CHECK(request.finish(12, 2, 20004, 0) && request.result(20004) == 2);
  const uint32_t first = u.disconnect();
  CHECK(u.join());
  const uint32_t later = u.disconnect();
  u.stopped(first);                // a late completion cannot consume a newer stop
  CHECK(u.pending(later));
  u.stopped(later);
  CHECK(!u.pending(later) && !u.allowed(u.read()));
  CHECK(u.join() && u.allowed(u.read()));
  const uint32_t off = u.setPower(false);
  u.setPower(true);
  u.setPower(false);               // new off between physical off and acknowledgement
  u.radioOff(off);
  CHECK(u.radioStop() && u.pending(u.read()));
  u.radioOff(u.read());
  CHECK(!WifiUserControl::power(u.read()) && !u.pending(u.read()));
  u.setPower(true);
  CHECK(u.allowed(u.read()));
  // Polling GET13/19/21 before the timer does not consume events. Exactly one
  // notification survives repeated polls, with coherent current reads.
  Association canceled = {}, newJoin = {}, failed = {};
  canceled.result = 2;
  CHECK(!associationLinkDownEvent(canceled, newJoin)); // canceled2 -> fresh pending0 must not abort auto-join
  failed.result = 2;
  CHECK(associationLinkDownEvent(newJoin, failed));
  Association oldLink = {}; oldLink.associated = oldLink.authorized = true; oldLink.result = 1;
  CHECK(associationLinkDownEvent(oldLink, newJoin));
  State st;
  Association connected = {};
  connected.associated = connected.authorized = true;
  observeAssociation(st, connected, true, true);
  for (unsigned i = 0; i < 4; ++i) {
    const Association old = observeAssociation(st, Association{}, false, false);
    CHECK(associationTransition(old, st.association).disconnected);
    CHECK(!st.power && st.eventPower);
  }
  Association old = observeAssociation(st, Association{}, false, true);
  CHECK(associationTransition(old, st.association).disconnected && !st.eventPower);
  old = observeAssociation(st, Association{}, false, true);
  CHECK(!associationTransition(old, st.association).disconnected);
  std::printf("user_control_host: %s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
