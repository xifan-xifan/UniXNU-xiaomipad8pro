// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeGopDisplay's Night Shift entry points on 27.3 and their logging helpers (tests/gop_host.cpp).
//   - set_matrix(location, const uint64_t (*)[3][3]) (slot 309): QuartzCore's update_color_matrix calls
//     IOMobileFramebufferSetMatrix(handle, 9, matrix) (user client selector 0x37) with nine words of two's-complement S31.32, row-major
//     out = M x in (1.0 = 0x0000000100000000, -1.0 = 0xffffffff00000000);
//   - setBrightnessCorrection(value) (slot 314): IOMobileFramebufferSetBrightnessCorrection, an unsigned 16.16 scale (selector 0x32).
#pragma once
#include <stdint.h>

namespace gopcolor {

constexpr uint32_t kNightShiftLocation = 9;
constexpr bool nightShiftEnabled(uint32_t arg, bool external) { return arg == 1 && !external; }

// A two's-complement S31.32 word in thousandths, rounded to nearest (exact for any 64-bit value: the integer part separately).
inline int64_t s3132ToMilli(uint64_t raw)
{
  const int64_t v = static_cast<int64_t>(raw);
  const int64_t whole = v >> 32;                                  // floor
  const uint64_t frac = static_cast<uint64_t>(v) & 0xffffffffull;  // 0 .. 2^32-1, adds to the floor
  return whole * 1000 + static_cast<int64_t>((frac * 1000 + 0x80000000ull) >> 32);
}

// An unsigned 16.16 value in thousandths.
inline uint32_t u1616ToMilli(uint32_t v) { return static_cast<uint32_t>((static_cast<uint64_t>(v) * 1000 + 0x8000) >> 16); }

// FNV-1a over the location and the nine words: the same matrix again (it may come with every frame) is only counted.
inline uint32_t matrixHash(uint32_t location, const uint64_t m[9])
{
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t w) { h = (h ^ w) * 16777619u; };
  mix(location);
  for (unsigned k = 0; k < 9; ++k) {
    mix(static_cast<uint32_t>(m[k]));
    mix(static_cast<uint32_t>(m[k] >> 32));
  }
  return h;
}

// What a matrix is, for the log: the identity, a diagonal (Night Shift's warm white point is close to one), or general.
enum class Shape : uint8_t { zero, identity, diagonal, general };
inline Shape shape(const uint64_t m[9])
{
  bool diag = true, ident = true, zero = true;
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c) {
      const uint64_t v = m[3 * r + c];
      zero &= v == 0;
      if (r != c && v) diag = ident = false;
      if (r == c && v != 0x0000000100000000ull) ident = false;
    }
  return zero ? Shape::zero : ident ? Shape::identity : diag ? Shape::diagonal : Shape::general;
}
inline const char *shapeName(Shape s)
{
  switch (s) {
    case Shape::zero: return "zero";
    case Shape::identity: return "identity";
    case Shape::diagonal: return "diagonal";
    case Shape::general: return "general";
  }
  return "unknown";
}

// Whether a call is logged: the first 16 changes, then every 100th (the console stays readable on a Night Shift ramp).
inline bool logged(uint32_t changes) { return changes <= 16 || changes % 100 == 0; }

}  // namespace gopcolor
