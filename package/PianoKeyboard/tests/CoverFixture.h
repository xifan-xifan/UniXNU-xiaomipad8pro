// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_KEYBOARD_TEST_COVER_FIXTURE_H
#define PIANO_KEYBOARD_TEST_COVER_FIXTURE_H
// Offline fixture for the cover's I2C reports, HID output and property changes. The strict frame checks, coordinate clamp and
// UART-style byte-stream replay are fixture policies. The WN8030 layout uses additive checksums. See PianoKeyboard.cpp's readFrame.
#include "CoverPower.h"
#include "HidDescriptors.h"
#include "Wn8030Piano.h"
#include "../../PianoBacklight/PianoBacklightClient.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace pianokeyboard;
using Bytes = std::span<const uint8_t>;
using WireFrame = std::array<uint8_t, kFrameBytes>;
using KeyReport = std::array<uint8_t, kKeyboardBytes>;
using ConsumerReport = std::array<uint8_t, kConsumerBytes>;
using TouchReport = std::array<uint8_t, kTouchpadBytes>;
using PointerReport = std::array<uint8_t, kPointerBytes>;
using CommandWire = std::array<uint8_t, kCommandWire>;
using Property = std::pair<std::string_view, int>;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); std::abort(); \
} } while (false)

constexpr std::string_view kAttachedProperty = "piano-keyboard-attached", kHallProperty = "piano-keyboard-hall";
constexpr KeyReport kKeysUp{kIdKeyboard};
constexpr ConsumerReport kConsumerUp{kIdConsumer};
constexpr PointerReport kPointerUp{};
constexpr TouchReport kLift{kIdTouchpad};

// Only the input, LED and publication parts of the serving loop are represented here. Events stand in for IOHIDDevice and IORegistry.
class CoverFixture {
 public:
  explicit CoverFixture(PointerConfig config = {}, bool lidDrop = false, bool blank = false, bool fnRow = false) : pointer_(config)
  {
    gate_.configure(lidDrop);
    power_.configure(blank);
    filter_.fnRow = fnRow;
  }

  bool i2cRead(Bytes bytes, uint64_t ms)
  {
    if (power_.asleep() || !power_.running() || bytes.size() != kFrameBytes || !frameSumOk(bytes.data())) return false;
    Report reports[24];
    unsigned count = 0;
    if (unpackFrame(bytes.data(), reports, std::size(reports), &count) != Frame::reports) return false;
    // Preflight all reports before publishing even the valid prefix of a bad transaction.
    for (unsigned i = 0; i < count; ++i)
      if (!validPayload(reports[i])) return false;
    for (unsigned i = 0; i < count; ++i) dispatch(reports[i], ms);
    return true;
  }

  // Replay the same WN8030 bytes across arbitrary stream chunks. Keep a possible sync byte after a failed candidate.
  unsigned streamRead(Bytes bytes, uint64_t ms)
  {
    if (power_.asleep() || !power_.running()) return 0;
    unsigned accepted = 0;
    for (uint8_t byte : bytes) {
      if (!streamSize_ && !syncByte(byte)) continue;
      stream_[streamSize_++] = byte;
      if (streamSize_ < stream_.size()) continue;
      if (i2cRead(stream_, ms)) {
        streamSize_ = 0;
        ++accepted;
      } else {
        const auto next = std::find_if(stream_.begin() + 1, stream_.end(), syncByte);
        streamSize_ = size_t(stream_.end() - next);
        std::copy(next, stream_.end(), stream_.begin());
      }
    }
    return accepted;
  }

  void sleep(bool asleep, uint64_t ms)
  {
    if (asleep) streamSize_ = 0;
    applyPower(power_.sleep(asleep, ms));
  }

  void display(int state, uint64_t ms) { applyPower(power_.display(state, ms)); }
  const CoverPower &power() const { return power_; }

  void sampleHall(uint32_t raw, bool edge, uint64_t ms)
  {
    const int sample = hallOpenFromRaw(raw, true);
    if (!hallDebounce_.sample(hall_, sample, edge, ms)) return;
    hall_ = sample;
    changes.emplace_back(kHallProperty, hall_);
    if (gate_.hall(hall_)) releaseInput();
  }

  bool ledOutput(Bytes bytes, uint8_t optionId = 0)
  {
    if (bytes.size() == 2 && bytes[0] == kIdKeyboard) ledWanted_ = (bytes[1] & kLedCapsLock) != 0;
    else if (bytes.size() == 1 && optionId == kIdKeyboard) ledWanted_ = (bytes[0] & kLedCapsLock) != 0;
    else return false;
    return true;
  }

  void serviceLeds()
  {
    if (power_.asleep() || !ledWanted_) return;
    const bool wanted = *ledWanted_;
    ledWanted_.reset();
    if (wanted == caps_) return;
    caps_ = wanted;
    if (attached_ && power_.running()) sendCommand(kCmdIndicator, caps_ ? kLedCapsOn : kLedCapsOff);
  }

  int property(std::string_view name, int fallback) const
  {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
      if (it->first == name) return it->second;
    return fallback;
  }

  uint32_t continuations() const { return pointer_.continuations(); }

  std::vector<KeyReport> keys;
  std::vector<ConsumerReport> consumer;
  std::vector<PointerReport> pointer;
  std::vector<Gesture> gestures;
  std::vector<int> brightnessSteps;
  std::vector<CommandWire> commands;
  std::vector<Property> changes;

 private:
  static bool syncByte(uint8_t byte) { return (byte & 0x7f) == 0x57; }

  static bool validPayload(const Report &r)
  {
    if (r.id == kIdTouchpad) return r.bytes[kTouchpadBytes - 1] <= 5;
    int attached;
    if (vendorKind(r, &attached) != Vendor::attach) return true;
    // Attach needs payload bytes 0..3 (the bit is in byte 9 of the report), followed by a checksum inside this report.
    const unsigned size = r.bytes[5];
    const unsigned checksum = 6 + size;
    return size >= 4 && checksum < r.length && sum8(r.bytes, checksum) == r.bytes[checksum];
  }

  void releaseInput()
  {
    keys.push_back(kKeysUp);
    consumer.push_back(kConsumerUp);
    auto &report = pointer.emplace_back();
    Pointer::release(report.data());
    pointer_.reset();
    filter_.reset();
    lastKeys_ = kKeysUp;
    brightness_ = {};
  }

  void sendCommand(uint8_t cmd, uint8_t value) { command(commands.emplace_back().data(), cmd, value); }

  void sendAttachCommands()
  {
    Command list[5];
    const unsigned n = attachCommands(caps_, 1, 0, list);
    for (unsigned i = 0; i < n; ++i) sendCommand(list[i].cmd, list[i].value);
  }

  void applyPower(CoverPower::Step step)
  {
    if (CoverPower::releasesInput(step)) releaseInput();
    if (step == CoverPower::Step::blank) sendCommand(kCmdKeyboardPower, 0);
    if (step == CoverPower::Step::unblank) {
      if (attached_) sendAttachCommands();
      else sendCommand(kCmdKeyboardPower, 1);
    }
  }

  void attach(bool on)
  {
    if (gate_.attach(on)) releaseInput();
    if (attached_ == on) return;
    attached_ = on;
    changes.emplace_back(kAttachedProperty, int(on));
    if (on) sendAttachCommands();
  }

  void touch(const uint8_t *raw, uint64_t ms)
  {
    TouchReport report;
    std::copy_n(raw, report.size(), report.begin());
    for (unsigned slot = 0; slot < kFingers; ++slot) {
      uint8_t *contact = report.data() + 2 + 8 * slot;
      putLe16(contact + 4, std::min<unsigned>(le16(contact + 4), kTouchXMax));
      putLe16(contact + 6, std::min<unsigned>(le16(contact + 6), kTouchYMax));
    }
    uint8_t out[2][kPointerBytes];
    const unsigned n = pointer_.touch(report.data(), ms, out);
    CHECK(n <= std::size(out));
    for (unsigned i = 0; i < n; ++i) std::copy_n(out[i], kPointerBytes, pointer.emplace_back().begin());
    const Swipe swipe = pointer_.takeSwipe();
    if (swipe != Swipe::none) {
      uint8_t shortcut[4][kKeyboardBytes];
      const unsigned count = filter_.shortcut(swipeUsage(swipe), shortcut);
      CHECK(count == std::size(shortcut));
      for (const auto &key : shortcut) std::copy_n(key, kKeyboardBytes, keys.emplace_back().begin());
    }
    Gesture gesture;
    if (pointer_.takeGesture(&gesture)) gestures.push_back(gesture);
  }

  void dispatch(const Report &r, uint64_t ms)
  {
    int attached;
    if (vendorKind(r, &attached) == Vendor::attach) {
      attach(attached == 1);
      return;
    }
    switch (r.id) {
    case kIdKeyboard: {
      if (!gate_.pass()) return;
      if (!filter_.filter(r.bytes, keys.emplace_back().data())) {
        if (Keys::newKey(lastKeys_.data(), r.bytes)) pointer_.keyTyped(ms);
        std::copy_n(r.bytes, lastKeys_.size(), lastKeys_.begin());
        const int step = brightness_.keyboard(r.bytes);
        if (step) brightnessSteps.push_back(step);
      }
      break;
    }
    case kIdConsumer: {
      if (!gate_.pass()) return;
      std::copy_n(r.bytes, kConsumerBytes, consumer.emplace_back().begin());
      const int step = brightness_.consumer(le16(r.bytes + 1));
      if (step) brightnessSteps.push_back(step);
      break;
    }
    case kIdTouchpad:
      if (gate_.pass()) touch(r.bytes, ms);
      break;
    }
  }

  WireFrame stream_{};
  size_t streamSize_ = 0;
  InputGate gate_;
  CoverPower power_;
  HallDebounce hallDebounce_;
  Pointer pointer_;
  Keys filter_;
  pianobacklight::KeyWatch brightness_;
  KeyReport lastKeys_ = kKeysUp;
  bool attached_ = false, caps_ = false;
  int hall_ = -1;
  std::optional<bool> ledWanted_;
};

inline void seal(WireFrame &frame)
{
  frame.back() = sum8(frame.data() + 2, kFrameBytes - 3);
}

inline WireFrame packet(std::initializer_list<Bytes> reports)
{
  WireFrame frame{0x57, 1, 0x39};
  size_t at = 3;
  for (Bytes report : reports) {
    CHECK(report.size() < frame.size() - at);
    std::copy(report.begin(), report.end(), frame.begin() + at);
    at += report.size();
  }
  seal(frame);
  return frame;
}

inline std::array<uint8_t, 32> coverState(uint8_t state)
{
  std::array<uint8_t, 32> report{0x23, 0x31, 0x38, 0x80, 0xa2, 4, 1, 0, 0xff, state};
  report[10] = sum8(report.data(), 10);
  return report;
}

inline std::array<uint8_t, 5> media(uint16_t usage)
{
  return {kIdConsumer, uint8_t(usage), uint8_t(usage >> 8), 0xab, 0xcd};
}

struct Finger { uint8_t id; uint16_t x, y; uint8_t flags = 7; };

inline TouchReport contacts(uint8_t buttons, uint8_t count, std::initializer_list<Finger> fingers)
{
  CHECK(fingers.size() <= kFingers);
  TouchReport report{kIdTouchpad, buttons};
  report.back() = count;
  auto *slot = report.data() + 2;
  for (const Finger &finger : fingers) {
    slot[0] = finger.flags;
    slot[1] = finger.id;
    putLe16(slot + 4, finger.x);
    putLe16(slot + 6, finger.y);
    slot += 8;
  }
  return report;
}

#endif
