// SPDX-License-Identifier: BSD-2-Clause-Patent
// The multitouch HID device PianoTouch publishes with piano-touch-mt=1 (TouchMt.h decides what goes to it): a trackpad of Apple's
// multitouch family as macOS's AppleMultitouchDriver drives one, so that its MultitouchHID plug-in recognises the gestures (pinch,
// rotate, swipes, scrolling with momentum) and WindowServer gives AppKit magnify / rotate / swipe / gesture events and NSTouches.
// Written for piano from documented behaviour only (no code text of any of these): the frame layout of Linux's hid-magicmouse
// (GPL-2.0: facts read, nothing taken) and of VoodooInput's trackpad simulator (acidanthera, GPL-2.0: the same), the feature reports
// such a trackpad answers as VoodooInput documents them (device answers, i.e. data), the HID 1.11 specification for the descriptor.
// Private analysis: host-checks/20261122-touch/ANALYSIS.md section 5.
//   Identity    vendor 0x05AC, product 0x0281, version 0x0804, transport "I2C" (the built-in SPI/I2C trackpad family 0x81 the
//               feature answers describe); our own location ID 0x50544d00 ('PTM'), which the personality (PianoTouch.json
//               "PianoTouchMultitouch") also matches, so no real Apple trackpad is ever taken.
//   Descriptor  application Generic Desktop / Mouse with report 0x02 (three buttons, relative X/Y bytes, four constant bytes: the
//               shape a trackpad's mouse report has; the multitouch frames use the same report ID and are longer), application
//               Digitizer / Touch Pad with a 16-byte vendor report 0x3F, a vendor application 0xFF00 / 0x0C with report 0x44 of 1387
//               bytes (so MaxInputReportSize covers any frame).
//   Frame       report 0x02: a 12-byte header (ID, buttons, five zero bytes, 3 while a finger or the button is active else 2, 0x31,
//               a 24-bit little-endian timestamp = milliseconds << 3 | 4) and 9 bytes a finger: a little-endian 32-bit word with X
//               (bits 0-12, signed), Y (bits 13-25, signed, up), the finger type (26-28: 1 thumb, 2 index, 3 middle, 4 ring, 5 little)
//               and the state (29-31: 3 start, 4 touching, 7 lift, 0 gone), then touch major, minor, size, pressure, and the finger
//               number 1..15 (bits 0-3) with the angle 4 (bits 5-7, upright).
//   Surface     X -4067..4067, Y -2603..2603; its size in 0.01 mm from feature report 0xD9.
//   Features    getReport answers (bytes after the ID): 0x00 01; 0x02 01; 0xD1 81 (family); 0xD3 sensor rows/columns; 0xD0 the sensor
//               region; 0xA1 its parameters; 0xD9 width and height (u32 LE, 0.01 mm) and eight fixed bytes; 0x7F four zeros; 0xC8 08;
//               0xDB all of D1, D3, D0, A1, D9, 7F, each after its u16 LE length. setReport 0x01 with byte 1 = R asks for report R's
//               size: the next getReport 0x01 answers 01 R 00 LEN 00 (LEN: the bytes after R's ID).
// Integer only, no allocation, no I/O: tests/touch_mt_host.cpp checks the descriptor parse, every answer and the frame bit packing.
#ifndef PIANO_TOUCH_MT_HID_H
#define PIANO_TOUCH_MT_HID_H
#include <stdint.h>

namespace pianotouch {

constexpr uint32_t kMtVendorId = 0x05ac, kMtProductId = 0x0281, kMtVersion = 0x0804, kMtLocationId = 0x50544d00;
constexpr int32_t kMtMinX = -4067, kMtMaxX = 4067, kMtMinY = -2603, kMtMaxY = 2603;
constexpr unsigned kMtHeaderBytes = 12, kMtFingerBytes = 9, kMtMaxFingers = 10;
constexpr unsigned kMtMaxReport = kMtHeaderBytes + kMtFingerBytes * kMtMaxFingers;
constexpr uint8_t kMtReportId = 0x02;
constexpr uint8_t kMtStateGone = 0, kMtStateStart = 3, kMtStateTouching = 4, kMtStateLift = 7;
constexpr uint8_t kMtTypeUndefined = 0, kMtTypeThumb = 1, kMtTypeIndex = 2, kMtTypeMiddle = 3, kMtTypeRing = 4, kMtTypeLittle = 5;
// A finger without force sensing: the sizes a fingertip reports, the pressure of a light touch (and of a press with the button).
constexpr uint8_t kMtMajor = 20, kMtMinor = 20, kMtSize = 10, kMtPressure = 5, kMtPressureButton = 120;

struct MtFinger {
  uint8_t id;       // 1..15
  int32_t x, y;     // surface units, y up
  uint8_t type, state;
  uint8_t major, minor, size, pressure;
};

// ---------------------------------------------------------------------------------------------------------------- the descriptor
struct MtDescriptor {
  uint8_t bytes[128];
  unsigned length;
};
constexpr MtDescriptor makeMtDescriptor()
{
  MtDescriptor d = {};
  unsigned n = 0;
  auto item = [&](uint8_t tag, int32_t value, unsigned size) {   // a short item: tag | size, then 1 or 2 data bytes
    d.bytes[n++] = uint8_t(tag | (size == 4 ? 3 : size));
    for (unsigned i = 0; i < size; ++i) d.bytes[n++] = uint8_t(uint32_t(value) >> (8 * i));
  };
  // Generic Desktop / Mouse: report 0x02 = buttons, X, Y, four constant bytes
  item(0x04, 0x01, 1); item(0x08, 0x02, 1); item(0xa0, 0x01, 1);          // Usage Page (GD), Usage (Mouse), Collection (Application)
  item(0x08, 0x01, 1); item(0xa0, 0x00, 1);                               //   Usage (Pointer), Collection (Physical)
  item(0x84, kMtReportId, 1);                                             //     Report ID (2)
  item(0x04, 0x09, 1); item(0x18, 1, 1); item(0x28, 3, 1);                //     Usage Page (Button), Usage Minimum 1, Maximum 3
  item(0x14, 0, 1); item(0x24, 1, 1); item(0x74, 1, 1); item(0x94, 3, 1);
  item(0x80, 0x02, 1);                                                    //     Input (Data, Variable, Absolute)
  item(0x74, 5, 1); item(0x94, 1, 1); item(0x80, 0x01, 1);                //     5 bits Input (Constant)
  item(0x04, 0x01, 1); item(0x08, 0x30, 1); item(0x08, 0x31, 1);          //     Usage Page (GD), X, Y
  item(0x14, -127, 1); item(0x24, 127, 1); item(0x74, 8, 1); item(0x94, 2, 1);
  item(0x80, 0x06, 1);                                                    //     Input (Data, Variable, Relative)
  item(0x74, 8, 1); item(0x94, 4, 1); item(0x80, 0x01, 1);                //     4 bytes Input (Constant)
  d.bytes[n++] = 0xc0; d.bytes[n++] = 0xc0;                               //   End Collection, End Collection
  // Digitizer / Touch Pad: a 16-byte vendor report 0x3F
  item(0x04, 0x0d, 1); item(0x08, 0x05, 1); item(0xa0, 0x01, 1);          // Usage Page (Digitizer), Usage (Touch Pad), Application
  item(0x04, 0xff00, 2); item(0x08, 0x0c, 1);                             //   Usage Page (vendor 0xFF00), Usage (0x0C)
  item(0x14, 0, 1); item(0x24, 0xff, 2); item(0x74, 8, 1); item(0x94, 16, 1);
  item(0x84, 0x3f, 1); item(0x80, 0x22, 1);                               //   Report ID (0x3F), Input (Data, Variable, No Preferred)
  d.bytes[n++] = 0xc0;
  // vendor 0xFF00 / 0x0C: report 0x44 of 1387 bytes
  item(0x04, 0xff00, 2); item(0x08, 0x0c, 1); item(0xa0, 0x01, 1);        // Usage Page (vendor), Usage (0x0C), Application
  item(0x04, 0xff00, 2); item(0x08, 0x0c, 1);
  item(0x14, 0, 1); item(0x24, 0xff, 2); item(0x84, 0x44, 1); item(0x74, 8, 1);
  item(0x94, 1387, 2); item(0x80, 0x00, 1);                               //   1387 bytes, Input (Data, Array)
  d.bytes[n++] = 0xc0;
  d.length = n;
  return d;
}
constexpr MtDescriptor kMtDescriptor = makeMtDescriptor();

// ------------------------------------------------------------------------------------------------------------ the feature answers
// The sensor surface's size (0.01 mm) for a square of side `side` (the panel's long side, so the desktop fits in either rotation).
struct MtSurface {
  uint32_t width, height;
};
namespace detail {
constexpr uint8_t kMtD3[] = {0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00};   // rows 22, columns 30, ...
constexpr uint8_t kMtD0[] = {0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00};
constexpr uint8_t kMtA1[] = {0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01};
constexpr uint8_t kMtD9Tail[] = {0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26};
inline unsigned put(uint8_t *out, unsigned cap, unsigned at, const uint8_t *src, unsigned n)
{
  for (unsigned i = 0; i < n && at + i < cap; ++i) out[at + i] = src[i];
  return at + n;
}
inline unsigned putLe(uint8_t *out, unsigned cap, unsigned at, uint32_t v, unsigned bytes)
{
  for (unsigned i = 0; i < bytes; ++i)
    if (at + i < cap) out[at + i] = uint8_t(v >> (8 * i));
  return at + bytes;
}
inline unsigned d9(uint8_t *out, unsigned cap, unsigned at, MtSurface s)
{
  if (at < cap) out[at] = 0xd9;
  at = putLe(out, cap, at + 1, s.width, 4);
  at = putLe(out, cap, at, s.height, 4);
  return put(out, cap, at, kMtD9Tail, sizeof kMtD9Tail);
}
}  // namespace detail

// The size of report `id`'s answer, ID included (0: not a report this device has).
inline unsigned mtFeatureBytes(uint8_t id)
{
  switch (id) {
  case 0x00: case 0x02: case 0xd1: case 0xc8: return 2;
  case 0x01: return 5;
  case 0xd3: return sizeof detail::kMtD3;
  case 0xd0: return sizeof detail::kMtD0;
  case 0xa1: return sizeof detail::kMtA1;
  case 0xd9: return 17;
  case 0x7f: return 5;
  case 0xdb: return 2 + 2 * 6 + 2 + 13 + 16 + 7 + 17 + 5;   // 74
  default: return 0;
  }
}

// getReport(id): the answer into out (at most cap bytes); `asked` is the report a setReport 0x01 asked about (for id 0x01).
// Returns the length, 0 for a report this device does not have.
inline unsigned mtFeature(uint8_t id, uint8_t asked, MtSurface s, uint8_t *out, unsigned cap)
{
  using namespace detail;
  unsigned at = 0;
  switch (id) {
  case 0x00: case 0x02: { const uint8_t b[] = {id, 0x01}; return put(out, cap, 0, b, 2); }
  case 0xd1: { const uint8_t b[] = {0xd1, 0x81}; return put(out, cap, 0, b, 2); }
  case 0xc8: { const uint8_t b[] = {0xc8, 0x08}; return put(out, cap, 0, b, 2); }
  case 0x7f: { const uint8_t b[] = {0x7f, 0, 0, 0, 0}; return put(out, cap, 0, b, 5); }
  case 0xd3: return put(out, cap, 0, kMtD3, sizeof kMtD3);
  case 0xd0: return put(out, cap, 0, kMtD0, sizeof kMtD0);
  case 0xa1: return put(out, cap, 0, kMtA1, sizeof kMtA1);
  case 0xd9: return d9(out, cap, 0, s);
  case 0x01: {
    const unsigned len = mtFeatureBytes(asked);
    const uint8_t b[] = {0x01, asked, 0x00, uint8_t(len ? len - 1 : 0), 0x00};
    return put(out, cap, 0, b, 5);
  }
  case 0xdb: {
    const uint8_t head[] = {0xdb, 0x01};
    at = put(out, cap, 0, head, 2);
    const uint8_t d1[] = {0xd1, 0x81}, f7[] = {0x7f, 0, 0, 0, 0};
    at = putLe(out, cap, at, 2, 2); at = put(out, cap, at, d1, 2);
    at = putLe(out, cap, at, sizeof kMtD3, 2); at = put(out, cap, at, kMtD3, sizeof kMtD3);
    at = putLe(out, cap, at, sizeof kMtD0, 2); at = put(out, cap, at, kMtD0, sizeof kMtD0);
    at = putLe(out, cap, at, sizeof kMtA1, 2); at = put(out, cap, at, kMtA1, sizeof kMtA1);
    at = putLe(out, cap, at, 17, 2); at = d9(out, cap, at, s);
    at = putLe(out, cap, at, 5, 2); at = put(out, cap, at, f7, 5);
    return at;
  }
  default: return 0;
  }
}

// ------------------------------------------------------------------------------------------------------------------- the frames
inline uint32_t mtTimestamp(uint32_t ms) { return ((ms << 3) | 4) & 0xffffff; }

// One frame (report 0x02): the header and n fingers. Returns its length (12 + 9 n).
inline unsigned mtFrame(const MtFinger *f, unsigned n, uint8_t button, uint32_t ms, uint8_t out[kMtMaxReport])
{
  if (n > kMtMaxFingers) n = kMtMaxFingers;
  bool active = button != 0;
  for (unsigned i = 0; i < n; ++i) active = active || f[i].state == kMtStateStart || f[i].state == kMtStateTouching;
  for (unsigned i = 0; i < kMtHeaderBytes; ++i) out[i] = 0;
  out[0] = kMtReportId;
  out[1] = button & 1;
  out[7] = active ? 0x03 : 0x02;
  out[8] = 0x31;
  const uint32_t t = mtTimestamp(ms);
  out[9] = uint8_t(t);
  out[10] = uint8_t(t >> 8);
  out[11] = uint8_t(t >> 16);
  for (unsigned i = 0; i < n; ++i) {
    uint8_t *p = out + kMtHeaderBytes + kMtFingerBytes * i;
    const int32_t x = f[i].x < kMtMinX ? kMtMinX : f[i].x > kMtMaxX ? kMtMaxX : f[i].x;
    const int32_t y = f[i].y < kMtMinY ? kMtMinY : f[i].y > kMtMaxY ? kMtMaxY : f[i].y;
    const uint32_t w = (uint32_t(x) & 0x1fff) | ((uint32_t(y) & 0x1fff) << 13) | (uint32_t(f[i].type & 7) << 26) |
                       (uint32_t(f[i].state & 7) << 29);
    p[0] = uint8_t(w);
    p[1] = uint8_t(w >> 8);
    p[2] = uint8_t(w >> 16);
    p[3] = uint8_t(w >> 24);
    p[4] = f[i].major;
    p[5] = f[i].minor;
    p[6] = f[i].size;
    p[7] = f[i].pressure;
    p[8] = uint8_t((f[i].id & 0x0f) | (4u << 5));
  }
  return kMtHeaderBytes + kMtFingerBytes * n;
}

// The bit fields back (tests and the trace).
inline int32_t mtSigned13(uint32_t v) { return int32_t(v << 19) >> 19; }
inline void mtUnpack(const uint8_t *p, MtFinger &f)
{
  const uint32_t w = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  f.x = mtSigned13(w & 0x1fff);
  f.y = mtSigned13((w >> 13) & 0x1fff);
  f.type = uint8_t((w >> 26) & 7);
  f.state = uint8_t((w >> 29) & 7);
  f.major = p[4];
  f.minor = p[5];
  f.size = p[6];
  f.pressure = p[7];
  f.id = p[8] & 0x0f;
}

}  // namespace pianotouch
#endif
