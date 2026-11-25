// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoTouch's multitouch path (piano-touch-mt=1) and the test-only injection, no hardware:
//   - MtHid.h: the descriptor parsed (three application collections, report 0x02 with the buttons and X/Y, 0x3F of 16 bytes, 0x44 of
//     1387 bytes, primary usage Generic Desktop / Mouse); every feature answer's bytes and length, the size answer of report 0x01, the
//     0xDB bundle's sub-reports each after its length, the 0xD9 surface; the frame header and the bit packing round trip at the
//     extremes (signed 13-bit X and Y, type, state, finger number, angle) against an independent decoder written from the field
//     list (Linux hid-magicmouse's reading of the same bytes: X = bytes 0-1, Y negated from bytes 1-3, state = byte 3 & 0xC0 0x80);
//   - TouchMt.h: a two-finger pinch, a rotation and a three-finger swipe through the tracker (TouchTrack.h) and the router: the
//     pointer warps to the middle, start / touching / lift / gone states in order, the documented end (lift, +10 ms gone, +20 ms
//     empty), the pointer engine abandoned without a click; 1:1 millimetres (a 100 px spread = 7.4 mm = the same on the surface);
//     the right edge; a finger joining and one leaving; palms (a palm at the start, a finger turning palm mid-gesture); the two-finger
//     tap's right click; one finger untouched by the path (tap and drag as before); disabled = no claim; the rotation's axes;
//   - TouchInject.h: scripts parsed (errors at the right character), interpolation, palm areas, the bounding box.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. touch_mt_host.cpp -o /tmp/touch_mt_host && /tmp/touch_mt_host
#include "TouchMt.h"
#include "TouchInject.h"
#include "TouchTrack.h"
#include "../PianoAuto/TouchRotation.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pianotouch;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------- a small HID descriptor parser
struct ParsedReport {
  unsigned id = 0, bits = 0;
};
struct Parsed {
  std::vector<ParsedReport> input;
  std::vector<std::pair<unsigned, unsigned>> apps;   // usage page, usage of each application collection
  bool ok = true;
};
static Parsed parse(const uint8_t *d, unsigned n)
{
  Parsed p;
  unsigned page = 0, size = 0, count = 0, id = 0, depth = 0;
  std::vector<unsigned> usages;
  auto bitsFor = [&](unsigned rid) -> ParsedReport & {
    for (auto &r : p.input) if (r.id == rid) return r;
    p.input.push_back(ParsedReport{rid, 0});
    return p.input.back();
  };
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
    case 0x08: usages.push_back(v); break;
    case 0x74: size = v; break;
    case 0x94: count = v; break;
    case 0x84: id = v; break;
    case 0xa0:
      if (v == 1) p.apps.push_back({page, usages.empty() ? 0 : usages.back()});
      ++depth;
      usages.clear();
      break;
    case 0xc0: if (!depth) p.ok = false; --depth; break;
    case 0x80: bitsFor(id).bits += size * count; usages.clear(); break;
    default: break;
    }
  }
  if (depth) p.ok = false;
  return p;
}

static void testDescriptor()
{
  const Parsed p = parse(kMtDescriptor.bytes, kMtDescriptor.length);
  CHECK(p.ok);
  CHECK(kMtDescriptor.length <= sizeof kMtDescriptor.bytes);
  CHECK(p.apps.size() == 3);
  if (p.apps.size() == 3) {
    CHECK(p.apps[0].first == 0x01 && p.apps[0].second == 0x02);       // the primary usage: Generic Desktop / Mouse
    CHECK(p.apps[1].first == 0x0d && p.apps[1].second == 0x05);       // Digitizer / Touch Pad
    CHECK(p.apps[2].first == 0xff00 && p.apps[2].second == 0x0c);
  }
  unsigned b2 = 0, b3f = 0, b44 = 0;
  for (auto &r : p.input) {
    if (r.id == 0x02) b2 = r.bits;
    if (r.id == 0x3f) b3f = r.bits;
    if (r.id == 0x44) b44 = r.bits;
  }
  CHECK(b2 == 3 + 5 + 16 + 32);      // buttons, padding, X, Y, four constant bytes
  CHECK(b3f == 16 * 8);
  CHECK(b44 == 1387 * 8);
  CHECK(b44 / 8 + 1 >= kMtMaxReport); // MaxInputReportSize covers every frame
}

static void testFeatures()
{
  const MtSurface s{23680, 23680};
  uint8_t b[128];
  struct Want { uint8_t id; std::vector<uint8_t> bytes; };
  const Want wants[] = {
    {0x00, {0x00, 0x01}}, {0x02, {0x02, 0x01}}, {0xd1, {0xd1, 0x81}}, {0xc8, {0xc8, 0x08}}, {0x7f, {0x7f, 0, 0, 0, 0}},
    {0xd3, {0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00}},
    {0xd0, {0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00}},
    {0xa1, {0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01}},
    {0xd9, {0xd9, 0x80, 0x5c, 0x00, 0x00, 0x80, 0x5c, 0x00, 0x00, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26}},
  };
  for (const Want &w : wants) {
    const unsigned n = mtFeature(w.id, 0, s, b, sizeof b);
    CHECK(n == w.bytes.size());
    CHECK(n == mtFeatureBytes(w.id));
    CHECK(n && std::memcmp(b, w.bytes.data(), n) == 0);
    // the size answer of report 0x01 for it: 01 ID 00 LEN 00, LEN = the bytes after the ID
    const unsigned m = mtFeature(0x01, w.id, s, b, sizeof b);
    CHECK(m == 5 && b[0] == 0x01 && b[1] == w.id && b[2] == 0 && b[3] == w.bytes.size() - 1 && b[4] == 0);
  }
  CHECK(mtFeature(0x01, 0xdb, s, b, sizeof b) == 5 && b[3] == 0x49);   // the bundle's payload is 73 bytes
  CHECK(mtFeature(0x55, 0, s, b, sizeof b) == 0);                       // no such report
  // the bundle: DB 01, then each of D1 D3 D0 A1 D9 7F after its u16 LE length, equal to its own answer
  const unsigned n = mtFeature(0xdb, 0, s, b, sizeof b);
  CHECK(n == 74 && n == mtFeatureBytes(0xdb));
  CHECK(b[0] == 0xdb && b[1] == 0x01);
  unsigned at = 2;
  for (uint8_t id : {0xd1, 0xd3, 0xd0, 0xa1, 0xd9, 0x7f}) {
    const unsigned len = b[at] | b[at + 1] << 8;
    uint8_t one[64];
    const unsigned own = mtFeature(id, 0, s, one, sizeof one);
    CHECK(len == own);
    CHECK(at + 2 + len <= n && std::memcmp(b + at + 2, one, own) == 0);
    at += 2 + len;
  }
  CHECK(at == n);
  // a short buffer is never overrun
  uint8_t small[4] = {0xaa, 0xaa, 0xaa, 0xaa};
  CHECK(mtFeature(0xd9, 0, s, small, 3) == 17);
  CHECK(small[3] == 0xaa);
  // the surface: the panel's long side at 74 um, scaled
  MtConfig c;
  CHECK(mtSurface(c).width == 23680 && mtSurface(c).height == 23680);
  c.scalePct = 50;
  CHECK(mtSurface(c).width == 11840);
}

// An independent reading of a finger record, from the field list (as hid-magicmouse reads the same 9 bytes).
struct Read {
  int x, y, id, state2, typeBits, angle;
};
static Read readFinger(const uint8_t *t)
{
  Read r;
  r.x = int(int16_t(uint16_t((t[1] & 0x1f) << 8 | t[0]) << 3) >> 3);
  const int yraw = (t[3] & 0x03) << 11 | t[2] << 3 | t[1] >> 5;
  r.y = (yraw & 0x1000) ? yraw - 0x2000 : yraw;
  r.id = t[8] & 0x0f;
  r.state2 = t[3] & 0xc0;
  r.typeBits = (t[3] >> 2) & 7;
  r.angle = t[8] >> 5;
  return r;
}

static void testFrames()
{
  uint8_t out[kMtMaxReport];
  MtFinger f[3] = {
    {1, kMtMinX, kMtMaxY, kMtTypeIndex, kMtStateStart, kMtMajor, kMtMinor, kMtSize, kMtPressure},
    {2, kMtMaxX, kMtMinY, kMtTypeMiddle, kMtStateTouching, 30, 25, 12, 7},
    {15, -1, -1, kMtTypeThumb, kMtStateLift, 0, 0, 0, 0},
  };
  const unsigned n = mtFrame(f, 3, 0, 1000, out);
  CHECK(n == 12 + 27);
  CHECK(out[0] == 0x02 && out[1] == 0 && out[7] == 0x03 && out[8] == 0x31);
  for (unsigned i = 2; i <= 6; ++i) CHECK(out[i] == 0);
  const uint32_t t = uint32_t(out[9]) | uint32_t(out[10]) << 8 | uint32_t(out[11]) << 16;
  CHECK(t == ((1000u << 3) | 4));
  CHECK((t >> 3) == 1000 && (t & 7) == 4);
  for (unsigned i = 0; i < 3; ++i) {
    MtFinger back;
    mtUnpack(out + 12 + 9 * i, back);
    CHECK(back.x == f[i].x && back.y == f[i].y && back.id == f[i].id && back.type == f[i].type && back.state == f[i].state);
    CHECK(back.major == f[i].major && back.minor == f[i].minor && back.size == f[i].size && back.pressure == f[i].pressure);
    const Read r = readFinger(out + 12 + 9 * i);
    CHECK(r.x == f[i].x && r.y == f[i].y && r.id == f[i].id && r.angle == 4);
    CHECK(r.typeBits == f[i].type);
  }
  // hid-magicmouse's "down": byte 3 & 0xC0 == 0x80 exactly for the touching state
  CHECK(readFinger(out + 12 + 9).state2 == 0x80);
  CHECK(readFinger(out + 12).state2 != 0x80);
  // no finger active: TouchActive 2; the button alone: 3 and bit 0
  MtFinger gone = f[2];
  gone.state = kMtStateGone;
  CHECK(mtFrame(&gone, 1, 0, 5, out) == 21 && out[7] == 0x02);
  CHECK(mtFrame(nullptr, 0, 1, 5, out) == 12 && out[7] == 0x03 && out[1] == 1);
  // clamped coordinates, the timestamp's 21 bits wrap
  MtFinger far = f[0];
  far.x = 99999;
  far.y = -99999;
  mtFrame(&far, 1, 0, (1u << 21) + 7, out);
  MtFinger back;
  mtUnpack(out + 12, back);
  CHECK(back.x == kMtMaxX && back.y == kMtMinY);
  CHECK(((uint32_t(out[9]) | uint32_t(out[10]) << 8 | uint32_t(out[11]) << 16) >> 3) == 7);
}

// ----------------------------------------------------------------------------------------------------- the router through the tracker
struct Rig {
  Tracker tr;
  Gesture g;
  MtRouter r;
  uint32_t degrees = 0;
  uint64_t now = 1000;
  std::vector<MtFrameOut> frames;
  std::vector<PointerReport> pointer;
  std::vector<MtNote> notes;
  unsigned claimed = 0, warps = 0, rights = 0;
  int32_t warpX = 0, warpY = 0;
  Rig(bool enabled = true)
  {
    trackerReset(tr, TrackConfig());
    GestureConfig gc;
    gestureReset(g, gc);
    MtConfig mc;
    mc.enabled = enabled;
    mtReset(r, mc);
  }
  // one frame of panel points (x, y, area)
  void frame(std::vector<InjectContact> pts)
  {
    MatrixPoint p[kMaxPoints] = {};
    unsigned n = 0;
    InjectScript sc;
    sc.count = 1;
    sc.s[0].ms = 0;
    sc.s[0].n = uint8_t(pts.size());
    for (unsigned i = 0; i < pts.size(); ++i) sc.s[0].c[i] = pts[i];
    n = injectAt(sc, 0, 200, p);
    trackerUpdate(tr, p, n, now);
    GestureContact c[kMaxTracks];
    unsigned k = 0;
    for (const Track &t : tr.t) {
      if (t.state != TrackState::active) continue;
      const PatPoint q = pat_rotate(degrees, t.x, t.y, kPanelW, kPanelH);
      c[k++] = GestureContact{t.id, t.palm, q.x, q.y, t.frames};
    }
    const PatPoint desk = pat_rotate(degrees, 0, 0, kPanelW, kPanelH);
    MtOut o;
    mtUpdate(r, g, c, k, desk.width, desk.height, now, o);
    if (o.abandon) gestureAbandon(g);
    if (o.warp) { ++warps; warpX = o.wx; warpY = o.wy; }
    for (unsigned i = 0; i < o.frameCount; ++i) frames.push_back(o.frames[i]);
    if (o.rightClick) ++rights;
    for (unsigned i = 0; i < o.noteCount; ++i) notes.push_back(o.notes[i]);
    if (o.claimed) ++claimed;
    else {
      GestureOut go;
      gestureUpdate(g, c, k, now, go);
      for (unsigned i = 0; i < go.n; ++i) pointer.push_back(go.r[i]);
    }
    now += 8;
  }
  void lift(unsigned n = 6) { for (unsigned i = 0; i < n; ++i) frame({}); }
  bool has(MtNoteKind k) const { for (auto &m : notes) if (m.kind == k) return true; return false; }
};

static double surfMm(int units, bool x) { return units * 236.8 / (x ? (kMtMaxX - kMtMinX) : (kMtMaxY - kMtMinY)); }

static void testPinch()
{
  Rig rig;
  // two fingers 200 px apart about the panel's centre, spreading to 800 px over 30 frames
  for (int i = 0; i <= 30; ++i) {
    const int half = 100 + i * 10;
    rig.frame({{1600 - half, 1068, kFingerNodes}, {1600 + half, 1068, kFingerNodes}});
  }
  rig.lift();
  CHECK(rig.warps == 1);
  CHECK(std::abs(rig.warpX - 1600) <= 12 && std::abs(rig.warpY - 1068) <= 2);
  CHECK(rig.has(MtNoteKind::start) && rig.has(MtNoteKind::end));
  CHECK(rig.r.gestures == 1 && rig.g.taps == 0 && rig.g.rights == 0 && rig.rights == 0);
  for (auto &p : rig.pointer) CHECK(p.buttons == 0 && p.wheel == 0);   // the pointer engine: at most a move, never a click or wheel
  CHECK(rig.frames.size() >= 20);
  // first frame: both fingers starting; then touching; the end: lift, gone (+10 ms), empty (+20 ms)
  const MtFrameOut &first = rig.frames.front();
  CHECK(first.n == 2 && first.f[0].state == kMtStateStart && first.f[1].state == kMtStateStart);
  CHECK(first.f[0].id != first.f[1].id && first.f[0].type != first.f[1].type);
  const size_t e = rig.frames.size();
  CHECK(e >= 3);
  const MtFrameOut &a = rig.frames[e - 3], &b = rig.frames[e - 2], &c = rig.frames[e - 1];
  CHECK(a.n == 2 && a.f[0].state == kMtStateLift && a.f[0].pressure == 0 && a.dtMs == 0);
  CHECK(b.n == 2 && b.f[0].state == kMtStateGone && b.f[0].type == kMtTypeUndefined && b.dtMs == 10);
  CHECK(c.n == 0 && c.dtMs == 20);
  for (size_t i = 1; i + 3 < e; ++i)
    for (unsigned j = 0; j < rig.frames[i].n; ++j) CHECK(rig.frames[i].f[j].state == kMtStateTouching);
  // 1:1: the spread on the surface in mm = the spread on the glass at 74 um (to the jitter filter and rounding)
  const MtFrameOut &last = rig.frames[e - 4];
  const double glass = 0.074 * 800, surface = surfMm(last.f[1].x - last.f[0].x, true);
  CHECK(std::fabs(surface - glass) < 1.0);
  // the fingers stay on their numbers
  CHECK(last.f[0].id == first.f[0].id && last.f[1].id == first.f[1].id);
}

static void testRotateAndEdge()
{
  Rig rig;
  // two fingers 600 px apart turning 90 degrees about (2400, 1068)
  for (int i = 0; i <= 24; ++i) {
    const double a = i * (M_PI / 2) / 24;
    const int dx = int(300 * std::cos(a)), dy = int(300 * std::sin(a));
    rig.frame({{2400 - dx, 1068 - dy, kFingerNodes}, {2400 + dx, 1068 + dy, kFingerNodes}});
  }
  CHECK(rig.r.mode == MtMode::multi);
  const MtFrameOut &f = rig.frames.back();
  // the line between the fingers turned to vertical on the surface; screen down = surface y down (negative)
  CHECK(f.n == 2 && std::abs(f.f[1].x - f.f[0].x) < 60 && std::abs(f.f[1].y - f.f[0].y) > 900);   // 600 px = 44.4 mm = 976 units
  CHECK((f.f[1].y - f.f[0].y) < 0);
  rig.lift();
  // the right edge of the screen is the right edge of the surface (x near kMtMaxX for the last pixel column)
  MtFinger e;
  detail::mtPlace(kPanelW - 1, kPanelH / 2, kPanelW, kPanelH, e);
  CHECK(e.x >= kMtMaxX - 3 && std::abs(e.y) <= 2);
  detail::mtPlace(0, 0, kPanelW, kPanelH, e);
  CHECK(e.x <= kMtMinX + 3 && e.y > 0);
}

static void testSwipeJoinLeave()
{
  Rig rig;
  // two fingers, then a third joins, all move left 600 px, one lifts, then the rest
  for (int i = 0; i < 4; ++i) rig.frame({{1500, 900, kFingerNodes}, {1700, 900, kFingerNodes}});
  for (int i = 0; i < 4; ++i) rig.frame({{1500, 900, kFingerNodes}, {1700, 900, kFingerNodes}, {1900, 920, kFingerNodes}});
  for (int i = 0; i <= 20; ++i)
    rig.frame({{1500 - 30 * i, 900, kFingerNodes}, {1700 - 30 * i, 900, kFingerNodes}, {1900 - 30 * i, 920, kFingerNodes}});
  CHECK(rig.has(MtNoteKind::join));
  CHECK(rig.r.most == 3);
  unsigned three = 0;
  for (auto &f : rig.frames) three += f.n == 3;
  CHECK(three >= 15);
  // three distinct numbers and types
  const MtFrameOut &f3 = rig.frames.back();
  CHECK(f3.n == 3 && f3.f[0].id != f3.f[1].id && f3.f[1].id != f3.f[2].id && f3.f[0].id != f3.f[2].id);
  CHECK(f3.f[0].type != f3.f[1].type && f3.f[1].type != f3.f[2].type && f3.f[0].type != f3.f[2].type);
  // one leaves: sent lifting once, then two
  const size_t before = rig.frames.size();
  for (int i = 0; i < 6; ++i) rig.frame({{900, 900, kFingerNodes}, {1100, 900, kFingerNodes}});
  CHECK(rig.has(MtNoteKind::leave));
  bool sawLift = false;
  for (size_t i = before; i < rig.frames.size(); ++i)
    for (unsigned j = 0; j < rig.frames[i].n; ++j) sawLift = sawLift || rig.frames[i].f[j].state == kMtStateLift;
  CHECK(sawLift);
  CHECK(rig.frames.back().n == 2);
  rig.lift();
  CHECK(mtIdle(rig.r) && rig.rights == 0);
}

static void testPalms()
{
  {  // a palm resting while two fingers pinch: the palm never becomes a contact (TouchTrack.h), never in a frame
    Rig rig;
    for (int i = 0; i <= 12; ++i) rig.frame({{1000 - 10 * i, 1000, kFingerNodes}, {1300 + 10 * i, 1000, kFingerNodes}, {2400, 1800, 80}});
    rig.lift();
    CHECK(rig.r.gestures == 1 && rig.r.most == 2 && rig.tr.palmPoints >= 12);
    for (auto &f : rig.frames) CHECK(f.n <= 2);
    CHECK(rig.g.taps == 0 && rig.g.rights == 0);
  }
  {  // a contact that turned palm before the second finger came: no gesture from this touch
    Rig rig;
    for (int i = 0; i < 4; ++i) rig.frame({{2400, 1800, kFingerNodes}});
    for (int i = 0; i < 2; ++i) rig.frame({{2400, 1800, 90}});
    for (int i = 0; i < 6; ++i) rig.frame({{2400, 1800, 90}, {1000, 1000, kFingerNodes}, {1300, 1000, kFingerNodes}});
    rig.lift();
    CHECK(rig.r.gestures == 0 && rig.frames.empty() && rig.r.palms == 1);
  }
  {  // a finger that grows into a palm mid-gesture: the gesture ends at once, nothing until every finger is up
    Rig rig;
    for (int i = 0; i < 6; ++i) rig.frame({{1000, 1000, kFingerNodes}, {1300, 1000, kFingerNodes}});
    CHECK(rig.r.mode == MtMode::multi);
    rig.frame({{1000, 1000, kFingerNodes}, {1300, 1000, 120}});
    CHECK(rig.has(MtNoteKind::palm));
    CHECK(rig.r.mode == MtMode::inert);
    const size_t n = rig.frames.size();
    CHECK(rig.frames[n - 1].n == 0 && rig.frames[n - 3].f[0].state == kMtStateLift);
    for (int i = 0; i < 4; ++i) rig.frame({{1050, 1000, kFingerNodes}, {1300, 1000, 120}});
    CHECK(rig.frames.size() == n);   // nothing more
    rig.lift();
    CHECK(mtIdle(rig.r) && rig.g.taps == 0);
  }
}

static void testTwoTapAndOneFinger()
{
  {  // a two-finger tap: a right click at the middle, after the frames
    Rig rig;
    for (int i = 0; i < 5; ++i) rig.frame({{1000, 1000, kFingerNodes}, {1250, 1000, kFingerNodes}});
    rig.lift();
    CHECK(rig.rights == 1 && rig.has(MtNoteKind::twoTap));
    CHECK(rig.g.rights == 0);   // not the pointer engine's
  }
  {  // one finger: the path never claims it (tap, then drag)
    Rig rig;
    for (int i = 0; i < 4; ++i) rig.frame({{800, 600, kFingerNodes}});
    rig.lift();
    CHECK(rig.g.taps == 1 && rig.claimed == 0 && rig.frames.empty());
    for (int i = 0; i < 40; ++i) rig.frame({{800 + 10 * i, 600, kFingerNodes}});
    rig.lift();
    CHECK(rig.g.drags == 1 && rig.claimed == 0 && rig.frames.empty());
  }
  {  // a drag holds its button: a second finger does not take it over
    Rig rig;
    for (int i = 0; i < 30; ++i) rig.frame({{800 + 10 * i, 600, kFingerNodes}});
    CHECK(rig.g.mode == GestureMode::drag);
    for (int i = 0; i < 6; ++i) rig.frame({{1100 + 10 * i, 600, kFingerNodes}, {1500, 900, kFingerNodes}});
    CHECK(rig.r.gestures == 0 && rig.frames.empty());
    rig.lift();
  }
  {  // disabled: no claim, the old two-finger scroll
    Rig rig(false);
    for (int i = 0; i <= 20; ++i) rig.frame({{1000, 1000 - 10 * i, kFingerNodes}, {1300, 1000 - 10 * i, kFingerNodes}});
    rig.lift();
    CHECK(rig.claimed == 0 && rig.frames.empty() && rig.g.scrolls == 1);
  }
}

static void testRotation90()
{
  // the display at 90 degrees: a spread along the panel's Y axis is horizontal in the rotated desktop and on the surface
  Rig rig;
  rig.degrees = 90;
  for (int i = 0; i <= 10; ++i) rig.frame({{1600, 800 - 20 * i, kFingerNodes}, {1600, 1300 + 20 * i, kFingerNodes}});
  const MtFrameOut &f = rig.frames.back();
  CHECK(f.n == 2 && std::abs(f.f[0].y - f.f[1].y) < 30 && std::abs(f.f[0].x - f.f[1].x) > 1000);
  // the warp in the rotated desktop (2136 wide): x = h - 1 - y
  CHECK(std::abs(rig.warpX - (kPanelH - 1 - 1050)) <= 3 && std::abs(rig.warpY - 1600) <= 3);
  rig.lift();
}

static void testInjectParse()
{
  InjectScript s;
  const char *ok = "60:1400,1068;1800,1068 400:1000,1068;2200,1068 60:";
  CHECK(injectParse(ok, unsigned(std::strlen(ok)), s) == 0);
  CHECK(s.count == 3 && s.totalMs == 520 && s.s[0].n == 2 && s.s[1].n == 2 && s.s[2].n == 0);
  CHECK(s.s[1].c[1].x == 2200 && s.s[1].c[1].area == kFingerNodes);
  const char *palm = "100:2400,1800,90";
  CHECK(injectParse(palm, unsigned(std::strlen(palm)), s) == 0 && s.s[0].c[0].area == 90);
  struct Bad { const char *t; unsigned at; };
  const Bad bad[] = {{"", 1}, {"x:", 1}, {"60", 3}, {"60:1,", 6}, {"60:5000,10", 11}, {"60:10,10,0", 11},
                     {"60:1,1;2,2;3,3;4,4;5,5;6,6", 24}, {"20000:1,1", 6}, {"60:1,1x", 7}};
  for (const Bad &b : bad) {
    const unsigned at = injectParse(b.t, unsigned(std::strlen(b.t)), s);
    if (at != b.at) std::printf("  parse %-28s -> %u (want %u)\n", b.t, at, b.at);
    CHECK(at == b.at);
  }
  // interpolation: halfway through the second segment, the middle of the way
  CHECK(injectParse(ok, unsigned(std::strlen(ok)), s) == 0);
  MatrixPoint p[kInjectMaxContacts];
  CHECK(injectAt(s, 0, 200, p) == 2 && p[0].x == 1400);
  CHECK(injectAt(s, 60 + 200, 200, p) == 2 && p[0].x == 1200 && p[1].x == 2000);
  CHECK(injectAt(s, 470, 200, p) == 0);
  CHECK(injectAt(s, 9999, 200, p) == 0);
  CHECK(p[0].peak == 250 && p[0].strong && p[0].area == kFingerNodes);
  // a palm's points are a palm to the tracker (palmNodes 48 by default)
  CHECK(injectParse(palm, unsigned(std::strlen(palm)), s) == 0);
  CHECK(injectAt(s, 0, 200, p) == 1 && p[0].area == 90 && p[0].r1 > p[0].r0 && p[0].c1 > p[0].c0);
  Tracker tr;
  trackerReset(tr, TrackConfig());
  for (int i = 0; i < 4; ++i) trackerUpdate(tr, p, 1, 1000 + 8 * i);
  CHECK(tr.palmPoints >= 4 && trackerActive(tr, true) == 0);
}

int main()
{
  testDescriptor();
  testFeatures();
  testFrames();
  testPinch();
  testRotateAndEdge();
  testSwipeJoinLeave();
  testPalms();
  testTwoTapAndOneFinger();
  testRotation90();
  testInjectParse();
  if (failures) {
    std::printf("touch_mt_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("touch_mt_host: all passed (descriptor %u bytes, frames up to %u bytes)\n", kMtDescriptor.length, kMtMaxReport);
  return 0;
}
