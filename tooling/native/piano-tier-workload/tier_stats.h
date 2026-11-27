// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
// Portable helpers of piano-tier-workload: the machdep.piano_gpu_tier line, the expected private scratch classes, phase statistics.
#ifndef PIANO_TIER_STATS_H
#define PIANO_TIER_STATS_H
#include <stddef.h>
#include <stdint.h>

// The counters of one machdep.piano_gpu_tier line (package/PianoA830/board/PianoA830.cpp sysctlGpuTier).
typedef struct {
    int valid;              // every field below was found
    unsigned mode;
    char state[24];
    uint64_t free_mib, used_mib, refusals, allocs;
    uint64_t legacy_small, legacy_large, contiguous, private_req;
    uint64_t host_import, scanout_import, cpu_maps, import_denied, cpu_map_denied;
} tier_line;

// 0 when the line carries every counter; -1 otherwise (t->valid stays 0).
int tier_parse(const char *line, tier_line *t);

// Parse one unsigned decimal number, stopping at the first non-digit.
int tier_decimal(const char *s, const char **end, uint64_t *value);

// Turnip's device scratch (tu_get_scratch_bo): one BO per power-of-two class of at least 4 KiB, reused for any request at or below
// an existing larger class. For staging sizes taken in the given order, the number of new scratch BOs and their bytes.
void scratch_classes(const uint64_t *staging_bytes, size_t n, unsigned *new_bos, uint64_t *bytes);

// The verdict of a before / during / after triple. Returns a bit mask of failures (0 = pass):
enum {
    TIER_FAIL_UNREADABLE = 1 << 0,  // a line did not parse
    TIER_FAIL_MODE = 1 << 1,        // mode 1 / ready required for each sample
    TIER_FAIL_PRIVATE_REQ = 1 << 2, // private-req rose by fewer than expected_bos
    TIER_FAIL_ALLOCS = 1 << 3,      // allocs rose by fewer than expected_bos
    TIER_FAIL_USED = 1 << 4,        // used-mib rose by less than expected_mib
    TIER_FAIL_RETURN = 1 << 5,      // after exit used-mib is more than slack_mib above the before line
    TIER_FAIL_DENIED = 1 << 6,      // tier-cpu-map-denied or tier-import-denied rose
};
unsigned tier_snapshot_failure(const tier_line *sample);
unsigned tier_live_verdict(const tier_line *before, const tier_line *during, unsigned expected_bos,
                           uint64_t expected_mib);
unsigned tier_verdict(const tier_line *before, const tier_line *during, const tier_line *after, unsigned expected_bos,
                      uint64_t expected_mib, uint64_t slack_mib);

// Nearest-rank percentile of n sorted doubles (p in 0..100); 0 for n == 0.
double percentile_sorted(const double *v, size_t n, double p);
#endif
