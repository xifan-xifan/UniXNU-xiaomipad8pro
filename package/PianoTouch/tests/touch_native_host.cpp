// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoTouch's native touch screen (piano-touch-native=1|2), no hardware:
//   - TouchScreenHid.h: the usage constants against the SDK's IOHIDUsageTables.h (when the host has it); the descriptor parsed by an
//     independent item parser written from the HID 1.11 specification: one application collection Digitizer / Touch Screen, report 1
//     of 49 bytes, eight Finger logical collections each with Tip Switch, In Range, Touch Valid, Untouch (1 bit), padding, Contact
//     Identifier (8 bits, 0..255), X and Y (16 bits, logical 0..32767, physical 0.1 mm, absolute), nothing global leaking from one item
//     into the next (the bits logical 0..1, unit none); the packer's bytes read back through the parsed field offsets;
//   - a model of IOHIDEventDriver's digitizer rules (Apple's public IOHIDFamily-2238 IOHIDEventDriver.cpp, read for the behaviour only:
//     transducers per innermost Digitizer collection, collection dispatch because of an Untouch element, Finger only with Touch Valid,
//     the transducer index from the Contact Identifier, untouched out-of-range slots dropped unless they were touching, the touch /
//     range / position mask, tip + Untouch = cancel, the transducer-state limit) fed with TouchNative.h's reports;
//   - TouchNative.h through the tracker (TouchTrack.h) and the injection's points (TouchInject.h), as PianoTouch's frame thread runs it:
//     a tap (one down, one up, path index 1, the position over the desktop), a drag, two fingers (distinct path indices, a lift of one
//     while the other moves), eight plus one fingers, a palm resting (never a touch), a finger turning palm (a cancel of every touch,
//     then nothing until every contact is up), frames lost (nativeCancel), the rotation (90 degrees: desktop axes vs panel axes), idle
//     (no report at rest), path indices under BackBoard's 30.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. touch_native_host.cpp -o /tmp/touch_native_host && /tmp/touch_native_host
#include "TouchNative.h"
#include "TouchScreenHid.h"
#include "TouchInject.h"
#include "TouchTrack.h"
#include "../PianoAuto/TouchRotation.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#if defined(__APPLE__) && __has_include(<IOKit/hid/IOHIDUsageTables.h>)
#include <IOKit/hid/IOHIDUsageTables.h>
#define HAVE_SDK_USAGES 1
#endif

using namespace pianotouch;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#ifdef HAVE_SDK_USAGES
static_assert(kPageDigitizer == kHIDPage_Digitizer && kPageGenericDesktop == kHIDPage_GenericDesktop, "pages");
static_assert(kUsageTouchScreen == kHIDUsage_Dig_TouchScreen && kUsageFinger == kHIDUsage_Dig_Finger, "collections");
static_assert(kUsageTipSwitch == kHIDUsage_Dig_TipSwitch && kUsageInRange == kHIDUsage_Dig_InRange, "tip, range");
static_assert(kUsageTouchValid == kHIDUsage_Dig_TouchValid && kUsageUntouch == kHIDUsage_Dig_Untouch, "valid, untouch");
static_assert(kUsageContactId == kHIDUsage_Dig_ContactIdentifier && kUsageX == kHIDUsage_GD_X && kUsageY == kHIDUsage_GD_Y, "id, x, y");
#endif

// ------------------------------------------------------------------------------------------------------ an independent item parser
struct Coll {
  unsigned page, usage, type;   // type: 0 physical, 1 application, 2 logical
};
struct Field {
  unsigned report, bitOffset, bits, page, usage;
  int32_t lmin, lmax, pmin, pmax;
  unsigned unit;
  int unitExp;
  bool constant, relative;
  std::vector<Coll> path;
};
struct Parsed {
  std::vector<Field> fields;
  std::vector<Coll> apps;
  std::map<unsigned, unsigned> reportBits;
  bool ok = true;
  unsigned depthEnd = 0;
};
static int32_t sx(uint32_t v, unsigned len)
{
  if (len == 1) return int8_t(v);
  if (len == 2) return int16_t(v);
  return int32_t(v);
}
static Parsed parse(const uint8_t *d, unsigned n)
{
  Parsed p;
  unsigned page = 0, size = 0, count = 0, id = 0, unit = 0;
  int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0;
  int unitExp = 0;
  std::vector<unsigned> usages;
  std::vector<Coll> stack;
  for (unsigned i = 0; i < n;) {
    const uint8_t b = d[i++];
    unsigned len = b & 3;
    if (len == 3) len = 4;
    if (i + len > n) { p.ok = false; break; }
    uint32_t v = 0;
    for (unsigned k = 0; k < len; ++k) v |= uint32_t(d[i + k]) << (8 * k);
    i += len;
    switch (b & 0xfc) {
    case 0x04: page = v; break;
    case 0x14: lmin = sx(v, len); break;
    case 0x24: lmax = sx(v, len); break;
    case 0x34: pmin = sx(v, len); break;
    case 0x44: pmax = sx(v, len); break;
    case 0x54: unitExp = (v & 8) ? int(v & 0xf) - 16 : int(v & 0xf); break;
    case 0x64: unit = v; break;
    case 0x74: size = v; break;
    case 0x84: id = v; break;
    case 0x94: count = v; break;
    case 0x08: usages.push_back(page << 16 | v); break;
    case 0xa0: {
      const unsigned u = usages.empty() ? 0 : usages.front();
      Coll c{u >> 16, u & 0xffff, v == 1 ? 1u : v == 2 ? 2u : 0u};
      if (stack.empty() && c.type == 1) p.apps.push_back(c);
      stack.push_back(c);
      usages.clear();
      break;
    }
    case 0xc0:
      if (stack.empty()) p.ok = false;
      else stack.pop_back();
      break;
    case 0x80:
      for (unsigned k = 0; k < count; ++k) {
        Field f{};
        f.report = id;
        f.bitOffset = p.reportBits[id];
        f.bits = size;
        const bool cst = (v & 1) != 0;
        const unsigned u = cst || usages.empty() ? 0 : usages[k < usages.size() ? k : usages.size() - 1];
        f.page = u >> 16;
        f.usage = u & 0xffff;
        f.lmin = lmin; f.lmax = lmax; f.pmin = pmin; f.pmax = pmax; f.unit = unit; f.unitExp = unitExp;
        f.constant = cst;
        f.relative = (v & 4) != 0;
        f.path = stack;
        p.fields.push_back(f);
        p.reportBits[id] += size;
      }
      usages.clear();
      break;
    default: p.ok = false; break;
    }
  }
  if (!stack.empty()) p.ok = false;
  return p;
}
static uint32_t getBits(const uint8_t *r, unsigned off, unsigned bits)
{
  uint32_t v = 0;
  for (unsigned k = 0; k < bits; ++k) v |= uint32_t((r[(off + k) / 8] >> ((off + k) % 8)) & 1) << k;
  return v;
}

static void testDescriptor()
{
  const Parsed p = parse(kScreenDescriptor.bytes, kScreenDescriptor.length);
  CHECK(p.ok);
  CHECK(p.apps.size() == 1 && p.apps[0].page == kPageDigitizer && p.apps[0].usage == kUsageTouchScreen);
  CHECK(p.reportBits.size() == 1 && p.reportBits.count(kScreenReportId) == 1);
  CHECK(p.reportBits.at(kScreenReportId) == 8 * (kScreenReportBytes - 1));
  unsigned slots = 0;
  for (unsigned s = 0; s < kScreenSlots; ++s) {
    unsigned tip = 0, range = 0, valid = 0, untouch = 0, id = 0, x = 0, y = 0;
    for (const Field &f : p.fields) {
      if (f.constant) continue;
      // every data field sits in the application collection and one Finger logical collection
      CHECK(f.path.size() == 2 && f.path[0].type == 1 && f.path[1].type == 2 && f.path[1].page == kPageDigitizer && f.path[1].usage == kUsageFinger);
      if (f.bitOffset / 48 != s) continue;
      const unsigned rel = f.bitOffset - 48 * s;
      CHECK(!f.relative);
      if (f.page == kPageDigitizer && f.usage == kUsageTipSwitch) { ++tip; CHECK(rel == 0 && f.bits == 1 && f.lmin == 0 && f.lmax == 1 && f.unit == 0); }
      if (f.page == kPageDigitizer && f.usage == kUsageInRange) { ++range; CHECK(rel == 1 && f.bits == 1 && f.lmax == 1); }
      if (f.page == kPageDigitizer && f.usage == kUsageTouchValid) { ++valid; CHECK(rel == 2 && f.bits == 1 && f.lmax == 1); }
      if (f.page == kPageDigitizer && f.usage == kUsageUntouch) { ++untouch; CHECK(rel == 3 && f.bits == 1 && f.lmax == 1); }
      if (f.page == kPageDigitizer && f.usage == kUsageContactId) { ++id; CHECK(rel == 8 && f.bits == 8 && f.lmin == 0 && f.lmax == 255 && f.unit == 0); }
      if (f.page == kPageGenericDesktop && f.usage == kUsageX) {
        ++x; CHECK(rel == 16 && f.bits == 16 && f.lmin == 0 && f.lmax == kScreenMax && f.pmin == 0 && f.pmax == kScreenPhysW);
        CHECK(f.unit == 0x11 && f.unitExp == -2);
      }
      if (f.page == kPageGenericDesktop && f.usage == kUsageY) {
        ++y; CHECK(rel == 32 && f.bits == 16 && f.lmax == kScreenMax && f.pmax == kScreenPhysH && f.unit == 0x11 && f.unitExp == -2);
      }
    }
    CHECK(tip == 1 && range == 1 && valid == 1 && untouch == 1 && id == 1 && x == 1 && y == 1);
    slots += tip == 1 && x == 1;
  }
  CHECK(slots == kScreenSlots);
  // the physical size: 236.8 mm x 158.1 mm = the panel's 3200 x 2136 pixels of 74 um
  CHECK(std::fabs(kScreenPhysW / 10.0 - 3200 * 0.074) < 0.1 && std::fabs(kScreenPhysH / 10.0 - 2136 * 0.074) < 0.1);
  // the packer against the parsed offsets, at the extremes
  ScreenSlot sl[kScreenSlots] = {};
  sl[0] = ScreenSlot{uint8_t(kSlotTip | kSlotInRange | kSlotValid), 1, 0, uint16_t(kScreenMax)};
  sl[7] = ScreenSlot{uint8_t(kSlotTip | kSlotUntouch | kSlotValid), 8, uint16_t(kScreenMax), 12345};
  uint8_t r[kScreenReportBytes];
  CHECK(screenReport(sl, r) == kScreenReportBytes && r[0] == kScreenReportId);
  for (const Field &f : p.fields) {
    if (f.constant) { CHECK(getBits(r + 1, f.bitOffset, f.bits) == 0); continue; }
    const unsigned s = f.bitOffset / 48;
    const uint32_t v = getBits(r + 1, f.bitOffset, f.bits);
    if (f.usage == kUsageTipSwitch && f.page == kPageDigitizer) CHECK(v == ((sl[s].flags & kSlotTip) ? 1u : 0u));
    if (f.usage == kUsageInRange && f.page == kPageDigitizer) CHECK(v == ((sl[s].flags & kSlotInRange) ? 1u : 0u));
    if (f.usage == kUsageTouchValid && f.page == kPageDigitizer) CHECK(v == ((sl[s].flags & kSlotValid) ? 1u : 0u));
    if (f.usage == kUsageUntouch && f.page == kPageDigitizer) CHECK(v == ((sl[s].flags & kSlotUntouch) ? 1u : 0u));
    if (f.usage == kUsageContactId && f.page == kPageDigitizer) CHECK(v == sl[s].contact);
    if (f.usage == kUsageX && f.page == kPageGenericDesktop) CHECK(v == sl[s].x);
    if (f.usage == kUsageY && f.page == kPageGenericDesktop) CHECK(v == sl[s].y);
  }
  CHECK(screenScale(0, 3200) == 0 && screenScale(3199, 3200) == kScreenMax && screenScale(-5, 3200) == 0 && screenScale(9999, 3200) == kScreenMax);
  CHECK(screenScale(1600, 3201) == kScreenMax / 2);
}

// ------------------------------------------------------------------------------------- a model of IOHIDEventDriver's digitizer rules
enum : uint32_t { kMaskRange = 1, kMaskTouch = 2, kMaskPosition = 4, kMaskCancel = 0x80 };
struct Child {
  uint32_t index;       // the transducer index (Contact Identifier): BackBoard's path index
  bool finger, touch, range;
  uint32_t mask;
  double x, y;          // 0..1
};
struct Collection {
  std::vector<Child> children;
  bool touch = false, range = false;
};
struct DriverModel {
  struct Transducer {
    std::vector<const Field *> el;
  };
  struct State {
    bool byId;
    uint32_t index;
    bool touch = false, range = false;
    double x = 0, y = 0;
  };
  Parsed p;
  std::vector<Transducer> tr;
  std::vector<State> states;
  bool collectionDispatch = false;
  unsigned overloads = 0;
  DriverModel() : p(parse(kScreenDescriptor.bytes, kScreenDescriptor.length))
  {
    // transducers keyed by their collection: its depth and which Finger collection (the parser's fields carry no collection
    // identity, and each Finger collection spans its own 48 bits)
    std::map<std::pair<unsigned, unsigned>, unsigned> byColl;
    for (const Field &f : p.fields) {
      if (f.constant || f.usage == 0) continue;
      if (f.page == kPageDigitizer && f.usage == kUsageUntouch) collectionDispatch = true;
      // the innermost Digitizer collection: logical / physical of usage Stylus..Gesture Character, or the application
      int parent = -1;
      for (int k = int(f.path.size()) - 1; k >= 0; --k) {
        const Coll &c = f.path[k];
        if (c.page != kPageDigitizer) continue;
        if (c.type == 1 ? (c.usage >= 0x01 && c.usage <= 0x0e) : (c.usage >= 0x20 && c.usage <= 0x24)) { parent = k; break; }
      }
      if (parent < 0) continue;
      const auto key = std::make_pair(unsigned(parent), f.bitOffset / 48);
      if (!byColl.count(key)) { byColl[key] = unsigned(tr.size()); tr.push_back({}); }
      tr[byColl[key]].el.push_back(&f);
    }
  }
  // one input report: the collection event (or none), as handleDigitizerCollectionReport makes it
  bool report(const uint8_t *r, Collection &out)
  {
    out = Collection{};
    bool any = false;
    for (const Transducer &t : tr) {
      bool touch = false, untouch = false, range = true, hasRange = false, valid = false, hasId = false;
      uint32_t id = 0;
      double x = 0, y = 0;
      for (const Field *f : t.el) {
        const uint32_t v = getBits(r + 1, f->bitOffset, f->bits);
        if (f->page == kPageGenericDesktop && f->usage == kUsageX) x = double(int32_t(v) - f->lmin) / (f->lmax - f->lmin);
        if (f->page == kPageGenericDesktop && f->usage == kUsageY) y = double(int32_t(v) - f->lmin) / (f->lmax - f->lmin);
        if (f->page != kPageDigitizer) continue;
        if (f->usage == kUsageContactId) { id = v; hasId = true; }
        if (f->usage == kUsageUntouch) untouch = v != 0;
        if (f->usage == kUsageTipSwitch) touch = v != 0;
        if (f->usage == kUsageInRange) { range = v != 0; hasRange = true; }
        if (f->usage == kUsageTouchValid) valid = v == 1;
      }
      if (!hasRange && (untouch || !touch)) range = false;
      State *st = nullptr, *backup = nullptr;
      for (State &s : states) {
        if (s.byId == hasId && s.index == id) { st = &s; break; }
        if (!s.touch) backup = &s;
      }
      if (!st) {
        if (backup) { st = backup; st->byId = hasId; st->index = id; st->range = false; st->x = st->y = 0; }
        else if (states.size() < tr.size()) { states.push_back(State{hasId, id}); st = &states.back(); }
      }
      if (!st) { ++overloads; continue; }
      uint32_t mask = 0;
      if (touch != st->touch) mask |= kMaskTouch;
      if (touch && untouch) mask |= kMaskCancel;
      if (range && (st->x != x || st->y != y)) mask |= kMaskPosition;
      if (range != st->range) mask |= kMaskRange;
      const bool drop = st->touch == touch && !touch && !range;
      if (range) { st->x = x; st->y = y; }
      st->touch = touch;
      st->range = range;
      if (drop) continue;
      out.children.push_back(Child{id, valid, touch, range, mask, x, y});
      out.touch = out.touch || touch;
      out.range = out.range || range;
      any = true;
    }
    return any;
  }
};

static void testDriverModel()
{
  DriverModel m;
  CHECK(m.collectionDispatch);          // the Untouch elements
  CHECK(m.tr.size() == kScreenSlots);   // a finger transducer per Finger collection, none for the application collection
  for (auto &t : m.tr) CHECK(t.el.size() == 7);
  // an empty report: no event
  ScreenSlot sl[kScreenSlots] = {};
  uint8_t r[kScreenReportBytes];
  Collection c;
  screenReport(sl, r);
  CHECK(!m.report(r, c));
  // a touch in slot 2 (contact 3): one finger child, touch + range masks
  sl[2] = ScreenSlot{uint8_t(kSlotTip | kSlotInRange | kSlotValid), 3, 16384, 8192};
  screenReport(sl, r);
  CHECK(m.report(r, c) && c.children.size() == 1);
  CHECK(c.children[0].index == 3 && c.children[0].finger && c.children[0].touch && (c.children[0].mask & kMaskTouch) && (c.children[0].mask & kMaskRange));
  CHECK(std::fabs(c.children[0].x - 0.5) < 0.001 && std::fabs(c.children[0].y - 0.25) < 0.001);
  // cancel: tip + Untouch
  sl[2].flags = uint8_t(kSlotTip | kSlotInRange | kSlotValid | kSlotUntouch);
  screenReport(sl, r);
  CHECK(m.report(r, c) && c.children.size() == 1 && (c.children[0].mask & kMaskCancel));
  // without Touch Valid the same touch would be a Hand
  sl[2].flags = uint8_t(kSlotTip | kSlotInRange);
  screenReport(sl, r);
  CHECK(m.report(r, c) && c.children.size() == 1 && !c.children[0].finger);
}

// ------------------------------------------------------------------------------------------------- the router through the tracker
struct Rig {
  Tracker tr;
  NativeRouter r;
  DriverModel m;
  uint32_t degrees = 0;
  uint64_t now = 1000;
  std::vector<std::vector<uint8_t>> reports;
  std::vector<Collection> events;
  std::vector<NativeNote> notes;
  Rig(bool panelAxes = false)
  {
    trackerReset(tr, TrackConfig());
    nativeReset(r, panelAxes);
  }
  void take(const NativeOut &o)
  {
    for (unsigned i = 0; i < o.count; ++i) {
      reports.emplace_back(o.report[i], o.report[i] + kScreenReportBytes);
      Collection c;
      if (m.report(o.report[i], c)) events.push_back(c);
    }
    for (unsigned i = 0; i < o.noteCount; ++i) notes.push_back(o.notes[i]);
  }
  void frame(std::vector<InjectContact> pts)
  {
    MatrixPoint p[kMaxPoints] = {};
    InjectScript sc;
    sc.count = 1;
    sc.s[0].ms = 0;
    sc.s[0].n = uint8_t(pts.size());
    for (unsigned i = 0; i < pts.size(); ++i) sc.s[0].c[i] = pts[i];
    const unsigned n = injectAt(sc, 0, 200, p);
    trackerUpdate(tr, p, n, now);
    NativeContact c[kMaxTracks];
    unsigned k = 0;
    for (const Track &t : tr.t) {
      if (t.state != TrackState::active) continue;
      const PatPoint q = pat_rotate(degrees, t.x, t.y, kPanelW, kPanelH);
      c[k++] = NativeContact{t.id, t.palm, q.x, q.y, t.x, t.y};
    }
    const PatPoint desk = pat_rotate(degrees, 0, 0, kPanelW, kPanelH);
    NativeOut o;
    nativeUpdate(r, c, k, desk.width, desk.height, kPanelW, kPanelH, o);
    take(o);
    now += 8;
  }
  void lift(unsigned n = 6) { for (unsigned i = 0; i < n; ++i) frame({}); }
  unsigned count(NativeNoteKind k) const { unsigned n = 0; for (auto &x : notes) n += x.kind == k; return n; }
  // touch transitions in the model's events: (index, down?)
  std::vector<std::pair<uint32_t, bool>> transitions() const
  {
    std::vector<std::pair<uint32_t, bool>> t;
    for (auto &e : events)
      for (auto &ch : e.children)
        if (ch.mask & kMaskTouch) t.push_back({ch.index, ch.touch});
    return t;
  }
};

static void testTap()
{
  Rig rig;
  for (int i = 0; i < 6; ++i) rig.frame({{1600, 1068, kFingerNodes}});
  rig.lift();
  CHECK(rig.r.downs == 1 && rig.r.ups == 1 && rig.r.cancels == 0);
  const auto t = rig.transitions();
  CHECK(t.size() == 2 && t[0] == std::make_pair(1u, true) && t[1] == std::make_pair(1u, false));
  // the lift is the last report (the tracker holds a contact a few frames after it is gone): nothing at rest after it
  CHECK(!rig.events.empty() && rig.events.back().children.size() == 1 && !rig.events.back().children[0].touch);
  CHECK(rig.reports.size() == rig.events.size());
  const size_t afterLift = rig.reports.size();
  rig.lift();
  CHECK(rig.reports.size() == afterLift && nativeIdle(rig.r));
  // the position: the panel's middle = the desktop's middle (0..1)
  const Child &first = rig.events.front().children.front();
  CHECK(first.finger && std::fabs(first.x - 1600.0 / 3199) < 0.002 && std::fabs(first.y - 1068.0 / 2135) < 0.002);
  for (auto &e : rig.events) CHECK(e.children.size() == 1);
  // after the lift no event; a second tap takes path index 1 again
  for (int i = 0; i < 4; ++i) rig.frame({{800, 500, kFingerNodes}});
  rig.lift();
  const auto t2 = rig.transitions();
  CHECK(t2.size() == 4 && t2[2] == std::make_pair(1u, true) && t2[3] == std::make_pair(1u, false));
  CHECK(rig.m.overloads == 0);
}

static void testDrag()
{
  Rig rig;
  for (int i = 0; i <= 40; ++i) rig.frame({{800 + 25 * i, 600, kFingerNodes}});
  rig.lift();
  unsigned moves = 0;
  double lastX = -1;
  bool monotonic = true;
  for (auto &e : rig.events)
    for (auto &ch : e.children) {
      if (ch.touch && (ch.mask & kMaskPosition) && !(ch.mask & kMaskTouch)) ++moves;
      if (ch.touch) { monotonic = monotonic && ch.x >= lastX; lastX = ch.x; }
    }
  CHECK(moves >= 30 && monotonic);
  CHECK(std::fabs(lastX - 1800.0 / 3199) < 0.01);
  CHECK(rig.transitions().size() == 2);
}

static void testTwoFingers()
{
  Rig rig;
  for (int i = 0; i <= 20; ++i) rig.frame({{1400 - 10 * i, 1068, kFingerNodes}, {1800 + 10 * i, 1068, kFingerNodes}});
  CHECK(rig.r.downs == 2 && rig.r.maxFingers == 2);
  const Collection &mid = rig.events.back();
  CHECK(mid.children.size() == 2 && mid.children[0].index != mid.children[1].index);
  for (auto &ch : mid.children) CHECK(ch.index >= 1 && ch.index <= 2 && ch.touch && ch.finger);
  // one finger lifts, the other goes on moving
  for (int i = 0; i < 10; ++i) rig.frame({{1200 - 10 * i, 1068, kFingerNodes}});
  rig.lift();
  const auto t = rig.transitions();
  CHECK(t.size() == 4);
  CHECK(rig.r.ups == 2 && nativeIdle(rig.r) && rig.m.overloads == 0);
  // the remaining finger kept its path index: after the right finger's lift every touching child is the left one's
  uint32_t leftIndex = 0, rightIndex = 0;
  for (auto &ch : rig.events.front().children) (ch.x < 0.5 ? leftIndex : rightIndex) = ch.index;
  size_t rightUp = 0;
  for (size_t i = 0; i < rig.events.size() && !rightUp; ++i)
    for (auto &ch : rig.events[i].children) if (ch.index == rightIndex && !ch.touch && (ch.mask & kMaskTouch)) rightUp = i;
  bool kept = rightUp > 0;
  unsigned after = 0;
  for (size_t i = rightUp + 1; i < rig.events.size(); ++i)
    for (auto &ch : rig.events[i].children) if (ch.touch) { kept = kept && ch.index == leftIndex; ++after; }
  CHECK(leftIndex != 0 && rightIndex != 0 && leftIndex != rightIndex && kept && after >= 5);
}

static void testNineFingers()
{
  // TouchTrack.h follows eight contacts and the injection plays five at most: a direct router test of the ninth finger
  NativeRouter r;
  nativeReset(r, false);
  NativeContact c[9];
  for (unsigned i = 0; i < 9; ++i) c[i] = NativeContact{uint8_t(10 + i), false, int32_t(200 + 300 * i), 1000, 0, 0};
  NativeOut o;
  nativeUpdate(r, c, 9, 3200, 2136, 3200, 2136, o);
  CHECK(o.count == 1 && r.downs == 8 && r.full == 1);
  for (unsigned s = 0; s < kScreenSlots; ++s) CHECK(o.report[0][1 + 6 * s + 1] == s + 1);   // path indices 1..8 (< 30)
  nativeUpdate(r, c, 0, 3200, 2136, 3200, 2136, o);
  CHECK(o.count == 1 && r.ups == 8);
  for (unsigned s = 0; s < kScreenSlots; ++s) CHECK((o.report[0][1 + 6 * s] & kSlotTip) == 0);
  nativeUpdate(r, c, 0, 3200, 2136, 3200, 2136, o);
  CHECK(o.count == 0 && nativeIdle(r));
}

static void testPalms()
{
  {  // a palm resting while a finger drags: the palm never becomes a touch
    Rig rig;
    for (int i = 0; i <= 12; ++i) rig.frame({{1000 + 10 * i, 1000, kFingerNodes}, {2400, 1800, 80}});
    rig.lift();
    for (auto &e : rig.events) CHECK(e.children.size() <= 1);
    CHECK(rig.r.downs == 1 && rig.r.cancels == 0 && rig.tr.palmPoints >= 12);
  }
  {  // a palm-sized contact alone: no report at all
    Rig rig;
    for (int i = 0; i < 10; ++i) rig.frame({{1600, 1068, 90}});
    rig.lift();
    CHECK(rig.reports.empty() && rig.r.downs == 0);
  }
  {  // a finger that grows into a palm while another touches: every touch cancelled, then nothing until every contact is up
    Rig rig;
    for (int i = 0; i < 6; ++i) rig.frame({{1000, 1000, kFingerNodes}, {1300, 1000, kFingerNodes}});
    CHECK(rig.r.downs == 2);
    rig.frame({{1000, 1000, kFingerNodes}, {1300, 1000, 120}});
    CHECK(rig.r.palms == 1 && rig.r.cancels == 1 && rig.count(NativeNoteKind::cancel) == 2);
    const size_t n = rig.reports.size();
    // the cancel: both children with the cancel mask, then both released
    const Collection &cancel = rig.events[rig.events.size() - 2], &release = rig.events.back();
    CHECK(cancel.children.size() == 2);
    for (auto &ch : cancel.children) CHECK((ch.mask & kMaskCancel) != 0);
    CHECK(release.children.size() == 2);
    for (auto &ch : release.children) CHECK(!ch.touch && (ch.mask & kMaskTouch));
    for (int i = 0; i < 4; ++i) rig.frame({{1050, 1000, kFingerNodes}, {1300, 1000, 120}});
    CHECK(rig.reports.size() == n && !nativeIdle(rig.r));   // nothing more while any contact stays
    rig.lift();
    CHECK(nativeIdle(rig.r));
    for (int i = 0; i < 4; ++i) rig.frame({{2800, 300, kFingerNodes}});   // a new touch away from the palm's zone works again
    rig.lift();
    CHECK(rig.r.downs == 3 && rig.m.overloads == 0);
  }
}

static void testLost()
{
  Rig rig;
  for (int i = 0; i < 6; ++i) rig.frame({{1000, 1000, kFingerNodes}, {1300, 1000, kFingerNodes}});
  NativeOut o;
  nativeCancel(rig.r, o);
  rig.take(o);
  CHECK(o.count == 2 && rig.r.cancels == 1 && nativeIdle(rig.r));
  nativeCancel(rig.r, o);   // nothing touching: nothing sent
  CHECK(o.count == 0);
  trackerCancel(rig.tr);
  rig.lift();
}

static void testRotation()
{
  {  // 90 degrees, desktop axes (default): the panel's (x, y) is the desktop's (h - 1 - y, x) over 2136 x 3200
    Rig rig;
    rig.degrees = 90;
    for (int i = 0; i < 4; ++i) rig.frame({{400, 1800, kFingerNodes}});
    const Child &ch = rig.events.front().children.front();
    CHECK(std::fabs(ch.x - (2135.0 - 1800) / 2135) < 0.003 && std::fabs(ch.y - 400.0 / 3199) < 0.003);
    rig.lift();
  }
  {  // 90 degrees, panel axes: the panel's own pixels
    Rig rig(true);
    rig.degrees = 90;
    for (int i = 0; i < 4; ++i) rig.frame({{400, 1800, kFingerNodes}});
    const Child &ch = rig.events.front().children.front();
    CHECK(std::fabs(ch.x - 400.0 / 3199) < 0.003 && std::fabs(ch.y - 1800.0 / 2135) < 0.003);
    rig.lift();
  }
}

static void testInjectedScript()
{
  // a two-finger spread as the tablet test plays it, through injectAt per 8 ms frame
  InjectScript sc;
  const char *text = "80:1400,1068;1800,1068 600:900,1068;2300,1068 80:";
  CHECK(injectParse(text, unsigned(std::strlen(text)), sc) == 0);
  Rig rig;
  for (uint32_t t = 0;; t += 8) {
    if (t > sc.totalMs) t = sc.totalMs;
    MatrixPoint p[kMaxPoints] = {};
    const unsigned n = injectAt(sc, t, 200, p);
    trackerUpdate(rig.tr, p, n, rig.now);
    NativeContact c[kMaxTracks];
    unsigned k = 0;
    for (const Track &tt : rig.tr.t)
      if (tt.state == TrackState::active) c[k++] = NativeContact{tt.id, tt.palm, tt.x, tt.y, tt.x, tt.y};
    NativeOut o;
    nativeUpdate(rig.r, c, k, kPanelW, kPanelH, kPanelW, kPanelH, o);
    rig.take(o);
    rig.now += 8;
    if (t >= sc.totalMs) break;
  }
  rig.lift(10);
  CHECK(rig.r.downs == 2 && rig.r.ups == 2 && rig.r.cancels == 0 && nativeIdle(rig.r));
  double spread0 = 0, spread1 = 0;
  for (auto &e : rig.events)
    if (e.children.size() == 2 && e.children[0].touch && e.children[1].touch) {
      const double s = std::fabs(e.children[0].x - e.children[1].x);
      if (spread0 == 0) spread0 = s;
      spread1 = s;
    }
  CHECK(spread0 > 0.1 && spread0 < 0.15 && spread1 > 0.42);   // 400 px -> 1400 px of 3200
}

static void testUuid()
{
  CHECK(screenUuidValid("01234567-89AB-CDEF-0123-456789abcdef"));
  CHECK(!screenUuidValid(nullptr) && !screenUuidValid("") && !screenUuidValid("<main>"));
  CHECK(!screenUuidValid("01234567-89AB-CDEF-0123-456789abcde"));     // 35 characters
  CHECK(!screenUuidValid("01234567-89AB-CDEF-0123-456789abcdef0"));   // 37
  CHECK(!screenUuidValid("0123456789AB-CDEF-0123-456789abcdef-"));    // dashes misplaced
  CHECK(!screenUuidValid("01234567-89AB-CDEF-0123-456789abcdeg"));    // not hex
}

int main()
{
  testUuid();
  testDescriptor();
  testDriverModel();
  testTap();
  testDrag();
  testTwoFingers();
  testNineFingers();
  testPalms();
  testLost();
  testRotation();
  testInjectedScript();
  if (failures) {
    std::printf("touch_native_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("touch_native_host: all passed (descriptor %u bytes, report %u bytes, %u slots%s)\n", kScreenDescriptor.length,
              kScreenReportBytes, kScreenSlots,
#ifdef HAVE_SDK_USAGES
              ", usages checked against the SDK"
#else
              ""
#endif
  );
  return 0;
}
