// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. The same V3 bank, with independent backed session pages and invalid guards.
#pragma once
#include "VideoOps.h"
#include "VideoBuffers.h"
#include "VideoSmmu.h"
namespace pianovideo
{
namespace session
{
inline bool buildArena(vsmmu::Tables &t, const Layout &layout, const uint64_t *pages, uint32_t backingBytes = 0)
{
  const uint32_t arenaBytes = layout.arenaBytes();
  if (!backingBytes) backingBytes = arenaBytes;
  const unsigned tablePages = vsmmu::sessionTablePages(backingBytes);
  if (!t.cpu || !pages || (t.pa & 4095) || !tablePages || t.pageCount != tablePages ||
      !arenaBytes || layout.bytes > arenaBytes || arenaBytes > backingBytes)
    return false;
  const unsigned arenaTables = t.pageCount - vsmmu::kTablePages;
  const unsigned arenaPages = arenaTables * 512;
  uint64_t *l2 = t.cpu + 512;
  static_assert(((kArenaBase >> 21) & 511u) + kArenaBytes / (2 * 1024 * 1024) <= 512, "arena fits one L2");
  static_assert(vsmmu::kSessionTablePages == 4 + kAvcArenaBytes / (2 * 1024 * 1024), "AVC arena L3 table coverage");
  static_assert(vsmmu::kMaxSessionTablePages == 4 + kArenaBytes / (2 * 1024 * 1024), "HEVC arena L3 table coverage");
  for (unsigned i = 0; i < arenaPages; ++i)
    if (layout.mapped(i) && !arenaPagePa(pages[i]))   // XNU pages or the RAM16G extra-RAM run (VideoHw.h)
      return false;
  // A resident core keeps the UC mappings and L2 links. Each changed leaf
  // either becomes invalid or maps the same reserved PA for that slot.
  for (unsigned i = 0; i < arenaTables; ++i) {
    auto &link = l2[((kArenaBase >> 21) & 511u) + i];
    const uint64_t want = vsmmu::tableDesc(t.pa + (vsmmu::kTablePages + i) * 4096);
    if (link != want) link = want;
  }
  for (unsigned i = 0; i < arenaPages; ++i) {
    auto &leaf = t.cpu[vsmmu::kTablePages * 512 + i];
    const uint64_t want = layout.mapped(i) ? vsmmu::pageDesc(pages[i]) : 0;
    if (leaf != want) leaf = want;
  }
  for (unsigned i = 0; i < arenaPages; ++i)
    if (vsmmu::walk(t, kArenaBase + 4096 * i) != (layout.mapped(i) ? pages[i] : 0))
      return false;
  return vsmmu::walk(t, kArenaBase + 4096 * arenaPages) == 0;
}
} // namespace session
} // namespace pianovideo
