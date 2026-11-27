// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all input_buttons_safety_host.cpp -o /tmp/input_buttons_safety_host && /tmp/input_buttons_safety_host

#include "../Buttons.h"
#include "../KeySim.h"
#include "../PowerKey.h"
#include "../../PianoPolicy/Policy.h"
#include "../../../tooling/k27/piano-pm/piano_pm_policy.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using namespace pianoinput;

int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #__VA_ARGS__); ++failures; } } while (0)

struct Levels {
  uint8_t pon;
  uint8_t gpio;
  bool valid = true;
};

// Raw register fixtures keep the other PON and GPIO bits set independently of the keys.
constexpr Levels kReleased{0x10, 0xff};
constexpr Levels kUp{0x10, 0xfe};       // gpio-keys, PM8550 GPIO6 active low
constexpr Levels kDown{0x50, 0xff};     // pmic_resin, PMK8550 PON bit 6
constexpr Levels kPower{0x90, 0xff};    // pon_pwrkey, PMK8550 PON bit 7
constexpr Levels kBothVolumes{0x50, 0xfe};
constexpr Levels kPowerAndDown{0xd0, 0xff};

struct Report {
  uint64_t at;
  uint8_t byte;
  bool operator==(const Report &) const = default;
};

struct PolicyAsk {
  uint64_t at;
  uint32_t duration;
  pianopolicy::Key decision;
  unsigned actions;
  bool operator==(const PolicyAsk &) const = default;
};

// A host-only HID consumer with a one-entry repeat queue. It coalesces missed ticks,
// cancels a pending tick on a volume change, and pauses while both volumes are held.
// Power shares the report byte and leaves the volume repeat schedule alone.
class RepeatSink {
 public:
  void accept(uint64_t at, uint8_t report)
  {
    const uint8_t volume = report & (kKeyVolumeUp | kKeyVolumeDown);
    const uint8_t target = volume == kKeyVolumeUp || volume == kKeyVolumeDown ? volume : 0;
    if (target == target_) return;
    target_ = target;
    pending_ = 0;
    due_ = at + 300;
  }

  void tick(uint64_t at)
  {
    if (target_ && at >= due_) {
      pending_ = target_;
      due_ = at + 100;
    }
  }

  uint8_t take()
  {
    const uint8_t ready = pending_;
    pending_ = 0;
    return ready;
  }

 private:
  uint64_t due_ = 0;
  uint8_t target_ = 0;
  uint8_t pending_ = 0;
};

// Compose the portable parts at PianoInput::poll and pollThread's boundaries.
// All sends succeed in this fixture. The suspend poll reads only the physical PON sample.
class ButtonRun {
 public:
  explicit ButtonRun(bool policyPresent = false) : policyPresent_(policyPresent) {}

  KeySim sim;
  pianopolicy::Inputs policyInputs;
  std::vector<Report> reports;
  std::vector<PolicyAsk> asks;
  RepeatSink repeat;

  SimResult inject(int64_t value, bool enabled = true) { return sim.arm(enabled, value, nextPoll_); }

  void runUntil(uint64_t until, Levels levels = kReleased)
  {
    for (; nextPoll_ < until; nextPoll_ += kPollMs) {
      if (parked_) continue;
      const uint8_t simulated = sim.held(nextPoll_);
      const uint8_t raw = levels.valid ? uint8_t(decodeKeys(levels.pon, levels.gpio) | simulated) : 0;
      const uint8_t down = filter_.update(levels.valid, raw, nextPoll_);
      const bool power = down & kKeyPower;
      uint32_t duration = 0;
      const PowerStep step = power_.update(power, power_.starts(power) && policyPresent_, nextPoll_, &duration);
      const uint8_t report = power_.report(down, kKeysAll);
      if (step == PowerStep::ask) {
        pianopolicy::Actions actions;
        const char *why = nullptr;
        const auto decision = policy_.powerKey(policyInputs, duration, actions, &why);
        asks.push_back({nextPoll_, duration, decision, actions.n});
        if (decision == pianopolicy::Key::passThrough) {
          send(uint8_t(report | kKeyPower));
          send(report);
        }
      }
      if (report != sent_) send(report);
      repeat.tick(nextPoll_);
    }
  }

  void suspend(Levels baseline, uint32_t sources = PIANO_SRC_POWER)
  {
    parked_ = true;
    piano_pm_wake_arm(&wake_, sources, baseline.valid, baseline.pon, false, 0, false, 0, 0, false, 0);
  }

  int wakePoll(Levels levels)
  {
    CHECK(parked_);
    return piano_pm_wake_check(&wake_, levels.valid, levels.pon, 0, 0, false, 0);
  }

  void resume()
  {
    parked_ = false;
    filter_ = Debounce();
    power_.reset();
    sim.reset();
    // The last sent byte survives sleep so the next sample can release it.
  }

 private:
  void send(uint8_t report)
  {
    reports.push_back({nextPoll_, report});
    sent_ = report;
    repeat.accept(nextPoll_, report);
  }

  const bool policyPresent_;
  const pianopolicy::Engine policy_{pianopolicy::Config{.act = true}};
  Debounce filter_;
  PowerKey power_;
  piano_pm_wake wake_{};
  uint64_t nextPoll_ = 0;
  uint8_t sent_ = 0;
  bool parked_ = false;
};

void testPhysicalMappingAndBounce()
{
  // Wire contract: one unnumbered byte with Consumer Power, Volume Increment and Volume Decrement.
  constexpr uint8_t descriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
    0x09, 0x30, 0x09, 0xe9, 0x09, 0xea, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0xc0,
  };
  const DeviceInfo &device = deviceInfo(Kind::buttons);
  CHECK(kButtonsReportBytes == 1 && device.usagePage == 0x0c && device.usage == 0x01);
  CHECK(device.descriptorBytes == sizeof descriptor &&
        std::memcmp(device.descriptor, descriptor, sizeof descriptor) == 0);

  const struct { Levels pressed; uint8_t byte; } keys[] = {{kUp, 0x02}, {kDown, 0x04}, {kPower, 0x01}};
  for (const auto &key : keys) {
    ButtonRun r;
    r.runUntil(100);
    r.runUntil(130, key.pressed);
    r.runUntil(150);
    r.runUntil(190, key.pressed);
    CHECK(r.reports.empty());
    r.runUntil(200, key.pressed);
    r.runUntil(230);               // Bounce on release.
    r.runUntil(250, key.pressed);
    r.runUntil(290);
    CHECK(r.reports == std::vector<Report>{{190, key.byte}});
    r.runUntil(300);
    CHECK(r.reports == std::vector<Report>{{190, key.byte}, {290, 0x00}} && r.asks.empty());
  }

  ButtonRun burst;
  burst.runUntil(100);
  std::vector<Report> expected;
  for (uint64_t t = 100; t < 100 + 32 * 300; t += 300) {
    uint64_t at = t;
    for (const auto &key : keys) {
      burst.runUntil(at + 50, key.pressed);
      burst.runUntil(at + 100);
      expected.push_back({at + 40, key.byte});
      expected.push_back({at + 90, 0x00});
      at += 100;
    }
  }
  CHECK(burst.reports == expected && burst.asks.empty() && burst.repeat.take() == 0);
}

void testHeldVolumeAndOpposingChord()
{
  ButtonRun simultaneous;
  simultaneous.runUntil(100);
  simultaneous.runUntil(3000, kBothVolumes);
  CHECK(simultaneous.repeat.take() == 0);
  simultaneous.runUntil(4000);
  CHECK(simultaneous.reports == std::vector<Report>{{140, 0x06}, {3040, 0x00}} && simultaneous.asks.empty());

  constexpr uint64_t chordAt = 600000;
  for (bool upReleasedFirst : {false, true}) {
    ButtonRun r;
    r.runUntil(100);
    r.runUntil(440, kUp);
    CHECK(r.repeat.take() == 0);
    r.runUntil(450, kUp);
    CHECK(r.repeat.take() == 0x02 && r.repeat.take() == 0);
    r.runUntil(540, kUp);
    CHECK(r.repeat.take() == 0);
    r.runUntil(550, kUp);
    CHECK(r.repeat.take() == 0x02);
    r.runUntil(chordAt, kUp);   // Ten minutes with no HID consumer draining the repeat queue.
    CHECK(r.reports == std::vector<Report>{{140, 0x02}});
    CHECK(r.repeat.take() == 0x02 && r.repeat.take() == 0);
    r.runUntil(chordAt + 50, kUp);   // Another repeat is pending when the chord settles.
    r.runUntil(chordAt + 1000, kBothVolumes);
    CHECK(r.repeat.take() == 0);

    const Levels remaining = upReleasedFirst ? kDown : kUp;
    const uint8_t byte = upReleasedFirst ? 0x04 : 0x02;
    r.runUntil(chordAt + 1340, remaining);
    CHECK(r.repeat.take() == 0);
    r.runUntil(chordAt + 1350, remaining);
    CHECK(r.repeat.take() == byte && r.repeat.take() == 0);
    r.runUntil(chordAt + 1650, remaining);   // Leave a repeat queued at the final release.
    r.runUntil(chordAt + 2500);
    CHECK(r.repeat.take() == 0 && r.asks.empty());
    CHECK(r.reports == std::vector<Report>{{140, 0x02}, {chordAt + 90, 0x06},
                                          {chordAt + 1040, byte}, {chordAt + 1690, 0x00}});
  }
}

void testPowerAndVolumeDownChord()
{
  for (bool powerReleasedFirst : {false, true}) {
    ButtonRun r(true);
    r.policyInputs.sleepOn = true;
    r.policyInputs.screen = pianopolicy::Screen::on;
    r.runUntil(100);
    r.runUntil(1640, kPowerAndDown);
    CHECK(r.reports == std::vector<Report>{{140, 0x04}} && r.asks.empty());
    r.runUntil(1650, kPowerAndDown);
    CHECK(r.reports == std::vector<Report>{{140, 0x04}, {1640, 0x05}} && r.repeat.take() == 0x04);
    r.runUntil(2400, kPowerAndDown);
    r.runUntil(2800, powerReleasedFirst ? kDown : kPower);
    CHECK(r.repeat.take() == (powerReleasedFirst ? 0x04 : 0x00) && r.repeat.take() == 0);
    r.runUntil(3400);
    CHECK(r.reports == std::vector<Report>{{140, 0x04}, {1640, 0x05},
                                          {2440, uint8_t(powerReleasedFirst ? 0x04 : 0x01)}, {2840, 0x00}});
    CHECK(r.repeat.take() == 0 && r.asks.empty());
  }

  ButtonRun shortPress(true);   // Policy declines with system sleep disabled.
  shortPress.runUntil(100);
  shortPress.runUntil(300, kPowerAndDown);
  shortPress.runUntil(600, kDown);
  shortPress.runUntil(1000);
  CHECK(shortPress.asks == std::vector<PolicyAsk>{{340, 200, pianopolicy::Key::passThrough, 0}});
  CHECK(shortPress.reports == std::vector<Report>{{140, 0x04}, {340, 0x05}, {340, 0x04}, {640, 0x00}});
  CHECK(shortPress.repeat.take() == 0);
}

void testSuspendWakeAndStuckRelease()
{
  ButtonRun r;
  r.runUntil(100);
  r.runUntil(500, kPowerAndDown);
  r.suspend(kPowerAndDown);
  CHECK(r.wakePoll(kPowerAndDown) == 0);   // Already down at suspend.
  CHECK(r.inject(150) == SimResult::ok);
  CHECK(r.wakePoll(kBothVolumes) == 0);    // Releases Power; volume sources alone do not wake in this fixture.
  CHECK(r.wakePoll({kPowerAndDown.pon, kPowerAndDown.gpio, false}) == 0);
  CHECK(r.wakePoll(kPowerAndDown) == PIANO_PM_WAKE_POWER_KEY);
  CHECK(r.wakePoll(kPowerAndDown) == 0);   // Same edge cannot be emitted twice.
  r.runUntil(550, kPowerAndDown);
  CHECK(r.sim.active(550) && r.reports == std::vector<Report>{{140, 0x05}});

  r.resume();   // Model the kernel's release-wait timeout, with both buttons still held.
  CHECK(!r.sim.active(550));
  r.runUntil(900, kPowerAndDown);
  CHECK(r.reports == std::vector<Report>{{140, 0x05}, {550, 0x00}} && r.repeat.take() == 0);
  r.runUntil(1000, kPower);        // Re-arm Volume- independently of the stuck Power key.
  r.runUntil(1100, kPowerAndDown);
  r.runUntil(1200, kDown);         // Power has now been up long enough to re-arm.
  r.runUntil(1300, kPowerAndDown);
  r.runUntil(1400);
  CHECK(r.reports == std::vector<Report>{{140, 0x05}, {550, 0x00}, {1040, 0x04}, {1240, 0x05}, {1340, 0x00}});

  ButtonRun volWake;
  volWake.suspend(kReleased, PIANO_SRC_POWER | PIANO_SRC_VOLUME_DOWN);
  CHECK(volWake.wakePoll(kUp) == 0);
  CHECK(volWake.wakePoll(kPowerAndDown) == PIANO_PM_WAKE_POWER_KEY);   // Power wins on simultaneous edges.
  CHECK(volWake.wakePoll(kBothVolumes) == 0);
  CHECK(volWake.wakePoll(kReleased) == 0);
  CHECK(volWake.wakePoll(kDown) == PIANO_PM_WAKE_VOLUME_DOWN);
  CHECK(volWake.wakePoll(kDown) == 0 && volWake.reports.empty());

  ButtonRun pending(true);
  pending.policyInputs.sleepOn = true;
  pending.policyInputs.screen = pianopolicy::Screen::on;
  pending.runUntil(100);
  CHECK(pending.inject(150) == SimResult::ok);
  pending.runUntil(160, kUp);   // Policy has a short press in progress when the poll thread parks.
  pending.suspend(kUp);
  CHECK(pending.wakePoll(kUp) == 0);
  CHECK(pending.wakePoll(kPower) == PIANO_PM_WAKE_POWER_KEY);
  pending.runUntil(210, kPower);
  CHECK(pending.sim.active(210));
  pending.resume();            // The kernel saw Power release before the first awake sample.
  CHECK(!pending.sim.active(210));
  pending.runUntil(500, kUp);
  pending.runUntil(600);
  pending.runUntil(700, kUp);
  pending.runUntil(800);
  CHECK(pending.asks.empty());
  CHECK(pending.reports == std::vector<Report>{{140, 0x02}, {210, 0x00}, {640, 0x02}, {740, 0x00}});
}

void testSimulationIsolation()
{
  ButtonRun r(true);
  r.policyInputs.sleepOn = true;
  r.policyInputs.screen = pianopolicy::Screen::off;
  r.runUntil(100);
  r.runUntil(450, kUp);
  CHECK(r.repeat.take() == 0x02);
  CHECK(r.inject(150, false) == SimResult::off);
  for (const int64_t value : {std::numeric_limits<int64_t>::min(), int64_t{-1}, int64_t{0}, int64_t{49}, int64_t{10001},
                              int64_t{0x80096}, std::numeric_limits<int64_t>::max()}) {
    CHECK(r.inject(value) == SimResult::invalid);
    CHECK(!r.sim.active(450));
  }
  CHECK(r.sim.presses == 0 && r.sim.refused == 8);
  CHECK(r.inject(150) == SimResult::ok);   // 150 ms of simulated Power enters the awake input path.
  r.runUntil(550, kUp);
  CHECK(r.repeat.take() == 0x02 && r.asks.empty());
  CHECK(r.inject(10001) == SimResult::invalid && r.inject(150) == SimResult::busy);
  r.runUntil(650, kBothVolumes);
  CHECK(r.asks == std::vector<PolicyAsk>{{640, 150, pianopolicy::Key::wakeDisplay, 1}});
  CHECK(!r.sim.active(650) && r.sim.presses == 1 && r.sim.refused == 10);
  r.runUntil(750, kDown);
  r.runUntil(850);
  CHECK(r.reports == std::vector<Report>{{140, 0x02}, {590, 0x06}, {690, 0x04}, {790, 0x00}});
  CHECK(r.repeat.take() == 0 && r.asks.size() == 1);

  ButtonRun failedRead;
  failedRead.runUntil(100);
  failedRead.runUntil(200, kDown);
  CHECK(failedRead.inject(150) == SimResult::ok);
  failedRead.runUntil(230, {kDown.pon, kDown.gpio, false});   // Brief bus fault preserves the reported volume.
  failedRead.runUntil(240, kDown);
  failedRead.runUntil(400, {kDown.pon, kDown.gpio, false});   // Sustained fault releases it, with no simulated edge.
  failedRead.runUntil(500, kDown);
  failedRead.runUntil(600);
  CHECK(failedRead.reports == std::vector<Report>{{140, 0x04}, {280, 0x00}, {440, 0x04}, {540, 0x00}});
}
}  // namespace

int main()
{
  testPhysicalMappingAndBounce();
  testHeldVolumeAndOpposingChord();
  testPowerAndVolumeDownChord();
  testSuspendWakeAndStuckRelease();
  testSimulationIsolation();
  if (failures) return 1;
  std::puts("input_buttons_safety_host: all Goals 6 and 9 scenarios passed");
  return 0;
}
