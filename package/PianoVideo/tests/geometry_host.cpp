// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Geometry vectors include padding-heavy small pictures and the cropped 1080
// allocation. Check capacities, the two scratch generations and DMA guard gaps.
// c++ -std=c++20 -O2 -Wall -Wextra -Werror -I .. geometry_host.cpp -o /tmp/video_geometry_host && /tmp/video_geometry_host
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "VideoArena.h"
#include "../../include/PianoExtraRam.h"
using namespace pianovideo;
using namespace pianovideo::session;
static unsigned failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %u: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Vector {
  uint32_t w, h, stride, rows, uv, raw, dpb, bin, comv4, comv16, non, line;
};
static constexpr Vector vectors[] = {
  {160,96,256,96,24576,36864,49152,5652480,35328,139776,249856,347904},
  {320,240,384,256,98304,147456,155648,5652480,172544,688640,557056,685824},
  {1280,720,1280,736,942080,1413120,1445888,5652480,1946112,7782912,1217536,2689280},
  {1920,1088,1920,1088,2088960,3133440,3153920,12533760,4311552,17244672,1217536,4054272},
};
static void verifyLayout(const Layout &l) {
  CHECK(l.bytes <= l.arenaBytes());
  uint32_t previousEnd = 0;
  for (unsigned i = 0; i < l.used; ++i) {
    const auto &b = l.buf[i];
    CHECK(b.capacity && b.size == b.capacity && !(b.off & 16383));
    CHECK(b.off >= previousEnd);
    CHECK(l.mapped(b.off / 4096));
    CHECK(l.mapped((b.off + b.capacity - 1) / 4096));
    const uint32_t roundedEnd = b.off + ((b.capacity + 16383) & ~16383u);
    CHECK(!l.mapped(roundedEnd / 4096));
    CHECK(!l.mapped((roundedEnd + 16383) / 4096));
    previousEnd = roundedEnd + 16384;
  }
  CHECK(previousEnd == l.bytes);
  CHECK(!l.mapped(l.bytes / 4096));
  CHECK(!l.mapped(l.arenaBytes() / 4096));
  CHECK(!l.mapped(UINT32_MAX));
}
// Independent size vectors from the pinned reference's layout facts, including
// heights that give different NV12 and P010/reference plane padding.
struct HevcVector {
  uint32_t w, h, depth, stride, rows, uv, raw, dpb, bin, motion, non, line;
};
static constexpr HevcVector hevcVectors[] = {
  {2,2,8,128,32,4096,8192,16384,5652480,512,453888,115712},
  {2,2,10,256,32,8192,12288,16384,5652480,512,453888,115712},
  {160,96,8,256,96,24576,36864,49152,5652480,15360,1568256,305152},
  {160,96,10,512,96,49152,73728,45056,5652480,15360,1568256,305152},
  {320,240,8,384,256,98304,147456,155648,5652480,76800,1636864,510976},
  {320,240,10,768,256,196608,294912,196608,5652480,76800,1636864,510976},
  {1920,1088,8,1920,1088,2088960,3133440,3153920,12533760,2088960,1668352,2920448},
  {1920,1088,10,3840,1088,4177920,6266880,4210688,12533760,2088960,1668352,2920448},
  {3840,2160,8,3840,2176,8355840,12533760,12591104,49766400,8294400,1766144,5956096},
  {3840,2160,10,7680,2176,16711680,25067520,16736256,49766400,8294400,1766144,5956096},
  {3840,2174,8,3840,2176,8355840,12533760,12591104,50135040,8355840,1766144,5960192},
  {3840,2174,10,7680,2176,16711680,25067520,16818176,50135040,8355840,1766144,5960192},
  {3840,2176,8,3840,2176,8355840,12533760,12591104,50135040,8355840,1766144,5960192},
  {3840,2176,10,7680,2176,16711680,25067520,16818176,50135040,8355840,1766144,5960192},
};
static unsigned extraArenas = 0;
static void invalidTableRangeTest() {
  Layout layout;
  CHECK(layout.init());
  const unsigned count = vsmmu::sessionTablePages(layout.arenaBytes());
  std::vector<uint64_t> words(count * 512), pages(layout.arenaBytes() / 4096), uc(vsmmu::kUcPages);
  for (unsigned i = 0; i < uc.size(); ++i) uc[i] = kXnuDramBase + i * 4096;
  for (unsigned i = 0; i < pages.size(); ++i) pages[i] = kXnuDramBase + 0x1000000 + i * 4096;
  vsmmu::Tables t{words.data(), kXnuDramEnd - 4096, count};
  const auto untouched = words;
  CHECK(!vsmmu::build(t, uc.data(), unsigned(uc.size()), kXnuDramBase + 0x800000) && words == untouched);
  words = untouched;
  t.cpu = words.data();
  CHECK(!buildArena(t, layout, pages.data()) && words == untouched);
}
static void verifyArena(const Layout &l) {
  const unsigned pageCount = l.arenaBytes() / 4096;
  const unsigned tableCount = vsmmu::sessionTablePages(l.arenaBytes());
  CHECK(tableCount == 4 + (l.hevc ? (l.bytes + 2097151) / 2097152 : 128));
  std::vector<uint64_t> words(tableCount * 512), pages(pageCount), uc(vsmmu::kUcPages);
  for (unsigned i = 0; i < pages.size(); ++i)
    pages[i] = kXnuDramBase + 0x100000000ull + uint64_t((i * 149u) % pageCount) * 4096;
  for (unsigned i = 0; i < uc.size(); ++i) uc[i] = kXnuDramBase + 0x1000000 + uint64_t(i) * 4096;
  vsmmu::Tables t{words.data(), kXnuDramBase + 0x2000000, tableCount};
  CHECK(vsmmu::build(t, uc.data(), unsigned(uc.size()), kXnuDramBase + 0x3000000));
  CHECK(buildArena(t, l, pages.data()));
  unsigned mapped = 0, unmapped = 0;
  for (unsigned i = 0; i < pageCount; ++i) {
    const bool present = l.mapped(i);
    mapped += present; unmapped += !present;
    CHECK(vsmmu::walk(t, kArenaBase + i * 4096) == (present ? pages[i] : 0));
    CHECK(vsmmu::walk(t, kArenaBase + i * 4096 + 4095) == (present ? pages[i] + 4095 : 0));
  }
  CHECK(mapped && unmapped >= l.used * 4);
  CHECK(vsmmu::walk(t, kArenaBase - 1) == kXnuDramBase + 0x3000fff);
  CHECK(!vsmmu::walk(t, kArenaBase + l.arenaBytes()));
  CHECK(!vsmmu::walk(t, 0xc0000000));
  // Every dynamic L3 page is backed by exactly the expected table page.
  for (unsigned i = 0; i < tableCount - 4; ++i)
    CHECK(words[512 + ((kArenaBase >> 21) & 511u) + i] == vsmmu::tableDesc(t.pa + (4 + i) * 4096));
  CHECK(!buildArena(t, l, nullptr));
  const unsigned savedCount = t.pageCount;
  t.pageCount = savedCount + 1;
  CHECK(!buildArena(t, l, pages.data()));
  t.pageCount = savedCount;
  const unsigned last = (l.buf[l.used - 1].off + l.buf[l.used - 1].capacity - 1) / 4096;
  const uint64_t saved = pages[last];
  for (uint64_t bad : {saved + 1, kXnuDramBase - 4096, kXnuDramEnd, kExtraArenaBase - 4096, kExtraArenaEnd, kExtraArenaBase + 1,
                       0x9c300000ull /* the video carve-out */, 0x9d200000ull /* cdsp */}) {
    pages[last] = bad;
    CHECK(!buildArena(t, l, pages.data()));
  }
  pages[last] = saved;
  CHECK(buildArena(t, l, pages.data()));
  // RAM16G: the same arena backed by the region0 run (2 MiB slabs, here in reverse slab order), and mixed with XNU pages; only
  // arenas the run holds (the AVC 256 MiB one; HEVC's larger ones stay XNU-backed).
  if (uint64_t(pageCount) * 4096 <= kExtraArenaEnd - kExtraArenaBase) {
    using namespace pianoxram;
    const uint8_t property[kMaxRuns * kRecordBytes] = {
        0, 0, 0, 0x80, 8, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, // GPU
        0, 0, 0xe0, 0x89, 0, 0, 0, 0, 0, 0, 0x20, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, // modem
        0, 0, 0xe0, 0x9e, 0, 0, 0, 0, 0, 0, 0x60, 4, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, // pil-tail
    };
    Pool pool;
    CHECK(pool.load(property, sizeof property, Config{UseVideo}) == 1 && pool.count == 3);
    CHECK(pool.runs[0].state == StateOtherUse && pool.runs[2].state == StateOtherUse);
    const auto &modem = pool.runs[1];
    CHECK(modem.state == StateOurs && modem.first == kExtraArenaBase && modem.slabs == 137);
    ++extraArenas;
    std::vector<uint64_t> extra(pageCount);
    const unsigned slabs = (pageCount + 511) / 512;
    CHECK(slabs <= modem.slabs);
    for (unsigned i = 0; i < pageCount; ++i) extra[i] = modem.slab(slabs - 1 - i / 512) + (i % 512) * 4096ull;
    CHECK(extra.back() < kExtraArenaEnd);
    CHECK(buildArena(t, l, extra.data()));
    for (unsigned i = 0; i < pageCount; ++i) CHECK(vsmmu::walk(t, kArenaBase + i * 4096 + 8) == (l.mapped(i) ? extra[i] + 8 : 0));
    extra[0] = pages[0];
    CHECK(buildArena(t, l, extra.data()));
  }
  CHECK(buildArena(t, l, pages.data()));
  // Rebuilding with fewer allocated slots must not leave an old valid PTE.
  Layout shortened = l;
  shortened.used -= 2;
  CHECK(buildArena(t, shortened, pages.data()));
  CHECK(!vsmmu::walk(t, kArenaBase + last * 4096));
}
static void residentArenaTest() {
  constexpr unsigned tableCount = vsmmu::kSessionTablePages;
  std::vector<uint64_t> words(tableCount * 512), pages(kAvcArenaBytes / 4096), uc(vsmmu::kUcPages);
  for (unsigned i = 0; i < uc.size(); ++i) uc[i] = kXnuDramBase + i * 4096ull;
  for (unsigned i = 0; i < pages.size(); ++i) pages[i] = kXnuDramBase + 0x100000000ull + i * 4096ull;
  vsmmu::Tables t{words.data(), kXnuDramBase + 0x2000000, tableCount};
  const uint64_t scratch = kXnuDramBase + 0x3000000;
  CHECK(vsmmu::build(t, uc.data(), unsigned(uc.size()), scratch));
  Layout layout;
  CHECK(layout.init(1920,1088));
  CHECK(buildArena(t,layout,pages.data(),kAvcArenaBytes));
  layout.buf[0].queued = layout.buf[0].readonly = layout.buf[0].pendingRelease = true;
  const uint64_t queuePage = vsmmu::walk(t,kIovaBase);
  const std::vector<uint64_t> links(words.begin(),words.begin()+vsmmu::kTablePages*512);
  CHECK(layout.initHevc(1920,1088,10));
  CHECK(layout.arenaBytes() == 188 * 1024 * 1024);
  CHECK(!layout.buf[0].queued && !layout.buf[0].readonly && !layout.buf[0].pendingRelease);
  CHECK(buildArena(t,layout,pages.data(),kAvcArenaBytes));
  CHECK(std::equal(links.begin(),links.end(),words.begin()));
  CHECK(vsmmu::walk(t,kIovaBase) == queuePage);
  for (unsigned i = 0; i < pages.size(); ++i)
    CHECK(vsmmu::walk(t,kArenaBase+i*4096) == (layout.mapped(i) ? pages[i] : 0));
  CHECK(layout.init(1920,1088));
  CHECK(buildArena(t,layout,pages.data(),kAvcArenaBytes));
}
static void hevcTests() {
  CHECK(kAvcArenaBytes == 256 * 1024 * 1024 && kArenaBytes == 768 * 1024 * 1024);
  CHECK(kArenaPages == 196608 && vsmmu::kSessionTablePages == 132 && vsmmu::kMaxSessionTablePages == 388);
  CHECK(vsmmu::sessionTablePages(kAvcArenaBytes) == 132 && vsmmu::sessionTablePages(kArenaBytes) == 388);
  CHECK(!vsmmu::sessionTablePages(0) && !vsmmu::sessionTablePages(1));
  CHECK(!vsmmu::sessionTablePages(kArenaBytes + 2 * 1024 * 1024) && !vsmmu::sessionTablePages(UINT32_MAX));
  CHECK(kArenaBase + kArenaBytes == 0xb0400000);
  for (const auto &v : hevcVectors) {
    Layout l;
    CHECK(l.initHevc(v.w, v.h, v.depth));
    const auto &g = l.hevcGeometry;
    CHECK(l.general && l.hevc && !l.geometry.width && l.arenaLimit() == kArenaBytes);
    CHECK(l.arenaBytes() >= l.bytes && l.arenaBytes() - l.bytes < 2097152);
    CHECK(!(l.arenaBytes() & 2097151));
    CHECK(l.maxN() == 10 && l.scratchCount() == 4 && l.used == 31);
    CHECK(g.width == v.w && g.height == v.h && g.bitDepth == v.depth);
    CHECK(g.stride == v.stride && g.scanlines == v.rows && g.comvPerPicture == v.motion);
    CHECK(l.uvOffset() == v.uv && l.rawBytes() == v.raw && l.dpbBytes() == v.dpb);
    CHECK(l.scratchSize(0,4) == v.bin && l.scratchSize(1,4) == v.motion * 4 + 512);
    CHECK(l.scratchSize(1,10) == v.motion * 10 + 512 && l.scratchSize(2,4) == v.non && l.scratchSize(3,4) == v.line);
    CHECK(!l.scratchSize(1,0) && !l.scratchSize(1,11) && !l.scratchSize(4,4));
    CHECK(l.find(Persist,0) && l.find(Persist,0)->capacity == 422144);
    CHECK(l.find(Raw,9) && l.find(Dpb,9) && !l.find(Raw,10) && !l.find(Partial,0));
    for (unsigned i = 0; i < 4; ++i) {
      const Buffer *a = l.find(scratchType[i],0), *b = l.find(scratchType[i],1);
      CHECK(a && b && a->capacity == l.scratchSize(i,10) && b->capacity == a->capacity && a->off != b->off);
    }
    verifyLayout(l);
    verifyArena(l);
  }
  // The admitted 1080p budgets include all Ncap10 buffers and invalid guards,
  // not the stream's SPS minimum or the first PSC's possibly smaller N.
  for (unsigned depth : {8u,10u}) {
    Layout l;
    CHECK(l.initHevc(1920,1088,depth));
    CHECK(l.bytes == (depth == 8 ? 154255360u : 196034560u));
    CHECK(l.arenaBytes() == (depth == 8 ? 155189248u : 197132288u));
    CHECK(vsmmu::sessionTablePages(l.arenaBytes()) == (depth == 8 ? 78u : 98u));
    const unsigned span = l.arenaBytes();
    // PSC adoption changes geometry/payload sizes, never reserved capacities.
    CHECK(l.hevcGeometry.configure(1920,1080,depth));
    l.find(Dpb,0)->size = l.hevcGeometry.dpb;
    CHECK(l.arenaBytes() == span);
    CHECK(l.init(1920,1088) && l.arenaBytes() == kAvcArenaBytes);
    CHECK(l.initHevc(1920,1088,depth) && l.arenaBytes() == span);
  }
  // Literal budgets independently calculated for the two scratch generations,
  // both inputs and N split DPB/RAW pairs. Each capacity has its own 16 KiB guard.
  static constexpr uint32_t bytes[2][7] = {
    {298041344,339918848,381796352,423673856,465551360,507428864,549306368},
    {365084672,423723008,482361344,540999680,599638016,658276352,716914688},
  };
  for (unsigned depth = 8; depth <= 10; depth += 2) {
    Layout full;
    CHECK(full.initHevc(3840,2176,depth));
    CHECK(full.bytes == bytes[(depth - 8) / 2][6]);
    for (unsigned n = 4; n <= 10; ++n) {
      Layout l = full;
      l.used = l.bytes = 0;
      CHECK(l.add(Bitstream,0,kInputBytes,1) && l.add(Bitstream,1,kInputBytes,1) && l.add(Persist,0,422144,0));
      for (unsigned set = 0; set < 2; ++set)
        for (unsigned i = 0; i < 4; ++i) CHECK(l.add(scratchType[i],set,l.scratchSize(i,n),1));
      for (unsigned i = 0; i < n; ++i) CHECK(l.add(Dpb,i,l.dpbBytes(),2) && l.add(Raw,i,l.rawBytes(),2));
      CHECK(l.used == 11 + 2 * n && l.bytes == bytes[(depth - 8) / 2][n - 4]);
      verifyLayout(l);
      verifyArena(l);
    }
    CHECK(full.init(1920,1088) && !full.hevc && !full.hevcGeometry.width && full.arenaBytes() == kAvcArenaBytes);
    CHECK(full.initHevc(3840,2176,depth) && full.init() && !full.hevc && !full.hevcGeometry.width);
  }
  const uint32_t invalid[][3] = {
    {0,96,8},{160,0,8},{161,96,8},{160,95,10},{3842,2176,10},{3840,2178,10},
    {160,96,0},{160,96,9},{160,96,12},{UINT32_MAX,UINT32_MAX,10},{3840,2176,UINT32_MAX},
  };
  for (const auto &v : invalid) {
    Layout l;
    CHECK(l.initHevc(3840,2176,10));
    CHECK(!l.initHevc(v[0],v[1],v[2]));
    CHECK(!l.hevc && !l.general && !l.used && !l.bytes && !l.hevcGeometry.width && !l.geometry.width);
  }
  for (bool hevc : {false,true}) {
    Layout l;
    l.hevc = hevc;
    const uint32_t limit = l.arenaLimit();
    if(hevc) CHECK(!l.arenaBytes());
    CHECK(!l.add(Raw,0,0,2) && !l.add(Raw,0,UINT32_MAX,2));
    CHECK(!l.used && !l.bytes);
    CHECK(l.add(Raw,0,limit - 16384,2));
    CHECK(l.bytes == limit && l.arenaBytes() == limit && !l.add(Raw,1,1,2) && l.used == 1);
    CHECK(!l.mapped((l.arenaBytes() - 16384) / 4096));
    l.bytes = UINT32_MAX;
    if(hevc) CHECK(!l.arenaBytes());
    CHECK(!l.add(Raw,1,4096,2) && l.used == 1 && l.bytes == UINT32_MAX);
    l.bytes = 0; l.used = kSlots;
    CHECK(!l.add(Raw,1,4096,2) && l.used == kSlots && !l.bytes);
  }
}
int main() {
  for (const auto &v : vectors) {
    Layout l;
    CHECK(l.init(v.w, v.h));
    CHECK(l.general && l.maxN() == 16 && l.scratchCount() == 4 && l.used == 43);
    CHECK(l.geometry.width == v.w && l.geometry.height == v.h);
    CHECK(l.geometry.stride == v.stride && l.geometry.scanlines == v.rows);
    CHECK(l.uvOffset() == v.uv && l.rawBytes() == v.raw && l.dpbBytes() == v.dpb);
    CHECK(l.scratchSize(0,4) == v.bin && l.scratchSize(1,4) == v.comv4);
    CHECK(l.scratchSize(1,16) == v.comv16 && l.scratchSize(2,4) == v.non && l.scratchSize(3,4) == v.line);
    CHECK(!l.scratchSize(1,0) && !l.scratchSize(1,17) && !l.scratchSize(4,4));
    CHECK(l.find(Raw,15) && l.find(Dpb,15) && !l.find(Raw,16) && !l.find(Partial,0));
    for (unsigned i = 0; i < 4; ++i) {
      const Buffer *a = l.find(scratchType[i],0), *b = l.find(scratchType[i],1);
      CHECK(a && b && a->capacity == l.scratchSize(i,16) && b->capacity == a->capacity);
      CHECK(a && b && a->off != b->off);
    }
    verifyLayout(l);
    CHECK(!l.hevc && l.arenaBytes() == kAvcArenaBytes);
    if (v.w == 1920) CHECK(l.bytes == 186449920); // 177.8125 MiB, including every guard
  }
  Layout legacy;
  CHECK(legacy.init() && !legacy.general && legacy.used == 141 && legacy.bytes == 56623104);
  CHECK(legacy.maxN() == 64 && legacy.scratchCount() == 5 && legacy.find(Partial,1));
  CHECK(legacy.rawBytes() == 147456 && legacy.dpbBytes() == 155648 && legacy.uvOffset() == 98304);
  verifyLayout(legacy);
  verifyArena(legacy);
  const uint32_t invalid[][2] = {{0,96},{160,0},{161,96},{160,95},{1922,1088},{1920,1090},{UINT32_MAX,UINT32_MAX}};
  for (const auto &v : invalid) {
    CHECK(!legacy.init(v[0],v[1]));
    CHECK(!legacy.general && !legacy.used && !legacy.bytes && !legacy.geometry.width);
  }
  CHECK(legacy.init(1920,1088) && legacy.init() && !legacy.general && legacy.bytes == 56623104);
  hevcTests();
  residentArenaTest();
  invalidTableRangeTest();
  CHECK(extraArenas > 0);   // the RAM16G region0 backing was exercised
  std::printf("geometry: %s (%u failures, %u arenas also on region0 slabs)\n", failures ? "FAIL" : "PASS", failures, extraArenas);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
