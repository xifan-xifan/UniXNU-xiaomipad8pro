// SPDX-License-Identifier: BSD-2-Clause-Patent
// B1b's presentation cadence and the split clean (docs/drivers/dpu.md §4.1 "cadence"): portable, no kernel headers.
//   - Hist: a fixed histogram of microsecond intervals (0.25 ms buckets to 50 ms, one overflow bucket), its percentiles and the
//     coarse bands the 120 Hz analysis grades by (<=9.5 ms one 120 Hz period, 9.5-12.5, 12.5-18 two periods, 18-26, 26-35, >35 ms).
//     No allocation, no floating point in the kernel path; the report reads it once per summary period.
//   - cleanBand: [begin, end) of band i of n over a range of bytes, on line boundaries, the n bands covering every line exactly once
//     (piano-dpu-clean-threads: the same DC CVAC over the same lines, from n threads).
// DpuFlip.h (the flipper) and DpuFlip.cpp (the kernel's threads) use it; tests/dpu_flip_host.cpp tests it.
#pragma once
#include <stdint.h>

namespace pianodpu {

struct Hist {
  static constexpr uint32_t kBucketUs = 250, kBuckets = 200;   // 0..50 ms, then the overflow bucket kBuckets
  uint32_t b[kBuckets + 1] = {};
  uint64_t n = 0, sumUs = 0, maxUs = 0;
  void add(uint64_t us)
  {
    const uint64_t i = us / kBucketUs;
    ++b[i < kBuckets ? i : kBuckets];
    ++n;
    sumUs += us;
    if (us > maxUs) maxUs = us;
  }
  void clear() { *this = Hist{}; }
  // The p-th percentile (p in per mille, 500 = median) as its bucket's upper edge in us (the maximum for the overflow bucket); 0 if
  // empty. The rank is ceil(n * p / 1000), at least 1: p999 of 10 samples is the largest.
  uint64_t pct(uint32_t perMille) const
  {
    if (!n) return 0;
    uint64_t rank = (n * perMille + 999) / 1000;
    if (!rank) rank = 1;
    uint64_t seen = 0;
    for (uint32_t i = 0; i <= kBuckets; ++i) {
      seen += b[i];
      if (seen >= rank) return i < kBuckets ? uint64_t(i + 1) * kBucketUs : maxUs;
    }
    return maxUs;
  }
  // How many samples are at most us (us a multiple of kBucketUs: exact; else the bucket holding us counts whole).
  uint64_t atMost(uint64_t us) const
  {
    uint64_t c = 0;
    const uint64_t last = us / kBucketUs;   // buckets [0, last) hold values < last * kBucketUs <= us
    for (uint32_t i = 0; i < kBuckets && i < last; ++i) c += b[i];
    if (last < kBuckets && us % kBucketUs) c += b[last];
    return c;
  }
  // The six bands: <=9.5, 9.5-12.5, 12.5-18, 18-26, 26-35, >35 ms.
  void bands(uint64_t out[6]) const
  {
    static constexpr uint64_t edges[5] = {9500, 12500, 18000, 26000, 35000};
    uint64_t prev = 0;
    for (int i = 0; i < 5; ++i) {
      const uint64_t c = atMost(edges[i]);
      out[i] = c - prev;
      prev = c;
    }
    out[5] = n - prev;
  }
};

// Band i of n of [0, bytes) in whole lines (line a power of two): every line in exactly one band, the bands in order, the last one
// to the end. Empty bands when there are fewer lines than bands.
struct Band {
  uint64_t begin, end;
};
constexpr Band cleanBand(uint64_t bytes, unsigned i, unsigned n, uint64_t line)
{
  if (!n || i >= n || !line) return Band{0, 0};
  const uint64_t lines = (bytes + line - 1) / line;
  const uint64_t b = lines * i / n * line, e = lines * (i + 1) / n * line;
  return Band{b < bytes ? b : bytes, e < bytes ? e : bytes};
}

}  // namespace pianodpu
