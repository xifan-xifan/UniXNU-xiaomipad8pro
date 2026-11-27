// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. keyboard_cover_churn_stress_host.cpp -o /tmp/keyboard_cover_churn_stress_host && /tmp/keyboard_cover_churn_stress_host
// Synthetic WN8030 reports replayed through the portable codec, input filters and cover power policy.
#include "CoverFixture.h"

constexpr unsigned kChurnRounds = 768;
constexpr uint8_t kOpen = 1, kClosed = 0; // gpio80 with piano-keyboard-hall-active-low=1
constexpr uint8_t kFnUsages[] = {0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e};
constexpr uint16_t kMediaUsages[] = {0xcd, 0xb5, 0xb6, 0xe9, 0xea, 0xe2, 0x6f, 0x70, 0x79, 0x7a};
constexpr uint8_t kPress = 1, kRight = 2, kMiddle = 4;

static void send(CoverFixture &cover, uint64_t ms, std::initializer_list<Bytes> reports)
{
  CHECK(cover.i2cRead(packet(reports), ms));
}

struct InputCounts {
  size_t keys, consumer, pointer, gestures, brightness;
  bool operator==(const InputCounts &) const = default;
};

static InputCounts inputCounts(const CoverFixture &cover)
{
  return {cover.keys.size(), cover.consumer.size(), cover.pointer.size(), cover.gestures.size(), cover.brightnessSteps.size()};
}

static void expectRelease(const CoverFixture &cover, InputCounts counts)
{
  ++counts.keys;
  ++counts.consumer;
  ++counts.pointer;
  CHECK(inputCounts(cover) == counts);
  CHECK(cover.keys.back() == kKeysUp && cover.consumer.back() == kConsumerUp && cover.pointer.back() == kPointerUp);
}

// Five concurrent tips arrive as three slots in the primary report, then two in a zero-count continuation.
// The relative-pointer path uses the primary slots for gesture math.
static auto fiveContacts(uint8_t buttons, uint16_t y, uint8_t flags = 7)
{
  return std::array{contacts(buttons, 5, {{1, 0, y, flags}, {2, 640, y, flags}, {3, kTouchXMax, y, flags}}),
                    contacts(buttons, 0, {{4, 0, 0, flags}, {5, kTouchXMax, kTouchYMax, flags}})};
}

static void sendScan(CoverFixture &cover, uint64_t ms, const std::array<TouchReport, 2> &scan)
{
  const auto before = cover.continuations();
  send(cover, ms, {scan[0], scan[1]});
  CHECK(cover.continuations() == before + 1);
}

static void interruptChord(CoverFixture &cover, unsigned round, uint64_t ms, const KeyReport &chord, uint16_t usage)
{
  const auto held = fiveContacts(kPress, 700);
  if (round % 2 == 0) sendScan(cover, ms, held);
  else send(cover, ms, {held[0]}); // interrupt with the continuation still in flight
  CHECK(cover.pointer.back()[0] == kRight);
  const auto pressed = inputCounts(cover);
  const auto detach = coverState(0x22), attach = coverState(0x23);
  const auto stillHeld = packet({chord, media(usage), held[0]});
  const KeyReport lateRollover{kIdKeyboard, 0, 0, 1};

  switch (round % 3) {
  case 0:
    send(cover, ms + 1, {detach});
    expectRelease(cover, pressed);
    send(cover, ms + 2, {detach});
    CHECK(cover.i2cRead(stillHeld, ms + 3));
    expectRelease(cover, pressed);
    send(cover, ms + 4, {attach});
    break;
  case 1:
    cover.sampleHall(kClosed, true, ms + 1);
    cover.sampleHall(kClosed, false, ms + 20);
    CHECK(inputCounts(cover) == pressed);
    cover.sampleHall(kClosed, false, ms + 21);
    expectRelease(cover, pressed);
    CHECK(cover.i2cRead(stillHeld, ms + 22));
    expectRelease(cover, pressed);
    cover.sampleHall(kOpen, true, ms + 23);
    cover.sampleHall(kOpen, false, ms + 43);
    break;
  case 2:
    cover.sleep(true, ms + 1);
    expectRelease(cover, pressed);
    const auto blankCount = cover.power().blanks();
    cover.sleep(true, ms + 2);
    CHECK(cover.power().blanks() == blankCount && !cover.power().running());
    CHECK(!cover.i2cRead(stillHeld, ms + 3));
    expectRelease(cover, pressed);
    cover.sleep(false, ms + 4);
    CHECK(cover.power().running() && cover.power().blanks() == cover.power().unblanks());
    break;
  }

  // A late all-up and an overflow marker cannot resurrect keys, buttons or the interrupted touch.
  const auto released = inputCounts(cover);
  const auto continuationCount = cover.continuations();
  send(cover, ms + 44, {lateRollover, kLift});
  CHECK(cover.keys.size() == released.keys + 1 && cover.keys.back() == kKeysUp);
  CHECK(cover.pointer.size() == released.pointer && cover.gestures.size() == released.gestures);
  CHECK(cover.continuations() == continuationCount);

  // A fresh tap needs no idle wait even if the interrupted chord had just started the typing guard.
  const auto fresh = contacts(0, 1, {{1, 800, 900}});
  send(cover, ms + 45, {fresh, kLift});
  CHECK(cover.pointer.size() == released.pointer + 2 && cover.pointer[released.pointer][0] == kPress);
  CHECK(cover.pointer.back() == kPointerUp && cover.gestures.back().tap == kPress);
}

static void testRolloverChurn(bool fnRow)
{
  CoverFixture cover({}, true, true, fnRow);
  send(cover, 0, {coverState(0x23)});
  cover.sampleHall(kOpen, true, 1);
  cover.sampleHall(kOpen, false, 21);
  std::vector<int> brightness;
  for (unsigned i = 0; i < kChurnRounds; ++i) {
    const uint64_t t = 1000 + i * 80;
    const unsigned fn = i % std::size(kFnUsages);
    const uint16_t usage = kMediaUsages[i % std::size(kMediaUsages)];
    const uint8_t mods = uint8_t((i * 37 | kModLeftCtrl) & ~kModRightCtrl);
    KeyReport chord{kIdKeyboard, uint8_t(mods | kModRightCtrl), 0, kFnUsages[fn]};
    for (size_t slot = 4; slot < chord.size(); ++slot) chord[slot] = uint8_t(4 + (i + slot * 3) % 24);
    KeyReport expected = chord;
    if (fnRow) {
      expected[1] = mods;
      expected[3] = uint8_t(0x3a + fn);
    }
    const ConsumerReport consumer{kIdConsumer, uint8_t(usage), uint8_t(usage >> 8)};
    const auto before = inputCounts(cover);
    send(cover, t, {chord, media(usage), chord, media(usage)});
    CHECK(cover.keys.size() == before.keys + 2 && cover.keys[before.keys] == expected && cover.keys.back() == expected);
    CHECK(cover.consumer.size() == before.consumer + 2 && cover.consumer[before.consumer] == consumer && cover.consumer.back() == consumer);
    if (fn < 2) brightness.push_back(fn == 0 ? -1 : 1);
    if (usage == 0x6f || usage == 0x70) brightness.push_back(usage == 0x6f ? 1 : -1);
    CHECK(cover.brightnessSteps == brightness);

    for (uint8_t error : {1, 2, 3}) {
      KeyReport overflow{kIdKeyboard, uint8_t(chord[1] ^ (1u << error)), 0};
      const unsigned slot = 3 + (i + error) % 6;
      overflow[slot] = error;
      overflow[3 + (slot - 2) % 6] = fn == 0 ? 0x1f : 0x1e; // the non-error slots are unreliable too
      KeyReport held = expected;
      held[1] ^= uint8_t(1u << error);
      send(cover, t + error, {overflow});
      CHECK(cover.keys.size() == before.keys + 2 + error && cover.keys.back() == held);
    }
    send(cover, t + 4, {chord, media(usage)});
    CHECK(cover.keys.back() == expected && cover.consumer.back() == consumer);
    CHECK(cover.brightnessSteps == brightness); // resuming the same held keys may not fire a fresh step

    if (i % 4 == 0) {
      interruptChord(cover, i / 4, t + 5, chord, usage);
    } else {
      chord[1] = expected[1] = mods;
      send(cover, t + 5, {chord}); // release Right Ctrl before releasing the other six keys
      CHECK(cover.keys.back() == expected);
      const KeyReport lastKey{kIdKeyboard, 0, 0, chord.back()};
      const size_t at = cover.keys.size();
      send(cover, t + 6, {lastKey, kKeysUp, media(0)});
      CHECK(cover.keys.size() == at + 2 && cover.keys[at] == lastKey && cover.keys.back() == kKeysUp);
      CHECK(cover.consumer.back() == kConsumerUp);
    }
    const KeyReport plain{kIdKeyboard, 0, 0, kFnUsages[fn]};
    const size_t at = cover.keys.size();
    send(cover, t + 51, {plain, kKeysUp});
    CHECK(cover.keys.size() == at + 2 && cover.keys[at] == plain);
    CHECK(cover.keys.back() == kKeysUp && cover.brightnessSteps == brightness);
  }
  CHECK(cover.power().blanks() == kChurnRounds / 12);
}

static void testFiveContactGestures()
{
  CoverFixture cover;
  const KeyReport chord{kIdKeyboard, 0x42, 0, 4, 8, 12, 16, 20, 24};
  const KeyReport rollover{kIdKeyboard, 0x42, 0, 1, 1, 1, 1, 1, 1};
  for (unsigned i = 0; i < 96; ++i) {
    const uint64_t t = 1000 + 500 * i;
    const auto baseline = inputCounts(cover);
    send(cover, t, {chord});
    sendScan(cover, t + 239, fiveContacts(0, 90));
    sendScan(cover, t + 240, fiveContacts(0, 0));
    sendScan(cover, t + 241, fiveContacts(0, kTouchYMax, 3)); // the tracked fingers lose confidence
    CHECK(cover.pointer.size() == baseline.pointer && cover.gestures.size() == baseline.gestures + 1);
    CHECK(cover.gestures.back().typing && !cover.gestures.back().tap && !cover.gestures.back().scrolls && cover.gestures.back().y1 == 0);
    send(cover, t + 242, {kLift});
    CHECK(cover.pointer.size() == baseline.pointer && cover.gestures.size() == baseline.gestures + 1);

    send(cover, t + 248, {rollover, chord});
    sendScan(cover, t + 250, fiveContacts(0, 90)); // the original typing guard has expired
    sendScan(cover, t + 251, fiveContacts(0, 85));
    send(cover, t + 260, {kLift});
    CHECK(cover.pointer.size() == baseline.pointer + 2);
    CHECK((cover.pointer[baseline.pointer] == PointerReport{kMiddle}) && cover.pointer.back() == kPointerUp);
    CHECK(!cover.gestures.back().typing && cover.gestures.back().tap == kMiddle && cover.gestures.back().most == kFingers);

    const bool down = i % 2;
    const uint16_t edge = down ? kTouchYMax : 0;
    sendScan(cover, t + 270, fiveContacts(0, down ? edge - 90 : 90));
    sendScan(cover, t + 271, fiveContacts(0, edge));
    CHECK((cover.pointer.back() == PointerReport{0, 0, 0, 0, 0, uint8_t(down ? -12 : 12), 0}));
    sendScan(cover, t + 272, fiveContacts(kPress, edge));
    CHECK((cover.pointer.back() == PointerReport{kRight}));
    const auto clickCount = cover.pointer.size();
    sendScan(cover, t + 321, fiveContacts(kPress, down ? edge - 30 : 30));
    CHECK(cover.pointer.size() == clickCount);
    sendScan(cover, t + 322, fiveContacts(kPress, down ? edge - 120 : 120));
    CHECK(cover.pointer.size() == clickCount + 1);
    CHECK((cover.pointer.back() == PointerReport{kRight, 0, 0, 0, 0, uint8_t(down ? 12 : -12), 0}));
    const uint16_t nearX = down ? kTouchXMax - 40 : 40, nearY = down ? kTouchYMax - 60 : 60;
    const uint16_t edgeX = down ? kTouchXMax : 0;
    const auto nearCorner = contacts(kPress, 1, {{1, nearX, nearY}});
    const auto corner = contacts(kPress, 1, {{1, edgeX, edge}});
    send(cover, t + 323, {corner});
    CHECK(cover.pointer.size() == clickCount + 1); // the change from five to one cannot jump
    send(cover, t + 324, {nearCorner});
    CHECK(cover.pointer.size() == clickCount + 2 && cover.pointer.back()[0] == kRight);
    CHECK(int16_t(le16(cover.pointer.back().data() + 1)) == (down ? -40 : 40));
    CHECK(int16_t(le16(cover.pointer.back().data() + 3)) == (down ? -40 : 40));
    send(cover, t + 325, {kLift, kKeysUp});
    CHECK(cover.pointer.size() == clickCount + 3 && cover.pointer.back() == kPointerUp);
    CHECK(cover.gestures.back().pressed && cover.gestures.back().pressButton == kRight && cover.gestures.back().tap == 0);
    CHECK(cover.keys.size() == baseline.keys + 4 && cover.keys.back() == kKeysUp);
  }
}

static void testHallBlanking()
{
  CoverFixture cover({}, false, true);
  send(cover, 0, {coverState(0x23)});
  cover.sampleHall(kOpen, true, 1);
  cover.sampleHall(kOpen, false, 21);
  cover.display(1, 21);
  for (unsigned i = 0; i < 114; ++i) {
    const uint64_t t = 100 + i * 160;
    const unsigned pulse = 1 + i % 19;
    const auto properties = cover.changes.size();
    const auto commands = cover.commands.size();
    const auto before = inputCounts(cover);
    cover.sampleHall(kClosed, true, t);
    cover.sampleHall(kClosed, false, t + pulse - 1);
    cover.sampleHall(kOpen, true, t + pulse);
    cover.sampleHall(kOpen, false, t + 20);
    CHECK(cover.changes.size() == properties && cover.commands.size() == commands);
    CHECK(cover.power().running() && cover.power().blanks() == i && inputCounts(cover) == before);

    cover.sampleHall(kClosed, true, t + 21);
    cover.sampleHall(kClosed, true, t + 40); // the last edge restarts confirmation
    cover.sampleHall(kClosed, false, t + 59);
    CHECK(cover.changes.size() == properties && cover.power().running());
    cover.sampleHall(kClosed, false, t + 60);
    CHECK((cover.changes.size() == properties + 1 && cover.changes.back() == Property{kHallProperty, 0}));
    // Replay the panel-power notification after the accepted fold. CoverPower follows the display.
    cover.display(0, t + 61);
    expectRelease(cover, before);
    CHECK(!cover.power().running() && cover.power().blanks() == i + 1);
    CHECK(!cover.i2cRead(packet({media(0xe9)}), t + 62));
    CommandWire off;
    command(off.data(), kCmdKeyboardPower, 0);
    CHECK(cover.commands.size() == commands + 1 && cover.commands.back() == off);
    cover.display(0, t + 63);

    cover.sampleHall(kOpen, true, t + 64);
    cover.sampleHall(kOpen, false, t + 64 + pulse - 1);
    cover.sampleHall(kClosed, true, t + 64 + pulse);
    CHECK(cover.changes.size() == properties + 1 && cover.commands.size() == commands + 1);
    CHECK(!cover.power().running() && cover.power().unblanks() == i);
    cover.sampleHall(kOpen, true, t + 85);
    cover.sampleHall(kOpen, false, t + 104);
    CHECK(cover.changes.size() == properties + 1 && !cover.power().running());
    cover.sampleHall(kOpen, false, t + 105 + i % 3);
    CHECK((cover.changes.size() == properties + 2 && cover.changes.back() == Property{kHallProperty, 1}));
    cover.display(1, t + 108);
    cover.display(1, t + 109);
    CHECK(cover.power().running() && cover.power().unblanks() == i + 1);
    CHECK(cover.commands.size() == commands + 6);
    CommandWire on;
    command(on.data(), kCmdKeyboardPower, 1);
    CHECK(cover.commands[commands + 1] == on);
    send(cover, t + 110, {media(0xe9), media(0)});
    CHECK(cover.consumer.size() == before.consumer + 3 && cover.consumer.back() == kConsumerUp);
  }
}

static void testCorruptionAndRecovery()
{
  const auto attach = packet({coverState(0x23)}), detach = packet({coverState(0x22)});
  const KeyReport held{kIdKeyboard, 0x97, 0, 4, 8, 12, 16, 20, 24};
  const auto recovery = packet({held, media(0xe9)});
  const auto released = packet({kKeysUp, media(0), kLift});
  const std::array<uint8_t, 2> capsOn{kIdKeyboard, kLedCapsLock};
  std::array<uint8_t, 16> staleOff{0x22, 0x10, 0x38, 0x80, kCmdIndicator, 1, kLedCapsOff};
  staleOff[7] = sum8(staleOff.data(), 7);
  const auto echo = packet({staleOff});
  int attached = -1;
  CHECK(vendorKind({staleOff[0], staleOff.size(), staleOff.data()}, &attached) == Vendor::state);
  CommandWire capsCommand;
  command(capsCommand.data(), kCmdIndicator, kLedCapsOn);

  for (unsigned i = 0; i < 250; ++i) {
    CoverFixture cover;
    CHECK(cover.i2cRead(attach, 0) && cover.ledOutput(capsOn));
    cover.serviceLeds();
    CHECK(cover.commands.size() == 6 && cover.commands.back() == capsCommand);
    const auto before = inputCounts(cover);
    const auto shortLength = i % recovery.size();
    const std::vector<uint8_t> shortRead(recovery.begin(), recovery.begin() + shortLength);
    std::vector<uint8_t> longRead(recovery.begin(), recovery.end());
    longRead.resize(kFrameBytes + 1 + i % kFrameBytes, 0xff);
    CHECK(!cover.i2cRead(shortRead, 1) && !cover.i2cRead(longRead, 1));

    auto badSum = detach;
    badSum.back() ^= uint8_t(i + 1);
    auto unknown = packet({held});
    unknown[3 + kKeyboardBytes] = 0x77;
    seal(unknown);
    auto overrun = packet({held, fiveContacts(0, 700)[0], fiveContacts(0, 700)[0]});
    overrun[3 + kKeyboardBytes + 2 * kTouchpadBytes] = kIdKeyboard;
    seal(overrun);
    auto badSize = coverState(0x22);
    badSize[5] = uint8_t(i + 6); // read far beyond the attach payload if its length is trusted
    auto badCount = fiveContacts(kPress, 700)[0];
    badCount.back() = uint8_t(i + 6);
    const std::array malformed{badSum, unknown, overrun, packet({held, badSize}), packet({held, badCount})};
    for (const auto &bad : malformed) CHECK(!cover.i2cRead(bad, 2));
    CHECK(cover.i2cRead(echo, 3));
    cover.serviceLeds();
    CHECK(inputCounts(cover) == before && cover.changes.size() == 1 && cover.commands.size() == 6);

    // A short candidate, malformed complete candidates, an in-band false sync, then a valid high-bit sync.
    std::vector<uint8_t> stream{0xff, 0x00, 0x57, 0xd7, 0x39, kIdTouchpad, 0};
    for (const auto &bad : malformed) stream.insert(stream.end(), bad.begin(), bad.end());
    const size_t noiseEnd = stream.size();
    auto clean = recovery;
    clean[0] = 0xd7;
    stream.insert(stream.end(), clean.begin(), clean.end());
    stream.insert(stream.end(), released.begin(), released.end());
    const size_t chunk = 1 + i % (2 * kFrameBytes);
    unsigned accepted = 0;
    for (size_t at = 0; at < stream.size(); at += chunk) {
      const size_t n = std::min(chunk, stream.size() - at);
      accepted += cover.streamRead(Bytes(stream).subspan(at, n), 4 + at);
      if (at + n <= noiseEnd) CHECK(accepted == 0 && inputCounts(cover) == before);
    }
    CHECK(accepted == 2 && cover.keys.size() == before.keys + 2 && cover.keys[before.keys] == held && cover.keys.back() == kKeysUp);
    CHECK(cover.consumer.size() == before.consumer + 2 && cover.consumer[before.consumer][1] == 0xe9 && cover.consumer.back() == kConsumerUp);
    CHECK(cover.pointer.size() == before.pointer && cover.gestures.size() == before.gestures && cover.brightnessSteps.empty());
    CHECK(cover.changes.size() == 1 && cover.commands.size() == 6);
    CHECK(cover.i2cRead(detach, 1000) && cover.i2cRead(attach, 1001));
    CHECK(cover.commands.size() == 11 && cover.commands[7] == capsCommand);
  }
}

int main()
{
  testRolloverChurn(false);
  testRolloverChurn(true);
  testFiveContactGestures();
  testHallBlanking();
  testCorruptionAndRecovery();
  std::printf("keyboard_cover_churn_stress_host: %u six-key cycles, five-contact gestures, hall debounce and frame recovery passed\n",
              2 * kChurnRounds);
}
