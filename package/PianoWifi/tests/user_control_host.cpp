// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// c++ -std=c++20 -Wall -Wextra -Werror -I.. user_control_host.cpp -o /tmp/user_control_host && /tmp/user_control_host
#include "WifiUserControl.h"
#include "WifiApple80211.h"
#include <cstdio>
using namespace pianowifi;
using namespace pianowifi::a80211;
static unsigned failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void joinReceipt()
{
  WifiJoinRequest request;
  request.cancel();
  CHECK(!request.currentReceipt(1, 0, 0, false, false));
  request.accept(1, 2, 0);
  CHECK(request.currentReceipt(2, 0, 0, false, false) == 1); // airportd pending at boot
  request.consume();
  CHECK(request.currentReceipt(3, 0, 1, true, false) == 1);
  CHECK(!request.currentReceipt(3, 0, 1, false, false)); // loss before authorization
  CHECK(!request.currentReceipt(2 + WifiJoinRequest::kTimeoutMs, 0, 1, true, false));
  CHECK(request.finish(1, 1, 4, 0));
  CHECK(request.currentReceipt(4, 0, 1, true, true) == 1); // live duplicate, including PS-only change
  CHECK(!request.currentReceipt(5, 0, 0, true, true)); // another station's generation
  CHECK(!request.currentReceipt(5, 0, 1, false, false)); // link loss without a new radio pass
  request.cancel(); // SET19 OFF
  CHECK(request.generation == 1 && !request.currentReceipt(6, 0, 1, true, true));
  CHECK(!request.currentReceipt(7, 1, 0, false, false)); // radio ready after ON
  request.accept(2, 8, 1);
  request.consume();
  CHECK(request.finish(2, 1, 9, 1));
  CHECK(request.currentReceipt(10, 1, 2, true, true) == 2); // radio retained on sleep/wake
  CHECK(!request.currentReceipt(11, 2, 2, true, true)); // old receipt across parked sleep/wake
  request.accept(3, 12, 2);
  CHECK(!request.currentReceipt(13, 3, 0, false, false)); // pending receipt across a restart
  request.consume();
  CHECK(request.finish(3, 1, 14, 3)); // queued request establishes a live association on the new pass
  CHECK(request.currentReceipt(14, 3, 3, true, true) == 3);
  request.accept(4, 15, 3);
  request.consume();
  CHECK(request.finish(4, 2, 16, 3));
  CHECK(!request.currentReceipt(16, 3, 4, true, false));
}

int main()
{
  joinReceipt();
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
