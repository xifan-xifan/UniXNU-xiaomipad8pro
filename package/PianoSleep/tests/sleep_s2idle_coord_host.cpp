// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer sleep_s2idle_coord_host.cpp -o /tmp/sleep_s2idle_coord_host && /tmp/sleep_s2idle_coord_host
//
// Virtual-time model of IOCPUSleepKernel and piano_s2idle_enter at piano-sleep-state=1
// (k27 0062–0064, docs/power.md §3–4).
// The wake decoder and cancel decisions are the kernel's. Driver ACKs, IRQ masking, IPIs and the
// virtual timer are modeled as serial events so both orders of a race can be reproduced offline.

#include "../../../tooling/k27/piano-pm/piano_s2idle_policy.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace {

using Milliseconds = uint64_t;
constexpr unsigned kCpuCount = 8;
constexpr unsigned kBootCpu = 0;
constexpr uint32_t bit(unsigned index) { return uint32_t{1} << index; }
constexpr uint32_t kAllCpus = bit(kCpuCount) - 1;
constexpr uint32_t kSecondaries = kAllCpus & ~bit(kBootCpu);
constexpr Milliseconds kFreezeWindow = 100;
constexpr Milliseconds kPollInterval = 100;
constexpr uint32_t kKnownSources =
    PIANO_SRC_POWER | PIANO_SRC_LID | PIANO_SRC_USB | PIANO_SRC_VOLUME_DOWN | PIANO_SRC_KEYBOARD;

struct BootArgs {
  uint32_t sleep = 0;
  uint32_t wake = PIANO_SRC_POWER;
  int64_t maxSeconds = 0;
  uint32_t cancel = 0;
  bool scheduledTest = false;
};

// Bound the model's numeric fixtures to the kernel's uint32_t seconds field. The kernel uses the
// parsed uint32_t directly, with zero disabling timed wake (or giving harness sleeps 30 seconds).
constexpr Milliseconds timerLimit(BootArgs args)
{
  const auto seconds = std::clamp<int64_t>(args.maxSeconds, 0, UINT32_MAX);
  return Milliseconds{1000} * (seconds == 0 && args.scheduledTest ? 30 : seconds);
}

constexpr BootArgs kTestArgs{.sleep = 1, .wake = 19, .maxSeconds = 60, .cancel = 1};

struct WakeSample {
  uint8_t pon = 0;
  uint32_t hall = 1;
  uint32_t keyboard = 0;
  uint8_t usb = 0;
};

enum class Phase { awake, driversGoingOff, parking, freezing, preWfi, asleep, releasing, driversGoingOn };
enum class Driver { wifi, audio, backlight, touch, count };
constexpr auto kAllDrivers = bit(static_cast<unsigned>(Driver::count)) - 1;
constexpr std::array kDrivers{Driver::wifi, Driver::audio, Driver::backlight, Driver::touch};
constexpr std::array<unsigned, 7> kWorkers{1, 2, 3, 4, 5, 6, 7};

// Non-source reasons from the kernel's piano_pm.h (0062 and 0064).
enum Reason { none = 0, aborted = 1, timer = 6, cancelled = 9 };
enum class Step { driverOff, preSleep, queueQuiesce, restoreTimer, queueActive, cpusReady,
                  postResume, driverOn, policyPoweredOn };

struct Effect {
  Step step;
  Driver driver = Driver::count;
  bool operator==(const Effect &) const = default;
};

struct Coordinator {
  explicit Coordinator(BootArgs bootArgs) : args(bootArgs) {}

  const BootArgs args;
  Phase phase = Phase::awake;
  unsigned attempt = 0;
  uint32_t parked = 0;
  uint32_t frozen = 0;
  uint32_t missing = 0;
  uint32_t driversOff = 0;
  int reason = none;
  Milliseconds slept = 0;
  std::optional<Milliseconds> pollDue;
  piano_cancel cancelWindow{};
  std::vector<Effect> effects;

  bool requestSleep()
  {
    if (args.sleep != 1 || phase != Phase::awake) return false;
    ++attempt;
    missing = 0;
    reason = none;
    slept = 0;
    effects.clear();
    piano_cancel_open(&cancelWindow, attempt, true);
    phase = Phase::driversGoingOff;
    return true;
  }

  bool driverOffAck(unsigned cycle, Driver driver)
  {
    if (cycle != attempt || phase != Phase::driversGoingOff || (driversOff & driverBit(driver))) return false;
    driversOff |= driverBit(driver);
    effects.push_back({Step::driverOff, driver});
    return true;
  }

  bool preSleep(WakeSample sample)
  {
    if (phase != Phase::driversGoingOff || driversOff != kAllDrivers) return false;
    piano_cancel_open(&cancelWindow, attempt, false);  // preserves a kOff cancel
    effects.push_back({Step::preSleep});
    phase = Phase::parking;
    if (piano_cancel_pending(&cancelWindow)) {
      finishPark(0);
    } else {
      piano_pm_wake_arm(&wake_, args.wake, true, sample.pon, true, sample.hall,
                        true, PIANO_TLMM_RAW_STATUS, sample.keyboard, true, sample.usb);
    }
    return true;
  }

  int cancel(const char *who)
  {
    return piano_cancel_request(&cancelWindow, args.sleep == 1 && args.cancel == 1, who, "suspend veto");
  }

  bool parkAck(unsigned cycle, unsigned cpu)
  {
    if (cycle != attempt || phase != Phase::parking || cpu == kBootCpu || cpu >= kCpuCount ||
        (parked & bit(cpu))) return false;
    parked |= bit(cpu);
    return true;
  }

  bool finishPark(Milliseconds now, bool forceLocalFailure = false)
  {
    if (phase != Phase::parking || (parked != kSecondaries && !piano_cancel_pending(&cancelWindow))) return false;
    if (piano_cancel_shut(&cancelWindow)) {
      wake(cancelled);
    } else if (forceLocalFailure) {
      // Fault injection at CPU0's local IRQ-mask step. The hardware only waits for remote ACKs.
      missing = bit(kBootCpu);
      wake(aborted);
    } else {
      frozen = bit(kBootCpu);
      freezeNow_ = now;
      freezeDeadline_ = now + kFreezeWindow;
      phase = Phase::freezing;
    }
    return true;
  }

  bool freezeAck(unsigned cycle, unsigned cpu, Milliseconds now, bool failed = false)
  {
    if (cpu == kBootCpu || cpu >= kCpuCount || !advanceFreeze(cycle, now) || (frozen & bit(cpu))) return false;
    if (failed) {
      missing = bit(cpu);
      wake(aborted);
    } else {
      frozen |= bit(cpu);
      if (frozen == kAllCpus) phase = Phase::preWfi;
    }
    return true;
  }

  bool advanceFreeze(unsigned cycle, Milliseconds now)
  {
    if (cycle != attempt || phase != Phase::freezing || now < freezeNow_) return false;
    freezeNow_ = now;
    // The kernel checks for the missing ACKs with a strict 'now > deadline' comparison.
    if (now <= freezeDeadline_) return true;
    missing = kAllCpus & ~frozen;
    wake(aborted);
    return false;
  }

  uint32_t armedSources() const { return wake_.src & kKnownSources; }

  bool enterWfi(Milliseconds now, WakeSample sample)
  {
    if (phase != Phase::preWfi || now < freezeNow_) return false;
    sleepStart_ = now;
    phase = Phase::asleep;
    effects.push_back({Step::queueQuiesce});
    checkWake(now, sample);
    return true;
  }

  bool poll(unsigned cycle, Milliseconds now, WakeSample sample)
  {
    if (cycle != attempt || phase != Phase::asleep || pollDue != now) return false;
    checkWake(now, sample);
    return true;
  }

  bool unparkAck(unsigned cycle, unsigned cpu)
  {
    if (cycle != attempt || phase != Phase::releasing || cpu >= kCpuCount || !(parked & bit(cpu))) return false;
    parked &= ~bit(cpu);
    frozen &= ~bit(cpu);
    return true;
  }

  bool postResume()
  {
    if (phase != Phase::releasing || parked != 0) return false;
    frozen = 0;  // On normal wake, CPU0 restores interrupts after the secondaries leave the park.
    effects.push_back({Step::cpusReady});
    piano_cancel_close(&cancelWindow);
    effects.push_back({Step::postResume});
    phase = Phase::driversGoingOn;
    return true;
  }

  bool driverOnAck(unsigned cycle, Driver driver)
  {
    if (cycle != attempt || phase != Phase::driversGoingOn || !(driversOff & driverBit(driver))) return false;
    driversOff &= ~driverBit(driver);
    effects.push_back({Step::driverOn, driver});
    return true;
  }

  bool notifyPoweredOn()
  {
    if (phase != Phase::driversGoingOn || driversOff != 0) return false;
    effects.push_back({Step::policyPoweredOn});  // PianoPolicy observes kIOMessageSystemHasPoweredOn.
    phase = Phase::awake;
    return true;
  }

 private:
  static uint32_t driverBit(Driver driver) { return bit(static_cast<unsigned>(driver)); }

  void checkWake(Milliseconds now, WakeSample sample)
  {
    const int source = piano_pm_wake_check(&wake_, true, sample.pon, sample.hall, sample.keyboard,
                                          true, sample.usb);
    const Milliseconds elapsed = now - sleepStart_;
    const Milliseconds limit = timerLimit(args);
    if (source != none || (limit != 0 && elapsed >= limit)) {
      slept = elapsed;
      wake(source != none ? source : timer);
    } else {
      pollDue = now + kPollInterval;
    }
  }

  void wake(int why)
  {
    reason = why;
    pollDue.reset();
    wake_ = {};
    if (phase == Phase::asleep) {
      effects.push_back({Step::restoreTimer});
      effects.push_back({Step::queueActive});
    } else {
      frozen &= ~bit(kBootCpu);  // The abort path restores local interrupts before waiting for the park to drain.
    }
    phase = Phase::releasing;
  }

  piano_pm_wake wake_{};
  Milliseconds freezeNow_ = 0;
  Milliseconds freezeDeadline_ = 0;
  Milliseconds sleepStart_ = 0;
};

struct Cycle {
  explicit Cycle(Coordinator &coord, std::mt19937 &random) : sleep(coord)
  {
    std::shuffle(offOrder.begin(), offOrder.end(), random);
    std::shuffle(onOrder.begin(), onOrder.end(), random);
    std::shuffle(cpus.begin(), cpus.end(), random);
    assert(sleep.requestSleep());
    id = sleep.attempt;
    assert(!sleep.requestSleep());
    assert(!sleep.preSleep({}));
    assert(!sleep.driverOffAck(id - 1, offOrder.front()));
  }

  void driversDown()
  {
    for (auto driver : offOrder) {
      assert(sleep.driverOffAck(id, driver));
      assert(!sleep.driverOffAck(id, driver));
    }
  }

  void parkAll()
  {
    assert(!sleep.parkAck(id, kBootCpu));
    for (auto cpu : cpus) assert(sleep.parkAck(id, cpu));
  }

  void freezeAll()
  {
    assert(sleep.finishPark(0));
    assert(sleep.frozen == bit(kBootCpu));
    assert(!sleep.freezeAck(id - 1, cpus.front(), 1));
    assert(!sleep.freezeAck(id, 32, 1));
    Milliseconds at = 1;
    for (auto cpu : cpus) assert(sleep.freezeAck(id, cpu, at++));
    assert(sleep.phase == Phase::preWfi && sleep.frozen == kAllCpus && !sleep.pollDue);
  }

  void startWfi(WakeSample baseline = {})
  {
    driversDown();
    assert(sleep.preSleep(baseline));
    parkAll();
    freezeAll();
    assert(sleep.enterWfi(wfiAt, baseline));
    assert(!sleep.enterWfi(wfiAt, baseline));
  }

  void waitForTimer(unsigned seconds, WakeSample sample = {})
  {
    const auto deadline = wfiAt + Milliseconds{1000} * seconds;
    for (auto at = wfiAt + kPollInterval; at <= deadline; at += kPollInterval) {
      assert(sleep.pollDue == at && sleep.reason == none);
      assert(sleep.poll(id, at, sample));
    }
    assert(sleep.reason == timer && sleep.slept == deadline - wfiAt);
  }

  void resume(int expectedReason, bool fromWfi)
  {
    assert(sleep.phase == Phase::releasing && sleep.reason == expectedReason && !sleep.pollDue);
    assert(sleep.armedSources() == 0);
    if (fromWfi) {
      assert(sleep.parked == kSecondaries && sleep.frozen == kAllCpus);
    } else {
      assert((sleep.frozen & bit(kBootCpu)) == 0);
    }
    assert(!sleep.requestSleep() && !sleep.poll(id, wfiAt + kPollInterval, {}));
    assert(!sleep.driverOnAck(id, onOrder.front()) && !sleep.notifyPoweredOn());
    for (auto cpu : cpus) {
      if (!(sleep.parked & bit(cpu))) continue;
      assert(!sleep.postResume());
      assert(!sleep.unparkAck(id - 1, cpu));
      assert(sleep.unparkAck(id, cpu));
      assert(!sleep.unparkAck(id, cpu));
    }
    assert(sleep.parked == 0 && (sleep.frozen & kSecondaries) == 0);
    assert(sleep.postResume() && !sleep.postResume());
    assert(sleep.parked == 0 && sleep.frozen == 0 && sleep.cancelWindow.window == PIANO_WINDOW_CLOSED);
    for (auto driver : onOrder) {
      assert(!sleep.notifyPoweredOn());
      assert(!sleep.driverOnAck(id - 1, driver));
      assert(sleep.driverOnAck(id, driver));
      assert(!sleep.driverOnAck(id, driver));
    }
    assert(sleep.notifyPoweredOn() && !sleep.notifyPoweredOn());
    assert(sleep.phase == Phase::awake && sleep.driversOff == 0 && !sleep.pollDue);

    std::vector<Effect> expected;
    for (auto driver : offOrder) expected.push_back({Step::driverOff, driver});
    expected.push_back({Step::preSleep});
    if (fromWfi) {
      expected.push_back({Step::queueQuiesce});
      expected.push_back({Step::restoreTimer});
      expected.push_back({Step::queueActive});
    } else {
      assert(sleep.slept == 0);
    }
    expected.push_back({Step::cpusReady});
    expected.push_back({Step::postResume});
    for (auto driver : onOrder) expected.push_back({Step::driverOn, driver});
    expected.push_back({Step::policyPoweredOn});
    assert(sleep.effects == expected);
  }

  static constexpr Milliseconds wfiAt = 120;
  Coordinator &sleep;
  unsigned id;
  std::array<Driver, kDrivers.size()> offOrder = kDrivers, onOrder = kDrivers;
  std::array<unsigned, kWorkers.size()> cpus = kWorkers;
};

void nominalCycles(std::mt19937 &random)
{
  for (unsigned seconds : {60, 90}) {
    auto args = kTestArgs;
    args.maxSeconds = seconds;
    Coordinator sleep(args);
    for (unsigned i = 0; i < 64; ++i) {
      Cycle cycle(sleep, random);
      cycle.startWfi();
      assert(sleep.armedSources() == (PIANO_SRC_POWER | PIANO_SRC_KEYBOARD));
      assert(!sleep.poll(cycle.id - 1, Cycle::wfiAt + kPollInterval, {.keyboard = 1}));
      assert(!sleep.poll(cycle.id, Cycle::wfiAt + kPollInterval - 1, {.keyboard = 1}));
      cycle.waitForTimer(seconds, {.pon = PIANO_PON_RESIN, .usb = 1});
      cycle.resume(timer, true);
    }
    assert(sleep.attempt == 64 && sleep.cancelWindow.total == 0);
  }
}

void wakeSources(std::mt19937 &random)
{
  Coordinator sleep(kTestArgs);
  {
    Cycle cycle(sleep, random);
    cycle.startWfi({.pon = PIANO_PON_KPDPWR, .hall = 0});
    assert(sleep.armedSources() == (PIANO_SRC_POWER | PIANO_SRC_LID));
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.pon = PIANO_PON_KPDPWR, .keyboard = 1}));
    assert(sleep.reason == none);  // Held power key, one open sample and an unarmed keyboard.
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.hall = 0}));
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {}));
    assert(sleep.reason == none);  // The bounce restarted the two-sample cover debounce.
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {}));
    cycle.resume(PIANO_PM_WAKE_LID, true);
  }
  {
    Cycle cycle(sleep, random);
    cycle.startWfi();
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.keyboard = 1}));
    cycle.resume(PIANO_PM_WAKE_KEYBOARD, true);
  }
  {
    Cycle cycle(sleep, random);
    cycle.startWfi({.pon = PIANO_PON_KPDPWR, .keyboard = 1});
    assert(sleep.armedSources() == PIANO_SRC_POWER);
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {}));  // Release the key.
    assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.pon = PIANO_PON_KPDPWR}));
    cycle.resume(PIANO_PM_WAKE_POWER_KEY, true);
  }
}

void freezeAborts(std::mt19937 &random)
{
  Coordinator sleep(kTestArgs);
  for (unsigned missing = 0; missing < kCpuCount; ++missing) {
    Cycle cycle(sleep, random);
    cycle.driversDown();
    assert(sleep.preSleep({}));
    cycle.parkAll();
    assert(sleep.finishPark(0, missing == kBootCpu));
    if (missing != kBootCpu) {
      for (auto cpu : cycle.cpus) {
        if (cpu != missing) assert(sleep.freezeAck(cycle.id, cpu, 1));
      }
      assert(sleep.freezeAck(cycle.id, missing, 2, true));
    }
    assert(sleep.parked == kSecondaries && sleep.missing == bit(missing));
    assert(!sleep.enterWfi(2, {}));
    cycle.resume(aborted, false);

    Cycle retry(sleep, random);
    retry.startWfi();
    assert(sleep.poll(retry.id, sleep.pollDue.value(), {.keyboard = 1}));
    retry.resume(PIANO_PM_WAKE_KEYBOARD, true);
  }

  // Seven consecutive attempts, with the historical CPU4 miss on the seventh. The other
  // missed secondaries exercise the same natural timeout with a different ACK order.
  constexpr std::array misses{0u, 0u, 0u, 0u, 0u, 0u, 4u, 1u, 2u, 3u, 5u, 6u, 7u, 0u};
  for (unsigned missing : misses) {
    Cycle cycle(sleep, random);
    cycle.driversDown();
    assert(sleep.preSleep({}));
    cycle.parkAll();
    assert(sleep.finishPark(10));
    for (auto cpu : cycle.cpus) {
      if (cpu != missing && cpu != cycle.cpus.back()) assert(sleep.freezeAck(cycle.id, cpu, 11));
    }
    const auto deadline = 10 + kFreezeWindow;
    assert(sleep.advanceFreeze(cycle.id, deadline));
    if (cycle.cpus.back() != missing) assert(sleep.freezeAck(cycle.id, cycle.cpus.back(), deadline));
    if (missing != 0) {
      assert(sleep.phase == Phase::freezing && sleep.reason == none);
      assert(sleep.cancel("freeze deadline") == PIANO_CANCEL_LATE);
      assert(!sleep.advanceFreeze(cycle.id, deadline + 1));
      assert(sleep.missing == bit(missing) && sleep.parked == kSecondaries);
      assert(!sleep.freezeAck(cycle.id, missing, deadline + 1));
      cycle.resume(aborted, false);
    } else {
      assert(sleep.missing == 0 && sleep.frozen == kAllCpus);
      assert(sleep.enterWfi(Cycle::wfiAt, {}));
      assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.keyboard = 1}));
      cycle.resume(PIANO_PM_WAKE_KEYBOARD, true);
    }
  }
}

void cancelRaces(std::mt19937 &random)
{
  auto args = kTestArgs;
  args.maxSeconds = 1;
  Coordinator sleep(args);
  {
    Cycle cycle(sleep, random);
    assert(sleep.driverOffAck(cycle.id, cycle.offOrder.front()));
    assert(sleep.cancel("PianoAudio") == PIANO_CANCEL_TAKEN);
    assert(sleep.cancel("PianoWifi") == PIANO_CANCEL_TAKEN);
    for (auto driver : cycle.offOrder) {
      if (driver != cycle.offOrder.front()) assert(sleep.driverOffAck(cycle.id, driver));
    }
    assert(sleep.preSleep({}));
    assert(sleep.parked == 0 && sleep.frozen == 0 && !sleep.pollDue);
    assert(sleep.cancelWindow.callers == 2 && sleep.cancelWindow.total == 1);
    assert(std::string_view(sleep.cancelWindow.who) == "PianoAudio");
    cycle.resume(cancelled, false);
  }
  for (unsigned alreadyParked : {3, 7}) {
    Cycle cycle(sleep, random);
    cycle.driversDown();
    assert(sleep.preSleep({}) && !sleep.finishPark(0));
    for (unsigned i = 0; i < alreadyParked; ++i) assert(sleep.parkAck(cycle.id, cycle.cpus[i]));
    assert(sleep.cancel("PianoAudio") == PIANO_CANCEL_TAKEN);
    assert(sleep.finishPark(0) && sleep.frozen == 0);
    assert(!sleep.parkAck(cycle.id, cycle.cpus.back()));
    cycle.resume(cancelled, false);
  }
  for (bool sourceAtDeadline : {false, true}) {
    Cycle cycle(sleep, random);
    cycle.driversDown();
    assert(sleep.preSleep({}));
    cycle.parkAll();
    assert(sleep.finishPark(0));
    assert(sleep.freezeAck(cycle.id, cycle.cpus.front(), 1));
    assert(sleep.cancel("mid-freeze") == PIANO_CANCEL_LATE);
    for (unsigned i = 1; i < cycle.cpus.size(); ++i) assert(sleep.freezeAck(cycle.id, cycle.cpus[i], 2));
    assert(sleep.phase == Phase::preWfi && sleep.cancel("pre-WFI") == PIANO_CANCEL_LATE);
    assert(sleep.enterWfi(Cycle::wfiAt, {}));
    const auto deadline = Cycle::wfiAt + 1000;
    while (sleep.pollDue.value() < deadline) {
      assert(sleep.poll(cycle.id, sleep.pollDue.value(), {}));
      assert(sleep.pollDue);
    }
    assert(sleep.cancel("just before timer") == PIANO_CANCEL_LATE && sleep.pollDue == deadline);
    WakeSample sample{.pon = sourceAtDeadline ? uint8_t{PIANO_PON_KPDPWR} : uint8_t{0}};
    assert(sleep.poll(cycle.id, deadline, sample));
    assert(sleep.cancel("after wake selected") == PIANO_CANCEL_LATE && !sleep.pollDue);
    assert(!sleep.poll(cycle.id, deadline, sample));
    assert(sleep.slept == 1000);
    cycle.resume(sourceAtDeadline ? PIANO_PM_WAKE_POWER_KEY : timer, true);
    assert(sleep.cancel("after PostResume") == PIANO_CANCEL_NONE);
  }
  assert(sleep.cancelWindow.total == 3 && sleep.cancelWindow.refused == 10);

  Cycle next(sleep, random);
  next.startWfi();
  next.waitForTimer(1);
  next.resume(timer, true);
}

void bootPolicy(std::mt19937 &random)
{
  for (auto args : {BootArgs{}, BootArgs{.sleep = 0, .wake = 19, .maxSeconds = 90, .cancel = 1},
                    BootArgs{.sleep = 2, .cancel = 1}}) {
    Coordinator disabled(args);
    assert(!disabled.requestSleep() && disabled.cancel("PianoAudio") == PIANO_CANCEL_OFF);
    assert(disabled.attempt == 0 && disabled.phase == Phase::awake && disabled.effects.empty() && !disabled.pollDue);
  }

  assert(timerLimit({.maxSeconds = INT64_MIN}) == 0);
  assert(timerLimit({.maxSeconds = 0}) == 0);
  assert(timerLimit({.maxSeconds = 1}) == 1000);
  assert(timerLimit({.maxSeconds = UINT32_MAX - 1}) == 4294967294000ULL);
  assert(timerLimit({.maxSeconds = INT64_MAX}) == 4294967295000ULL);
  assert(timerLimit({.scheduledTest = true}) == 30000);
  assert(timerLimit({.maxSeconds = 90, .scheduledTest = true}) == 90000);

  for (uint32_t mask : {0u, 0x80000000u, UINT32_MAX}) {
    auto args = kTestArgs;
    args.wake = mask;
    args.maxSeconds = 1;
    args.cancel = 0;
    Coordinator sleep(args);
    Cycle cycle(sleep, random);
    assert(sleep.cancel("PianoAudio") == PIANO_CANCEL_OFF);
    cycle.startWfi();
    if (mask == UINT32_MAX) {
      assert(sleep.armedSources() == (kKnownSources & ~PIANO_SRC_LID));
      assert(sleep.poll(cycle.id, sleep.pollDue.value(), {.usb = 1}));
      cycle.resume(PIANO_PM_WAKE_USB, true);
    } else {
      assert(sleep.armedSources() == 0);
      cycle.waitForTimer(1, {.pon = PIANO_PON_KPDPWR | PIANO_PON_RESIN, .keyboard = 1, .usb = 1});
      cycle.resume(timer, true);
    }
  }

  Coordinator noLimit({.sleep = 1});
  Cycle cycle(noLimit, random);
  cycle.startWfi();
  assert(noLimit.armedSources() == PIANO_SRC_POWER);
  for (unsigned i = 0; i < 1000; ++i) {
    assert(noLimit.poll(cycle.id, noLimit.pollDue.value(), {}));
    assert(noLimit.pollDue && noLimit.reason == none);
  }
  assert(noLimit.poll(cycle.id, noLimit.pollDue.value(), {.pon = PIANO_PON_KPDPWR}));
  cycle.resume(PIANO_PM_WAKE_POWER_KEY, true);
}

}  // namespace

int main()
{
  std::mt19937 random(0x5a2d1e);
  nominalCycles(random);
  wakeSources(random);
  freezeAborts(random);
  cancelRaces(random);
  bootPolicy(random);
  std::puts("PianoSleep s2idle: 128 timer cycles, all CPU fault injection, natural freeze timeouts, wake policy and cancel races passed");
}
