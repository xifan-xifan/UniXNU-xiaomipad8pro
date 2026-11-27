// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Output-only geometry and pacing. The hardware sensor remains at 15 fps.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { kPianoNativeFps = 15, kPianoCompatMaxFps = 30, kPianoCompatWidth = 640, kPianoCompatHeight = 480 };
#define PIANO_OUTPUT_MAX_SOURCE_AGE_NS UINT64_C(250000000)

// Buffers a client may hold from one delivery pool at once. The only avc1 encoder on this system (VideoToolbox's software H.264)
// returns its first frame after 14-15 inputs and keeps them until then; a capture graph queues a few more. With 4 the encoder
// starved: 4 frames sent, every later tick found the pool empty (QuickTime "High" could not record). Pools allocate on demand:
// at most 24 x 0.46 MB per stream for 640x480 420v, 24 x 1.5/3.1 MB only if a client holds that many native front/rear frames.
enum { kPianoDeliveryBuffers = 24 };

// A dropped-sample discontinuity tells the client its timeline breaks (QuickTime restarts its compressor and cuts the movie's edit
// list). Only a real format/duration change or a stall flags one: kernel source drops are covered by repeating the latest frame,
// and an output gap counts as a stall only past the source age limit plus one frame interval.
static inline int pianoOutputDiscontinuity(int formatChanged, uint64_t lastNs, uint64_t nowNs, uint64_t intervalNs)
{
  if (formatChanged) return 1;
  if (!lastNs || nowNs <= lastNs) return 0;
  const uint64_t gap = nowNs - lastNs;
  return gap > intervalNs && gap - intervalNs > PIANO_OUTPUT_MAX_SOURCE_AGE_NS;
}

typedef struct {
  uint32_t width, height, minFps, maxFps;
} PianoOutputFormat;

typedef struct {
  uint32_t x, y, width, height;
} PianoOutputRect;

static inline PianoOutputFormat pianoOutputFormat(uint32_t nativeWidth, uint32_t nativeHeight, unsigned index)
{
  PianoOutputFormat f = {nativeWidth, nativeHeight, kPianoNativeFps, kPianoNativeFps};
  if (index == 1) {
    f.width = kPianoCompatWidth;
    f.height = kPianoCompatHeight;
    f.maxFps = kPianoCompatMaxFps;
  }
  return f;
}

static inline int pianoOutputDurationValid(unsigned index, int64_t value, int32_t timescale)
{
  if (index > 1 || value <= 0 || timescale <= 0) return 0;
  const int maxFps = index ? kPianoCompatMaxFps : kPianoNativeFps;
  return (__int128)value * maxFps >= timescale && (__int128)value * kPianoNativeFps <= timescale;
}

static inline int pianoOutputSourceFresh(uint64_t now, uint64_t source)
{
  return source && now >= source && now - source <= PIANO_OUTPUT_MAX_SOURCE_AGE_NS;
}

// Fit the entire source, rounding at most half a pixel, then center black letterboxing.
static inline PianoOutputRect pianoOutputFit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
  PianoOutputRect r = {0, 0, 0, 0};
  if (!sw || !sh || !dw || !dh) return r;
  if ((uint64_t)dw * sh <= (uint64_t)dh * sw) {
    r.width = dw;
    r.height = (uint32_t)(((uint64_t)sh * dw + sw / 2) / sw);
    if (!r.height) r.height = 1;
  } else {
    r.height = dh;
    r.width = (uint32_t)(((uint64_t)sw * dh + sh / 2) / sh);
    if (!r.width) r.width = 1;
  }
  r.x = (dw - r.width) / 2;
  r.y = (dh - r.height) / 2;
  return r;
}

static inline int pianoOutputRowsValid(size_t stride, uint64_t rowBytes, uint32_t height)
{
  return rowBytes && rowBytes <= stride && height && stride <= SIZE_MAX / height;
}

// Source is immutable BGRA. Pixel-center nearest sampling avoids any temporary frame allocation.
static inline int pianoOutputScaleBGRA(const uint8_t *src, uint32_t sw, uint32_t sh, size_t ss,
                                       uint8_t *dst, uint32_t dw, uint32_t dh, size_t ds)
{
  if (!src || !dst || !pianoOutputRowsValid(ss, (uint64_t)sw * 4, sh) ||
      !pianoOutputRowsValid(ds, (uint64_t)dw * 4, dh)) return 0;
  const PianoOutputRect r = pianoOutputFit(sw, sh, dw, dh);
  if (sw == dw && sh == dh) {
    for (uint32_t y = 0; y < dh; ++y) memcpy(dst + y * ds, src + y * ss, (size_t)dw * 4);
    return 1;
  }
  for (uint32_t y = 0; y < dh; ++y) {
    uint8_t *row = dst + y * ds;
    for (size_t x = 0; x < dw; ++x) {
      row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
      row[x * 4 + 3] = 255;
    }
    if (y < r.y || y >= r.y + r.height) continue;
    const uint32_t sy = (uint32_t)(((uint64_t)(y - r.y) * sh + sh / 2) / r.height);
    for (uint32_t x = 0; x < r.width; ++x) {
      const uint32_t sx = (uint32_t)(((uint64_t)x * sw + sw / 2) / r.width);
      memcpy(row + (size_t)(r.x + x) * 4, src + sy * ss + (size_t)sx * 4, 4);
    }
  }
  return 1;
}

// BT.709 video-range NV12. Complete both CPU planes before publishing the buffer;
// the compatibility mode must not require a downstream GPU color conversion.
// Each UV pair averages the same 2x2 block as the four individually written Y codes.
static inline int pianoOutputBGRAto420v(const uint8_t *src, uint32_t w, uint32_t h, size_t ss,
                                       uint8_t *ys, size_t yStride, uint8_t *uv, size_t uvStride)
{
  if (!src || !ys || !uv || (w & 1) || (h & 1) || !pianoOutputRowsValid(ss, (uint64_t)w * 4, h) ||
      !pianoOutputRowsValid(yStride, w, h) || !pianoOutputRowsValid(uvStride, w, h / 2)) return 0;
  for (uint32_t y = 0; y < h; y += 2) {
    for (uint32_t x = 0; x < w; x += 2) {
      int r = 0, g = 0, b = 0;
      for (unsigned dy = 0; dy < 2; ++dy) {
        for (unsigned dx = 0; dx < 2; ++dx) {
          const uint8_t *p = src + (y + dy) * ss + (size_t)(x + dx) * 4;
          ys[(y + dy) * yStride + x + dx] =
              (uint8_t)(16 + (11966 * p[2] + 40254 * p[1] + 4064 * p[0] + 32768) / 65536);
          b += p[0]; g += p[1]; r += p[2];
        }
      }
      uint8_t *chroma = uv + (y / 2) * uvStride + x;
      chroma[0] = (uint8_t)((128 * 262144 - 6596 * r - 22188 * g + 28784 * b + 131072) / 262144);
      chroma[1] = (uint8_t)((128 * 262144 + 28784 * r - 26145 * g - 2639 * b + 131072) / 262144);
    }
  }
  return 1;
}
