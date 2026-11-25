// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded VPU35 AVC/HEVC layouts, from the size facts at the upstream pin in UPSTREAM.
// The default layout preserves the 320x240 qualification; an explicit geometry
// selects the separate NV12 mode with at most sixteen reference pictures.
#pragma once
#include <stdint.h>
#include "VideoHw.h"
#include "VideoGeometry.h"

namespace pianovideo
{
namespace session
{
constexpr uint32_t kArenaBase = kIovaBase + kUcRegionBytes;
constexpr uint32_t kAvcArenaBytes = 256 * 1024 * 1024, kArenaBytes = 768 * 1024 * 1024;
constexpr unsigned kArenaPages = kArenaBytes / 4096, kMaxN = 64, kSlots = 141;
constexpr uint32_t kInputBytes = 7077888, kRawBytes = 147456, kDpbBytes = 155648, kUvOffset = 98304;
constexpr uint32_t comvBytes(unsigned n) { return 43008 * n + 512; }
enum BufferType : uint32_t { Bitstream = 1, Raw = 2, Partial = 5, Dpb = 6, Bin = 7, Line = 8, Comv = 10, NonComv = 11, Persist = 12 };
constexpr uint32_t scratchType[5] = {Bin, Comv, NonComv, Line, Partial};
constexpr uint32_t scratchBytes(unsigned i, unsigned n)
{
  return i == 0 ? 5652480 : i == 1 ? comvBytes(n) : i == 2 ? 557056 : i == 3 ? 685824 : i == 4 ? 196608 : 0;
}
struct Buffer {
  uint32_t type = 0, index = 0, off = 0, capacity = 0, size = 0, port = 0;
  bool queued = false, pendingRelease = false, readonly = false, released = false;
  uint32_t iova() const { return kArenaBase + off; }
};
struct Layout {
  Buffer buf[kSlots] = {};
  unsigned used = 0;
  uint32_t bytes = 0;
  bool general = false;
  bool hevc = false;
  AvcGeometry geometry;
  HevcGeometry hevcGeometry;
  uint32_t arenaLimit() const { return hevc ? kArenaBytes : kAvcArenaBytes; }
  uint32_t arenaBytes() const {
    if (!hevc) return kAvcArenaBytes;
    // Back the complete capacity layout, including both scratch generations,
    // Ncap DPB/RAW pairs and their guards. The 4K ceiling is not a minimum
    // allocation for an admitted 1080p session. L3 coverage needs 2 MiB units.
    const uint64_t span = (uint64_t(bytes) + 0x1fffff) & ~0x1fffffull;
    return bytes && span <= kArenaBytes ? uint32_t(span) : 0;
  }
  unsigned scratchCount() const { return hevc ? HevcGeometry::ScratchCount : general ? AvcGeometry::ScratchCount : 5; }
  unsigned maxN() const { return hevc ? HevcGeometry::MaxN : general ? AvcGeometry::MaxN : kMaxN; }
  uint32_t scratchSize(unsigned i, unsigned n) const { return hevc ? hevcGeometry.scratchBytes(i, n) : general ? geometry.scratchBytes(i, n) : scratchBytes(i, n); }
  uint32_t rawBytes() const { return hevc ? hevcGeometry.raw : general ? geometry.raw : kRawBytes; }
  uint32_t dpbBytes() const { return hevc ? hevcGeometry.dpb : general ? geometry.dpb : kDpbBytes; }
  uint32_t uvOffset() const { return hevc ? hevcGeometry.uvOffset : general ? geometry.uvOffset : kUvOffset; }
  uint32_t persistBytes() const { return hevc ? HevcGeometry::PersistBytes : 498176; }
  bool add(uint32_t type, unsigned index, uint32_t capacity, uint32_t port)
  {
    const uint64_t off = (uint64_t(bytes) + 16383) & ~16383ull;
    const uint64_t end = off + ((uint64_t(capacity) + 16383) & ~16383ull) + 16384; // invalid guard, including XNU page boundary
    if (!capacity || used >= kSlots || end > arenaLimit())
      return false;
    Buffer &b = buf[used++];
    b = Buffer{};
    b.type = type;
    b.index = index;
    b.off = uint32_t(off);
    b.capacity = capacity;
    b.size = capacity;
    b.port = port;
    bytes = uint32_t(end);
    return true;
  }
  bool init()
  {
    general = hevc = false;
    geometry = AvcGeometry{};
    hevcGeometry = HevcGeometry{};
    return populate();
  }
  bool init(uint32_t codedWidth, uint32_t codedHeight)
  {
    used = bytes = 0;
    general = hevc = false;
    hevcGeometry = HevcGeometry{};
    if (!geometry.configure(codedWidth, codedHeight)) return false;
    general = true;
    return populate();
  }
  bool initHevc(uint32_t codedWidth, uint32_t codedHeight, uint32_t depth)
  {
    used = bytes = 0;
    general = hevc = false;
    geometry = AvcGeometry{};
    if (!hevcGeometry.configure(codedWidth, codedHeight, depth)) return false;
    general = hevc = true;
    return populate();
  }
  bool populate()
  {
    used = bytes = 0;
    if (!add(Bitstream, 0, kInputBytes, 1) || !add(Bitstream, 1, kInputBytes, 1) || !add(Persist, 0, persistBytes(), 0))
      return false;
    for (unsigned set = 0; set < 2; ++set)
      for (unsigned i = 0; i < scratchCount(); ++i)
        if (!add(scratchType[i], set, scratchSize(i, maxN()), 1))
          return false;
    for (unsigned i = 0; i < maxN(); ++i)
      if (!add(Dpb, i, dpbBytes(), 2) || !add(Raw, i, rawBytes(), 2))
        return false;
    return used == 3 + 2 * scratchCount() + 2 * maxN();
  }
  Buffer *find(uint32_t type, uint32_t index)
  {
    for (unsigned i = 0; i < used; ++i)
      if (buf[i].type == type && buf[i].index == index)
        return &buf[i];
    return nullptr;
  }
  bool mapped(unsigned page) const
  {
    if (page >= arenaBytes() / 4096) return false;
    const uint32_t off = page * 4096;
    for (unsigned i = 0; i < used; ++i)
      if (off >= buf[i].off && off - buf[i].off < ((uint64_t(buf[i].capacity) + 4095) & ~4095ull))
        return true;
    return false;
  }
};
// CPU hash reads active pixels only. Both kernel SHA256 and the independent host implementation use this traversal.
template <class Update> void activeRows(const volatile uint8_t *base, Update update)
{
  uint8_t row[320];
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned y = 0; y < (plane ? 120u : 240u); ++y) {
      const uint32_t off = (plane ? kUvOffset : 0) + 384 * y;
      for (unsigned x = 0; x < 320; ++x)
        row[x] = base[off + x];
      update(row, sizeof row);
    }
}
inline void hexDigest(const uint8_t digest[32], char text[65])
{
  const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    text[2 * i] = hex[digest[i] >> 4];
    text[2 * i + 1] = hex[digest[i] & 15];
  }
  text[64] = 0;
}
inline bool sameText(const char *a, const char *b)
{
  for (unsigned i = 0; i < 65; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}
} // namespace session
} // namespace pianovideo
