/* SPDX-License-Identifier: MIT */
/* The region1 GPU tier (A830GpuTier.h, RAM16G R1; boot argument piano-gpu-tier, default off).
 *  1. /chosen piano-extra-ram parsing: both banks in one property, empty padding, malformed records and the XNU window.
 *  2. The off switch: modes 1 and 2; the last token wins; prefixes and other values stay off.
 *  3. The slab allocator: whole 2 MiB slabs, lowest first, spans inside one run, all-or-nothing refusals
 *     (span limit, size and address width), release refuses foreign / partial / double frees, exhaustion and refill over 2000 rounds.
 *  4. The per-run self-test: a run whose probe fails is kept out (its line says FAIL) and never handed out.
 * Build: clang++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined test_a830gputier.cpp -o /tmp/test_a830gputier && /tmp/test_a830gputier
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
static const uint64_t MiB = 1ull << 20;
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
  CHECK(load(prop({modem, pil, {modem.base, modem.size, Region0, UseGpu},
                  {a.base, a.size, Region1, UseAny}, {b.base, b.size, Region1, UseVideo}})) == Parse::ok && n == 0);
  CHECK(load(prop({{0x880001000ull, 4 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x880000000ull, 0, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x80000000ull, 2 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({{0x8bfe00000ull, 4 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  CHECK(load(prop({a, {0x880200000ull, 2 * MiB, Region1, UseGpu}})) == Parse::rejected && n == 0);
  const auto mixed = prop({a, modem, pil});
  CHECK(loadRuns(mixed.data(), mixed.size(), 0x880000000ull, MiB, r, n) == Parse::rejected && n == 0);
  /* 2 */
  CHECK(!bootArgMode(nullptr)); CHECK(!bootArgMode("")); CHECK(!bootArgMode("piano-dpu=2 piano-gpu-user-vm=1"));
  CHECK(bootArgMode("piano-dpu=2 piano-gpu-tier=1")); CHECK(!bootArgMode("piano-gpu-tier=0"));
  CHECK(!bootArgMode("piano-gpu-tier=1 piano-gpu-tier=0")); CHECK(bootArgMode("piano-gpu-tier=0 piano-gpu-tier=1"));
  CHECK(!bootArgMode("piano-gpu-tier=11")); CHECK(!bootArgMode("piano-gpu-tiers=1")); CHECK(!bootArgMode("xpiano-gpu-tier=1"));
  CHECK(bootArgMode("piano-gpu-tier=1\tpiano-gpu-tier=2  ") == 2);
  CHECK(bootArgMode("piano-gpu-tier=2 piano-gpu-tier=") == 0);
  CHECK(bootArgMode("piano-gpu-tier=2 piano-gpu-tier=1") == 1);
  CHECK(!eligible(0, kSlab) && !eligible(1, kSlab - kPage) && eligible(1, kSlab) && eligible(2, kPage));
  /* 3 */
  std::vector<Run> sm = {{0x880000000ull, 512 * MiB}, {0x8a0400000ull, 256 * MiB}, {0x8b0400000ull, 106 * MiB}};
  Ctx ctx; Tier t;
  const uint64_t limit = UINT64_C(1) << 36;
  t.init(sm.data(), 3, limit, probe, &ctx, line);
  CHECK(ctx.probed.size() == 6);
  for (unsigned i = 0; i < sm.size(); ++i) {
      CHECK(ctx.probed[2*i].base == sm[i].base && ctx.probed[2*i].size == i);
      CHECK(ctx.probed[2*i+1].base == sm[i].base + sm[i].size - kPage && ctx.probed[2*i+1].size == i);
  }
  CHECK(t.totalBytes() == 874 * MiB && t.freeBytes() == 874 * MiB && ctx.lines.size() == 3);
  CHECK(ctx.lines[0] == "gpu tier: run 0 0x880000000+512 MiB self-test ok");
  Span s[8];
  CHECK(t.alloc(1, s, 8, limit) == 1 && s[0].pa == 0x880000000ull && s[0].bytes == 2 * MiB);
  CHECK(t.freeBytes() == 872 * MiB);
  Span big[8];
  unsigned bn = t.alloc(600 * MiB, big, 8, limit);                     /* rest of run 0 (510 MiB) + 90 MiB of run 1 */
  CHECK(bn == 2 && big[0].pa == 0x880200000ull && big[0].bytes == 510 * MiB && big[1].pa == 0x8a0400000ull && big[1].bytes == 90 * MiB);
  struct Piece { uint64_t offset, physical, bytes; } pc[2];
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
  Span w[8]; unsigned wn = u.alloc(600 * MiB, w, 8, limit);
  CHECK(wn == 2 && w[0].pa == 0x880000000ull && w[1].pa == 0x8b0400000ull && !u.owns(0x8a0400000ull));
  Tier e;
  Ctx narrow;
  e.init(sm.data(), 3, UINT64_C(1) << 32, probe, &narrow, nullptr);
  CHECK(e.runs() == 3 && !e.runPassed(0) && !e.runPassed(2) && narrow.probed.empty());
  CHECK(e.alloc(kSlab, w, 8, limit) == 0);
  if (fails) { std::fprintf(stderr, "%d of %d checks failed\n", fails, checks); return 1; }
  std::printf("PASS: gpu tier: %d checks for ADT parsing, modes 0/1/2, exact probe endpoints, failed runs, address widths, "
              "all-or-nothing slabs and 2000 churn rounds\n", checks);
  return 0;
}
