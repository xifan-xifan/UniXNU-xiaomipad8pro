// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "../CameraOutput.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// An independent reference checks all active bytes, including different colors
// inside a 2x2 chroma block, unequal row pitches, padding and allocation canaries.
static void nv12(void)
{
  enum { w = 640, h = 480, ss = w * 4 + 28, yr = w + 64, cr = w + 128 };
  uint8_t *src = malloc(ss * h), *ys = malloc(yr * h + 32), *uv = malloc(cr * h / 2 + 32);
  assert(src && ys && uv);
  for (unsigned frame = 0; frame < 6; ++frame) {
    memset(ys, 0xa5, yr * h + 32); memset(uv, 0xa5, cr * h / 2 + 32);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      uint8_t *p = src + y * ss + x * 4;
      p[0] = (uint8_t)(x + frame * 17); p[1] = (uint8_t)(y + frame * 31);
      p[2] = (uint8_t)(x ^ y ^ frame); p[3] = 255;
    }
    assert(pianoOutputBGRAto420v(src, w, h, ss, ys, yr, uv, cr));
    for (unsigned y = 0; y < h; ++y) {
      for (unsigned x = 0; x < w; ++x) {
        const uint8_t *p = src + y * ss + x * 4;
        double l = .2126 * p[2] + .7152 * p[1] + .0722 * p[0];
        assert(abs(ys[y * yr + x] - (int)lround(16 + 219 * l / 255)) <= 1);
        if (!(x & 1) && !(y & 1)) {
          double sumU = 0, sumV = 0;
          for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
            const uint8_t *q = src + (y + dy) * ss + (x + dx) * 4;
            double ql = .2126 * q[2] + .7152 * q[1] + .0722 * q[0];
            sumU += 128 + (q[0] - ql) * 224 / (510 * (1 - .0722));
            sumV += 128 + (q[2] - ql) * 224 / (510 * (1 - .2126));
          }
          assert(abs(uv[(y / 2) * cr + x] - (int)lround(sumU / 4)) <= 1);
          assert(abs(uv[(y / 2) * cr + x + 1] - (int)lround(sumV / 4)) <= 1);
        }
      }
      for (size_t x = w; x < yr; ++x) assert(ys[y * yr + x] == 0xa5);
      if (!(y & 1)) for (size_t x = w; x < cr; ++x) assert(uv[(y / 2) * cr + x] == 0xa5);
    }
    for (unsigned x = 0; x < 32; ++x) assert(ys[yr * h + x] == 0xa5 && uv[cr * h / 2 + x] == 0xa5);
  }
  assert(!pianoOutputBGRAto420v(src, w - 1, h, ss, ys, yr, uv, cr));
  assert(!pianoOutputBGRAto420v(src, w, h - 1, ss, ys, yr, uv, cr));
  assert(!pianoOutputBGRAto420v(src, w, h, w * 4 - 1, ys, yr, uv, cr));
  assert(!pianoOutputBGRAto420v(src, w, h, ss, ys, w - 1, uv, cr));
  assert(!pianoOutputBGRAto420v(src, w, h, ss, ys, yr, uv, w - 1));
  assert(!pianoOutputBGRAto420v(src, w, h, SIZE_MAX, ys, yr, uv, cr));
  free(src); free(ys); free(uv);

  // An unaligned 2x2 block with odd pitches and no padding after the final row.
  const uint8_t block[] = {0xa5, 0, 0, 0, 1, 255, 255, 255, 2, 0xa5, 0, 0, 0, 3, 255, 255, 255, 4};
  uint8_t yCodes[] = {0xa5, 0, 0, 0xa5, 0, 0}, uvCodes[] = {0xa5, 0, 0};
  assert(pianoOutputBGRAto420v(block + 1, 2, 2, 9, yCodes + 1, 3, uvCodes + 1, 3));
  const uint8_t expectedY[] = {0xa5, 16, 235, 0xa5, 16, 235}, expectedUV[] = {0xa5, 128, 128};
  assert(memcmp(yCodes, expectedY, sizeof yCodes) == 0 && memcmp(uvCodes, expectedUV, sizeof uvCodes) == 0);
  puts("camera CPU 420v all-plane reference/padded-pitches/canaries PASS");
}

static void geometry(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  PianoOutputRect r = pianoOutputFit(sw, sh, dw, dh);
  assert(r.x == x && r.y == y && r.width == w && r.height == h);
  if (r.width > 1 && r.height > 1) {
    assert((uint64_t)r.width * sh <= (uint64_t)r.height * sw + sw / 2);
    assert((uint64_t)r.height * sw <= (uint64_t)r.width * sh + sh / 2);
  }
  const size_t ss = sw * 4 + 12, ds = dw * 4 + 20;
  uint8_t *src = malloc(ss * sh), *dst = malloc(ds * dh + 32);
  assert(src && dst);
  memset(src, 0xdd, ss * sh);
  memset(dst, 0xee, ds * dh + 32);
  for (uint32_t sy = 0; sy < sh; ++sy)
    for (uint32_t sx = 0; sx < sw; ++sx) {
      uint8_t *p = src + sy * ss + sx * 4;
      p[0] = (uint8_t)sx; p[1] = (uint8_t)sy; p[2] = 37; p[3] = 255;
    }
  assert(!pianoOutputScaleBGRA(src, sw, sh, ss, dst, dw, dh, (size_t)dw * 4 - 1));
  if (sh > 1) assert(!pianoOutputScaleBGRA(src, sw, sh, SIZE_MAX, dst, dw, dh, ds));
  assert(!pianoOutputScaleBGRA(NULL, sw, sh, ss, dst, dw, dh, ds));
  assert(pianoOutputScaleBGRA(src, sw, sh, ss, dst, dw, dh, ds));
  for (uint32_t dy = 0; dy < dh; ++dy) {
    for (uint32_t dx = 0; dx < dw; ++dx) {
      const uint8_t *p = dst + dy * ds + dx * 4;
      assert(p[3] == 255);
      if (dx < r.x || dx >= r.x + r.width || dy < r.y || dy >= r.y + r.height)
        assert(p[0] == 0 && p[1] == 0 && p[2] == 0);
      else {
        uint32_t sx = (uint32_t)(((uint64_t)(dx - r.x) * 2 + 1) * sw / ((uint64_t)w * 2));
        uint32_t sy = (uint32_t)(((uint64_t)(dy - r.y) * 2 + 1) * sh / ((uint64_t)h * 2));
        assert(p[0] == (uint8_t)sx && p[1] == (uint8_t)sy && p[2] == 37);
      }
    }
    for (size_t i = dw * 4; i < ds; ++i) assert(dst[dy * ds + i] == 0xee);
  }
  for (size_t i = 0; i < 32; ++i) assert(dst[ds * dh + i] == 0xee);
  free(src); free(dst);
}

static void cadence(void)
{
  assert(!pianoOutputDiscontinuity(0, 1, UINT64_MAX, UINT64_MAX));
  // A real source every two 30 fps ticks: one unique and one repeated output per source.
  uint64_t source = 0, sequence = 0, published = 0, repeats = 0, lastSequence = 0;
  for (uint64_t tick = 0; tick < 30; ++tick) {
    uint64_t now = UINT64_C(1000000000) + tick * UINT64_C(1000000000) / 30;
    if (!(tick % 2)) { source = now; ++sequence; }
    if (pianoOutputSourceFresh(now, source)) {
      ++published;
      if (lastSequence == sequence) ++repeats;
      lastSequence = sequence;
    }
  }
  assert(published == 30 && sequence == 15 && repeats == 15);
  // No source means no frame. A stalled source expires after 250 ms, including across clock errors.
  assert(!pianoOutputSourceFresh(1, 0));
  assert(!pianoOutputSourceFresh(999, 1000));
  assert(pianoOutputSourceFresh(source + PIANO_OUTPUT_MAX_SOURCE_AGE_NS, source));
  assert(!pianoOutputSourceFresh(source + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + 1, source));
  // The rear on the tablet (inst-v131w): a new converted frame every 66-143 ms (the CPU ISP pass). Counted from arrival, a 30 fps
  // output never sees a stale source; counted from the start of frame (about 140-250 ms earlier) it went stale before most arrivals.
  {
    static const uint64_t gaps_ms[] = {73, 66, 143, 80, 70, 120, 66, 90, 143, 73};
    uint64_t arrival = UINT64_C(2000000000), next = arrival, stale = 0, staleSof = 0;
    size_t k = 0;
    for (uint64_t tick = 0; tick < 300; ++tick) {
      const uint64_t now = UINT64_C(2000000000) + tick * UINT64_C(1000000000) / 30;
      while (next <= now) { arrival = next; next += gaps_ms[k++ % 10] * UINT64_C(1000000); }
      if (!pianoOutputSourceFresh(now, arrival)) ++stale;
      const uint64_t sof = arrival - UINT64_C(140000000) - (k % 3) * UINT64_C(50000000);
      if (!pianoOutputSourceFresh(now, sof)) ++staleSof;
    }
    assert(stale == 0);
    assert(staleSof > 30);
  }
}

static void discontinuity(void)
{
  const uint64_t i30 = UINT64_C(1000000000) / 30, i15 = UINT64_C(1000000000) / 15, t = UINT64_C(5000000000);
  // The first sample, a regular cadence and a skipped tick or two (a dropped source or a pool miss) keep the timeline.
  assert(!pianoOutputDiscontinuity(0, 0, t, i30));
  assert(!pianoOutputDiscontinuity(0, t, t + i30, i30) && !pianoOutputDiscontinuity(0, t, t + 3 * i30, i30));
  assert(!pianoOutputDiscontinuity(0, t, t + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + i30, i30));
  // A stall past the source age limit plus one interval, or any format/duration change, breaks it.
  assert(pianoOutputDiscontinuity(0, t, t + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + i30 + 1, i30));
  assert(pianoOutputDiscontinuity(0, t, t + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + i15 + 1, i15));
  assert(!pianoOutputDiscontinuity(0, t, t + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + i15, i15));
  assert(pianoOutputDiscontinuity(1, 0, t, i30) && pianoOutputDiscontinuity(1, t, t + i30, i30));
  // Clock errors never invent a stall.
  assert(!pianoOutputDiscontinuity(0, t, t, i30) && !pianoOutputDiscontinuity(0, t, t - 1, i30));
  assert(kPianoDeliveryBuffers >= 24);
}

int main(void)
{
  PianoOutputFormat native = pianoOutputFormat(816, 459, 0), compat = pianoOutputFormat(816, 459, 1);
  assert(native.width == 816 && native.height == 459 && native.minFps == 15 && native.maxFps == 15);
  assert(compat.width == 640 && compat.height == 480 && compat.minFps == 15 && compat.maxFps == 30);
  assert(pianoOutputDurationValid(0, 2, 30));
  assert(!pianoOutputDurationValid(0, 1, 30));
  assert(pianoOutputDurationValid(1, 1, 15) && pianoOutputDurationValid(1, 1, 24));
  assert(pianoOutputDurationValid(1, 1, 30) && pianoOutputDurationValid(1, 1001, 30000));
  assert(!pianoOutputDurationValid(1, 1, 31) && !pianoOutputDurationValid(1, 1, 14));
  assert(!pianoOutputDurationValid(1, INT64_MAX, INT32_MAX));
  assert(!pianoOutputDurationValid(1, -1, 30) && !pianoOutputDurationValid(1, 1, 0));
  assert(!pianoOutputDurationValid(2, 1, 15));
  geometry(816, 459, 640, 480, 0, 60, 640, 360);
  geometry(1020, 768, 640, 480, 1, 0, 638, 480);
  geometry(816, 459, 816, 459, 0, 0, 816, 459);
  geometry(1280, 720, 640, 480, 0, 60, 640, 360);
  geometry(1, 1000, 640, 480, 319, 0, 1, 480);
  geometry(5, 3, 7, 5, 0, 0, 7, 4);
  cadence();
  discontinuity();
  nv12();
  puts("camera output geometry/durations/freshness/cadence/discontinuity PASS");
  return 0;
}
