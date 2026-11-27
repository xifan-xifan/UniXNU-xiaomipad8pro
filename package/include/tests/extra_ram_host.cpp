// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoExtraRam.h (docs/extra-ram.md): ADT parsing, the allow/never/window checks, ownership by use tag, slab
// hand-out, the self-test against a simulated RAM with aliases and bit errors, the device hook and the report.
#include "PianoExtraRam.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianoxram;

static int failures = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                                \
    }                                                                            \
  } while (0)

static void put64(std::vector<uint8_t> &v, uint64_t x)
{
  for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static void put32(std::vector<uint8_t> &v, uint32_t x)
{
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static std::vector<uint8_t> adt(std::initializer_list<Run> runs)
{
  std::vector<uint8_t> v;
  for (const Run &r : runs) {
    put64(v, r.base);
    put64(v, r.size);
    put32(v, r.bank);
    put32(v, r.use);
  }
  return v;
}

// XNU's window as the installed system has it.
static Config cfg(uint32_t use, uint32_t any = UseNone, uint32_t stage = 1)
{
  Config c;
  c.use = use;
  c.anyOwner = any;
  c.stage = stage;
  c.xnuBase = 0x8c0004000ull;
  c.xnuSize = 0x33fe00000ull;
  return c;
}

struct FakeRam {
  std::map<uint64_t, std::vector<uint64_t>> slabs;
  std::map<uint64_t, uint64_t> alias;   // slab -> the slab whose memory it really is
  uint64_t flipPa = 0;                  // at barrier flipAt (the end of pass 1), flip a bit of the word at this PA (once)
  int flipAt = 0;
  int barriers = 0, maps = 0, unmaps = 0, devices = 0;
  int deviceAnswer = -1;
  uint64_t mapFailPa = 0;
  std::vector<uint64_t> &backing(uint64_t pa)
  {
    uint64_t s = pa & ~(kSlab - 1);
    if (alias.count(s)) s = alias[s];
    auto &b = slabs[s];
    if (b.empty()) b.assign(kSlab / 8, 0xdeadbeefdeadbeefull);
    return b;
  }
  volatile uint64_t *map(uint64_t pa, uint64_t bytes)
  {
    ++maps;
    if (bytes != kSlab || (pa & (kSlab - 1)) || pa == mapFailPa) return nullptr;
    return backing(pa).data();
  }
  void unmap(volatile uint64_t *, uint64_t) { ++unmaps; }
  void barrier()
  {
    ++barriers;
    if (flipPa && barriers == flipAt) {
      backing(flipPa)[(flipPa & (kSlab - 1)) / 8] ^= 1;
      flipPa = 0;
    }
  }
  int device(uint64_t, uint64_t)
  {
    ++devices;
    return deviceAnswer;
  }
};

static std::string text(const Pool &p)
{
  char buf[4096];
  const size_t n = p.report(buf, sizeof buf, 1, 1);
  return std::string(buf, n);
}

int main()
{
  // The tables: allowed spans inside their banks, ordered, disjoint from every never span; 2 MiB-aligned region0 spans.
  for (const Span &a : kAllowed) {
    CHECK(a.base < a.end);
    if (a.bank == Region0) {
      CHECK(a.base >= 0x80000000ull && a.end <= 0x100000000ull);
      CHECK(a.base % kSlab == 0 && a.end % kSlab == 0);
    } else {
      CHECK(a.base >= 0x880000000ull && a.end <= 0x8c0000000ull);
    }
    for (const Span &n : kNever) CHECK(!overlaps(a.base, a.end, n.base, n.end));
  }
  // Never-use carve-outs the brief names: qdss, splash, Display_Demura, Piano_Log, cvp, cdsp, video.
  const uint64_t mustRefuse[] = {0x82800000, 0xfc800000, 0xa3500000, 0xd7f00000, 0x9cb00000, 0x9d200000, 0x9c300000, 0xf3800000};
  for (uint64_t pa : mustRefuse) {
    bool hit = false;
    for (const Span &n : kNever) hit |= pa >= n.base && pa < n.end;
    CHECK(hit);
  }

  // No property, a bad length.
  {
    Pool p;
    CHECK(p.load(nullptr, 0, cfg(UseVideo)) == 0 && !p.present);
    CHECK(text(p).find("adt=absent") != std::string::npos);
    const uint8_t zero[kMaxRuns * kRecordBytes] = {};
    CHECK(p.load(zero, sizeof zero, cfg(UseVideo)) == 0 && p.present && !p.formatBad);
    CHECK(p.records == 0 && p.count == 0 && p.dropped == 0);
    const uint8_t old[136] = {};
    CHECK(p.load(old, sizeof old, cfg(UseVideo)) == 0 && p.formatBad);
    const uint8_t junk[25] = {};
    CHECK(p.load(junk, sizeof junk, cfg(UseVideo)) == 0 && p.formatBad);
    CHECK(text(p).find("adt=bad-length") != std::string::npos);
  }

  {
    Pool p;
    const auto v = adt({{kAllowed[0].base, kSlab, kAllowed[0].bank, kAllowed[0].use}});
    Config c = cfg(kAllowed[0].use);
    c.xnuBase = kAllowed[0].base;
    c.xnuSize = UINT64_MAX;
    CHECK(p.load(v.data(), v.size(), c) == 0 && p.runs[0].refusal == RefWindow);
  }

  // Classification of every refusal class, and ownership by tag.
  {
    auto v = adt({
        {0x89e00000, 0x11200000, Region0, UseVideo},          // 0 the modem run: ours
        {0x9ee00000, 0x4600000, Region0, UseDsp},             // 1 pil-tail: another kext's
        {0x9c300000, 0x800000, Region0, UseVideo},            // 2 the video carve-out: never
        {0xa0000000, 0x4000000, Region0, UseVideo},           // 3 crosses into ramoops/Demura: never
        {0x90000000, 0x200000, 2, UseVideo},                  // 4 bad bank
        {0x880000000, 0x10000000, Region1, UseGpu},           // 5 region1: R1's
        {0x8c0000000, 0x200000, Region1, UseGpu},             // 6 XNU's window / region1 hole: refused
        {0x89e00000, 0x200000, Region0, UseVideo},            // 7 overlaps run 0
        {0xe3800000, 0x10000000, Region0, UseAny},            // 8 stage 2: refused at stage 1
        {0x9a000000, 0x100000, Region0, UseVideo},            // 9 inside run 0: overlap
        {0x89e00000 + 0x11200000 - 0x200000, 0x400000, Region0, UseVideo},   // 10 runs past the span into a never span
        {0x60000000, 0x200000, Region0, UseVideo},            // 11 not in DRAM's allow-list
    });
    Pool p;
    v.resize(kMaxRuns * kRecordBytes);
    CHECK(p.load(v.data(), v.size(), cfg(UseVideo)) == 1);
    CHECK(p.count == 12 && p.records == 12);
    CHECK(p.runs[0].state == StateOurs && p.runs[0].slabs == 137 && p.runs[0].first == 0x89e00000ull);
    CHECK(p.runs[1].state == StateOtherUse && p.runs[1].slabs == 35);
    CHECK(p.runs[2].state == StateRefused && p.runs[2].refusal == RefNever);
    CHECK(p.runs[3].state == StateRefused && p.runs[3].refusal == RefNever);
    CHECK(p.runs[4].refusal == RefBank);
    CHECK(p.runs[5].state == StateOtherUse && p.runs[5].slabs == 128);
    CHECK(p.runs[6].state == StateRefused && p.runs[6].refusal == RefOutside);
    CHECK(p.runs[7].refusal == RefOverlap);
    CHECK(p.runs[8].refusal == RefStage);
    CHECK(p.runs[9].refusal == RefOverlap);
    const auto tiny = adt({{0x89e80000, 0x100000, Region0, UseVideo}});
    Pool small;
    CHECK(small.load(tiny.data(), tiny.size(), cfg(UseVideo)) == 0 && small.runs[0].refusal == RefSmall);
    CHECK(p.runs[10].refusal == RefNever);
    CHECK(p.runs[11].refusal == RefOutside);
    CHECK(p.refusals[RefNever] == 3 && p.refusals[RefStage] == 1);
    // Before the self-test nothing is handed out.
    uint64_t out[4];
    CHECK(!p.take(1, false, out) && p.takeFails == 1);
    // Stage 2 and the any-owner: the CMA block goes to the use named by piano-extra-ram-any only.
    Pool q;
    CHECK(q.load(v.data(), v.size(), cfg(UseVideo, UseVideo, 2)) == 2 && q.runs[8].state == StateOurs && q.runs[8].slabs == 128);
    Pool r;
    CHECK(r.load(v.data(), v.size(), cfg(UseCamera, UseVideo, 2)) == 0 && r.runs[8].state == StateOtherUse);
    // A window that covers the run (a RAM image with a different window): refused.
    Config w = cfg(UseVideo);
    w.xnuBase = 0x80000000ull;
    w.xnuSize = 0x40000000ull;
    Pool x;
    CHECK(x.load(v.data(), v.size(), w) == 0 && x.runs[0].refusal == RefWindow);
    // More records than kept.
    std::vector<uint8_t> many;
    for (unsigned i = 0; i < kMaxRuns + 3; ++i) {
      const auto one = adt({{0x89e00000ull + i * kSlab, kSlab, Region0, UseVideo}});
      many.insert(many.end(), one.begin(), one.end());
    }
    Pool m;
    CHECK(m.load(many.data(), many.size(), cfg(UseVideo)) == kMaxRuns && m.dropped == 3);
  }

  // Even a valid physical span cannot be assigned to a different use in the ADT.
  {
    const auto swapped = adt({{0x89e00000, 0x11200000, Region0, UseGpu},
                              {0x9ee00000, 0x4600000, Region0, UseVideo},
                              {0x880000000, kSlab, Region1, UseAny}});
    Pool p;
    CHECK(p.load(swapped.data(), swapped.size(), cfg(UseVideo, UseVideo)) == 0);
    CHECK(p.refusals[RefUse] == 3);
  }

  // Self-test: a clean run passes; slabs out, back, contiguity; holds().
  {
    const auto v = adt({{0x89e00000, 0x11200000, Region0, UseVideo}, {0xe3800000, 0x4600000, Region0, UseAny}});
    Pool p;
    CHECK(p.load(v.data(), v.size(), cfg(UseVideo, UseVideo, 2)) == 2);
    FakeRam ram;
    CHECK(p.selfTest(ram) == 2);
    CHECK(p.runs[0].tested == 1 && p.runs[0].testedPages == 137 * 512 && p.runs[0].badWords == 0 && p.runs[0].device == -1);
    CHECK(ram.maps == ram.unmaps && ram.maps == 2 * (137 + 35) && ram.devices == 2);
    // Zeroed after the test.
    for (auto &kv : ram.slabs)
      for (uint64_t w : kv.second) CHECK(w == 0);
    CHECK(p.freeSlabs() == 172);
    // The AVC arena: 128 slabs, not contiguous.
    uint64_t arena[256];
    CHECK(p.take(128, false, arena));
    CHECK(arena[0] == 0x89e00000ull && arena[127] == 0x89e00000ull + 127 * kSlab);
    CHECK(p.usedSlabs() == 128 && p.freeSlabs() == 44);
    CHECK(p.holds(arena[5] + 0x1000) && !p.holds(0x89e00000ull + 130 * kSlab) && !p.holds(0x8c0000000ull));
    // 44 left: 9 in run 0, 35 in run 1. Contiguous 10 must come from run 1; 36 fails; non-contiguous 44 spans both.
    uint64_t c[64];
    CHECK(p.take(10, true, c) && c[0] == 0xe3800000ull && c[9] == 0xe3800000ull + 9 * kSlab);
    CHECK(!p.take(30, true, c));
    CHECK(!p.take(35, false, c));   // only 34 left
    CHECK(p.give(0xe3800000ull + 3 * kSlab) && !p.give(0xe3800000ull + 3 * kSlab) && !p.give(0xe3800000ull + 0x1000));
    CHECK(!p.give(0x8c0000000ull) && p.giveBad == 3);
    for (unsigned i = 0; i < 128; ++i) CHECK(p.give(arena[i]));
    CHECK(p.usedSlabs() == 9);
    const std::string t = text(p);
    CHECK(t.find("run0 base=0x89e00000 size=0x11200000 bank=0 use=video state=ours span=modem slabs=137 used=0 free=137 "
                 "self-test=pass pages=70144 bad=0 device=none") != std::string::npos);
    CHECK(t.find("take-fails=2") != std::string::npos);
  }

  // Self-test failures: an alias between two slabs, one flipped bit, a CPU window that does not map, a device check that fails.
  {
    const auto v = adt({{0x89e00000, 0x800000, Region0, UseVideo}});
    Pool p;
    p.load(v.data(), v.size(), cfg(UseVideo));
    FakeRam alias;
    alias.alias[0x89e00000ull + 2 * kSlab] = 0x89e00000ull;   // slab 2 is slab 0's memory
    CHECK(p.selfTest(alias) == 0 && p.runs[0].state == StateRefused && p.runs[0].refusal == RefSelfTest);
    CHECK(p.runs[0].firstBad == 0x89e00000ull && p.runs[0].badWords > 0);
    uint64_t out[1];
    CHECK(!p.take(1, false, out) && p.freeSlabs() == 0);
    CHECK(text(p).find("self-test=FAIL") != std::string::npos);

    Pool q;
    q.load(v.data(), v.size(), cfg(UseVideo));
    FakeRam flip;
    flip.flipPa = 0x89e00000ull + kSlab + 255 * 8;
    flip.flipAt = 4;   // the run's 4 slabs written
    CHECK(q.selfTest(flip) == 0 && q.runs[0].badWords == 1 && q.runs[0].firstBad == 0x89e00000ull + kSlab + 255 * 8);

    Pool r;
    r.load(v.data(), v.size(), cfg(UseVideo));
    FakeRam nomap;
    nomap.mapFailPa = 0x89e00000ull + 3 * kSlab;
    CHECK(r.selfTest(nomap) == 0 && r.runs[0].refusal == RefSelfTest && nomap.maps == nomap.unmaps + 1);

    Pool s;
    s.load(v.data(), v.size(), cfg(UseVideo));
    FakeRam dev;
    dev.deviceAnswer = 0;
    CHECK(s.selfTest(dev) == 0 && s.runs[0].refusal == RefDevice && s.runs[0].tested == 1);
    CHECK(text(s).find("device=FAIL") != std::string::npos);
    Pool t;
    t.load(v.data(), v.size(), cfg(UseVideo));
    FakeRam devOk;
    devOk.deviceAnswer = 1;
    CHECK(t.selfTest(devOk) == 1 && t.runs[0].device == 1);
  }

  // The report truncates safely.
  {
    const auto v = adt({{0x89e00000, 0x11200000, Region0, UseVideo}});
    Pool p;
    p.load(v.data(), v.size(), cfg(UseVideo));
    char small[40];
    const size_t n = p.report(small, sizeof small, 1, 1);
    CHECK(n == sizeof small - 1 && std::strlen(small) == n && std::strcmp(small + n - 3, "...") == 0);
  }

  if (failures) {
    std::fprintf(stderr, "extra_ram_host: %d failures\n", failures);
    return 1;
  }
  std::printf("extra_ram_host: ok\n");
  return 0;
}
