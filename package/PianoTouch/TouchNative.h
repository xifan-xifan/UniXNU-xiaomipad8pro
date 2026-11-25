// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's native touch path (piano-touch-native=1|2; private host-checks 20261122-touch NATIVE-TOUCH.md): every followed finger
// (TouchTrack.h; palms never) goes to PianoTouchScreen (TouchScreenHid.h) as a touch of its own, and macOS makes of it what a
// touch-screen Mac makes of a finger (direct touches; for views that take none, AppKit's click, scroll, magnify, rotation and long-press
// right click). Nothing of TouchGesture.h's pointer gestures runs meanwhile.
// - A finger keeps one of the eight slots from its first frame to its lift; the slot's Contact Identifier is slot + 1 (IOHIDEventDriver's
//   transducer index, BackBoard's path index, which it keeps in a 30-bit mask). A ninth finger is not sent (counted).
// - Every frame with a finger down is one report (all slots); a lift is one report with the slot's tip and range cleared (its last
//   position), after which the slot is empty (contact 0, no bits) and free from the next frame on. Nothing is sent at rest.
// - A palm never takes a slot; a followed finger that turns palm (TouchTrack.h makes palms sticky) cancels every touch: one report with
//   tip and Untouch on each touching slot (IOHIDEventDriver's cancel), then one releasing them; then nothing until every contact is up.
// - nativeCancel (frames lost, the mode switched, the display off): the same cancel and release for whatever is touching.
// - Positions: the contacts as gestures() gives them (the rotated desktop, pixels) scaled over the desktop's size, or (axes = panel)
//   the panel's own pixels over kPanelW x kPanelH: a runtime choice until the run shows which frame BackBoard maps (NATIVE-TOUCH.md
//   gate 17).
// Integer only, no allocation, header-only: tests/touch_native_host.cpp runs it against a model of IOHIDEventDriver's rules.
#ifndef PIANO_TOUCH_NATIVE_H
#define PIANO_TOUCH_NATIVE_H
#include <stdint.h>
#include "TouchScreenHid.h"

namespace pianotouch {

// One contact as the router gets it: TouchTrack.h's ID, palm flag and position (desktop pixels), and the panel position for axes = panel.
struct NativeContact {
  uint8_t id;
  bool palm;
  int32_t x, y;        // the rotated desktop
  int32_t px, py;      // the panel
};

enum class NativeSlotState : uint8_t { free, touching, lifting };
struct NativeSlotRec {
  NativeSlotState state = NativeSlotState::free;
  uint8_t track = 0;   // TouchTrack.h's ID of the finger in it
  uint16_t x = 0, y = 0;
};

enum class NativeNoteKind : uint8_t { down, up, cancel, palm, full };
struct NativeNote {
  NativeNoteKind kind;
  uint8_t slot, track;
  uint16_t x, y;
};

struct NativeRouter {
  NativeSlotRec slot[kScreenSlots];
  bool blocked = false;        // after a cancel: nothing until every contact is up
  bool panelAxes = false;
  uint32_t reports = 0, downs = 0, ups = 0, cancels = 0, palms = 0, full = 0, maxFingers = 0;
};

constexpr unsigned kNativeMaxReports = 2, kNativeMaxNotes = 3 * kScreenSlots;
struct NativeOut {
  unsigned count = 0;          // reports to send, in order
  uint8_t report[kNativeMaxReports][kScreenReportBytes];
  unsigned noteCount = 0;
  NativeNote notes[kNativeMaxNotes];
};

inline void nativeReset(NativeRouter &r, bool panelAxes)
{
  r = NativeRouter{};
  r.panelAxes = panelAxes;
}

// No finger in a slot and no cancel waiting for every contact to go: the mode may be switched, the rotation latched. (A slot whose
// lift was sent is idle: it only stays out of use for the rest of that frame.)
inline bool nativeIdle(const NativeRouter &r)
{
  if (r.blocked) return false;
  for (const NativeSlotRec &s : r.slot)
    if (s.state == NativeSlotState::touching) return false;
  return true;
}

namespace detail {
inline void note(NativeOut &o, NativeNoteKind k, unsigned slot, const NativeSlotRec &s)
{
  if (o.noteCount < kNativeMaxNotes) o.notes[o.noteCount++] = NativeNote{k, uint8_t(slot), s.track, s.x, s.y};
}
inline void emit(NativeRouter &r, NativeOut &o, const ScreenSlot slots[kScreenSlots])
{
  if (o.count >= kNativeMaxReports) return;
  screenReport(slots, o.report[o.count++]);
  ++r.reports;
}
// The two reports of a cancel: tip + Untouch on every touching slot, then all of them released; every slot free afterwards.
inline void cancelAll(NativeRouter &r, NativeOut &o)
{
  ScreenSlot a[kScreenSlots] = {}, b[kScreenSlots] = {};
  bool any = false;
  for (unsigned s = 0; s < kScreenSlots; ++s) {
    NativeSlotRec &k = r.slot[s];
    if (k.state != NativeSlotState::touching) continue;
    any = true;
    a[s] = ScreenSlot{uint8_t(kSlotTip | kSlotInRange | kSlotValid | kSlotUntouch), uint8_t(s + 1), k.x, k.y};
    b[s] = ScreenSlot{uint8_t(kSlotValid), uint8_t(s + 1), k.x, k.y};
    note(o, NativeNoteKind::cancel, s, k);
  }
  for (NativeSlotRec &k : r.slot) k = NativeSlotRec{};
  if (!any) return;
  ++r.cancels;
  emit(r, o, a);
  emit(r, o, b);
}
}  // namespace detail

inline void nativeCancel(NativeRouter &r, NativeOut &o)
{
  o = NativeOut{};
  detail::cancelAll(r, o);
  r.blocked = false;
}

// One frame: c[0..n) the active contacts (TouchTrack.h active tracks, palms included and flagged), w x h the rotated desktop.
inline void nativeUpdate(NativeRouter &r, const NativeContact *c, unsigned n, int32_t w, int32_t h, int32_t panelW, int32_t panelH,
                         NativeOut &o)
{
  o = NativeOut{};
  // Slots a lift emptied last frame are free from this one on.
  for (NativeSlotRec &k : r.slot)
    if (k.state == NativeSlotState::lifting) k = NativeSlotRec{};
  if (r.blocked) {
    if (n == 0) r.blocked = false;
    return;
  }
  // A followed finger that turned palm: every touch is cancelled, then nothing until every contact is up.
  for (unsigned i = 0; i < n; ++i) {
    if (!c[i].palm) continue;
    for (const NativeSlotRec &k : r.slot)
      if (k.state == NativeSlotState::touching && k.track == c[i].id) {
        ++r.palms;
        NativeSlotRec tmp = k;
        detail::note(o, NativeNoteKind::palm, unsigned(&k - r.slot), tmp);
        detail::cancelAll(r, o);
        r.blocked = n > 0;
        return;
      }
  }
  auto pos = [&](const NativeContact &k, uint16_t &x, uint16_t &y) {
    if (r.panelAxes) { x = screenScale(k.px, panelW); y = screenScale(k.py, panelH); }
    else { x = screenScale(k.x, w); y = screenScale(k.y, h); }
  };
  ScreenSlot out[kScreenSlots] = {};
  bool send = false;
  // The fingers in slots: moved, or lifted (not in this frame any more).
  for (unsigned s = 0; s < kScreenSlots; ++s) {
    NativeSlotRec &k = r.slot[s];
    if (k.state != NativeSlotState::touching) continue;
    const NativeContact *found = nullptr;
    for (unsigned i = 0; i < n && !found; ++i)
      if (!c[i].palm && c[i].id == k.track) found = &c[i];
    send = true;
    if (found) {
      pos(*found, k.x, k.y);
      out[s] = ScreenSlot{uint8_t(kSlotTip | kSlotInRange | kSlotValid), uint8_t(s + 1), k.x, k.y};
    } else {
      out[s] = ScreenSlot{uint8_t(kSlotValid), uint8_t(s + 1), k.x, k.y};
      k.state = NativeSlotState::lifting;
      ++r.ups;
      detail::note(o, NativeNoteKind::up, s, k);
    }
  }
  // New fingers: the lowest free slot (one a lift is releasing in this report is not free yet).
  for (unsigned i = 0; i < n; ++i) {
    if (c[i].palm) continue;
    bool known = false;
    for (const NativeSlotRec &k : r.slot) known = known || (k.state != NativeSlotState::free && k.track == c[i].id);
    if (known) continue;
    unsigned s = 0;
    while (s < kScreenSlots && r.slot[s].state != NativeSlotState::free) ++s;
    if (s == kScreenSlots) {
      ++r.full;
      if (o.noteCount < kNativeMaxNotes) o.notes[o.noteCount++] = NativeNote{NativeNoteKind::full, 0xff, c[i].id, 0, 0};
      continue;
    }
    NativeSlotRec &k = r.slot[s];
    k.state = NativeSlotState::touching;
    k.track = c[i].id;
    pos(c[i], k.x, k.y);
    out[s] = ScreenSlot{uint8_t(kSlotTip | kSlotInRange | kSlotValid), uint8_t(s + 1), k.x, k.y};
    send = true;
    ++r.downs;
    detail::note(o, NativeNoteKind::down, s, k);
  }
  unsigned touching = 0;
  for (const NativeSlotRec &k : r.slot) touching += k.state == NativeSlotState::touching;
  if (touching > r.maxFingers) r.maxFingers = touching;
  if (send) detail::emit(r, o, out);
}

}  // namespace pianotouch
#endif
