// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's test-only frame injection (piano-touch-inject=1; never in a daily profile): a script of keyframes, written by root to
// the sysctl machdep.piano_touch_inject, played by the frame thread as decoded panel frames, so the tracker (TouchTrack.h: the
// confirmation, IDs, palms), the gestures (TouchGesture.h) and the multitouch path (TouchMt.h) run exactly as for a finger, and a
// tablet test needs no finger.
// The script: segments separated by spaces, each "MS:" followed by the contacts at its end, separated by ';', each "x,y" or
// "x,y,area" in panel pixels (TouchMatrix.h kPanelW x kPanelH; area in sensor nodes, default kFingerNodes; 48 or more is a palm
// under the default piano-touch-palm). A segment with as many contacts as the one before moves them there in a straight line over MS
// (one frame per frame period); otherwise the new contacts are there at once and held MS. "MS:" alone is no contact for MS (a lift).
// Example, a two-finger spread about (1600, 1068): "60:1400,1068;1800,1068 400:1000,1068;2200,1068 60:".
// Integer only, no allocation: tests/touch_mt_host.cpp parses and plays scripts on the host.
#ifndef PIANO_TOUCH_INJECT_H
#define PIANO_TOUCH_INJECT_H
#include <stdint.h>
#include "TouchMatrix.h"

namespace pianotouch {

constexpr unsigned kInjectMaxSegments = 64, kInjectMaxContacts = 5, kInjectMaxText = 2048;
constexpr uint32_t kInjectMaxMs = 10000;       // one segment at most
constexpr uint16_t kFingerNodes = 9;           // a fingertip's blob (3 x 3 nodes)
constexpr int32_t kNodePx = 53;                // the sensor's node pitch in panel pixels (TouchGesture.h slopPx note)

struct InjectContact {
  int32_t x, y;
  uint16_t area;
};
struct InjectSegment {
  uint32_t ms;
  uint8_t n;
  InjectContact c[kInjectMaxContacts];
};
struct InjectScript {
  InjectSegment s[kInjectMaxSegments];
  unsigned count = 0;
  uint32_t totalMs = 0;
};

// Parses text (NUL-terminated or len bytes). Returns 0, or the 1-based character position of the first error.
inline unsigned injectParse(const char *text, unsigned len, InjectScript &out)
{
  out.count = 0;
  out.totalMs = 0;
  unsigned i = 0;
  auto at = [&](unsigned k) -> char { return k < len ? text[k] : 0; };
  auto number = [&](int32_t &v) -> bool {
    bool neg = false, any = false;
    int64_t n = 0;
    if (at(i) == '-') { neg = true; ++i; }
    while (at(i) >= '0' && at(i) <= '9') {
      n = n * 10 + (at(i) - '0');
      if (n > 100000) return false;
      ++i;
      any = true;
    }
    v = int32_t(neg ? -n : n);
    return any;
  };
  for (;;) {
    while (at(i) == ' ' || at(i) == '\n' || at(i) == '\t') ++i;
    if (!at(i)) break;
    if (out.count >= kInjectMaxSegments) return i + 1;
    InjectSegment &s = out.s[out.count];
    s.n = 0;
    int32_t ms;
    if (!number(ms) || ms < 0 || uint32_t(ms) > kInjectMaxMs || at(i) != ':') return i + 1;
    ++i;
    s.ms = uint32_t(ms);
    while (at(i) && at(i) != ' ' && at(i) != '\n' && at(i) != '\t') {
      if (s.n >= kInjectMaxContacts) return i + 1;
      int32_t x, y, a = kFingerNodes;
      if (!number(x) || at(i) != ',') return i + 1;
      ++i;
      if (!number(y)) return i + 1;
      if (at(i) == ',') {
        ++i;
        if (!number(a) || a < 1 || a > 2400) return i + 1;
      }
      if (x < 0 || x >= kPanelW || y < 0 || y >= kPanelH) return i + 1;
      s.c[s.n++] = InjectContact{x, y, uint16_t(a)};
      if (at(i) == ';') ++i;
      else if (at(i) && at(i) != ' ' && at(i) != '\n' && at(i) != '\t') return i + 1;
    }
    out.totalMs += s.ms;
    ++out.count;
  }
  return out.count ? 0 : 1;
}

// The contacts of the script at t ms from its start (0 .. totalMs), as decoder points: the count, and pts filled. A segment moving
// from the one before interpolates linearly; peaks are strong (threshold + 50: the tracker's confirmation applies, not fastPeak).
inline unsigned injectAt(const InjectScript &sc, uint32_t t, int32_t threshold, MatrixPoint *pts)
{
  uint32_t start = 0;
  for (unsigned k = 0; k < sc.count; ++k) {
    const InjectSegment &s = sc.s[k];
    if (t < start + s.ms || k + 1 == sc.count) {
      const InjectSegment *prev = k ? &sc.s[k - 1] : nullptr;
      const bool move = prev && prev->n == s.n && s.n && s.ms;
      const uint32_t into = t < start ? 0 : t - start > s.ms ? s.ms : t - start;
      for (unsigned j = 0; j < s.n; ++j) {
        int32_t x = s.c[j].x, y = s.c[j].y;
        if (move) {
          x = prev->c[j].x + int32_t((int64_t(s.c[j].x) - prev->c[j].x) * into / s.ms);
          y = prev->c[j].y + int32_t((int64_t(s.c[j].y) - prev->c[j].y) * into / s.ms);
        }
        const uint16_t area = s.c[j].area;
        // the blob's bounding box: a square of `area` nodes about the peak (palm zones use it)
        int32_t half = 0;
        while ((2 * half + 1) * (2 * half + 1) < area && half < 40) ++half;
        const int32_t r = y / kNodePx, c = x / kNodePx;
        MatrixPoint &p = pts[j];
        p.x = x;
        p.y = y;
        p.peak = threshold + 50;
        p.row = uint16_t(r);
        p.col = uint16_t(c);
        p.area = area;
        p.blobPeaks = 1;   // a palm here is one by its size (the tracker's palmNodes rule)
        p.strong = true;
        p.r0 = uint8_t(r - half < 0 ? 0 : r - half > 255 ? 255 : r - half);
        p.c0 = uint8_t(c - half < 0 ? 0 : c - half > 255 ? 255 : c - half);
        p.r1 = uint8_t(r + half > 255 ? 255 : r + half);
        p.c1 = uint8_t(c + half > 255 ? 255 : c + half);
      }
      return s.n;
    }
    start += s.ms;
  }
  return 0;
}

}  // namespace pianotouch
#endif
