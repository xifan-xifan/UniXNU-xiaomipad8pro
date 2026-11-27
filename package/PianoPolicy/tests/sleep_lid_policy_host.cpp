// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. sleep_lid_policy_host.cpp -o /tmp/sleep_lid_policy_host && /tmp/sleep_lid_policy_host
#include "SleepLidPolicyModel.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace pianopolicy::host;
using pianopolicy::Lid;
using pianopolicy::Power;
using pianopolicy::kWakeLid;
using pianopolicy::kWakePowerKey;

namespace {

constexpr auto kUnknown = HallLevel::unknown;
constexpr auto kClosed = HallLevel::low;
constexpr auto kOpen = HallLevel::high;
constexpr uint32_t kSecondaries = 0xfe;
constexpr uint32_t kCpu4 = 1u << 4;
constexpr std::array kEffects{
    Effect{Subsystem::displayBacklight, true}, Effect{Subsystem::wifiBluetooth, true},
    Effect{Subsystem::touchCover, true}, Effect{Subsystem::timers, true},
    Effect{Subsystem::displayBacklight, false}, Effect{Subsystem::wifiBluetooth, false},
    Effect{Subsystem::touchCover, false}, Effect{Subsystem::timers, false},
};

void poll(SleepLidPolicyModel &model, uint64_t now, HallLevel hall, bool attached = true, bool powerDown = false)
{
  assert(model.poll(model.cycle(), now, {attached, hall, powerDown}));
}

void confirm(SleepLidPolicyModel &model, uint64_t start, HallLevel hall)
{
  poll(model, start, hall);
  poll(model, start + kFixtureDebounceMs, hall);
}

void ackCpus(SleepLidPolicyModel &model, uint64_t now, uint32_t mask = kSecondaries)
{
  const auto cycle = model.cycle();
  for (unsigned cpu = 0; cpu < 32; ++cpu) {
    if (mask & (uint32_t{1} << cpu)) assert(model.freezeAck(cycle, cpu, now));
  }
}

void checkQuiesced(const SleepLidPolicyModel &model)
{
  assert(model.suspendLockHeld() && !model.displayOn() && !model.coverInputEnabled());
  assert(std::ranges::equal(model.effects(), std::span(kEffects).first<4>()));
  for (const auto &effect : kEffects) assert(model.quiesced(effect.subsystem));
}

void checkRestored(const SleepLidPolicyModel &model, Outcome outcome)
{
  assert(model.phase() == Phase::awake && model.cycle() == 0 && model.outcome() == outcome);
  assert(!model.suspendLockHeld() && !model.lidWakeArmed());
  assert(model.parkedCpus() == 0 && model.frozenCpus() == 0);
  assert(std::ranges::equal(model.effects(), kEffects));
  for (const auto &effect : kEffects) assert(!model.quiesced(effect.subsystem));
}

void disarmedClamshell()
{
  for (const auto polarity : {HallPolarity::closedLow, HallPolarity::closedHigh}) {
    SleepLidPolicyModel model({.polarity = polarity});
    const auto closed = polarity == HallPolarity::closedLow ? kClosed : kOpen;
    const auto open = polarity == HallPolarity::closedLow ? kOpen : kClosed;
    confirm(model, 0, open);
    assert(model.displayOn() && model.coverInputEnabled());
    poll(model, 11, closed);
    poll(model, 20, closed);
    poll(model, 21, open);
    assert(model.lid() == Lid::open && model.displayOn() && model.lockRequests() == 0);
    confirm(model, 22, closed);
    assert(model.clamshellClosed() && !model.displayOn() && !model.coverInputEnabled() && model.lockRequests() == 1);
    poll(model, 33, closed);
    assert(model.phase() == Phase::awake && model.effects().empty() && !model.suspendLockHeld() &&
           !model.lidWakeArmed() && model.lockRequests() == 1);

    assert(model.requestSleep(34));
    ackCpus(model, 35);
    assert(model.phase() == Phase::asleep && !model.lidWakeArmed());
    confirm(model, 36, open);
    assert(model.phase() == Phase::asleep && !model.displayOn());
    poll(model, 47, open, true, true);
    checkRestored(model, Outcome::powerButton);
    assert(model.displayOn() && model.coverInputEnabled());
  }
}

void armedClamshell()
{
  for (const auto polarity : {HallPolarity::closedLow, HallPolarity::closedHigh}) {
    SleepLidPolicyModel model({.lidArmed = true, .polarity = polarity});
    const auto closed = polarity == HallPolarity::closedLow ? kClosed : kOpen;
    const auto open = polarity == HallPolarity::closedLow ? kOpen : kClosed;
    confirm(model, 0, closed);
    assert(model.clamshellClosed() && model.phase() == Phase::awake && model.lockRequests() == 1);
    confirm(model, 11, open);
    confirm(model, 22, closed);
    assert(model.phase() == Phase::freezing && model.clamshellClosed() && model.lockRequests() == 2);
    checkQuiesced(model);
    ackCpus(model, 34);
    assert(model.phase() == Phase::asleep && model.lidWakeArmed() && model.frozenCpus() == kSecondaries);

    poll(model, 35, open);
    poll(model, 44, open);
    poll(model, 45, closed);
    assert(model.phase() == Phase::asleep && model.clamshellClosed());
    poll(model, 46, open);
    poll(model, 55, open);
    assert(model.phase() == Phase::asleep);
    poll(model, 56, open);
    checkRestored(model, Outcome::lidOpen);
    assert(model.displayOn() && model.coverInputEnabled() && !model.clamshellClosed());
  }
}

void lidSleepEligibility()
{
  constexpr std::array lockOnly{
      Config{.power = Power::external, .lidSleepAC = true},
      Config{.lidArmed = true, .power = Power::external},
      Config{.lidArmed = true, .power = Power::unknown, .lidSleepAC = true},
      Config{.lidArmed = true, .sleepEnabled = false},
      Config{.lidArmed = true, .wakeMask = kWakePowerKey},
      Config{.lidArmed = true, .sleepMaxSeconds = 0},
  };
  for (const auto &config : lockOnly) {
    SleepLidPolicyModel model(config);
    confirm(model, 0, kOpen);
    confirm(model, 11, kClosed);
    assert(model.phase() == Phase::awake && model.clamshellClosed() && model.lockRequests() == 1);
    assert(model.effects().empty() && !model.displayOn() && !model.suspendLockHeld());
    confirm(model, 22, kOpen);
    assert(model.displayOn());
  }

  SleepLidPolicyModel optedIn({.lidArmed = true, .power = Power::external, .lidSleepAC = true});
  confirm(optedIn, 0, kOpen);
  confirm(optedIn, 11, kClosed);
  assert(optedIn.phase() == Phase::freezing);
  ackCpus(optedIn, 22);
  confirm(optedIn, 23, kOpen);
  checkRestored(optedIn, Outcome::lidOpen);
}

void unknownAndChatteringHall()
{
  SleepLidPolicyModel awake({.lidArmed = true});
  confirm(awake, 0, kOpen);
  for (uint64_t i = 0; i <= 20; ++i) poll(awake, 20 + 3 * i, i % 2 ? kOpen : kClosed);
  assert(awake.lid() == Lid::unknown && awake.displayOn() && awake.lockRequests() == 0);
  confirm(awake, 90, kClosed);
  assert(awake.phase() == Phase::awake && !awake.displayOn() && awake.lockRequests() == 1);
  for (uint64_t i = 0; i <= 20; ++i) poll(awake, 110 + 3 * i, i % 2 ? kClosed : kOpen);
  assert(awake.lid() == Lid::unknown && awake.displayOn() && awake.lockRequests() == 1);
  confirm(awake, 180, kClosed);
  poll(awake, 191, kUnknown);
  assert(awake.lid() == Lid::unknown && awake.displayOn() && !awake.coverInputEnabled());
  confirm(awake, 192, kOpen);
  assert(awake.coverInputEnabled());

  SleepLidPolicyModel asleep({.lidArmed = true, .wakeMask = kWakeLid, .sleepMaxSeconds = 1});
  confirm(asleep, 0, kOpen);
  confirm(asleep, 11, kClosed);
  ackCpus(asleep, 22);
  for (uint64_t i = 0; i <= 20; ++i) poll(asleep, 30 + 3 * i, i % 2 ? kClosed : kOpen);
  assert(asleep.phase() == Phase::asleep && asleep.lid() == Lid::unknown);
  poll(asleep, 1021, kUnknown, true, true);
  assert(asleep.phase() == Phase::asleep && !asleep.displayOn());
  poll(asleep, 1022, kUnknown, true, true);
  checkRestored(asleep, Outcome::timer);
  assert(asleep.displayOn());
}

void coverAttachment()
{
  SleepLidPolicyModel model({.lidArmed = true});
  confirm(model, 0, kOpen);
  poll(model, 11, kClosed);
  poll(model, 12, kClosed, false);
  assert(model.lid() == Lid::unknown && model.displayOn() && !model.coverInputEnabled());
  poll(model, 21, kClosed);
  assert(model.lid() == Lid::unknown && model.lockRequests() == 0);
  poll(model, 31, kClosed);
  assert(model.phase() == Phase::awake && model.clamshellClosed() && model.lockRequests() == 1);
  poll(model, 32, kClosed, false);
  assert(model.displayOn() && !model.clamshellClosed());
  confirm(model, 33, kOpen);
  assert(model.coverInputEnabled());
  confirm(model, 44, kClosed);
  ackCpus(model, 55);
  assert(model.lidWakeArmed());

  poll(model, 56, kOpen);  // Opening candidate from the old attachment.
  poll(model, 57, kOpen, false);
  assert(model.phase() == Phase::asleep && model.lid() == Lid::unknown && !model.lidWakeArmed());
  poll(model, 66, kOpen);
  assert(model.lid() == Lid::unknown);
  poll(model, 76, kOpen);
  assert(model.phase() == Phase::asleep && model.lid() == Lid::open && !model.coverInputEnabled());
  poll(model, 77, kOpen, false, true);
  checkRestored(model, Outcome::powerButton);
  assert(model.displayOn() && !model.coverInputEnabled());
  confirm(model, 78, kOpen);
  assert(model.coverInputEnabled());
}

void freezeTimeoutAndRetry()
{
  for (const bool lateAck : {false, true}) {
    SleepLidPolicyModel model;
    assert(model.requestSleep(10));
    const auto first = model.cycle();
    assert(model.parkedCpus() == kSecondaries && model.frozenCpus() == 0);
    checkQuiesced(model);
    assert(!model.requestSleep(11));
    assert(!model.freezeAck(first, 0, 11) && !model.freezeAck(first, 32, 11));
    ackCpus(model, 12, kSecondaries & ~kCpu4);
    assert(!model.freezeAck(first, 1, 12));
    assert(model.frozenCpus() == (kSecondaries & ~kCpu4));
    poll(model, 10 + kFreezeAckWindowMs, kUnknown);
    assert(model.phase() == Phase::freezing && model.parkedCpus() == kSecondaries && model.outcome() == Outcome::none);

    const auto expired = 11 + kFreezeAckWindowMs;
    if (lateAck) assert(!model.freezeAck(first, 4, expired));
    else poll(model, expired, kUnknown);
    checkRestored(model, Outcome::freezeTimeout);
    assert(model.missingAcks() == kCpu4 && model.displayOn());
    assert(!model.freezeAck(first, 4, expired + 1));
    assert(!model.poll(first, expired + 1, {true, kOpen, true}));

    assert(model.requestSleep(expired + 1));
    const auto second = model.cycle();
    assert(second != first && model.frozenCpus() == 0 && model.missingAcks() == 0);
    assert(!model.poll(first, expired + 5000, {true, kOpen, true}));
    assert(!model.freezeAck(first, 4, expired + 5000));
    ackCpus(model, expired + 2, kSecondaries & ~kCpu4);
    assert(model.freezeAck(second, 4, expired + 1 + kFreezeAckWindowMs));
    assert(model.phase() == Phase::asleep && model.frozenCpus() == kSecondaries);
    checkQuiesced(model);
    assert(!model.requestSleep(expired + 102));
    poll(model, expired + 103, kUnknown, false, true);
    checkRestored(model, Outcome::powerButton);
    assert(model.displayOn());
  }
}

void openingDuringFreeze()
{
  SleepLidPolicyModel model({.lidArmed = true});
  confirm(model, 0, kOpen);
  confirm(model, 11, kClosed);
  const auto first = model.cycle();
  ackCpus(model, 22, kSecondaries & ~kCpu4);
  confirm(model, 23, kOpen);
  checkRestored(model, Outcome::lidChanged);
  assert(model.displayOn() && model.coverInputEnabled() && model.missingAcks() == 0);
  assert(!model.freezeAck(first, 4, 34));

  confirm(model, 34, kClosed);
  ackCpus(model, 45);
  poll(model, 46, kUnknown);
  assert(model.phase() == Phase::asleep && model.lidWakeArmed());
  confirm(model, 47, kOpen);
  checkRestored(model, Outcome::lidOpen);
}

void closedLidAfterAbortAndWake()
{
  SleepLidPolicyModel model({.lidArmed = true, .sleepMaxSeconds = 1});
  confirm(model, 0, kOpen);
  confirm(model, 11, kClosed);
  ackCpus(model, 22, kSecondaries & ~kCpu4);
  poll(model, 122, kClosed);
  checkRestored(model, Outcome::freezeTimeout);
  assert(model.missingAcks() == kCpu4 && model.clamshellClosed() && !model.displayOn());
  poll(model, 200, kClosed);
  assert(model.phase() == Phase::awake && model.lockRequests() == 1);
  confirm(model, 201, kOpen);
  assert(model.displayOn() && model.coverInputEnabled());

  confirm(model, 212, kClosed);
  ackCpus(model, 223);
  poll(model, 250, kClosed, true, true);
  checkRestored(model, Outcome::powerButton);
  poll(model, 251, kClosed, true, true);
  assert(model.phase() == Phase::awake && !model.displayOn() && model.lockRequests() == 2);
  confirm(model, 252, kOpen);

  assert(model.requestSleep(270));
  ackCpus(model, 300);
  poll(model, 1223, kOpen);  // The previous sleep's timer would have expired here.
  assert(model.phase() == Phase::asleep);
  poll(model, 1300, kOpen);
  checkRestored(model, Outcome::timer);
  assert(model.displayOn() && model.coverInputEnabled());
}

void wakePriority()
{
  for (const auto expected : {Outcome::powerButton, Outcome::lidOpen, Outcome::timer}) {
    const bool openAtDeadline = expected != Outcome::timer;
    SleepLidPolicyModel model({.lidArmed = true, .sleepMaxSeconds = 1});
    confirm(model, 0, kClosed);
    assert(model.requestSleep(11));
    ackCpus(model, 61);
    const auto cycle = model.cycle();
    poll(model, 1051, openAtDeadline ? kOpen : kClosed);
    poll(model, 1060, openAtDeadline ? kOpen : kClosed);
    assert(model.phase() == Phase::asleep && model.outcome() == Outcome::none);
    poll(model, 1061, openAtDeadline ? kOpen : kClosed, true, expected == Outcome::powerButton);
    checkRestored(model, expected);
    assert(model.displayOn() == openAtDeadline);
    assert(!model.poll(cycle, 1062, {true, kOpen, true}));
    assert(model.outcome() == expected);
    if (!openAtDeadline) {
      poll(model, 1062, kClosed);
      assert(model.phase() == Phase::awake && model.lockRequests() == 1);
      confirm(model, 1063, kOpen);
      assert(model.displayOn());
    }
  }
}

void preSleepLevelsAndStaleSamples()
{
  SleepLidPolicyModel model({.lidArmed = true});
  confirm(model, 0, kClosed);
  assert(model.requestSleep(11));
  poll(model, 12, kOpen, true, true);
  ackCpus(model, 13);
  assert(model.lidWakeArmed());
  const auto cycle = model.cycle();
  poll(model, 22, kOpen, true, true);
  assert(model.phase() == Phase::asleep);  // The pre-sleep open candidate and held button cannot wake it.
  assert(!model.poll(cycle, 21, {true, kOpen, false}));
  assert(!model.poll(0, 22, {true, kClosed, false}));
  poll(model, 23, kClosed, true, true);
  assert(model.phase() == Phase::asleep);
  poll(model, 24, kClosed);
  poll(model, 25, kClosed, true, true);
  checkRestored(model, Outcome::powerButton);
  assert(!model.displayOn());

  confirm(model, 26, kOpen);
  assert(model.requestSleep(37));
  ackCpus(model, 38);
  assert(!model.lidWakeArmed());
  confirm(model, 39, kClosed);
  confirm(model, 50, kOpen);
  assert(model.phase() == Phase::asleep);  // Lid was open at entry and cannot become a wake source mid-cycle.
  poll(model, 62, kOpen, true, true);
  checkRestored(model, Outcome::powerButton);
}

void timerLimits()
{
  SleepLidPolicyModel uncapped({.sleepMaxSeconds = 0});
  assert(!uncapped.requestSleep(0));
  assert(uncapped.phase() == Phase::awake && uncapped.effects().empty());

  // The largest boot argument in seconds, with the model clock near its own limit.
  constexpr auto seconds = std::numeric_limits<uint32_t>::max();
  constexpr auto limit = std::numeric_limits<uint64_t>::max();
  constexpr uint64_t duration = uint64_t{seconds} * 1000;
  SleepLidPolicyModel bounded({.wakeMask = 0, .sleepMaxSeconds = seconds, .secondaryCpus = 1u << 31});
  assert(bounded.requestSleep(limit - duration - 1));
  ackCpus(bounded, limit - duration, 1u << 31);
  poll(bounded, limit - 1, kUnknown);
  assert(bounded.phase() == Phase::asleep);
  poll(bounded, limit, kUnknown);
  checkRestored(bounded, Outcome::timer);

  SleepLidPolicyModel singleCpu({.sleepMaxSeconds = 1, .secondaryCpus = 0});
  assert(singleCpu.requestSleep(0) && singleCpu.phase() == Phase::asleep);
  assert(singleCpu.parkedCpus() == 0 && singleCpu.frozenCpus() == 0);
  poll(singleCpu, 1000, kUnknown);
  checkRestored(singleCpu, Outcome::timer);
}

}  // namespace

int main()
{
  disarmedClamshell();
  armedClamshell();
  lidSleepEligibility();
  unknownAndChatteringHall();
  coverAttachment();
  freezeTimeoutAndRetry();
  openingDuringFreeze();
  closedLidAfterAbortAndWake();
  wakePriority();
  preSleepLevelsAndStaleSamples();
  timerLimits();
  std::puts("sleep_lid_policy_host: all Goal 9 scenarios passed");
}
