// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded AVC/NV12 and HEVC/NV12-or-P010 geometry. Arithmetic is independently expressed from the
// VPU 3.5 size and alignment facts documented at the pin in UPSTREAM.
#pragma once
#include <stdint.h>

namespace pianovideo { namespace session {
struct AvcGeometry {
  static constexpr unsigned MaxN = 16, ScratchCount = 4;
  uint32_t width = 0, height = 0, stride = 0, scanlines = 0;
  uint32_t uvOffset = 0, raw = 0, dpb = 0;
  uint32_t bin = 0, comvPerPicture = 0, nonComv = 0, line = 0;
  static constexpr uint64_t align(uint64_t x, uint64_t n) { return (x + n - 1) / n * n; }
  static constexpr uint64_t ceil(uint64_t x, uint64_t n) { return (x + n - 1) / n; }
  bool configure(uint32_t w, uint32_t h) {
    *this = AvcGeometry{};
    if (!w || !h || (w & 1) || (h & 1) || w > 1920 || h > 1088) return false;
    const uint64_t sw = align(w, 128), sh = align(h, 32), mbw = ceil(w, 16), mbh = ceil(h, 16);
    const uint64_t y = sw * sh, uv = sw * align(ceil(sh, 2), 16);
    // UBWC reference pictures have separate 4 KiB-aligned metadata and planes.
    const uint64_t reference = align(align(ceil(sw, 32), 64) * align(ceil(sh, 8), 16), 4096) +
      align(y, 4096) + align(align(ceil(sw / 2, 16), 64) * align(ceil(sh / 2, 8), 16), 4096) +
      align(sw * align(sh / 2, 32), 4096);
    const uint64_t pixels = align(w, 16) * align(h, 16);
    const uint64_t binPixels = pixels < 1280 * 736 ? 1280 * 736 : pixels;
    const uint64_t yuv = binPixels * 3 / 2;
    const uint64_t bins = 4 * (align(yuv / 4, 256) + align(yuv * 3 / 4, 256));
    const uint64_t halfRows = ceil(mbh, 2);
    const uint64_t motion = align(align(align(mbw * 128, 16) * halfRows, 64) * 2, 512) +
      align(align(align(mbw * 4, 16) * halfRows, 64) * 2, 512) + 512;
    const uint64_t slices = ceil(align(h, 32), 16) * 48;
    const uint64_t commands = slices < 1800 ? slices : 1800;
    const uint64_t vpp = commands * 512;
    const uint64_t non = align(commands * 128, 256) + align(vpp < 1048576 ? vpp : 1048576, 256) + 65536;
    uint64_t top = align(align(w, 32) / 32 * 256, 256) + 31 * 256;
    if (top < 32 * align(h, 8)) top = 32 * align(h, 8);
    const uint64_t opb = 2 * top + 2 * align(align(h, 8) / 2 * 64, 32);
    const uint64_t lines = align(640 * align(w, 16) * 3, 256) + align(64 * mbw, 256) +
      4 * align(64 * mbh, 256) + align(16 * mbw, 256) + 4 * align(16 * mbh, 256) +
      align(384 * mbw, 256) + align(128 * mbw, 256) + 2 * align(align(h, 16) * 32, 256) +
      align(ceil(w, 64) * ceil(h, 64) * 128, 256) + opb;
    const uint64_t rawSize = align(y + uv, 4096);
    if (rawSize > UINT32_MAX || reference > UINT32_MAX || bins > UINT32_MAX ||
        motion * MaxN + 512 > UINT32_MAX || non > UINT32_MAX || lines > UINT32_MAX) return false;
    width = w; height = h; stride = uint32_t(sw); scanlines = uint32_t(sh);
    uvOffset = uint32_t(y); raw = uint32_t(rawSize); dpb = uint32_t(reference);
    bin = uint32_t(bins); comvPerPicture = uint32_t(motion); nonComv = uint32_t(non); line = uint32_t(lines);
    return true;
  }
  uint32_t scratchBytes(unsigned i, unsigned n) const {
    if (!width || !n || n > MaxN) return 0;
    return i == 0 ? bin : i == 1 ? comvPerPicture * n + 512 : i == 2 ? nonComv : i == 3 ? line : 0;
  }
};
// Four-pipe VPU35 HEVC. References use QC08C/QC10C, while the CPU-visible
// output is linear NV12/P010. Stride is always bytes, including P010's two
// bytes per sample; the HFI pixel-stride property must convert it separately.
struct HevcGeometry {
  static constexpr unsigned MaxN = 10, ScratchCount = 4;
  // Includes the reference port's bounded three-dimensional SEI userdata.
  static constexpr uint32_t PersistBytes = 422144;
  uint32_t width = 0, height = 0, bitDepth = 0, stride = 0, scanlines = 0;
  uint32_t uvOffset = 0, raw = 0, dpb = 0;
  uint32_t bin = 0, comvPerPicture = 0, nonComv = 0, line = 0;
  static constexpr uint64_t align(uint64_t x, uint64_t n) { return (x + n - 1) / n * n; }
  static constexpr uint64_t ceil(uint64_t x, uint64_t n) { return (x + n - 1) / n; }
  bool configure(uint32_t w, uint32_t h, uint32_t depth) {
    *this = HevcGeometry{};
    if (!w || !h || (w & 1) || (h & 1) || w > 3840 || h > 2176 || (depth != 8 && depth != 10)) return false;
    const uint64_t sw = align(uint64_t(w) * (depth == 10 ? 2 : 1), depth == 10 ? 256 : 128);
    const uint64_t sh = align(h, 32), halfHeight = ceil(h, 2), y = sw * sh;
    const uint64_t rawSize = align(y + sw * align(halfHeight, 16), 4096);
    uint64_t reference = 0;
    if (depth == 8) {
      reference = align(align(ceil(w, 32), 64) * align(ceil(h, 8), 16), 4096) +
        align(align(w, 128) * align(h, 32), 4096) +
        align(align(ceil(w / 2, 16), 64) * align(ceil(h / 2, 8), 16), 4096) +
        align(align(w, 128) * align(h / 2, 32), 4096);
    } else {
      const uint64_t packedStride = align(uint64_t(w) * 4 / 3, 256);
      reference = align(align(ceil(w, 48), 64) * align(ceil(h, 4), 16), 4096) +
        align(packedStride * align(h, 16), 4096) +
        align(align(ceil(ceil(w, 2), 24), 64) * align(ceil(halfHeight, 4), 16), 4096) +
        align(packedStride * align(halfHeight, 16), 4096);
    }
    const uint64_t pixels = align(w, 16) * align(h, 16);
    const uint64_t binPixels = pixels < 1280 * 736 ? 1280 * 736 : pixels;
    const uint64_t bins = 8 * align(binPixels * 3 / 4, 256);
    const uint64_t motion = align(ceil(w, 16) * ceil(h, 16) * 256, 512);
    const uint64_t cells = (align(w, 64) / 16) * (align(h, 64) / 16);
    const uint64_t commandCells = align(cells * 32, 256);
    const uint64_t commands = commandCells < 3601 ? commandCells : 3601;
    const uint64_t vpp = align(commands, 4) * 512;
    const uint64_t non = align(commands * 128, 256) + align(vpp < 1048576 ? vpp : 1048576, 256) +
      align(32 * 20 * 22 * 4, 256) + align(4 * cells, 256) + align(32 * 2048, 256) + 4096;
    // The left edge accounts for the largest of the 16/32/64-pixel CTU cases.
    const uint64_t left = (uint64_t(h) + 63) / 8 * 16;
    uint64_t top = align(align(w, 32) / 32 * 256, 256) + 31 * 256;
    if (top < 32 * align(h, 8)) top = 32 * align(h, 8);
    const uint64_t opb = 2 * top + 2 * align(align(h, 8) / 2 * 64, 32);
    const uint64_t lines = align(640 * (align(w, 64) + 8) * 2, 256) +
      align(64 * align(w, 64) / 16, 256) + 4 * align(64 * align(h, 64) / 16, 256) +
      4 * align(left, 256) + align(128 * ceil(w, 16), 256) + align(384 * align(w, 16) / 16, 256) +
      align(ceil(w, 64) * 128, 256) + 4 * align(ceil(h, 64) * 128, 256) +
      4 * align(align(h, 16) * 32, 256) + align(ceil(w, 64) * ceil(h, 64) * 128, 256) + opb;
    if (rawSize > UINT32_MAX || reference > UINT32_MAX || bins > UINT32_MAX ||
        motion * MaxN + 512 > UINT32_MAX || non > UINT32_MAX || lines > UINT32_MAX) return false;
    width = w; height = h; bitDepth = depth; stride = uint32_t(sw); scanlines = uint32_t(sh);
    uvOffset = uint32_t(y); raw = uint32_t(rawSize); dpb = uint32_t(reference);
    bin = uint32_t(bins); comvPerPicture = uint32_t(motion); nonComv = uint32_t(non); line = uint32_t(lines);
    return true;
  }
  uint32_t scratchBytes(unsigned i, unsigned n) const {
    if (!width || !n || n > MaxN) return 0;
    return i == 0 ? bin : i == 1 ? comvPerPicture * n + 512 : i == 2 ? nonComv : i == 3 ? line : 0;
  }
};
} }
