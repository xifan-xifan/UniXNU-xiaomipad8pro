// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's multitouch path (piano-touch-mt=1, default off; docs/drivers/touch.md §16; private analysis host-checks/20261122-touch):
// two or more fingers go to macOS's own multitouch stack through MtHid.h's trackpad device, as a trackpad's do, instead of
// TouchGesture.h's Wheel and Command + Wheel. One finger stays TouchHid.h's absolute pointer (tap = click, hold = right click, drag).
// The touch screen is direct, macOS's multitouch stack indirect (it acts where the pointer is), so the two meet like this:
//   - two fingers counted (TouchTrack.h's confirmation; minFrames frames each, as TouchGesture.h's second finger) while the pointer
//     engine holds no button (idle, one undecided finger, or two): the pointer engine lets go (gestureAbandon), the pointer goes to the
//     fingers' middle (macOS zooms and rotates about the pointer and scrolls the window under it), and every finger goes to the
//     multitouch device from this frame on, at its place on the glass: the rotated desktop centred on a square surface of the panel's
//     long side at the panel's 74 um pixels, i.e. 1:1 in millimetres in either orientation (piano-touch-mt-scale changes the size the
//     surface declares, MtHid.h 0xD9), so a pinch, a rotation, a scroll or a swipe measures on the surface what it did on the glass,
//     and the screen's right edge is the surface's (a two-finger swipe in from it: Notification Center);
//   - a third, fourth or fifth finger joins (three- and four-finger swipes: Mission Control, App Expose, Spaces); a finger that lifts
//     while two or more stay is sent lifting once, then left out;
//   - fewer than two fingers left: the gesture ends (every finger lifting, gone, then an empty frame, as the device does), nothing
//     more from this touch until every finger is up (a remaining finger never becomes the pointer: it would jump); a two-finger touch
//     that lifted within twoTapMs without its middle or spread moving past slopPx is a right click at its middle (as before;
//     macOS's own two-finger tap needs tap to click);
//   - a followed finger that turns out a palm (TouchTrack.h, sticky), or a counted palm at the start: the gesture ends at once,
//     nothing until every finger is up; palms are never in a frame.
// Directions are the rotated desktop's: the caller maps the contacts through the display's rotation (TouchRotation.h) first.
// Integer only, no allocation, no I/O: tests/touch_mt_host.cpp runs it on synthetic contacts.
#ifndef PIANO_TOUCH_MT_H
#define PIANO_TOUCH_MT_H
#include <stdint.h>
#include "TouchGesture.h"
#include "MtHid.h"

namespace pianotouch {

constexpr int32_t kPanelPitchUm = 74;                                   // 3200 x 2136 on the 11.2" panel: 343.5 pixels per inch
constexpr int32_t kMtSideUm = (kPanelW > kPanelH ? kPanelW : kPanelH) * kPanelPitchUm;   // 236.8 mm
constexpr unsigned kMtMaxFrames = 3;

struct MtConfig {
  bool enabled = false;
  unsigned minFrames = 2;
  int32_t slopPx = 64;
  uint32_t twoTapMs = 700;
  bool twoTapRight = true;
  uint32_t scalePct = 100;
};
// The surface's declared size (MtHid.h 0xD9): the square, scaled (more than 100: the same travel counts as more millimetres).
inline MtSurface mtSurface(const MtConfig &c)
{
  const uint32_t side = uint32_t(int64_t(kMtSideUm) / 10 * c.scalePct / 100);   // 0.01 mm
  return MtSurface{side, side};
}

enum class MtMode : uint8_t { idle, multi, inert };
enum class MtNoteKind : uint8_t { start, join, leave, end, palm, twoTap };
struct MtNote {
  MtNoteKind kind;
  int32_t x, y;
  uint32_t ms;
  int32_t a, b;   // start, join: fingers; leave: fingers left; end: frames, most fingers; twoTap: travel, spread
};
struct MtFrameOut {
  MtFinger f[kMtMaxFingers];
  unsigned n;
  uint32_t dtMs;   // after the update's time (the lift sequence's later frames)
};
struct MtOut {
  bool claimed = false;     // the frame is the multitouch path's: the pointer engine is not fed
  bool abandon = false;     // the pointer engine lets go of its finger first (no output)
  bool warp = false;        // a pointer report first: no button, at (wx, wy) desktop pixels
  int32_t wx = 0, wy = 0;
  MtFrameOut frames[kMtMaxFrames];
  unsigned frameCount = 0;
  bool rightClick = false;  // a right click at (rx, ry) desktop pixels, after the frames
  int32_t rx = 0, ry = 0;
  MtNote notes[4];
  unsigned noteCount = 0;
};

struct MtRouter {
  MtConfig cfg;
  MtMode mode = MtMode::idle;
  unsigned count = 0, most = 0;          // fingers in the last frame; the most in this gesture
  uint8_t ids[kMtMaxFingers] = {};       // their tracker IDs
  MtFinger last[kMtMaxFingers] = {};     // what was sent for them
  int32_t ax = 0, ay = 0;                // the middle at the start (desktop pixels)
  uint64_t downMs = 0;
  uint8_t tapA = 0, tapB = 0;
  int32_t d0 = 0, maxTravel = 0, maxSpread = 0;
  bool tapPossible = false;
  uint32_t frames = 0;
  uint32_t gestures = 0, framesSent = 0, palms = 0, twoTaps = 0, joins = 0;
};

inline void mtReset(MtRouter &r, const MtConfig &cfg)
{
  const MtRouter keep = r;
  r = MtRouter();
  r.cfg = cfg;
  r.gestures = keep.gestures;
  r.framesSent = keep.framesSent;
  r.palms = keep.palms;
  r.twoTaps = keep.twoTaps;
  r.joins = keep.joins;
}
inline bool mtIdle(const MtRouter &r) { return r.mode == MtMode::idle; }

namespace detail {
inline void mtNote(MtOut &o, MtNoteKind k, int32_t x, int32_t y, uint64_t ms, int32_t a = 0, int32_t b = 0)
{
  if (o.noteCount < 4) o.notes[o.noteCount++] = MtNote{k, x, y, uint32_t(ms), a, b};
}
// A desktop point (w x h, the rotated desktop) on the surface: centred, 1:1 at the panel's pitch on the square's side.
inline void mtPlace(int32_t x, int32_t y, int32_t w, int32_t h, MtFinger &f)
{
  const int64_t dx = (int64_t(2 * x + 1 - w)) * kPanelPitchUm / 2, dy = (int64_t(2 * y + 1 - h)) * kPanelPitchUm / 2;   // um from the centre
  const int64_t sx = dx * (kMtMaxX - kMtMinX) / kMtSideUm, sy = -dy * (kMtMaxY - kMtMinY) / kMtSideUm;
  f.x = int32_t(sx < kMtMinX ? kMtMinX : sx > kMtMaxX ? kMtMaxX : sx);
  f.y = int32_t(sy < kMtMinY ? kMtMinY : sy > kMtMaxY ? kMtMaxY : sy);
}
constexpr uint8_t kMtTypes[5] = {kMtTypeIndex, kMtTypeMiddle, kMtTypeRing, kMtTypeLittle, kMtTypeThumb};
inline int mtFind(const MtRouter &r, uint8_t id)
{
  for (unsigned i = 0; i < r.count; ++i)
    if (r.ids[i] == id) return int(i);
  return -1;
}
inline MtFrameOut &mtNewFrame(MtOut &o, uint32_t dtMs)
{
  MtFrameOut &fr = o.frames[o.frameCount < kMtMaxFrames ? o.frameCount++ : kMtMaxFrames - 1];
  fr.n = 0;
  fr.dtMs = dtMs;
  return fr;
}
inline void mtLifted(MtFinger &f)
{
  f.state = kMtStateLift;
  f.major = f.minor = f.size = f.pressure = 0;
}
// The end of a gesture, as the device sends it: every finger lifting; 10 ms later gone (type undefined); 20 ms later no finger.
inline void mtEnd(MtRouter &r, MtOut &o, uint64_t now, MtMode next)
{
  MtFrameOut &a = mtNewFrame(o, 0), &b = mtNewFrame(o, 10), &c = mtNewFrame(o, 20);
  for (unsigned i = 0; i < r.count; ++i) {
    MtFinger f = r.last[i];
    mtLifted(f);
    a.f[a.n++] = f;
    f.state = kMtStateGone;
    f.type = kMtTypeUndefined;
    b.f[b.n++] = f;
  }
  c.n = 0;
  r.framesSent += 3;
  mtNote(o, MtNoteKind::end, r.ax, r.ay, now - r.downMs, int32_t(r.frames), int32_t(r.most));
  r.mode = next;
  r.count = 0;
}
}  // namespace detail

// One frame's active contacts (desktop pixels of the rotated desktop, w x h; palms flagged), the pointer engine as it is before
// this frame, the time (ms): what to send, in o.
inline void mtUpdate(MtRouter &r, const Gesture &g, const GestureContact *all, unsigned n, int32_t w, int32_t h, uint64_t now, MtOut &o)
{
  using namespace detail;
  o = MtOut();
  if (!r.cfg.enabled) return;
  const GestureContact *v[kMaxTracks];
  unsigned nv = 0, visible = 0;
  bool palmFollowed = false, palmCounted = false;
  for (unsigned i = 0; i < n; ++i) {
    const GestureContact &c = all[i];
    if (c.palm) {
      palmFollowed = palmFollowed || mtFind(r, c.id) >= 0;
      palmCounted = palmCounted || c.frames >= r.cfg.minFrames;
      continue;
    }
    ++visible;
    if (c.frames >= r.cfg.minFrames && nv < kMaxTracks) v[nv++] = &c;
  }
  auto tapCheck = [&]() {
    if (r.tapPossible && r.cfg.twoTapRight && r.most == 2 && now - r.downMs <= r.cfg.twoTapMs && r.maxTravel <= r.cfg.slopPx &&
        r.maxSpread <= r.cfg.slopPx) {
      o.rightClick = true;
      o.rx = r.ax;
      o.ry = r.ay;
      ++r.twoTaps;
      mtNote(o, MtNoteKind::twoTap, r.ax, r.ay, now - r.downMs, r.maxTravel, r.maxSpread);
    }
    r.tapPossible = false;
  };
  switch (r.mode) {
  case MtMode::idle: {
    const bool free = g.mode == GestureMode::idle || g.mode == GestureMode::one || g.mode == GestureMode::two;
    if (nv < 2 || !free || g.buttons) return;
    o.claimed = true;
    o.abandon = true;
    r.downMs = now;
    if (palmCounted) {   // a palm with the fingers: no gesture from this touch
      ++r.palms;
      r.mode = MtMode::inert;
      r.count = 0;
      mtNote(o, MtNoteKind::palm, v[0]->x, v[0]->y, 0);
      return;
    }
    const unsigned k = nv < kMtMaxFingers ? nv : kMtMaxFingers;
    int64_t sx = 0, sy = 0;
    for (unsigned i = 0; i < k; ++i) {
      sx += v[i]->x;
      sy += v[i]->y;
    }
    r.mode = MtMode::multi;
    r.count = 0;
    r.frames = 0;
    r.most = k;
    r.ax = int32_t(sx / k);
    r.ay = int32_t(sy / k);
    r.tapPossible = k == 2;
    r.maxTravel = r.maxSpread = 0;
    if (k == 2) {
      r.tapA = v[0]->id;
      r.tapB = v[1]->id;
      r.d0 = isqrt(dist2(v[0]->x, v[0]->y, v[1]->x, v[1]->y));
    }
    ++r.gestures;
    o.warp = true;
    o.wx = r.ax;
    o.wy = r.ay;
    mtNote(o, MtNoteKind::start, r.ax, r.ay, 0, int32_t(k));
    break;   // the first frame below
  }
  case MtMode::multi:
    o.claimed = true;
    if (palmFollowed) {
      ++r.palms;
      r.tapPossible = false;
      mtNote(o, MtNoteKind::palm, r.ax, r.ay, now - r.downMs);
      mtEnd(r, o, now, visible ? MtMode::inert : MtMode::idle);
      return;
    }
    if (nv < 2) {
      mtEnd(r, o, now, visible ? MtMode::inert : MtMode::idle);
      if (!visible) tapCheck();
      return;
    }
    break;
  case MtMode::inert:
    o.claimed = true;
    if (!visible) {
      tapCheck();
      r.mode = MtMode::idle;
    }
    return;
  }
  // The frame: fingers that left since the last one (lifting, once), then the counted ones (start, or touching).
  MtFrameOut &fr = mtNewFrame(o, 0);
  uint8_t ids[kMtMaxFingers];
  MtFinger sent[kMtMaxFingers];
  unsigned k = 0;
  bool joined = false;
  unsigned left = 0;
  for (unsigned i = 0; i < r.count; ++i) {
    bool still = false;
    for (unsigned j = 0; j < nv; ++j) still = still || v[j]->id == r.ids[i];
    if (!still && fr.n < kMtMaxFingers) {
      MtFinger f = r.last[i];
      mtLifted(f);
      fr.f[fr.n++] = f;
      ++left;
    }
  }
  for (unsigned j = 0; j < nv && k < kMtMaxFingers; ++j) {
    const int was = mtFind(r, v[j]->id);
    MtFinger f = {};
    if (was >= 0) {
      f = r.last[was];
      f.state = kMtStateTouching;
    } else {
      // the lowest finger number and the first finger type not in use (this frame, or a finger still in it)
      for (uint8_t s = 1; s < 16; ++s) {
        bool used = false;
        for (unsigned i = 0; i < r.count; ++i) used = used || r.last[i].id == s;
        for (unsigned i = 0; i < k; ++i) used = used || sent[i].id == s;
        if (!used) { f.id = s; break; }
      }
      f.type = kMtTypeUndefined;
      for (uint8_t t : kMtTypes) {
        bool used = false;
        for (unsigned i = 0; i < r.count; ++i) used = used || r.last[i].type == t;
        for (unsigned i = 0; i < k; ++i) used = used || sent[i].type == t;
        if (!used) { f.type = t; break; }
      }
      f.state = kMtStateStart;
      f.major = kMtMajor;
      f.minor = kMtMinor;
      f.size = kMtSize;
      f.pressure = kMtPressure;
      joined = joined || r.frames;
    }
    mtPlace(v[j]->x, v[j]->y, w, h, f);
    ids[k] = v[j]->id;
    sent[k++] = f;
    if (fr.n < kMtMaxFingers) fr.f[fr.n++] = f;
  }
  if (joined) {
    ++r.joins;
    mtNote(o, MtNoteKind::join, r.ax, r.ay, now - r.downMs, int32_t(k));
  }
  if (left) mtNote(o, MtNoteKind::leave, r.ax, r.ay, now - r.downMs, int32_t(k));
  r.count = k;
  for (unsigned i = 0; i < k; ++i) {
    r.ids[i] = ids[i];
    r.last[i] = sent[i];
  }
  if (k > r.most) r.most = k;
  if (r.tapPossible) {
    if (r.most != 2) {
      r.tapPossible = false;
    } else {
      int ia = -1, ib = -1;
      for (unsigned i = 0; i < nv; ++i) {
        if (v[i]->id == r.tapA) ia = int(i);
        if (v[i]->id == r.tapB) ib = int(i);
      }
      if (ia >= 0 && ib >= 0) {
        const int32_t cx = int32_t((int64_t(v[ia]->x) + v[ib]->x) / 2), cy = int32_t((int64_t(v[ia]->y) + v[ib]->y) / 2);
        const int32_t d = isqrt(dist2(v[ia]->x, v[ia]->y, v[ib]->x, v[ib]->y));
        const int32_t travel = isqrt(dist2(cx, cy, r.ax, r.ay)), spread = d > r.d0 ? d - r.d0 : r.d0 - d;
        if (travel > r.maxTravel) r.maxTravel = travel;
        if (spread > r.maxSpread) r.maxSpread = spread;
      }
    }
  }
  ++r.frames;
  ++r.framesSent;
}

// Frames lost (a read error, the watchdog, a stall): an ongoing gesture ends as if every finger lifted.
inline void mtCancel(MtRouter &r, uint64_t now, MtOut &o)
{
  o = MtOut();
  if (r.mode == MtMode::multi) {
    o.claimed = true;
    detail::mtEnd(r, o, now, MtMode::idle);
  }
  r.mode = MtMode::idle;
  r.tapPossible = false;
}

}  // namespace pianotouch
#endif
