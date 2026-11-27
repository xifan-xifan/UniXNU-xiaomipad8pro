// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline model for docs/drivers/stylus.md. The M80p Report Map and panel pen
// coordinate encoding are unknown. HOGP fields and panel coordinates are fixtures.
#ifndef PIANO_TOUCH_STYLUS_HID_MODEL_H
#define PIANO_TOUCH_STYLUS_HID_MODEL_H

#include "Nt36532.h"
#include "TouchMatrix.h"
#include "../PianoAuto/TouchRotation.h"
#include <algorithm>
#include <stddef.h>

namespace pianotouch::stylus {

constexpr uint8_t kTip = 1u << 0, kBarrel = 1u << 1, kEraser = 1u << 2,
                  kInvert = 1u << 3, kInRange = 1u << 4;
constexpr uint8_t kFlags = kTip | kBarrel | kEraser | kInvert | kInRange;
constexpr uint8_t kPenReportId = 1;
constexpr size_t kPenReportBytes = 10;
constexpr int32_t kXMax = kPanelW - 1, kYMax = kPanelH - 1;
constexpr int32_t kPressureMax = 4095, kTiltMax = 90;

namespace detail {
constexpr uint8_t kDigitizer = 0x0d, kDesktop = 0x01, kPen = 0x02, kStylus = 0x20;
constexpr uint8_t kApplication = 1, kLogical = 2;
constexpr uint8_t kAbsolute = 0x02, kPadding = 0x03;
constexpr uint8_t kDegrees = 0x14;  // HID English Rotation, length exponent 1

struct Input {
  uint8_t page, usages[5], bits, count, unit;
  int32_t min, max;
};

// Input groups in wire order, starting immediately after the report ID.
constexpr Input kInputs[] = {
  {kDigitizer, {0x42, 0x44, 0x45, 0x3c, 0x32}, 1, 5, 0, 0, 1}, // tip, barrel, eraser, invert, range
  {kDigitizer, {}, 1, 3, 0, 0, 1},
  {kDesktop, {0x30}, 16, 1, 0, 0, kXMax},
  {kDesktop, {0x31}, 16, 1, 0, 0, kYMax},
  {kDigitizer, {0x30}, 16, 1, 0, 0, kPressureMax},
  {kDigitizer, {0x3d, 0x3e}, 8, 2, kDegrees, -kTiltMax, kTiltMax},
};
constexpr size_t kInputCount = sizeof(kInputs) / sizeof(kInputs[0]);
constexpr size_t kMaxDescriptorBytes = 256;

constexpr size_t reportBits()
{
  size_t bits = 8;  // report ID
  for (const auto &input : kInputs) bits += input.bits * input.count;
  return bits;
}
static_assert(reportBits() == kPenReportBytes * 8);

// Interpret the sign at the encoded width before narrowing.
constexpr int64_t signedValue(uint32_t v, unsigned bytes)
{
  const int64_t modulus = int64_t{1} << (bytes * 8);
  return v >= modulus / 2 ? v - modulus : v;
}
}  // namespace detail

struct PenDescriptor {
  uint8_t bytes[detail::kMaxDescriptorBytes] = {};
  size_t length = 0;
};

constexpr PenDescriptor makePenDescriptor()
{
  using namespace detail;
  PenDescriptor d;
  auto item = [&](uint8_t tag, int32_t value, unsigned size = 1) {
    d.bytes[d.length++] = uint8_t(tag | size);
    for (unsigned i = 0; i < size; ++i) d.bytes[d.length++] = uint8_t(uint32_t(value) >> (8 * i));
  };
  item(0x04, kDigitizer);
  item(0x08, kPen);
  item(0xa0, kApplication);
  item(0x84, kPenReportId);
  item(0x08, kStylus);
  item(0xa0, kLogical);
  uint8_t page = kDigitizer;
  for (const auto &input : kInputs) {
    if (page != input.page) {
      page = input.page;
      item(0x04, page);
    }
    const bool padding = input.usages[0] == 0;
    if (!padding)
      for (unsigned i = 0; i < input.count; ++i) item(0x08, input.usages[i]);
    item(0x14, input.min);
    item(0x24, input.max, input.max > 127 ? 2 : 1);
    item(0x64, input.unit);
    item(0x74, input.bits);
    item(0x94, input.count);
    item(0x80, padding ? kPadding : kAbsolute);
  }
  item(0xc0, 0, 0);
  item(0xc0, 0, 0);
  return d;
}

// Verify the Pen/Stylus short-item schema and the layout consumed by the codec.
// Equivalent item widths are accepted. Additional reports, fields and collections
// are outside this descriptor's contract.
constexpr bool validatePenDescriptor(const uint8_t *data, size_t len)
{
  using namespace detail;
  if (!data || len > kMaxDescriptorBytes) return false;
  uint32_t page = 0, size = 0, count = 0, unit = 0, id = 0;
  struct Usage { uint32_t page, id; };
  Usage usages[5] = {};
  size_t used = 0, group = 0;
  int64_t min = 0, max = 0;
  unsigned depth = 0;
  for (size_t i = 0; i < len;) {
    const uint8_t prefix = data[i++];
    const unsigned bytes = (prefix & 3) == 3 ? 4 : prefix & 3;
    if (bytes > len - i) return false;
    const uint8_t tag = prefix & 0xfc;
    if (tag == 0xc0) {
      if (bytes || used || !depth || group != kInputCount) return false;
      if (--depth == 0) return i == len;
      continue;
    }
    if (!bytes) return false;
    uint32_t v = 0;
    for (unsigned b = 0; b < bytes; ++b) v |= uint32_t(data[i++]) << (8 * b);
    switch (tag) {
    case 0x04: page = v; break;
    case 0x14: min = signedValue(v, bytes); break;
    case 0x24: max = min < 0 ? signedValue(v, bytes) : v; break;
    case 0x64: unit = v; break;
    case 0x74: size = v; break;
    case 0x84:
      if (id || v != kPenReportId) return false;
      id = v;
      break;
    case 0x94: count = v; break;
    case 0x08:
      if (used == sizeof(usages) / sizeof(usages[0])) return false;
      // A four-byte Usage item carries its own page.
      usages[used++] = bytes == 4 ? Usage{v >> 16, v & 0xffff} : Usage{page, v};
      break;
    case 0xa0:
      if (group || depth >= 2 || used != 1 || usages[0].page != kDigitizer) return false;
      if (depth == 0 && (v != kApplication || usages[0].id != kPen)) return false;
      if (depth == 1 && (v != kLogical || usages[0].id != kStylus)) return false;
      ++depth;
      used = 0;
      break;
    case 0x80: {
      if (depth != 2 || !id || group == kInputCount) return false;
      const Input &input = kInputs[group++];
      const bool padding = input.usages[0] == 0;
      if (size != input.bits || count != input.count || min != input.min || max != input.max ||
          unit != input.unit || used != (padding ? 0 : count) || v != (padding ? kPadding : kAbsolute)) return false;
      for (size_t u = 0; u < used; ++u)
        if (usages[u].page != input.page || usages[u].id != input.usages[u]) return false;
      used = 0;
      break;
    }
    default: return false;
    }
  }
  return false;
}

constexpr PenDescriptor kPenDescriptor = makePenDescriptor();
static_assert(validatePenDescriptor(kPenDescriptor.bytes, kPenDescriptor.length));

struct PenAxes {
  uint8_t flags = 0;
  int32_t pressure = 0, tiltX = 0, tiltY = 0;
};

struct PanelPoint { int32_t x = 0, y = 0; };

struct PenSample {
  PanelPoint point;
  PenAxes axes;
};

namespace detail {
constexpr bool validAxes(const PenAxes &a)
{
  if (a.flags & ~kFlags) return false;
  if (!(a.flags & kInRange)) return !a.flags && !a.pressure && !a.tiltX && !a.tiltY;
  return a.pressure >= 0 && a.pressure <= kPressureMax &&
         ((a.flags & (kTip | kEraser)) || a.pressure == 0) &&
         a.tiltX >= -kTiltMax && a.tiltX <= kTiltMax && a.tiltY >= -kTiltMax && a.tiltY <= kTiltMax;
}

inline PenAxes decodeAxes(uint8_t flags, const uint8_t *data)
{
  return {flags, le16(data), int32_t(signedValue(data[2], 1)), int32_t(signedValue(data[3], 1))};
}

inline void put16(uint8_t *out, int32_t value)
{
  out[0] = uint8_t(value);
  out[1] = uint8_t(uint32_t(value) >> 8);
}
}  // namespace detail

// Clamp in panel space, then rotate clockwise as PianoTouch does. Scale the
// rotated desktop onto the descriptor's fixed logical bounds, rounding to the
// nearest integer. The signed tilt components follow the same clockwise transform.
inline PenSample mapPen(PenSample sample, uint32_t degrees)
{
  const PatPoint p = pat_rotate(degrees, sample.point.x, sample.point.y, kPanelW, kPanelH);
  const int32_t xmax = p.width - 1, ymax = p.height - 1;
  sample.point = {int32_t((int64_t(p.x) * kXMax + xmax / 2) / xmax),
                  int32_t((int64_t(p.y) * kYMax + ymax / 2) / ymax)};
  PenAxes &a = sample.axes;
  a.flags &= kFlags;
  if (!(a.flags & kInRange)) {
    a = {};
    return sample;  // keep the last position in a release
  }
  a.pressure = (a.flags & (kTip | kEraser)) ? std::clamp(a.pressure, int32_t{0}, kPressureMax) : 0;
  const int32_t tx = std::clamp(a.tiltX, -kTiltMax, kTiltMax);
  const int32_t ty = std::clamp(a.tiltY, -kTiltMax, kTiltMax);
  switch (degrees) {
  case 90:  a.tiltX = -ty; a.tiltY = tx; break;
  case 180: a.tiltX = -tx; a.tiltY = -ty; break;
  case 270: a.tiltX = ty; a.tiltY = -tx; break;
  default:  a.tiltX = tx; a.tiltY = ty; break;
  }
  return sample;
}

// USB report: ID, five switch bits with zero padding, X/Y/pressure LE16,
// and signed 8-bit X/Y tilt. Capacity may include room for following reports.
inline bool packPenReport(PenSample sample, uint32_t degrees, uint8_t *out, size_t capacity)
{
  if (!out || capacity < kPenReportBytes) return false;
  sample = mapPen(sample, degrees);
  out[0] = kPenReportId;
  out[1] = sample.axes.flags;
  detail::put16(out + 2, sample.point.x);
  detail::put16(out + 4, sample.point.y);
  detail::put16(out + 6, sample.axes.pressure);
  out[8] = uint8_t(sample.axes.tiltX);
  out[9] = uint8_t(sample.axes.tiltY);
  return true;
}

inline bool decodePenReport(const uint8_t *data, size_t len, PenSample &out)
{
  // The decoded position is in descriptor coordinates after rotation.
  if (!data || len != kPenReportBytes || data[0] != kPenReportId) return false;
  const PenSample p{{le16(data + 2), le16(data + 4)}, detail::decodeAxes(data[1], data + 6)};
  if (p.point.x > kXMax || p.point.y > kYMax || !detail::validAxes(p.axes)) return false;
  out = p;
  return true;
}

// HOGP 1.0 §4.8.1. The Report Reference supplies the ID and type separately
// from the ATT notification. The fixture Report Map assigns ID 2 to an Input
// value of flags, LE16 pressure, signed X tilt, signed Y tilt (five bytes).
constexpr uint8_t kBleReportId = 2, kInputReport = 1, kAttNotification = 0x1b;
constexpr size_t kBleNotificationBytes = 8;

struct HogpReport {
  uint16_t handle = 0;  // discovered Report characteristic value handle
  uint8_t id = 0, type = 0;
};

inline bool decodeHogpNotification(const HogpReport &report, const uint8_t *data, size_t len, PenAxes &out)
{
  if (!data || len != kBleNotificationBytes || report.id != kBleReportId || report.type != kInputReport ||
      !report.handle || data[0] != kAttNotification || le16(data + 1) != report.handle) return false;
  const PenAxes axes = detail::decodeAxes(data[3], data + 4);
  if (!detail::validAxes(axes)) return false;
  out = axes;
  return true;
}

// Full NT36532E frame, including the controller prefix. The expected payload
// length comes from the transport so that missing trailing bytes are rejected
// even when the checksum covers a shorter region.
inline bool validateMatrixFrame(const uint8_t *frame, size_t len, uint32_t payloadBytes)
{
  if (!frame || payloadBytes < 64 || payloadBytes > kMaxPayload ||
      len != kTransportBytes + payloadBytes || frame[0] || frameKind(frame) != FrameKind::data) return false;
  const uint8_t *p = frame + kTransportBytes;
  if (p[0] != 0xa5 || packetChecksum(p, payloadBytes) != 1) return false;
  constexpr unsigned cols = 60, rows = 40;
  const uint32_t coveredEnd = 20 + le32(p + 8) * 4;  // bounded by packetChecksum
  return p[48] == cols && p[49] == rows && coveredEnd >= 64 + cols * rows * 2;
}

struct PenUpdate {
  bool hasReport = false;
  bool cancelTouches = false;  // cancel and release forwarded contacts before the pen report
  uint8_t report[kPenReportBytes] = {};
};

// Feed host receipt timestamps from a single monotonic clock, in milliseconds.
// Both sources must be younger than maxAgeMs. poll() handles silence; disconnect()
// releases the pen and clears both caches. Calls older than the last are ignored.
class PenFusion {
public:
  explicit PenFusion(uint64_t maxAgeMs, uint32_t rotation = 0)
    : maxAgeMs_(std::max(uint64_t{1}, maxAgeMs)), rotation_(rotation) {}

  // The point is supplied by the fixture's panel-side coordinate source.
  PenUpdate panelFrame(uint64_t now, const uint8_t *frame, size_t len, uint32_t payloadBytes, PanelPoint point)
  {
    PenUpdate out;
    if (!advance(now, out) || !validateMatrixFrame(frame, len, payloadBytes)) return out;
    point_ = point;
    havePoint_ = true;
    pointTime_ = now;
    emitIfReady(out);
    return out;
  }

  PenUpdate notification(uint64_t now, const HogpReport &report, const uint8_t *data, size_t len)
  {
    PenUpdate out;
    PenAxes axes;
    if (!advance(now, out) || !decodeHogpNotification(report, data, len, axes)) return out;
    if (axes.flags & kInRange) {
      axes_ = axes;
      haveAxes_ = true;
      axesTime_ = now;
      emitIfReady(out);
    } else {
      endRange(out);
    }
    return out;
  }

  PenUpdate poll(uint64_t now)
  {
    PenUpdate out;
    advance(now, out);
    return out;
  }

  PenUpdate disconnect(uint64_t now)
  {
    PenUpdate out;
    if (advance(now, out)) endRange(out);
    return out;
  }

  // Call for each validated touch snapshot, including palms and the empty
  // snapshot. After range exit, require a fresh all-up frame before forwarding
  // further contacts so a resting finger or palm cannot become a new touch.
  bool forwardTouches(unsigned contacts)
  {
    if (!touchBlocked_) return true;
    if (!inRange_ && contacts == 0) touchBlocked_ = false;
    return false;
  }

private:
  bool advance(uint64_t now, PenUpdate &out)
  {
    if (now < now_) return false;
    now_ = now;
    if (havePoint_ && now - pointTime_ >= maxAgeMs_) havePoint_ = false;
    if (haveAxes_ && now - axesTime_ >= maxAgeMs_) haveAxes_ = false;
    if (inRange_ && (!havePoint_ || !haveAxes_)) endRange(out);
    return true;
  }

  void emit(PenSample sample, PenUpdate &out) const
  {
    out.hasReport = packPenReport(sample, rotation_, out.report, sizeof out.report);
  }

  void emitIfReady(PenUpdate &out)
  {
    if (!havePoint_ || !haveAxes_) return;
    if (!inRange_) {
      inRange_ = true;
      touchBlocked_ = true;
      out.cancelTouches = true;
    }
    emit({point_, axes_}, out);
  }

  void endRange(PenUpdate &out)
  {
    if (inRange_) emit({point_, {}}, out);
    inRange_ = havePoint_ = haveAxes_ = false;
  }

  const uint64_t maxAgeMs_;
  const uint32_t rotation_;
  uint64_t now_ = 0, pointTime_ = 0, axesTime_ = 0;
  PanelPoint point_;
  PenAxes axes_;
  bool havePoint_ = false, haveAxes_ = false, inRange_ = false, touchBlocked_ = false;
};

}  // namespace pianotouch::stylus
#endif
