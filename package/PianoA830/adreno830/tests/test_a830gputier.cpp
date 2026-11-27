/* SPDX-License-Identifier: MIT */
/* The region1 GPU tier (A830GpuTier.h, RAM16G R1; boot argument piano-gpu-tier, default off).
 *  1. /chosen piano-extra-ram parsing: both banks in one property, empty padding, malformed records and the XNU window.
 *  2. The off switch: modes 1 and 2; the last token wins; prefixes and other values stay off.
 *  3. The slab allocator: whole 2 MiB slabs, lowest first, spans inside one run, all-or-nothing refusals
 *     (span limit, size and address width), release refuses foreign / partial / double frees, exhaustion, refill and reset.
 *  4. The per-run self-test: a run whose probe fails is kept out (its line says FAIL) and never handed out.
 * The wired BO paths, including import and CPU-map denial and XNU fallback, are tested in test_tier_boalloc.cpp.
 * Build: clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wsign-conversion -fsanitize=address,undefined -fno-sanitize-recover=all test_a830gputier.cpp -o /tmp/test_a830gputier && /tmp/test_a830gputier
 * host-test: */
#include "../A830GpuTier.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace a830tier;
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static void put(uint8_t *p, uint64_t v, unsigned bytes) {
  for (unsigned i = 0; i < bytes; i++) p[i] = uint8_t(v >> (8 * i));
}
static std::vector<uint8_t> prop(std::initializer_list<pianoxram::Run> runs) {
  std::vector<uint8_t> bytes(pianoxram::kMaxRuns * pianoxram::kRecordBytes, 0);
  uint8_t *p = bytes.data();
  for (const auto &run : runs) {
    put(p, run.base, 8);
    put(p + 8, run.size, 8);
    put(p + 16, run.bank, 4);
    put(p + 20, run.use, 4);
    p += pianoxram::kRecordBytes;
  }
  return bytes;
}
struct Ctx {
    std::vector<Run> probed;   // base = PA, size = run number
    std::vector<uint64_t> badProbe;
    std::vector<std::string> lines;
};
static bool probe(uint64_t pa, unsigned run, void *c) {
  auto &ctx = *static_cast<Ctx *>(c);
  ctx.probed.push_back({pa, run});
  for (auto b : ctx.badProbe) if (b == pa) return false;
  return true;
}
static void line(const char *l, void *c) { ((Ctx *)c)->lines.push_back(l); }
static constexpr uint64_t MiB = 1ull << 20;

static void testFragmentation(const Run (&runs)[3], uint64_t limit) {
  Ctx ctx;
  Tier tier;
  tier.init(runs, 3, limit, probe, &ctx, nullptr);
  constexpr uint64_t capacity = 870 * MiB;
  const uint64_t sizes[] = {2 * MiB, 4 * MiB, 8 * MiB, 16 * MiB, 32 * MiB, 64 * MiB, 128 * MiB};
  const uint64_t residencyMiB[] = {2, 6, 14, 30, 62, 126, 254};
  Span pieces[3]{};
  Span prefix{}, held[7]{};
  CHECK(tier.alloc(504 * MiB, &prefix, 1, limit) == 1);
  uint64_t next = runs[0].base + prefix.bytes;
  for (unsigned i = 0; i < 7; ++i) {
    CHECK(tier.alloc(sizes[i], &held[i], 1, limit) == 1);
    CHECK(held[i].pa == next && held[i].bytes == sizes[i]);
    CHECK(tier.freeBytes() == capacity - prefix.bytes - residencyMiB[i] * MiB);
    next += sizes[i];
  }
  CHECK(next == runs[0].base + runs[0].size);
  CHECK(tier.alloc(112 * MiB, pieces, 3, limit) == 2);
  CHECK(pieces[0].pa == runs[1].base && pieces[0].bytes == runs[1].size);
  CHECK(pieces[1].pa == runs[2].base && pieces[1].bytes == runs[2].size);
  CHECK(tier.freeBytes() == 0 && tier.alloc(1, pieces + 2, 1, limit) == 0);

  // Leave two separated holes. Failing the span limit must put the first hole back.
  CHECK(tier.release(held + 4, 1) && tier.release(held + 6, 1));
  CHECK(tier.freeBytes() == 160 * MiB);
  Span holes[2]{};
  CHECK(tier.alloc(160 * MiB, holes, 1, limit) == 0 && tier.freeBytes() == 160 * MiB);
  CHECK(tier.alloc(160 * MiB, holes, 2, limit) == 2 && tier.freeBytes() == 0);
  CHECK(holes[0].pa == held[4].pa && holes[0].bytes == held[4].bytes);
  CHECK(holes[1].pa == held[6].pa && holes[1].bytes == held[6].bytes);
  CHECK(tier.release(holes, 2) && tier.release(held + 5, 1));
  Span coalesced{};
  CHECK(tier.alloc(224 * MiB, &coalesced, 1, limit) == 1);
  CHECK(coalesced.pa == held[4].pa && coalesced.bytes == 224 * MiB);

  const Span overflow{pieces[0].pa, UINT64_MAX & ~(kSlab - 1)};
  const Span acrossRuns{pieces[0].pa + pieces[0].bytes - kSlab, 2 * kSlab};
  const Span overlap[] = {coalesced, {coalesced.pa + kSlab, kSlab}};
  const Span invalidBatch[] = {pieces[1], acrossRuns};
  CHECK(!tier.release(&overflow, 1) && !tier.release(overlap, 2));
  CHECK(!tier.release(invalidBatch, 2) && tier.freeBytes() == 0);
  CHECK(!tier.release(nullptr, 1) && !tier.release(pieces, 0));
  CHECK(tier.release(&coalesced, 1) && tier.release(held, 4) && tier.release(&prefix, 1));
  CHECK(tier.release(pieces, 2) && tier.freeBytes() == capacity);

  // A narrower allocation limit can fail after taking a span from an accessible run.
  CHECK(tier.alloc(capacity, pieces, 3, runs[1].base) == 0 && tier.freeBytes() == capacity);
  CHECK(tier.alloc(capacity, pieces, 3, limit) == 3 && tier.freeBytes() == 0);
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(pieces[i].pa == runs[i].base && pieces[i].bytes == runs[i].size);
  }
  CHECK(tier.release(pieces, 3) && tier.freeBytes() == capacity);
  CHECK(tier.alloc(0, pieces, 3, limit) == 0 && tier.alloc(kSlab, nullptr, 3, limit) == 0);
  CHECK(tier.alloc(kSlab, pieces, 0, limit) == 0 && tier.freeBytes() == capacity);

  // The BO path's 64-span limit must reject 65 holes without consuming any of them.
  Span alternating[130]{};
  for (Span &slab : alternating) { CHECK(tier.alloc(kSlab, &slab, 1, limit) == 1); }
  Span tail[3]{};
  const unsigned tailCount = tier.alloc(tier.freeBytes(), tail, 3, limit);
  CHECK(tailCount == 3 && tier.freeBytes() == 0);
  for (unsigned i = 0; i < 130; i += 2) { CHECK(tier.release(alternating + i, 1)); }
  Span fragmented[65]{};
  CHECK(tier.alloc(65 * kSlab, fragmented, 64, limit) == 0 && tier.freeBytes() == 65 * kSlab);
  CHECK(tier.alloc(65 * kSlab, fragmented, 65, limit) == 65 && tier.freeBytes() == 0);
  for (unsigned i = 0; i < 65; ++i) { CHECK(fragmented[i].pa == alternating[2 * i].pa && fragmented[i].bytes == kSlab); }
  CHECK(tier.release(fragmented, 65) && tier.release(tail, tailCount));
  for (unsigned i = 1; i < 130; i += 2) { CHECK(tier.release(alternating + i, 1)); }
  CHECK(tier.freeBytes() == capacity);

  // Adjacent physical runs still have independent qualification and release boundaries.
  const Run adjacent[] = {{runs[0].base, kSlab}, {runs[0].base + kSlab, kSlab}};
  tier.init(adjacent, 2, limit, probe, &ctx, nullptr);
  CHECK(tier.alloc(2 * kSlab, pieces, 3, limit) == 2);
  const Span merged{adjacent[0].base, 2 * kSlab};
  CHECK(!tier.release(&merged, 1) && tier.freeBytes() == 0);
  CHECK(tier.release(pieces, 2) && tier.freeBytes() == 2 * kSlab);
}

int main() {
  /* 1 */
  using pianoxram::Region0; using pianoxram::Region1;
  using pianoxram::UseGpu; using pianoxram::UseDsp; using pianoxram::UseVideo; using pianoxram::UseAny;
  const pianoxram::Run a{0x880000000ull, 758 * MiB, Region1, UseGpu};
  const pianoxram::Run b{0x8b0000000ull, 84 * MiB, Region1, UseGpu};
  const pianoxram::Run c{0x8b5800000ull, 28 * MiB, Region1, UseGpu};
  const pianoxram::Run modem{0x89e00000, 274 * MiB, Region0, UseVideo};
  const pianoxram::Run pil{0x9ee00000, 70 * MiB, Region0, UseDsp};
  Run r[kMaxRuns]; unsigned n = 9;
  auto load = [&](const std::vector<uint8_t> &p) {
    return loadRuns(p.data(), p.size(), 0x8c0004000ull, 0x33fe00000ull, r, n);
  };
  CHECK(load(prop({b, modem, c, pil, a})) == Parse::ok && n == 3);
  CHECK(r[0].base == a.base && r[0].size == a.size && r[1].base == b.base && r[2].size == c.size);
  CHECK(loadRuns(nullptr, 0, 0, 0, r, n) == Parse::absent && n == 0);
  CHECK(load(prop({})) == Parse::absent && n == 0);
  CHECK(load(std::vector<uint8_t>(136, 0)) == Parse::badLength && n == 0);
  auto shortProperty = prop({a});
  shortProperty.resize(pianoxram::kRecordBytes);
  CHECK(load(shortProperty) == Parse::badLength && n == 0);
  CHECK(load(prop({a, {}, b})) == Parse::rejected && n == 0);
  CHECK(load(prop({modem, pil, {modem.base, modem.size, Region0, UseGpu},
                  {a.base, a.size, Region1, UseAny}, {b.base, b.size, Region1, UseVideo}})) == Parse::ok && n == 0);
  CHECK(load(prop({{0x880001000ull, 4 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x880000000ull, 0, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x80000000ull, 2 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x8bfe00000ull, 4 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({a, {0x880200000ull, 2 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  const pianoxram::Run wrongUse{a.base + kSlab, kSlab, Region1, UseVideo};
  CHECK(load(prop({a, wrongUse})) == Parse::rejected && n == 0);
  CHECK(load(prop({wrongUse, a})) == Parse::rejected && n == 0);
  const pianoxram::Run wrapping{UINT64_MAX - kSlab + 1, 2 * kSlab, Region1, UseGpu};
  CHECK(load(prop({a, wrapping})) == Parse::rejected && n == 0);
  CHECK(load(prop({wrapping, a})) == Parse::rejected && n == 0);
  const auto mixed = prop({a, modem, pil});
  CHECK(loadRuns(mixed.data(), mixed.size(), 0x880000000ull, MiB, r, n) == Parse::rejected && n == 0);
  CHECK(loadRuns(mixed.data(), mixed.size(), a.base - kSlab, MiB, r, n) == Parse::rejected && n == 0);
  CHECK(loadRuns(mixed.data(), mixed.size(), a.base, UINT64_MAX, r, n) == Parse::rejected && n == 0);
  /* 2 */
  CHECK(!bootArgMode(nullptr)); CHECK(!bootArgMode("")); CHECK(!bootArgMode("piano-dpu=2 piano-gpu-user-vm=1"));
  CHECK(bootArgMode("piano-dpu=2 piano-gpu-tier=1") == 1); CHECK(!bootArgMode("piano-gpu-tier=0"));
  CHECK(!bootArgMode("piano-gpu-tier=1 piano-gpu-tier=0")); CHECK(bootArgMode("piano-gpu-tier=0 piano-gpu-tier=1") == 1);
  CHECK(!bootArgMode("piano-gpu-tier=11")); CHECK(!bootArgMode("piano-gpu-tiers=1")); CHECK(!bootArgMode("xpiano-gpu-tier=1"));
  CHECK(bootArgMode("piano-gpu-tier=1\tpiano-gpu-tier=2  ") == 2);
  CHECK(bootArgMode("piano-gpu-tier=2 piano-gpu-tier=") == 0);
  CHECK(bootArgMode("piano-gpu-tier=2 piano-gpu-tier=1") == 1);
  CHECK(!eligible(0, kSlab) && !eligible(1, kSlab - kPage) && eligible(1, kSlab) && eligible(2, kPage));
  CHECK(!eligible(3, UINT64_MAX) && bootArgMode("piano-gpu-tier=2 piano-gpu-tier=-1") == 0);
  const uint64_t maxSlabBytes = UINT64_MAX & ~(kSlab - 1);
  CHECK(slabBytes(0) == 0 && slabBytes(1) == kSlab && slabBytes(kSlab + 1) == 2 * kSlab);
  CHECK(slabBytes(maxSlabBytes) == maxSlabBytes && slabBytes(maxSlabBytes + 1) == 0 && slabBytes(UINT64_MAX) == 0);
  /* 3 */
  std::vector<Run> sm = {{0x880000000ull, 512 * MiB}, {0x8a0400000ull, 256 * MiB}, {0x8b0400000ull, 106 * MiB}};
  Ctx ctx; Tier t;
  const uint64_t limit = UINT64_C(1) << 36;
  const Run region1[] = {{a.base, a.size}, {b.base, b.size}, {c.base, c.size}};
  testFragmentation(region1, limit);
  t.init(sm.data(), 3, limit, probe, &ctx, line);
  CHECK(ctx.probed.size() == 6);
  for (unsigned i = 0; i < sm.size(); ++i) {
      CHECK(ctx.probed[2*i].base == sm[i].base && ctx.probed[2*i].size == i);
      CHECK(ctx.probed[2*i+1].base == sm[i].base + sm[i].size - kPage && ctx.probed[2*i+1].size == i);
  }
  CHECK(t.totalBytes() == 874 * MiB && t.freeBytes() == 874 * MiB && ctx.lines.size() == 3);
  CHECK(ctx.lines[0] == "gpu tier: run 0 0x880000000+512 MiB self-test ok");
  Span s[8]{};
  CHECK(t.alloc(1, s, 8, limit) == 1 && s[0].pa == 0x880000000ull && s[0].bytes == 2 * MiB);
  CHECK(t.freeBytes() == 872 * MiB);
  Span big[8]{};
  unsigned bn = t.alloc(600 * MiB, big, 8, limit);                     /* rest of run 0 (510 MiB) + 90 MiB of run 1 */
  CHECK(bn == 2 && big[0].pa == 0x880200000ull && big[0].bytes == 510 * MiB && big[1].pa == 0x8a0400000ull && big[1].bytes == 90 * MiB);
  struct Piece { uint64_t offset, physical, bytes; } pc[8]{};
  asPieces(big, bn, pc);
  CHECK(pc[0].offset == 0 && pc[0].physical == 0x880200000ull && pc[1].offset == 510 * MiB && pc[1].physical == 0x8a0400000ull && pc[1].bytes == 90 * MiB);
  CHECK(t.owns(0x8a0400000ull) && !t.owns(0x8c0000000ull) && !t.owns(0x87fe00000ull));
  Span one[1]; uint64_t f0 = t.freeBytes();
  CHECK(t.alloc(300 * MiB, one, 1, limit) == 0 && t.freeBytes() == f0);   /* needs 2 spans: refused whole */
  CHECK(t.alloc(t.freeBytes() + 1, s + 1, 7, limit) == 0 && t.freeBytes() == f0);                     /* too big: nothing taken */
  CHECK(t.alloc(UINT64_MAX, one, 1, limit) == 0 && t.freeBytes() == f0);
  CHECK(t.alloc(kSlab, one, 1, UINT64_C(1) << 32) == 0 && t.freeBytes() == f0);
  Span duplicate[] = {s[0], s[0]};
  CHECK(!t.release(duplicate, 2) && t.freeBytes() == f0);
  Span foreign = {0x8c0000000ull, 2 * MiB}, part = {0x880100000ull, 2 * MiB};
  CHECK(!t.release(&foreign, 1)); CHECK(!t.release(&part, 1));
  CHECK(t.release(big, bn) && !t.release(big, bn));             /* double free refused, nothing changed */
  CHECK(t.release(s, 1) && t.freeBytes() == 874 * MiB);
  srand(7); std::vector<std::vector<Span>> held; uint64_t heldBytes = 0;
  for (int round = 0; round < 2000; round++) {
    if (held.empty() || (rand() % 3)) {
      uint64_t want = (uint64_t)(1 + rand() % 64) * MiB; std::vector<Span> v(8);
      unsigned k = t.alloc(want, v.data(), 8, limit);
      if (k) { v.resize(k); uint64_t b = 0; for (auto &x : v) b += x.bytes; CHECK(b == (want + kSlab - 1) / kSlab * kSlab); heldBytes += b; held.push_back(v); }
    } else {
      size_t i = (size_t)rand() % held.size(); uint64_t b = 0; for (auto &x : held[i]) b += x.bytes;
      CHECK(t.release(held[i].data(), (unsigned)held[i].size())); heldBytes -= b; held.erase(held.begin() + (long)i);
    }
    CHECK(t.freeBytes() + heldBytes == 874 * MiB);
  }
  for (auto &v : held) CHECK(t.release(v.data(), (unsigned)v.size()));
  CHECK(t.freeBytes() == 874 * MiB);
  /* 4 */
  Ctx bad; bad.badProbe.push_back(0x8a0400000ull + 256 * MiB - kPage); Tier u;
  u.init(sm.data(), 3, limit, probe, &bad, line);
  CHECK(u.runPassed(0) && !u.runPassed(1) && u.runPassed(2));
  CHECK(bad.lines[1] == "gpu tier: run 1 0x8a0400000+256 MiB self-test FAIL" && u.totalBytes() == 618 * MiB);
  Span w[8]{}; unsigned wn = u.alloc(600 * MiB, w, 8, limit);
  CHECK(wn == 2 && w[0].pa == 0x880000000ull && w[1].pa == 0x8b0400000ull && !u.owns(0x8a0400000ull));
  CHECK(u.release(w, wn) && u.freeBytes() == u.totalBytes());
  const Span failedRun{sm[1].base, kSlab};
  CHECK(!u.release(&failedRun, 1) && !u.runPassed(3) && u.freeBytes() == u.totalBytes());
  Ctx failed;
  for (const auto &run : region1) { failed.badProbe.push_back(run.base); }
  u.init(region1, 3, limit, probe, &failed, line);
  CHECK(failed.probed.size() == 6 && failed.lines.size() == 3 && u.runs() == 3);
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(!u.runPassed(i) && !u.owns(region1[i].base) && failed.lines[i].find("FAIL") != std::string::npos);
  }
  CHECK(u.totalBytes() == 0 && u.freeBytes() == 0 && u.alloc(kSlab, w, 8, limit) == 0);
  Tier e;
  Ctx narrow;
  e.init(sm.data(), 3, UINT64_C(1) << 32, probe, &narrow, nullptr);
  CHECK(e.runs() == 3 && !e.runPassed(0) && !e.runPassed(2) && narrow.probed.empty());
  CHECK(e.alloc(kSlab, w, 8, limit) == 0);
  const Run overlapping[] = {{a.base, 2 * kSlab}, {a.base + kSlab, kSlab}};
  Ctx invalid;
  e.init(region1, 3, limit, probe, &narrow, nullptr);
  Span heldSlab{};
  CHECK(e.alloc(kSlab, &heldSlab, 1, limit) == 1 && heldSlab.pa == a.base);
  e.init(overlapping, 2, limit, probe, &invalid, line);
  CHECK(!e.runs() && !e.totalBytes() && !e.freeBytes() && invalid.probed.empty() && invalid.lines.empty());
  CHECK(!e.owns(heldSlab.pa) && !e.release(&heldSlab, 1) && e.alloc(kSlab, w, 8, limit) == 0);
  e.init(region1, 3, limit, probe, &narrow, nullptr);
  CHECK(e.freeBytes() == 870 * MiB && e.alloc(kSlab, w, 8, limit) == 1 && w[0].pa == heldSlab.pa);
  CHECK(e.release(w, 1) && e.freeBytes() == 870 * MiB);
  e.init(nullptr, 1, limit, probe, &invalid, line);
  CHECK(!e.runs() && invalid.probed.empty());
  e.init(region1, kMaxRuns + 1, limit, probe, &invalid, line);
  CHECK(!e.runs() && invalid.probed.empty());
  e.init(region1, 3, limit, nullptr, &invalid, line);
  CHECK(!e.runs() && invalid.lines.empty());
  if (fails) { std::fprintf(stderr, "%d of %d checks failed\n", fails, checks); return 1; }
  std::printf("PASS: gpu tier: %d checks for ADT parsing, modes 0/1/2, exact probe endpoints, failed runs, address widths, "
              "all-or-nothing slabs, reset and 2000 churn rounds\n", checks);
  return 0;
}
