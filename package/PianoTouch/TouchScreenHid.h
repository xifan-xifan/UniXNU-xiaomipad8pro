// SPDX-License-Identifier: BSD-2-Clause-Patent
// The native touch screen PianoTouch publishes at piano-touch-native=1|2 (PianoTouchScreen): a standard HID multi-touch Touch Screen
// on the Digitizer page, so that macOS 27.3 takes the panel as a direct-touch screen (private host-checks 20261122-touch
// NATIVE-TOUCH.md): IOHIDFamily's IOHIDEventDriver parses it (a finger transducer per Finger collection, collection dispatch because
// of the Untouch usage, Finger rather than Hand because of Touch Valid), IOHIDEventService marks the service DisplayIntegrated for its
// Touch Screen usage, and WindowServer turns the digitizer events of a service that conforms to Digitizer / Touch Screen into
// direct-touch events (touches at the finger; AppKit's touch views get them, the others a click, scroll, magnify or rotation).
// Written from the HID 1.11 specification and the HID Usage Tables (pages 0x01 and 0x0D); no Apple descriptor or payload.
// Report (ID 1, 49 bytes): eight slots of 6 bytes. A slot: byte 0 bit 0 Tip Switch, 1 In Range, 2 Touch Valid (confidence), 3 Untouch
// (with the tip: the touch is cancelled), bits 4-7 zero; byte 1 the Contact Identifier (0 = an empty slot; TouchNative.h uses
// slot + 1, a path index BackBoard keeps under 30); bytes 2-3 X, 4-5 Y, LE16 in 0..32767 over the rotated desktop (or the panel,
// TouchNative.h's axes). Physical extents: the panel's 236.8 x 158.1 mm in 0.1 mm (IOHIDEventDriver's SurfaceDimensions).
#ifndef PIANO_TOUCH_SCREEN_HID_H
#define PIANO_TOUCH_SCREEN_HID_H
#include <stdint.h>

namespace pianotouch {

// Identity: vendor 0x1af4 as PianoTouchHID's, product 'TS'; the location ID 'PTS\0'. Never Apple's vendor or the manufacturer
// "Apple": AppleMultitouchDriver's "AppleMultitouchHIDService (0x0D,0x04)" personality would take an Apple touch screen for the
// multitouch parser.
constexpr uint32_t kScreenVendorId = 0x1af4, kScreenProductId = 0x5453, kScreenLocationId = 0x50545300;
constexpr unsigned kScreenSlots = 8, kScreenSlotBytes = 6;
constexpr unsigned kScreenReportBytes = 1 + kScreenSlots * kScreenSlotBytes;   // 49
constexpr uint8_t kScreenReportId = 1;
constexpr int32_t kScreenMax = 32767;
constexpr int32_t kScreenPhysW = 2368, kScreenPhysH = 1581;   // 0.1 mm: 3200 and 2136 pixels of 74 um
// Usage pages and usages (HID Usage Tables 1.x; tests/touch_native_host.cpp checks them against the SDK's IOHIDUsageTables.h).
constexpr uint8_t kPageGenericDesktop = 0x01, kPageDigitizer = 0x0d;
constexpr uint8_t kUsageX = 0x30, kUsageY = 0x31;
constexpr uint8_t kUsageTouchScreen = 0x04, kUsageFinger = 0x22, kUsageInRange = 0x32, kUsageUntouch = 0x34, kUsageTipSwitch = 0x42,
                  kUsageTouchValid = 0x47, kUsageContactId = 0x51;
constexpr uint8_t kSlotTip = 0x01, kSlotInRange = 0x02, kSlotValid = 0x04, kSlotUntouch = 0x08;

struct ScreenDescriptor {
  uint8_t bytes[768];
  unsigned length;
};

constexpr ScreenDescriptor makeScreenDescriptor()
{
  ScreenDescriptor d = {};
  unsigned n = 0;
  auto item = [&](uint8_t tag, int32_t value, unsigned size) {   // a short item with 1 or 2 data bytes
    d.bytes[n++] = uint8_t(tag | size);
    for (unsigned i = 0; i < size; ++i) d.bytes[n++] = uint8_t(uint32_t(value) >> (8 * i));
  };
  item(0x04, kPageDigitizer, 1); item(0x08, kUsageTouchScreen, 1);   // Usage Page (Digitizer), Usage (Touch Screen)
  item(0xa0, 0x01, 1);                                                // Collection (Application)
  item(0x84, kScreenReportId, 1);                                     //   Report ID (1)
  for (unsigned s = 0; s < kScreenSlots; ++s) {
    item(0x04, kPageDigitizer, 1); item(0x08, kUsageFinger, 1);       //   Usage Page (Digitizer), Usage (Finger)
    item(0xa0, 0x02, 1);                                              //   Collection (Logical)
    item(0x64, 0, 1); item(0x54, 0, 1);                               //     Unit (none), Unit Exponent (0)
    item(0x34, 0, 1); item(0x44, 0, 1);                               //     Physical 0..0 (= logical)
    item(0x08, kUsageTipSwitch, 1); item(0x08, kUsageInRange, 1);     //     Usage (Tip Switch), Usage (In Range)
    item(0x08, kUsageTouchValid, 1); item(0x08, kUsageUntouch, 1);    //     Usage (Touch Valid), Usage (Untouch)
    item(0x14, 0, 1); item(0x24, 1, 1);                               //     Logical 0..1
    item(0x74, 1, 1); item(0x94, 4, 1); item(0x80, 0x02, 1);          //     1 bit x 4, Input (Data, Variable, Absolute)
    item(0x94, 4, 1); item(0x80, 0x03, 1);                            //     1 bit x 4, Input (Constant, Variable): padding
    item(0x08, kUsageContactId, 1);                                   //     Usage (Contact Identifier)
    item(0x24, 255, 2);                                               //     Logical 0..255
    item(0x74, 8, 1); item(0x94, 1, 1); item(0x80, 0x02, 1);          //     8 bits, Input (Data, Variable, Absolute)
    item(0x04, kPageGenericDesktop, 1);                               //     Usage Page (Generic Desktop)
    item(0x24, kScreenMax, 2);                                        //     Logical 0..32767
    item(0x64, 0x11, 1); item(0x54, 0x0e, 1);                         //     Unit (SI linear: cm), Unit Exponent (-2): 0.1 mm
    item(0x74, 16, 1); item(0x94, 1, 1);                              //     16 bits x 1
    item(0x44, kScreenPhysW, 2); item(0x08, kUsageX, 1);              //     Physical 0..2368, Usage (X)
    item(0x80, 0x02, 1);                                              //     Input (Data, Variable, Absolute)
    item(0x44, kScreenPhysH, 2); item(0x08, kUsageY, 1);              //     Physical 0..1581, Usage (Y)
    item(0x80, 0x02, 1);                                              //     Input (Data, Variable, Absolute)
    d.bytes[n++] = 0xc0;                                              //   End Collection
  }
  d.bytes[n++] = 0xc0;                                                // End Collection
  d.length = n;
  return d;
}
constexpr ScreenDescriptor kScreenDescriptor = makeScreenDescriptor();
static_assert(kScreenDescriptor.length < sizeof kScreenDescriptor.bytes, "descriptor buffer");

// One slot of a report.
struct ScreenSlot {
  uint8_t flags;      // kSlotTip | kSlotInRange | kSlotValid | kSlotUntouch
  uint8_t contact;    // 0: empty
  uint16_t x, y;      // 0..32767
};

// Pixels (0..size-1) to 0..32767, clamped.
inline uint16_t screenScale(int32_t v, int32_t size)
{
  if (size < 2) return 0;
  if (v < 0) v = 0;
  if (v > size - 1) v = size - 1;
  return uint16_t(int64_t(v) * kScreenMax / (size - 1));
}

// A display UUID as WindowServer writes and parses one (NSUUID's string form, 8-4-4-4-12 hex digits, either case): the "displayUUID"
// the touch screen's event service carries (piano-touch-native-uuid, or root's "uuid=" word). BackBoard files a digitizer without one
// under "<main>", which WindowServer's render space (its hit test) cannot parse: no touch reaches an app (NATIVE-TOUCH.md section 8).
inline bool screenUuidValid(const char *u)
{
  if (!u) return false;
  for (unsigned i = 0; i < 36; ++i) {
    const char c = u[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return u[36] == 0;
}

// The report of eight slots: 49 bytes, the report ID first. Returns its length.
inline unsigned screenReport(const ScreenSlot slots[kScreenSlots], uint8_t out[kScreenReportBytes])
{
  out[0] = kScreenReportId;
  for (unsigned s = 0; s < kScreenSlots; ++s) {
    uint8_t *p = out + 1 + s * kScreenSlotBytes;
    p[0] = uint8_t(slots[s].flags & 0x0f);
    p[1] = slots[s].contact;
    p[2] = uint8_t(slots[s].x);
    p[3] = uint8_t(slots[s].x >> 8);
    p[4] = uint8_t(slots[s].y);
    p[5] = uint8_t(slots[s].y >> 8);
  }
  return kScreenReportBytes;
}

}  // namespace pianotouch
#endif
