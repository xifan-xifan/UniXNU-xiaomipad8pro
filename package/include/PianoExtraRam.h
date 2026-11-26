// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// RAM16G (docs/extra-ram.md): driver pools in HLOS memory XNU does not manage. DarwinBoot reserves the runs and publishes them in the
// ADT as /chosen/piano-extra-ram, an array of 24-byte little-endian records {base u64, size u64, bank u32, use u32}:
//   bank 1 = region1 below XNU's window (0x880000000..0x8b7500000, R1: the GPU tier), bank 0 = region0, the 2 GiB bank at 0x80000000
//   (R3: firmware/patches/darwinboot/region0-allow.json); use 0 any driver pool, 1 GPU, 2 DSP, 3 video, 4 camera, 5 display.
// An all-zero record is padding. An older FD leaves the whole property zeroed.
// This header is the portable half (no IOKit; host tests: tests/extra_ram_host.cpp): it parses the records, checks every run against
// the allow-list and the never-use list compiled in below (the JSON's mirror: tests/region0_allow_check.py keeps them equal) and
// against XNU's window, hands out 2 MiB slabs per use tag, runs a per-run self-test through a caller-supplied CPU window, and writes
// the report machdep.piano_extra_ram_<use> shows. The kernel half is PianoExtraRamKernel.h.
//
// One owner per run without a kernel-wide word: every kext that includes this parses the same ADT, but a run is served only to the
// kext whose use tag equals the run's tag; use-0 ("any") runs only to the one use named by the boot argument piano-extra-ram-any
// (default: nobody). So two kexts never take the same slab, and the header needs no state in another kext.
#ifndef PIANO_EXTRA_RAM_H
#define PIANO_EXTRA_RAM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>   // vsnprintf
#else
#include <stdio.h>
#endif

namespace pianoxram {

constexpr uint64_t kSlab = 2ull << 20;
constexpr unsigned kMaxRuns = 16;          // records read from the ADT (more: counted as refused)
constexpr unsigned kMaxSlabs = 512;        // per run: 1 GiB
constexpr size_t kRecordBytes = 24;
constexpr uint64_t kPattern = 0x5049414e4f58524dull;   // "PIANOXRM"

enum Use : uint32_t { UseAny = 0, UseGpu = 1, UseDsp = 2, UseVideo = 3, UseCamera = 4, UseDisplay = 5, UseCount = 6,
                      UseNone = 0xffffffffu };
enum Bank : uint32_t { Region0 = 0, Region1 = 1 };

inline const char *useName(uint32_t u)
{
  switch (u) {
  case UseAny: return "any";
  case UseGpu: return "gpu";
  case UseDsp: return "dsp";
  case UseVideo: return "video";
  case UseCamera: return "camera";
  case UseDisplay: return "display";
  case UseNone: return "none";
  default: return "?";
  }
}

struct Run {
  uint64_t base, size;
  uint32_t bank, use;
};

struct Span {
  uint64_t base, end;
  uint32_t bank, use, stage;
  const char *name;
};

// Where a published run may lie. Region0: region0-allow.json "allow" (exact). Region1: the three HLOS runs below the window (ABL
// /memory, docs/hyp-evidence/memory-map-running-dt.txt; RAM-WINDOW.md 1.2), R1's GPU tier.
inline constexpr Span kAllowed[] = {
    {0x89e00000ull, 0x9b000000ull, Region0, UseVideo, 1, "modem"},
    {0x9ee00000ull, 0xa3400000ull, Region0, UseDsp, 1, "pil-tail"},
    {0xe3800000ull, 0xf3800000ull, Region0, UseAny, 2, "cma-block"},
    {0x880000000ull, 0x8af6fd000ull, Region1, UseGpu, 1, "region1-a"},
    {0x8b0000000ull, 0x8b5500000ull, Region1, UseGpu, 1, "region1-b"},
    {0x8b5700000ull, 0x8b7500000ull, Region1, UseGpu, 1, "region1-c"},
};

// Never, whatever the ADT says (region0-allow.json "never", plus region1's undeclared holes). A run touching one is refused whole.
inline constexpr Span kNever[] = {
    {0x80000000ull, 0x82800000ull, Region0, UseNone, 0, "gunyah/cpucp/aop/smem/pdp/scandump/pvmfw/global_sync/tz_stat"},
    {0x82800000ull, 0x84a00000ull, Region0, UseNone, 0, "qdss"},
    {0x84a00000ull, 0x89d80000ull, Region0, UseNone, 0, "dsm_partition_1/2"},
    {0x9b000000ull, 0x9ee00000ull, Region0, UseNone, 0, "subsystem carve-outs"},
    {0xa3400000ull, 0xa7000000ull, Region0, UseNone, 0, "ramoops + Display_Demura"},
    {0xa7000000ull, 0xb8000000ull, Region0, UseNone, 0, "UEFI FD + Kernel"},
    {0xb8000000ull, 0xbd980000ull, Region0, UseNone, 0, "xbl_ramdump + adspslpi"},
    {0xbd930000ull, 0xd8000000ull, Region0, UseNone, 0, "DXE_Heap + hwfence + Piano_Log"},
    {0xd8000000ull, 0xe3720000ull, Region0, UseNone, 0, "tz_merged + undeclared hole"},
    {0xf3800000ull, 0xf8800000ull, Region0, UseNone, 0, "trust_ui_vm"},
    {0xf8800000ull, 0xfc400000ull, Region0, UseNone, 0, "oem_vm + vm-shared"},
    {0xfc400000ull, 0x100000000ull, Region0, UseNone, 0, "splash + cdsp_eva + llcc_lpi"},
    {0x8af6fd000ull, 0x8b0000000ull, Region1, UseNone, 0, "region1 hole 9 MiB"},
    {0x8b5500000ull, 0x8b5700000ull, Region1, UseNone, 0, "region1 hole 2 MiB"},
    {0x8b7500000ull, 0x8c0000000ull, Region1, UseNone, 0, "region1 hole 139 MiB"},
};

enum State : uint32_t { StateOurs = 1, StateOtherUse = 2, StateRefused = 3 };
enum Refusal : uint32_t {
  RefNone = 0,
  RefBank,       // bank not 0 or 1
  RefOutside,    // not inside one allowed span of its bank
  RefNever,      // touches a never-use span
  RefWindow,     // overlaps XNU's window (gPhysBase..+gPhysSize)
  RefOverlap,    // overlaps an earlier run
  RefSmall,      // no whole 2 MiB slab inside, or more than kMaxSlabs
  RefStage,      // its allowed span's stage is above piano-extra-ram-stage
  RefUse,        // use does not match the allowed span
  RefSelfTest,   // the CPU self-test failed
  RefDevice,     // the driver's device-side check failed
  RefCount
};
inline const char *refusalName(uint32_t r)
{
  static const char *const names[RefCount] = {"none", "bank", "outside-allow-list", "never-use", "xnu-window", "overlap",
                                              "no-slab", "stage", "use", "self-test", "device-check"};
  return r < RefCount ? names[r] : "?";
}

struct RunState {
  Run run{};
  uint64_t first = 0;   // first slab (2 MiB aligned)
  uint32_t slabs = 0, used = 0;
  uint64_t bits[kMaxSlabs / 64] = {};
  uint32_t state = 0, refusal = RefNone;
  const char *span = nullptr;   // the allowed span it lies in, or the never span it touched
  uint32_t tested = 0;          // 0 not run, 1 pass, 2 fail
  uint32_t testedPages = 0, badWords = 0;
  uint64_t firstBad = 0;
  int32_t device = -1;          // -1 not run / no hook, 0 fail, 1 pass
  bool taken(unsigned i) const { return (bits[i / 64] >> (i % 64)) & 1; }
  void mark(unsigned i, bool on)
  {
    if (on) bits[i / 64] |= 1ull << (i % 64);
    else bits[i / 64] &= ~(1ull << (i % 64));
  }
  uint64_t slab(unsigned i) const { return first + uint64_t(i) * kSlab; }
};

inline uint64_t rd64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
inline uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

inline bool overlaps(uint64_t a, uint64_t ae, uint64_t b, uint64_t be) { return a < be && b < ae; }

// The pattern a word at physical address pa holds during the self-test: distinct per address, so an alias between two slabs or two
// pages (one written after the other) reads back wrong.
inline uint64_t patternAt(uint64_t pa) { return (pa * 0x9e3779b97f4a7c15ull) ^ kPattern; }

struct Config {
  uint32_t use = UseNone;        // the caller's tag
  uint32_t anyOwner = UseNone;   // piano-extra-ram-any: the one use served the use-0 runs
  uint32_t stage = 1;            // piano-extra-ram-stage: the highest allowed-span stage served
  uint64_t xnuBase = 0, xnuSize = 0;
};

class Pool {
 public:
  Config cfg;
  RunState runs[kMaxRuns];
  uint32_t count = 0;            // records kept (<= kMaxRuns)
  uint32_t records = 0;          // nonempty records in the property
  bool present = false, formatBad = false;
  uint32_t refusals[RefCount] = {};
  uint32_t takes = 0, takeFails = 0, gives = 0, giveBad = 0, dropped = 0;

  // Parses /chosen/piano-extra-ram (bytes may be null: no property). Returns the number of runs this caller owns.
  unsigned load(const uint8_t *prop, size_t bytes, const Config &c)
  {
    reset();   // in place: no Pool-sized temporary on a kernel stack
    cfg = c;
    present = prop != nullptr;
    if (!prop) return 0;
    if (bytes % kRecordBytes) {
      formatBad = true;
      return 0;
    }
    unsigned owned = 0;
    for (size_t i = 0; i < bytes; i += kRecordBytes) {
      const uint8_t *r = prop + i;
      const Run run{rd64(r), rd64(r + 8), rd32(r + 16), rd32(r + 20)};
      if (!run.base && !run.size && !run.bank && !run.use) continue;
      ++records;
      if (count == kMaxRuns) {
        ++dropped;
        continue;
      }
      RunState &s = runs[count++];
      s.run = run;
      classify(s, count - 1);
      if (s.state == StateRefused) ++refusals[s.refusal];
      if (s.state == StateOurs) ++owned;
    }
    return owned;
  }

  // The self-test of every owned run, through a CPU window the caller supplies (Ops: volatile uint64_t *map(pa, bytes),
  // void unmap(volatile uint64_t *, bytes), void barrier(), int device(pa, bytes): -1 no check, 0 fail, 1 pass).
  // Pass 1 writes the address pattern into three words of every 4 KiB page of every slab; pass 2 reads them back, then zeroes the
  // slab and reads the zeros. Two passes over the whole run, so an alias inside the run shows. A failed run is refused (no slab of it
  // is ever handed out). Returns the runs that passed.
  template <class Ops> unsigned selfTest(Ops &ops)
  {
    unsigned passed = 0;
    for (uint32_t r = 0; r < count; ++r) {
      RunState &s = runs[r];
      if (s.state != StateOurs) continue;
      bool ok = true;
      s.testedPages = 0;
      s.badWords = 0;
      for (unsigned pass = 0; pass < 2 && ok; ++pass)
        for (uint32_t i = 0; i < s.slabs && ok; ++i) {
          const uint64_t pa = s.slab(i);
          volatile uint64_t *v = ops.map(pa, kSlab);
          if (!v) { ok = false; s.firstBad = pa; break; }
          constexpr unsigned words = unsigned(kSlab / 8), pageWords = 512;
          const unsigned probes[3] = {0, 255, 511};
          if (!pass) {
            for (unsigned w = 0; w < words; w += pageWords)
              for (unsigned p : probes) v[w + p] = patternAt(pa + 8ull * (w + p));
          } else {
            for (unsigned w = 0; w < words; w += pageWords) {
              ++s.testedPages;
              for (unsigned p : probes)
                if (v[w + p] != patternAt(pa + 8ull * (w + p))) {
                  if (!s.badWords++) s.firstBad = pa + 8ull * (w + p);
                }
            }
            for (unsigned w = 0; w < words; ++w) v[w] = 0;
            ops.barrier();
            for (unsigned w = 0; w < words; w += pageWords)
              for (unsigned p : probes)
                if (v[w + p]) {
                  if (!s.badWords++) s.firstBad = pa + 8ull * (w + p);
                }
            ok = !s.badWords;
          }
          ops.barrier();
          ops.unmap(v, kSlab);
        }
      s.tested = ok ? 1 : 2;
      if (ok) {
        s.device = ops.device(s.first, uint64_t(s.slabs) * kSlab);
        if (!s.device) refuse(s, RefDevice);
      } else {
        refuse(s, RefSelfTest);
      }
      if (s.state == StateOurs) ++passed;
    }
    return passed;
  }

  // n slabs, all or nothing; contiguous: one physically contiguous range in one run (out[0] its base, out[1..] the following slabs).
  bool take(unsigned n, bool contiguous, uint64_t *out)
  {
    if (!n || !out || n > freeSlabs()) return fail();
    if (contiguous) {
      for (uint32_t r = 0; r < count; ++r) {
        RunState &s = runs[r];
        if (s.state != StateOurs || s.tested != 1) continue;
        for (uint32_t i = 0; i + n <= s.slabs; ++i) {
          unsigned k = 0;
          while (k < n && !s.taken(i + k)) ++k;
          if (k == n) {
            for (k = 0; k < n; ++k) {
              s.mark(i + k, true);
              out[k] = s.slab(i + k);
            }
            s.used += n;
            ++takes;
            return true;
          }
          i += k;
        }
      }
      return fail();
    }
    unsigned got = 0;
    for (uint32_t r = 0; r < count && got < n; ++r) {
      RunState &s = runs[r];
      if (s.state != StateOurs || s.tested != 1) continue;
      for (uint32_t i = 0; i < s.slabs && got < n; ++i)
        if (!s.taken(i)) {
          s.mark(i, true);
          ++s.used;
          out[got++] = s.slab(i);
        }
    }
    ++takes;
    return true;
  }

  bool give(uint64_t slabPa)
  {
    for (uint32_t r = 0; r < count; ++r) {
      RunState &s = runs[r];
      if (s.state != StateOurs || slabPa < s.first || slabPa >= s.first + uint64_t(s.slabs) * kSlab) continue;
      const uint64_t off = slabPa - s.first;
      const unsigned i = unsigned(off / kSlab);
      if (off % kSlab || !s.taken(i)) break;
      s.mark(i, false);
      --s.used;
      ++gives;
      return true;
    }
    ++giveBad;
    return false;
  }

  // A 4 KiB page lies in a slab this caller holds now.
  bool holds(uint64_t pa) const
  {
    for (uint32_t r = 0; r < count; ++r) {
      const RunState &s = runs[r];
      if (s.state != StateOurs || pa < s.first || pa >= s.first + uint64_t(s.slabs) * kSlab) continue;
      return s.taken(unsigned((pa - s.first) / kSlab));
    }
    return false;
  }

  uint32_t freeSlabs() const
  {
    uint32_t n = 0;
    for (uint32_t r = 0; r < count; ++r)
      if (runs[r].state == StateOurs && runs[r].tested == 1) n += runs[r].slabs - runs[r].used;
    return n;
  }
  uint32_t usedSlabs() const
  {
    uint32_t n = 0;
    for (uint32_t r = 0; r < count; ++r)
      if (runs[r].state == StateOurs) n += runs[r].used;
    return n;
  }

  // The text of machdep.piano_extra_ram_<use> (no trailing NUL counted). Truncated output ends with "...".
  size_t report(char *out, size_t cap, uint32_t master, uint32_t driver) const
  {
    Writer w{out, cap, 0};
    w.put("piano-extra-ram=%u driver=%u use=%s any-owner=%s stage<=%u adt=%s records=%u kept=%u dropped=%u\n", master, driver,
          useName(cfg.use), useName(cfg.anyOwner), cfg.stage, !present ? "absent" : formatBad ? "bad-length" : "ok", records, count,
          dropped);
    for (uint32_t r = 0; r < count; ++r) {
      const RunState &s = runs[r];
      w.put("run%u base=%#llx size=%#llx bank=%u use=%s state=%s", r, (unsigned long long)s.run.base, (unsigned long long)s.run.size,
            s.run.bank, useName(s.run.use),
            s.state == StateOurs ? "ours" : s.state == StateOtherUse ? "other-use" : "refused");
      if (s.span) w.put(" span=%s", s.span);
      if (s.state == StateRefused) w.put(" why=%s", refusalName(s.refusal));
      if (s.slabs) w.put(" slabs=%u used=%u free=%u", s.slabs, s.used, s.slabs - s.used);
      if (s.tested) w.put(" self-test=%s pages=%u bad=%u", s.tested == 1 ? "pass" : "FAIL", s.testedPages, s.badWords);
      if (s.badWords) w.put(" first-bad=%#llx", (unsigned long long)s.firstBad);
      if (s.tested == 1 || s.refusal == RefDevice) w.put(" device=%s", s.device < 0 ? "none" : s.device ? "pass" : "FAIL");
      w.put("\n");
    }
    w.put("free=%u used=%u slabs (2 MiB) takes=%u take-fails=%u gives=%u give-bad=%u refusals", freeSlabs(), usedSlabs(), takes,
          takeFails, gives, giveBad);
    for (uint32_t i = 1; i < RefCount; ++i) w.put(" %s=%u", refusalName(i), refusals[i]);
    w.put("\n");
    return w.used;
  }

 private:
  struct Writer {
    char *out;
    size_t cap, used;
    void put(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
      if (!out || !cap || used + 1 >= cap) return;
      va_list ap;
      va_start(ap, fmt);
      const int n = vsnprintf(out + used, cap - used, fmt, ap);
      va_end(ap);
      if (n < 0) return;
      if (used + size_t(n) >= cap) {
        used = cap - 1;
        if (cap > 4) { out[cap - 4] = '.'; out[cap - 3] = '.'; out[cap - 2] = '.'; }
        out[cap - 1] = 0;
      } else {
        used += size_t(n);
      }
    }
  };

  void reset()
  {
    for (RunState &s : runs) s = RunState();
    count = records = 0;
    present = formatBad = false;
    for (uint32_t &r : refusals) r = 0;
    takes = takeFails = gives = giveBad = dropped = 0;
  }
  bool fail()
  {
    ++takeFails;
    return false;
  }
  void refuse(RunState &s, uint32_t why)
  {
    s.state = StateRefused;
    s.refusal = why;
    ++refusals[why];
  }

  void classify(RunState &s, uint32_t index)
  {
    const Run &r = s.run;
    const uint64_t end = r.base + r.size;
    auto refused = [&](uint32_t why) {
      s.state = StateRefused;
      s.refusal = why;
    };
    if (r.bank > Region1) return refused(RefBank);
    if (!r.size || end < r.base) return refused(RefOutside);
    for (const Span &n : kNever)
      if (overlaps(r.base, end, n.base, n.end)) {
        s.span = n.name;
        return refused(RefNever);
      }
    const Span *in = nullptr;
    for (const Span &a : kAllowed)
      if (a.bank == r.bank && r.base >= a.base && end <= a.end) in = &a;
    if (!in) return refused(RefOutside);
    s.span = in->name;
    if (r.use != in->use) return refused(RefUse);
    if (cfg.xnuSize && overlaps(r.base, end, cfg.xnuBase, cfg.xnuBase + cfg.xnuSize)) return refused(RefWindow);
    for (uint32_t j = 0; j < index; ++j)
      if (overlaps(r.base, end, runs[j].run.base, runs[j].run.base + runs[j].run.size)) return refused(RefOverlap);
    if (in->stage > cfg.stage) return refused(RefStage);
    s.first = (r.base + kSlab - 1) & ~(kSlab - 1);
    const uint64_t last = end & ~(kSlab - 1);
    if (last <= s.first || (last - s.first) / kSlab > kMaxSlabs) {
      s.first = 0;
      return refused(RefSmall);
    }
    s.slabs = uint32_t((last - s.first) / kSlab);
    const bool mine = cfg.use != UseNone && (r.use == cfg.use || (r.use == UseAny && cfg.anyOwner == cfg.use));
    s.state = mine ? StateOurs : StateOtherUse;
  }
};

} // namespace pianoxram

#endif
