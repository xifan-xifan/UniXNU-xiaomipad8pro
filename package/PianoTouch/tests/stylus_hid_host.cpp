// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline Pen/Stylus HID, synthetic HOGP and NT36532E, and touch handoff.
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. stylus_hid_host.cpp -o /tmp/stylus_hid_host && /tmp/stylus_hid_host
#include "StylusHidModel.h"
#include "TouchNative.h"
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

#if defined(__APPLE__) && __has_include(<IOKit/hid/IOHIDUsageTables.h>)
#include <IOKit/hid/IOHIDUsageTables.h>
#else
enum {
  kHIDPage_Digitizer = 0x0d, kHIDPage_GenericDesktop = 0x01,
  kHIDUsage_Dig_Pen = 0x02, kHIDUsage_Dig_Stylus = 0x20, kHIDUsage_Dig_TipSwitch = 0x42,
  kHIDUsage_Dig_BarrelSwitch = 0x44, kHIDUsage_Dig_Eraser = 0x45, kHIDUsage_Dig_Invert = 0x3c,
  kHIDUsage_Dig_InRange = 0x32, kHIDUsage_Dig_TipPressure = 0x30, kHIDUsage_Dig_XTilt = 0x3d,
  kHIDUsage_Dig_YTilt = 0x3e, kHIDUsage_GD_X = 0x30, kHIDUsage_GD_Y = 0x31,
};
#endif

using namespace pianotouch;
using namespace pianotouch::stylus;

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static bool sameAxes(const PenAxes &a, const PenAxes &b)
{
  return a.flags == b.flags && a.pressure == b.pressure && a.tiltX == b.tiltX && a.tiltY == b.tiltY;
}

static bool samePoint(PanelPoint a, PanelPoint b)
{
  return a.x == b.x && a.y == b.y;
}

static void testDescriptor()
{
  // Independent HID 1.11 fixture for the input layout. Check usage IDs
  // against Apple's SDK on macOS and fixed HID Usage Tables values elsewhere.
  constexpr uint8_t expected[] = {
    0x05, kHIDPage_Digitizer, 0x09, kHIDUsage_Dig_Pen, 0xa1, 1, 0x85, 1,
    0x09, kHIDUsage_Dig_Stylus, 0xa1, 2,
    0x09, kHIDUsage_Dig_TipSwitch, 0x09, kHIDUsage_Dig_BarrelSwitch,
    0x09, kHIDUsage_Dig_Eraser, 0x09, kHIDUsage_Dig_Invert, 0x09, kHIDUsage_Dig_InRange,
    0x15, 0, 0x25, 1, 0x65, 0, 0x75, 1, 0x95, 5, 0x81, 2,
    0x15, 0, 0x25, 1, 0x65, 0, 0x75, 1, 0x95, 3, 0x81, 3,
    0x05, kHIDPage_GenericDesktop, 0x09, kHIDUsage_GD_X,
    0x15, 0, 0x26, 0x7f, 0x0c, 0x65, 0, 0x75, 16, 0x95, 1, 0x81, 2,
    0x09, kHIDUsage_GD_Y,
    0x15, 0, 0x26, 0x57, 0x08, 0x65, 0, 0x75, 16, 0x95, 1, 0x81, 2,
    0x05, kHIDPage_Digitizer, 0x09, kHIDUsage_Dig_TipPressure,
    0x15, 0, 0x26, 0xff, 0x0f, 0x65, 0, 0x75, 16, 0x95, 1, 0x81, 2,
    0x09, kHIDUsage_Dig_XTilt, 0x09, kHIDUsage_Dig_YTilt,
    0x15, 0xa6, 0x25, 90, 0x65, 0x14, 0x75, 8, 0x95, 2, 0x81, 2,
    0xc0, 0xc0,
  };
  const auto d = makePenDescriptor();
  CHECK(d.length == sizeof expected);
  CHECK(std::equal(std::begin(expected), std::end(expected), d.bytes));
  CHECK(validatePenDescriptor(expected, sizeof expected));
  CHECK(!validatePenDescriptor(nullptr, sizeof expected));
  CHECK(!validatePenDescriptor(expected, std::numeric_limits<size_t>::max()));
  for (size_t len = 0; len < sizeof expected; ++len) {
    const std::vector<uint8_t> truncated(expected, expected + len);
    CHECK(!validatePenDescriptor(truncated.data(), truncated.size()));
  }
  std::vector<uint8_t> bytes(expected, std::end(expected));
  bytes.push_back(0xc0);
  CHECK(!validatePenDescriptor(bytes.data(), bytes.size()));
  bytes.pop_back();
  // Same schema with an extended Usage item and a two-byte Report Size.
  bytes.erase(bytes.begin() + 2, bytes.begin() + 4);
  bytes.insert(bytes.begin() + 2, {0x0b, 2, 0, 0x0d, 0});
  const auto size = std::find(bytes.begin(), bytes.end(), 0x75);
  *size = 0x76;
  bytes.insert(size + 2, 0);
  CHECK(validatePenDescriptor(bytes.data(), bytes.size()));
  // Corrupt page, collection types, ID, usage, count, flags, logical bounds or unit.
  constexpr size_t offsets[] = {1, 5, 7, 9, 11, 15, 31, 33, 43, 45, 49, 64, 79, 97, 99, 101};
  for (size_t i : offsets) {
    auto malformed = d;
    malformed.bytes[i] ^= 0x04;
    CHECK(!validatePenDescriptor(malformed.bytes, malformed.length));
  }
  // Large count/size values reach Input validation without driving its loop.
  for (const uint8_t item : {0x75, 0x95}) {
    bytes.assign(expected, std::end(expected));
    const auto first = std::find(bytes.begin(), bytes.end(), item);
    const auto insertAt = bytes.erase(first, first + 2);
    bytes.insert(insertAt, {uint8_t(item | 2), 0xff, 0xff, 0xff, 0xff});
    CHECK(!validatePenDescriptor(bytes.data(), bytes.size()));
  }
  for (const uint8_t item : {0xa1, 0xfe}) {
    bytes.assign(expected, std::end(expected));
    bytes.insert(bytes.begin() + 12, {item, 0xff, 0xff, 0xff, 0xff});
    CHECK(!validatePenDescriptor(bytes.data(), bytes.size()));
  }
}

using HidBytes = std::array<uint8_t, 10>;

static void testPenReports()
{
  const PenSample sample{{3199, 2135}, {0x1f, 4095, -90, 90}};
  constexpr HidBytes expected{1, 0x1f, 0x7f, 0x0c, 0x57, 0x08, 0xff, 0x0f, 0xa6, 0x5a};
  HidBytes packed{};
  CHECK(packPenReport(sample, 0, packed.data(), packed.size()));
  CHECK(packed == expected);
  PenSample decoded;
  CHECK(decodePenReport(expected.data(), expected.size(), decoded));
  CHECK(samePoint(decoded.point, sample.point) && sameAxes(decoded.axes, sample.axes));
  CHECK(!packPenReport(sample, 0, nullptr, sizeof packed));
  CHECK(!decodePenReport(nullptr, sizeof packed, decoded));
  constexpr uint8_t guard = 0xee;
  std::array<uint8_t, 12> buffer;
  buffer.fill(guard);
  for (size_t len = 0; len < expected.size(); ++len) {
    CHECK(!packPenReport(sample, 0, buffer.data() + 1, len));
    const std::vector<uint8_t> truncated(expected.data(), expected.data() + len);
    CHECK(!decodePenReport(truncated.data(), truncated.size(), decoded));
  }
  CHECK(std::all_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b == guard; }));
  CHECK(packPenReport(sample, 0, buffer.data() + 1, buffer.size() - 1));
  CHECK(buffer.front() == guard && buffer.back() == guard);
  CHECK(!decodePenReport(buffer.data() + 1, buffer.size() - 1, decoded));
  // Reject bad ID, nonzero padding, out-of-range bounds and inconsistent switches.
  const HidBytes invalid[] = {
    {0, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 0x30, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 0x10, 0x80, 0x0c, 0, 0, 0, 0, 0, 0},
    {1, 0x10, 0, 0, 0x58, 0x08, 0, 0, 0, 0},
    {1, 0x11, 0, 0, 0, 0, 0, 0x10, 0, 0},
    {1, 0x10, 0, 0, 0, 0, 1, 0, 0, 0},
    {1, 0x10, 0, 0, 0, 0, 0, 0, 0xa5, 0},
    {1, 0x10, 0, 0, 0, 0, 0, 0, 0, 91},
    {1, 0x01, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 0x00, 0, 0, 0, 0, 0, 0, 1, 0},
  };
  for (const auto &bad : invalid) {
    CHECK(!decodePenReport(bad.data(), bad.size(), decoded));
    CHECK(samePoint(decoded.point, sample.point) && sameAxes(decoded.axes, sample.axes));
  }
  for (unsigned flags = 0; flags <= 255; ++flags) {
    const PenSample raw{{0, 0}, {uint8_t(flags), 123, -1, 45}};
    CHECK(packPenReport(raw, 0, packed.data(), packed.size()));
    CHECK(decodePenReport(packed.data(), packed.size(), decoded));
    const unsigned activeFlags = flags & 16 ? flags & 31 : 0;
    CHECK(decoded.axes.flags == activeFlags);
    CHECK(decoded.axes.pressure == (activeFlags & 5 ? 123 : 0));
    CHECK(decoded.axes.tiltX == (activeFlags ? -1 : 0) && decoded.axes.tiltY == (activeFlags ? 45 : 0));
  }
}

static void testRotation()
{
  constexpr int32_t low = std::numeric_limits<int32_t>::min(), high = std::numeric_limits<int32_t>::max();
  constexpr PanelPoint inputs[]{{low, low}, {high, low}, {high, high}, {low, high}, {800, 500}};
  constexpr PanelPoint expected[][5] = {
    {{0, 0}, {3199, 0}, {3199, 2135}, {0, 2135}, {800, 500}},
    {{3199, 0}, {3199, 2135}, {0, 2135}, {0, 0}, {2450, 534}},
    {{3199, 2135}, {0, 2135}, {0, 0}, {3199, 0}, {2399, 1635}},
    {{0, 2135}, {0, 0}, {3199, 0}, {3199, 2135}, {749, 1601}},
  };
  constexpr int32_t tilt[][2]{{-33, 90}, {-90, -33}, {33, -90}, {90, 33}};
  for (unsigned r = 0; r < 4; ++r) {
    for (unsigned i = 0; i < 5; ++i) {
      HidBytes packet{};
      CHECK(packPenReport({inputs[i], {0x11, high, -33, high}}, r * 90, packet.data(), packet.size()));
      PenSample out;
      CHECK(decodePenReport(packet.data(), packet.size(), out));
      CHECK(samePoint(out.point, expected[r][i]));
      CHECK(sameAxes(out.axes, {0x11, 4095, tilt[r][0], tilt[r][1]}));
    }
  }
  const auto fallback = mapPen({inputs[4], {0x11, low, low, high}}, 45);
  CHECK(samePoint(fallback.point, inputs[4]));
  CHECK(sameAxes(fallback.axes, {0x11, 0, -90, 90}));
}

using BleBytes = std::array<uint8_t, 8>;
constexpr HogpReport report{0x3412, 2, 1};
constexpr BleBytes down{0x1b, 0x12, 0x34, 0x13, 0x34, 0x02, 0xe2, 0x5a};
constexpr BleBytes hover{0x1b, 0x12, 0x34, 0x12, 0, 0, 0xe2, 0x5a};
constexpr BleBytes exitRange{0x1b, 0x12, 0x34, 0, 0, 0, 0, 0};

static void testHogp()
{
  PenAxes axes;
  CHECK(decodeHogpNotification(report, down.data(), down.size(), axes));
  CHECK(sameAxes(axes, {0x13, 564, -30, 90}));
  const PenAxes accepted = axes;
  for (size_t len = 0; len < down.size(); ++len) {
    const std::vector<uint8_t> truncated(down.data(), down.data() + len);
    CHECK(!decodeHogpNotification(report, truncated.data(), truncated.size(), axes));
  }
  CHECK(!decodeHogpNotification(report, nullptr, down.size(), axes));
  const uint8_t extraId[] = {0x1b, 0x12, 0x34, 2, 0x13, 0x34, 0x02, 0xe2, 0x5a};
  CHECK(!decodeHogpNotification(report, extraId, sizeof extraId, axes));
  for (const HogpReport wrong : {HogpReport{0x1234, 2, 1}, {0, 2, 1}, {0x3412, 1, 1}, {0x3412, 2, 2}})
    CHECK(!decodeHogpNotification(wrong, down.data(), down.size(), axes));
  const BleBytes invalid[] = {
    {0x1d, 0x12, 0x34, 0x11, 0, 0, 0, 0},
    {0x1b, 0x34, 0x12, 0x11, 0, 0, 0, 0},
    {0x1b, 0x12, 0x34, 0x31, 0, 0, 0, 0},
    {0x1b, 0x12, 0x34, 0x11, 0, 16, 0, 0},
    {0x1b, 0x12, 0x34, 0x10, 1, 0, 0, 0},
    {0x1b, 0x12, 0x34, 0x10, 0, 0, 91, 0},
    {0x1b, 0x12, 0x34, 0x10, 0, 0, 0, 0xa5},
    {0x1b, 0x12, 0x34, 0x01, 0, 0, 0, 0},
    {0x1b, 0x12, 0x34, 0, 0, 0, 1, 0},
  };
  for (const auto &bad : invalid) CHECK(!decodeHogpNotification(report, bad.data(), bad.size(), axes));
  CHECK(sameAxes(axes, accepted));
  CHECK(decodeHogpNotification(report, hover.data(), hover.size(), axes) && sameAxes(axes, {0x12, 0, -30, 90}));
  const BleBytes eraser{0x1b, 0x12, 0x34, 0x1c, 0xff, 0x0f, 0xa6, 0x5a};
  CHECK(decodeHogpNotification(report, eraser.data(), eraser.size(), axes) && sameAxes(axes, {0x1c, 4095, -90, 90}));
  CHECK(decodeHogpNotification(report, exitRange.data(), exitRange.size(), axes) && sameAxes(axes, {}));
}

constexpr unsigned payloadBytes = 5160, prefixBytes = 257, matrixEnd = 64 + 60 * 40 * 2;

static void writeLe(uint8_t *bytes, uint32_t value, unsigned count)
{
  for (unsigned i = 0; i < count; ++i) bytes[i] = uint8_t(value >> (8 * i));
}

static void checksum(uint8_t *p, unsigned end)
{
  uint16_t sum = 0;
  for (unsigned i = 20; i < end; i += 2) sum = uint16_t(sum + p[i] + (p[i + 1] << 8));
  const unsigned words = (end - 20) / 4;
  writeLe(p + 4, uint16_t(0 - sum), 2);
  writeLe(p + 8, words, 4);
  writeLe(p + 12, uint16_t(sum - 1), 2);
  writeLe(p + 16, ~words, 4);
}

using MatrixBytes = std::array<uint8_t, prefixBytes + payloadBytes>;

static MatrixBytes matrixFrame()
{
  MatrixBytes bytes{};
  auto *p = bytes.data() + prefixBytes;
  p[0] = 0xa5;
  p[48] = 60;
  p[49] = 40;
  for (unsigned i = 64; i < matrixEnd; ++i) p[i] = uint8_t(i);
  checksum(p, matrixEnd);  // legal trailing payload after the checksum region
  return bytes;
}

static void testMatrix()
{
  const MatrixBytes good = matrixFrame();
  CHECK(validateMatrixFrame(good.data(), good.size(), payloadBytes));
  CHECK(!validateMatrixFrame(good.data(), good.size(), 0));
  CHECK(!validateMatrixFrame(good.data(), good.size(), std::numeric_limits<uint32_t>::max()));
  CHECK(!validateMatrixFrame(nullptr, good.size(), payloadBytes));
  for (size_t len = 0; len < good.size(); ++len) {
    const std::vector<uint8_t> truncated(good.data(), good.data() + len);
    CHECK(!validateMatrixFrame(truncated.data(), truncated.size(), payloadBytes));
    if (len >= prefixBytes && len < prefixBytes + matrixEnd)
      CHECK(!validateMatrixFrame(truncated.data(), truncated.size(), uint32_t(len - prefixBytes)));
  }
  std::vector<uint8_t> oversized(good.begin(), good.end());
  oversized.push_back(0);
  CHECK(!validateMatrixFrame(oversized.data(), oversized.size(), payloadBytes));
  for (const unsigned offset : {0u, prefixBytes, prefixBytes + 4, prefixBytes + 8,
                               prefixBytes + 12, prefixBytes + 16, prefixBytes + matrixEnd - 1}) {
    auto bad = good;
    bad[offset] ^= 0x80;
    CHECK(!validateMatrixFrame(bad.data(), bad.size(), payloadBytes));
  }
  for (const uint8_t prefix : {0x77, 0xfd, 0xfe}) {
    auto bad = good;
    std::fill_n(bad.begin() + 1, 6, prefix);
    CHECK(!validateMatrixFrame(bad.data(), bad.size(), payloadBytes));
  }
  for (const auto geometry : {std::array<uint8_t, 2>{0, 0}, {40, 60}, {59, 40}, {255, 255}}) {
    auto bad = good;
    auto *p = bad.data() + prefixBytes;
    p[48] = geometry[0];
    p[49] = geometry[1];
    checksum(p, matrixEnd);
    CHECK(!validateMatrixFrame(bad.data(), bad.size(), payloadBytes));
  }
  auto bad = good;
  auto *p = bad.data() + prefixBytes;
  checksum(p, matrixEnd - 4);
  CHECK(!validateMatrixFrame(bad.data(), bad.size(), payloadBytes));
  writeLe(p + 8, 0xffffffff, 4);
  writeLe(p + 16, 0, 4);
  CHECK(!validateMatrixFrame(bad.data(), bad.size(), payloadBytes));
}

static void expectPen(const PenUpdate &update, PanelPoint point, const PenAxes &axes, bool cancel = false)
{
  PenSample out;
  CHECK(update.hasReport && update.cancelTouches == cancel);
  CHECK(decodePenReport(update.report, sizeof update.report, out));
  CHECK(samePoint(out.point, point) && sameAxes(out.axes, axes));
}

static void expectQuiet(const PenUpdate &update)
{
  CHECK(!update.hasReport && !update.cancelTouches);
}

static void testHandoff()
{
  const auto matrix = matrixFrame();
  const NativeContact fingers[]{{9, false, 20, 30, 20, 30}, {43, false, 35, 60, 35, 60}};
  PenFusion model(40);
  NativeRouter touch;
  auto touchFrame = [&](unsigned n) {
    NativeOut out{};
    if (model.forwardTouches(n))
      nativeUpdate(touch, fingers, n, 3200, 2136, 3200, 2136, out);
    return out;
  };
  CHECK(touchFrame(2).count == 1 && touch.downs == 2);
  expectQuiet(model.panelFrame(10, matrix.data(), matrix.size(), payloadBytes, {14, 95}));
  const auto entry = model.notification(11, report, down.data(), down.size());
  expectPen(entry, {14, 95}, {0x13, 564, -30, 90}, true);
  NativeOut cancelled{};
  if (entry.cancelTouches) nativeCancel(touch, cancelled);
  CHECK(touch.cancels == 1 && nativeIdle(touch) && cancelled.count == 2);
  for (const unsigned offset : {1, 7}) {
    CHECK(cancelled.report[0][offset] == (kSlotValid | kSlotTip | kSlotInRange | kSlotUntouch));
    CHECK(cancelled.report[1][offset] == kSlotValid);
  }
  CHECK(touchFrame(2).count == 0);
  expectPen(model.panelFrame(12, matrix.data(), matrix.size(), payloadBytes, {15, 96}), {15, 96}, {0x13, 564, -30, 90});
  expectPen(model.notification(13, report, hover.data(), hover.size()), {15, 96}, {0x12, 0, -30, 90});
  CHECK(touchFrame(0).count == 0 && touchFrame(2).count == 0);
  expectPen(model.notification(14, report, exitRange.data(), exitRange.size()), {15, 96}, {});
  expectQuiet(model.notification(15, report, exitRange.data(), exitRange.size()));
  CHECK(touchFrame(2).count == 0 && touchFrame(1).count == 0);
  CHECK(touchFrame(0).count == 0 && touchFrame(1).count == 1 && touch.downs == 3);
  CHECK(touchFrame(0).count == 1 && touch.ups == 1 && nativeIdle(touch));
  // A second hover cannot reuse the old coordinate and emits a range entry
  // when the new panel sample arrives, even though no touches are active.
  expectQuiet(model.notification(16, report, hover.data(), hover.size()));
  expectPen(model.panelFrame(17, matrix.data(), matrix.size(), payloadBytes, {16, 97}), {16, 97}, {0x12, 0, -30, 90}, true);
}

static void testExpiry()
{
  const auto matrix = matrixFrame();
  const auto panel = [&](PenFusion &model, uint64_t now) {
    return model.panelFrame(now, matrix.data(), matrix.size(), payloadBytes, {0, 0});
  };
  const auto ble = [&](PenFusion &model, uint64_t now) {
    return model.notification(now, report, down.data(), down.size());
  };
  constexpr PanelPoint rotated{3199, 0};
  constexpr PenAxes axes{0x13, 564, -90, -30};
  // The BLE sample arrives first and starts the timeout even as the panel refreshes.
  PenFusion model(20, 90);
  expectQuiet(ble(model, 7));
  expectPen(panel(model, 8), rotated, axes, true);
  expectPen(panel(model, 26), rotated, axes);
  expectQuiet(ble(model, 25));
  expectQuiet(model.notification(26, report, down.data(), down.size() - 1));
  expectPen(model.poll(27), rotated, {});
  CHECK(!model.forwardTouches(1));
  CHECK(!model.forwardTouches(0));
  CHECK(model.forwardTouches(1));
  expectQuiet(model.poll(28));
  // Both caches are cleared on expiry. A late packet cannot pair with the panel sample.
  expectQuiet(ble(model, 29));
  expectPen(panel(model, 30), rotated, axes, true);
  expectPen(ble(model, 48), rotated, axes);
  expectQuiet(model.panelFrame(49, matrix.data(), matrix.size() - 1, payloadBytes, {10, 10}));
  expectPen(ble(model, 50), rotated, {});  // panel expiry while BLE still arrives
  expectQuiet(model.poll(51));
  expectPen(panel(model, 52), rotated, axes, true);
  expectPen(model.disconnect(53), rotated, {});
  expectQuiet(model.disconnect(54));
  CHECK(!model.forwardTouches(1) && !model.forwardTouches(0) && model.forwardTouches(1));
  expectQuiet(panel(model, 55));
  expectQuiet(model.notification(57, report, exitRange.data(), exitRange.size()));
  expectQuiet(ble(model, 58));  // explicit range exit cleared a pending panel sample
  expectQuiet(panel(model, 78));  // pending BLE expires at the exact boundary
  expectPen(ble(model, 78), rotated, axes, true);
  expectPen(model.notification(98, report, exitRange.data(), exitRange.size()), rotated, {});
  expectQuiet(model.poll(99));

  // Elapsed arithmetic must work through the largest monotonic timestamp.
  PenFusion edge(1);
  constexpr uint64_t last = std::numeric_limits<uint64_t>::max();
  expectQuiet(panel(edge, last - 1));
  expectPen(ble(edge, last - 1), {}, {0x13, 564, -30, 90}, true);
  expectPen(edge.poll(last), {}, {});
  expectQuiet(edge.poll(0));
}

int main()
{
  testDescriptor();
  testPenReports();
  testRotation();
  testHogp();
  testMatrix();
  testHandoff();
  testExpiry();
  std::printf("stylus_hid_host: %u passed, %u failed\n", checks - failures, failures);
  return failures ? 1 : 0;
}
