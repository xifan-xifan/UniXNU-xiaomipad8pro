// SPDX-License-Identifier: MIT
#include "frametime_stats.h"

#include <math.h>
#include <stdlib.h>

const double ft_histogram_upper_ms[] = {5, 7, 8, 9, 10, 12, 14, 17, 20, 25, 34, 50, 100};

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double nearest_rank(const double *ordered, size_t count, double fraction)
{
    return ordered[(size_t)ceil(fraction * count) - 1];
}

bool ft_analyze(const double *timestamps, size_t count, double start, double end,
                double nominal_hz, FtStats *stats)
{
    *stats = (FtStats){0};
    if (!isfinite(start) || !isfinite(end) || end <= start ||
        !isfinite(nominal_hz) || nominal_hz <= 0 || count > SIZE_MAX / sizeof(double))
        return false;
    if (!count)
        return true;
    double *ordered = malloc(count * sizeof(*ordered));
    if (!ordered)
        return false;

    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        double stamp = timestamps[i];
        if (!isfinite(stamp) || stamp <= 0)
            ++stats->missing;
        else if (stamp < start || stamp >= end)
            ++stats->outside;
        else
            ordered[kept++] = stamp;
    }
    qsort(ordered, kept, sizeof(*ordered), compare_double);
    size_t unique = 0;
    for (size_t i = 0; i < kept; ++i) {
        if (!unique || ordered[i] != ordered[unique - 1])
            ordered[unique++] = ordered[i];
    }
    stats->unique = unique;
    stats->duplicates = kept - unique;
    stats->window_hz = unique / (end - start);
    if (unique) {
        stats->first_s = ordered[0];
        stats->last_s = ordered[unique - 1];
        stats->span_s = stats->last_s - stats->first_s;
    }
    if (unique < 2) {
        free(ordered);
        return true;
    }

    stats->intervals = unique - 1;
    stats->cadence_hz = stats->intervals / stats->span_s;
    for (size_t i = 0; i < stats->intervals; ++i) {
        const double seconds = ordered[i + 1] - ordered[i];
        const double periods = seconds * nominal_hz;
        const double slots = round(periods);
        if (slots > 1)
            stats->missed_vsync += (uint64_t)(slots - 1);
        if (fabs(periods - 1) > 0.20)
            ++stats->non_single_period;
        if (fabs(periods - fmax(1, slots)) > 0.20)
            ++stats->off_grid;

        const double ms = seconds * 1000;
        size_t bin = 0;
        while (bin < FT_HISTOGRAM_BINS - 1 && ms > ft_histogram_upper_ms[bin])
            ++bin;
        ++stats->histogram[bin];
        ordered[i] = ms;
    }
    qsort(ordered, stats->intervals, sizeof(*ordered), compare_double);
    stats->p50_ms = nearest_rank(ordered, stats->intervals, 0.50);
    stats->p95_ms = nearest_rank(ordered, stats->intervals, 0.95);
    stats->p99_ms = nearest_rank(ordered, stats->intervals, 0.99);
    stats->max_ms = ordered[stats->intervals - 1];
    free(ordered);
    return true;
}
