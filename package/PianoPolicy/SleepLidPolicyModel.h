// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline Goal 9 fixture. See README.md for the hardware references and Fix-image gate.
#pragma once

#include "Policy.h"

#include <array>
#include <cstddef>
#include <span>

namespace pianopolicy::host {

constexpr uint64_t kFixtureDebounceMs = 10;
constexpr uint64_t kFixtureChatterMs = 50;
constexpr uint64_t kFreezeAckWindowMs = 100;  // k27 0062, piano_s2idle_enter

enum class HallLevel { unknown, low, high };
enum class HallPolarity { closedLow, closedHigh };
enum class Phase { awake, freezing, asleep };
enum class Outcome { none, powerButton, lidOpen, timer, freezeTimeout, lidChanged };
enum class Subsystem { displayBacklight, wifiBluetooth, touchCover, timers };

struct Effect {
  Subsystem subsystem;
  bool quiesced;
  bool operator==(const Effect &) const = default;
};

struct Config {
  bool lidArmed = false;  // Disarmed Goal 9 fixture (piano-policy-lid-arm=0).
  HallPolarity polarity = HallPolarity::closedLow;
  Power power = Power::battery;
  bool lidSleepAC = false;
  bool sleepEnabled = true;
  uint32_t wakeMask = kWakePowerKey | kWakeLid;
  uint32_t sleepMaxSeconds = 2;  // Synthetic piano-sleep-max. A bounded timer is required in this model.
  uint32_t secondaryCpus = 0xfe;  // Fixture topology: CPU0 coordinates, CPUs 1–7 park.
};

struct Sample {
  bool attached = false;
  HallLevel hall = HallLevel::unknown;
  bool powerDown = false;
};

// Callers serialize observations on a synthetic monotonic clock. Save cycle() when capturing an observation or ACK
// for delivery later. Awake observations use cycle 0. Stale deliveries cannot advance the clock or change state.
class SleepLidPolicyModel {
 public:
  explicit SleepLidPolicyModel(Config config = {}) : config_(config) {}

  Phase phase() const { return phase_; }
  uint64_t cycle() const { return phase_ == Phase::awake ? 0 : generation_; }
  Lid lid() const { return lid_; }
  bool clamshellClosed() const { return lid_ == Lid::closed; }
  bool displayOn() const { return !clamshellClosed() && !quiesced(Subsystem::displayBacklight); }
  bool coverInputEnabled() const
  {
    return attached_ && lid_ == Lid::open && displayOn() && !quiesced(Subsystem::touchCover);
  }
  unsigned lockRequests() const { return lockRequests_; }
  bool suspendLockHeld() const { return suspendLockHeld_; }
  bool lidWakeArmed() const { return lidWakeArmed_; }
  uint32_t parkedCpus() const { return parkedCpus_; }
  uint32_t frozenCpus() const { return frozenCpus_; }
  uint32_t missingAcks() const { return missingAcks_; }
  Outcome outcome() const { return outcome_; }
  bool quiesced(Subsystem subsystem) const { return quiesced_[static_cast<std::size_t>(subsystem)]; }
  std::span<const Effect> effects() const { return {effects_.data(), effectCount_}; }

  bool requestSleep(uint64_t nowMs)
  {
    if (phase_ != Phase::awake || !config_.sleepEnabled || !config_.sleepMaxSeconds || nowMs < nowMs_) return false;
    nowMs_ = nowMs;
    beginSleep(false);
    return true;
  }

  bool freezeAck(uint64_t cycle, unsigned cpu, uint64_t nowMs)
  {
    if (phase_ != Phase::freezing || cycle != this->cycle() || !advance(nowMs) || phase_ != Phase::freezing) return false;
    if (cpu >= 32) return false;
    const uint32_t bit = uint32_t{1} << cpu;
    if (!(parkedCpus_ & bit) || (frozenCpus_ & bit)) return false;
    frozenCpus_ |= bit;
    if (frozenCpus_ == parkedCpus_) enterSleep();
    return true;
  }

  bool poll(uint64_t cycle, uint64_t nowMs, Sample sample)
  {
    if (cycle != this->cycle() || !advance(nowMs)) return false;
    const Lid previous = lid_;
    updateHall(sample);
    const bool powerPress = sample.powerDown && !powerDown_;
    powerDown_ = sample.powerDown;
    const bool closed = clamshellClosed();
    const bool justClosed = closed && previous != Lid::closed;
    if (justClosed) ++lockRequests_;

    if (phase_ == Phase::freezing && lidInitiated_ && !closed) {
      restore(Outcome::lidChanged);
    } else if (phase_ == Phase::asleep) {
      // Match the precedence of the modeled sources in k27 0062, at a single poll.
      if ((config_.wakeMask & kWakePowerKey) && powerPress) restore(Outcome::powerButton);
      else if (lidWakeArmed_ && lid_ == Lid::open && previous != Lid::open) restore(Outcome::lidOpen);
      else if (nowMs_ - sleepStartMs_ >= uint64_t{config_.sleepMaxSeconds} * 1000) restore(Outcome::timer);
    } else if (cycle == 0 && justClosed && canSleepOnClose()) {
      beginSleep(true);
    }
    return true;
  }

 private:
  static constexpr std::array kOrder{Subsystem::displayBacklight, Subsystem::wifiBluetooth, Subsystem::touchCover,
                                     Subsystem::timers};

  bool advance(uint64_t nowMs)
  {
    if (nowMs < nowMs_) return false;
    nowMs_ = nowMs;
    // ACKs at the deadline count. A later observation expires the window before processing its ACK or edges.
    if (phase_ == Phase::freezing && nowMs_ - freezeStartMs_ > kFreezeAckWindowMs) {
      missingAcks_ = parkedCpus_ & ~frozenCpus_;
      restore(Outcome::freezeTimeout);
    }
    return true;
  }

  void invalidateHall()
  {
    lid_ = candidate_ = Lid::unknown;
    seenOpen_ = unstable_ = false;
  }

  void updateHall(Sample sample)
  {
    if (attached_ != sample.attached) {
      attached_ = sample.attached;
      lidWakeArmed_ = false;
      invalidateHall();
    }
    if (!attached_ || sample.hall == HallLevel::unknown) {
      invalidateHall();
      return;
    }
    const bool closed = (sample.hall == HallLevel::low) == (config_.polarity == HallPolarity::closedLow);
    const Lid reading = closed ? Lid::closed : Lid::open;
    if (reading != candidate_) {
      candidate_ = reading;
      candidateSinceMs_ = nowMs_;
      if (lid_ != Lid::unknown && !unstable_) {
        unstable_ = true;
        unstableSinceMs_ = nowMs_;
      }
    }
    if (nowMs_ - candidateSinceMs_ >= kFixtureDebounceMs) {
      lid_ = candidate_;
      seenOpen_ |= lid_ == Lid::open;
      unstable_ = false;
    } else if (unstable_ && nowMs_ - unstableSinceMs_ >= kFixtureChatterMs) {
      invalidateHall();
    }
  }

  bool canSleepOnClose() const
  {
    pianopolicy::Config policy;
    policy.lid = true;
    policy.lidSleepAC = config_.lidSleepAC;
    Inputs in;
    in.lid = lid_;
    in.power = config_.power;
    in.lidSleepArmed = config_.lidArmed;
    in.sleepOn = config_.sleepEnabled;
    in.wakeMask = config_.wakeMask;
    return config_.sleepMaxSeconds != 0 && view(policy, in, seenOpen_).close == Close::sleep;
  }

  void setSubsystems(bool quiesced)
  {
    for (const auto subsystem : kOrder) {
      quiesced_[static_cast<std::size_t>(subsystem)] = quiesced;
      effects_[effectCount_++] = {subsystem, quiesced};
    }
  }

  void beginSleep(bool lidInitiated)
  {
    ++generation_;
    effectCount_ = 0;
    outcome_ = Outcome::none;
    missingAcks_ = 0;
    suspendLockHeld_ = true;
    lidInitiated_ = lidInitiated;
    setSubsystems(true);

    // Model a successful park phase followed by one independent freeze ACK per parked secondary.
    parkedCpus_ = config_.secondaryCpus;
    freezeStartMs_ = nowMs_;
    phase_ = Phase::freezing;
    if (!parkedCpus_) enterSleep();
  }

  void enterSleep()
  {
    lidWakeArmed_ = config_.lidArmed && (config_.wakeMask & kWakeLid) && clamshellClosed();
    // Confirm open with samples from this sleep interval. The current power-button level is its baseline.
    candidate_ = lid_;
    candidateSinceMs_ = nowMs_;
    unstable_ = false;
    sleepStartMs_ = nowMs_;
    phase_ = Phase::asleep;
  }

  void restore(Outcome outcome)
  {
    parkedCpus_ = frozenCpus_ = 0;
    lidWakeArmed_ = lidInitiated_ = false;
    setSubsystems(false);
    suspendLockHeld_ = false;
    phase_ = Phase::awake;
    outcome_ = outcome;
  }

  Config config_;
  Phase phase_ = Phase::awake;
  Outcome outcome_ = Outcome::none;
  uint64_t nowMs_ = 0, freezeStartMs_ = 0, sleepStartMs_ = 0, generation_ = 0;
  uint64_t candidateSinceMs_ = 0, unstableSinceMs_ = 0;
  Lid lid_ = Lid::unknown, candidate_ = Lid::unknown;
  bool attached_ = false, seenOpen_ = false, unstable_ = false, powerDown_ = false;
  bool lidInitiated_ = false, lidWakeArmed_ = false, suspendLockHeld_ = false;
  unsigned lockRequests_ = 0;
  uint32_t parkedCpus_ = 0, frozenCpus_ = 0, missingAcks_ = 0;
  std::array<bool, kOrder.size()> quiesced_{};
  std::array<Effect, 2 * kOrder.size()> effects_{};
  std::size_t effectCount_ = 0;
};

}  // namespace pianopolicy::host
