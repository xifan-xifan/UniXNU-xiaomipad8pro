// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FT_HISTOGRAM_BINS = 14 };
extern const double ft_histogram_upper_ms[FT_HISTOGRAM_BINS - 1];

typedef struct {
    size_t unique, missing, duplicates, outside, intervals;
    size_t non_single_period, off_grid;
    uint64_t missed_vsync;
    double first_s, last_s, span_s, cadence_hz, window_hz;
    double p50_ms, p95_ms, p99_ms, max_ms;
    size_t histogram[FT_HISTOGRAM_BINS];
} FtStats;

// Timestamps can be unordered. Zero, negative, and nonfinite samples are missing.
// Only distinct events within the half-open measurement window contribute intervals.
// Returns false if the inputs or computed statistics cannot be represented.
bool ft_analyze(const double *timestamps, size_t count, double start, double end,
                double nominal_hz, FtStats *stats);

// A complete capture needs an observed presentation interval and successful GPU completion.
bool ft_capture_valid(const FtStats *presented, bool window_complete, bool stable_display,
                      size_t submitted, size_t completed, size_t gpu_errors);
