/* SPDX-License-Identifier: MIT */
#ifndef A830_GPU_TIER_H
#define A830_GPU_TIER_H
#include <stddef.h>
#include <stdint.h>
#include "../../include/PianoExtraRam.h"
#if defined(KERNEL)
#include <libkern/libkern.h>   /* snprintf */
#else
#include <stdio.h>
#endif

/* RAM16G R1: use the shared ADT validator for region1 GPU records. The tier
 * qualifies and allocates whole 2 MiB slabs and leaves zeroing to its caller. */
namespace a830tier {

constexpr uint64_t kSlab = pianoxram::kSlab;
constexpr unsigned kMaxRuns = 8;
constexpr uint64_t kPage = 4096;
constexpr unsigned kMaxSlabs = 1024;                   /* 2 GiB of slabs: region1's runs are below 1 GiB */
constexpr uint64_t kRegionLo = 0x880000000ull;         /* region1: the bank below the window's 0x8c0000000 */
constexpr uint64_t kRegionHi = 0x8c0000000ull;

struct Run { uint64_t base; uint64_t size; };
struct Span { uint64_t pa; uint64_t bytes; };

enum class Parse { ok, absent, badLength, rejected };

static inline const char *parseName(Parse p) {
  switch (p) {
    case Parse::ok: return "ok";
    case Parse::absent: return "absent";
    case Parse::badLength: return "bad length";
    case Parse::rejected: return "region1 record refused";
  }
  return "?";
}

static inline Parse loadRuns(const uint8_t *property, size_t length, uint64_t xnuBase, uint64_t xnuSize,
                             Run out[kMaxRuns], unsigned &n) {
  n = 0;
  if (!property) { return Parse::absent; }
  if (length != pianoxram::kMaxRuns * pianoxram::kRecordBytes) { return Parse::badLength; }
  if (xnuSize > UINT64_MAX - xnuBase) { return Parse::rejected; }
  pianoxram::Pool parsed;
  pianoxram::Config config;
  config.use = pianoxram::UseGpu;
  config.xnuBase = xnuBase;
  config.xnuSize = xnuSize;
  parsed.load(property, length, config);
  if (!parsed.count) { return Parse::absent; }
  for (unsigned i = 0; i < parsed.count; ++i) {
    // DarwinBoot packs the nonempty records before the padding, as required by pmap.
    const auto *record = property + i * pianoxram::kRecordBytes;
    if (!(pianoxram::rd64(record) | pianoxram::rd64(record + 8) | pianoxram::rd64(record + 16))) {
      return Parse::rejected;
    }
    const auto &run = parsed.runs[i].run;
    if (!run.size || run.size > UINT64_MAX - run.base) { return Parse::rejected; }
  }

  unsigned count = 0;
  for (unsigned i = 0; i < parsed.count; ++i) {
    const auto &entry = parsed.runs[i];
    const auto &run = entry.run;
    if (run.bank != pianoxram::Region1 || run.use != pianoxram::UseGpu) { continue; }
    if (entry.state != pianoxram::StateOurs || (run.base | run.size) & (kSlab - 1) ||
        run.base >= xnuBase || run.size > xnuBase - run.base || count == kMaxRuns) {
      return Parse::rejected;
    }
    // The shared parser checks earlier records. Later records also reserve their range, even with a different use tag.
    for (unsigned j = i + 1; j < parsed.count; ++j) {
      const auto &other = parsed.runs[j].run;
      if (other.size && pianoxram::overlaps(run.base, run.base + run.size, other.base, other.base + other.size)) {
        return Parse::rejected;
      }
    }
    unsigned pos = count++;
    while (pos && out[pos - 1].base > run.base) {
      out[pos] = out[pos - 1];
      --pos;
    }
    out[pos] = {run.base, run.size};
  }
  n = count;
  return Parse::ok;
}

/* Last matching token wins. Unknown values disable the tier. */
static inline unsigned bootArgMode(const char *args) {
  static const char key[] = "piano-gpu-tier=";
  unsigned mode = 0;
  if (!args) { return mode; }
  for (const char *s = args; *s; ) {
    while (*s == ' ' || *s == '\t') { ++s; }
    const char *end = s;
    while (*end && *end != ' ' && *end != '\t') { ++end; }
    constexpr size_t keyLen = sizeof key - 1;
    const size_t len = static_cast<size_t>(end - s);
    if (len >= keyLen && __builtin_memcmp(s, key, keyLen) == 0) {
      mode = len == keyLen + 1 && (s[keyLen] == '1' || s[keyLen] == '2') ?
          static_cast<unsigned>(s[keyLen] - '0') : 0u;
    }
    s = end;
  }
  return mode;
}

static inline bool eligible(unsigned mode, uint64_t bytes) {
  return mode == 2 || (mode == 1 && bytes >= kSlab);
}

static inline uint64_t slabBytes(uint64_t bytes) {
  if (bytes > UINT64_MAX - (kSlab - 1)) { return 0; }
  return (bytes + (kSlab - 1)) & ~(kSlab - 1);
}

/* Tier spans as the BO's piece list (NativeAdrenoPhysicalSpan {offset, physical, bytes}: the scattered-BO form poolTakeMany
 * fills in NativeAdrenoMemory.cpp): offsets are the running sum inside the BO. */
template <class Piece> static inline void asPieces(const Span *s, unsigned n, Piece *out) {
  uint64_t off = 0;
  for (unsigned i = 0; i < n; i++) { out[i].offset = off; out[i].physical = s[i].pa; out[i].bytes = s[i].bytes; off += s[i].bytes; }
}

class Tier {
 public:
  using ProbeFn = bool (*)(uint64_t pa, unsigned run, void *ctx);
  using LineFn = void (*)(const char *line, void *ctx);

  /* A run opens only after both of its actual endpoint pages pass. */
  void init(const Run *runs, unsigned n, uint64_t limit, ProbeFn probe, void *ctx, LineFn line) {
    *this = Tier();
    if (!runs || !probe || n > kMaxRuns) { return; }
    for (unsigned i = 0; i < n; i++) {
      const Run r = runs[i];
      if (!r.size || (r.base | r.size) & (kSlab - 1) || r.base < kRegionLo ||
          r.base >= kRegionHi || r.size > kRegionHi - r.base ||
          (i && r.base < runs[i - 1].base + runs[i - 1].size) ||
          r.size / kSlab > kMaxSlabs - slabs_) {
        *this = Tier();
        return;
      }
      runs_[i] = r;
      first_[i] = slabs_;
      slabs_ += unsigned(r.size / kSlab);
    }
    nRuns_ = n;
    // Validate the whole layout before S8 touches any physical page.
    for (unsigned i = 0; i < nRuns_; ++i) {
      const Run r = runs_[i];
      if (r.base < limit && r.size <= limit - r.base) {
        const bool first = probe(r.base, i, ctx);
        const bool last = probe(r.base + r.size - kPage, i, ctx);
        ok_[i] = first && last;
      }
      if (ok_[i]) { total_ += r.size; }
      if (line) {
        char b[112];
        snprintf(b, sizeof b, "gpu tier: run %u 0x%llx+%llu MiB self-test %s", i,
                 (unsigned long long)r.base, (unsigned long long)(r.size >> 20), ok_[i] ? "ok" : "FAIL");
        line(b, ctx);
      }
    }
    free_ = total_;
  }

  /* bytes rounded up to slabs, as up to maxSpans spans (each contiguous inside one run, lowest first).
   * Returns the span count, 0 when the tier cannot hold it whole (nothing taken: the caller uses XNU pages). */
  unsigned alloc(uint64_t bytes, Span *out, unsigned maxSpans, uint64_t limit) {
    if (!bytes || !out || !maxSpans || bytes > free_) { return 0; }
    const uint64_t need = slabBytes(bytes) / kSlab;
    unsigned n = 0; uint64_t got = 0;
    for (unsigned r = 0; r < nRuns_ && got < need; r++) {
      const Run run = runs_[r];
      if (!ok_[r] || run.base >= limit || run.size > limit - run.base) { continue; }
      unsigned end = first_[r] + (unsigned)(runs_[r].size / kSlab);
      for (unsigned k = first_[r]; k < end && got < need; ) {
        if (isSet(k)) { k++; continue; }
        unsigned j = k; while (j < end && !isSet(j) && got + (j - k) < need) { j++; }
        if (n == maxSpans) { undo(out, n); return 0; }
        out[n].pa = runs_[r].base + (uint64_t)(k - first_[r]) * kSlab; out[n].bytes = (uint64_t)(j - k) * kSlab;
        for (unsigned m = k; m < j; m++) { set(m); }
        got += j - k; n++; k = j;
      }
    }
    if (got < need) { undo(out, n); return 0; }
    free_ -= need * kSlab;
    return n;
  }

  /* Gives spans back; refuses (and changes nothing) a span that is not whole tier slabs all in use. */
  bool release(const Span *s, unsigned n) {
    if (!s || !n) { return false; }
    for (unsigned i = 0; i < n; i++) {
      unsigned k; uint64_t c;
      if (!locate(s[i], k, c)) { return false; }
      for (unsigned j = 0; j < i; ++j) {
        if (s[i].pa < s[j].pa + s[j].bytes && s[j].pa < s[i].pa + s[i].bytes) { return false; }
      }
      for (uint64_t m = 0; m < c; m++) { if (!isSet(k + (unsigned)m)) { return false; } }
    }
    for (unsigned i = 0; i < n; i++) {
      unsigned k; uint64_t c; locate(s[i], k, c);
      for (uint64_t m = 0; m < c; m++) { clear(k + (unsigned)m); }
      free_ += c * kSlab;
    }
    return true;
  }

  bool owns(uint64_t pa) const {
    for (unsigned r = 0; r < nRuns_; r++) { if (ok_[r] && pa >= runs_[r].base && pa - runs_[r].base < runs_[r].size) { return true; } }
    return false;
  }
  uint64_t totalBytes() const { return total_; }
  uint64_t freeBytes() const { return free_; }
  unsigned runs() const { return nRuns_; }
  bool runPassed(unsigned i) const { return i < nRuns_ && ok_[i]; }

 private:
  bool locate(const Span &s, unsigned &k, uint64_t &c) const {
    if (!s.bytes || (s.pa | s.bytes) & (kSlab - 1)) { return false; }
    for (unsigned r = 0; r < nRuns_; r++) {
      if (ok_[r] && s.pa >= runs_[r].base && s.pa - runs_[r].base < runs_[r].size && s.bytes <= runs_[r].base + runs_[r].size - s.pa) {
        k = first_[r] + (unsigned)((s.pa - runs_[r].base) / kSlab); c = s.bytes / kSlab; return true;
      }
    }
    return false;
  }
  void undo(const Span *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) { unsigned k; uint64_t c; if (locate(s[i], k, c)) { for (uint64_t m = 0; m < c; m++) { clear(k + (unsigned)m); } } }
  }
  bool isSet(unsigned k) const { return bits_[k / 64] >> (k % 64) & 1; }
  void set(unsigned k) { bits_[k / 64] |= 1ull << (k % 64); }
  void clear(unsigned k) { bits_[k / 64] &= ~(1ull << (k % 64)); }

  Run runs_[kMaxRuns] = {};
  unsigned first_[kMaxRuns] = {};
  bool ok_[kMaxRuns] = {};
  unsigned nRuns_ = 0, slabs_ = 0;
  uint64_t bits_[kMaxSlabs / 64] = {};
  uint64_t total_ = 0, free_ = 0;
};

}  // namespace a830tier
#endif
