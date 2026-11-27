// SPDX-License-Identifier: MIT
#include "frametime_stats.h"
#include "panel_match.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

static void check_stream(unsigned hz, bool drop, bool judder)
{
    double times[144 * 65];
    size_t count = 0;
    const double start = 100.0, end = start + 65;
    const size_t ticks = hz * 65;
    for (size_t i = 0; i < ticks; ++i) {
        if (drop && (i == 100 || i == 200 || i == 201))
            continue;
        const double phase = 0.25 + (judder && i % 2 ? 0.30 : 0);
        times[count++] = start + (i + phase) / hz;
    }

    FtStats result;
    assert(ft_analyze(times, count, start, end, hz, &result));
    assert(result.unique == count && result.intervals == count - 1);
    assert(!result.missing && !result.duplicates && !result.outside);
    assert(result.missed_vsync == (drop ? 3 : 0));
    assert(near(result.window_hz, (double)count / 65));
    assert(result.span_s >= 60);
    size_t histogram_total = 0;
    for (size_t i = 0; i < FT_HISTOGRAM_BINS; ++i)
        histogram_total += result.histogram[i];
    assert(histogram_total == result.intervals);

    const double period_ms = 1000.0 / hz;
    if (judder) {
        assert(near(result.cadence_hz, hz * (ticks - 1.0) / (ticks - 0.7)));
        assert(near(result.p95_ms, 1.3 * period_ms));
        assert(near(result.p99_ms, 1.3 * period_ms));
        assert(result.off_grid == result.intervals && result.non_single_period == result.intervals);
    } else {
        assert(near(result.cadence_hz, hz * ((double)count - 1) / (ticks - 1)));
        assert(near(result.p50_ms, period_ms) && near(result.p95_ms, period_ms));
        assert(near(result.p99_ms, period_ms));
        assert(near(result.max_ms, period_ms * (drop ? 3 : 1)));
        assert(result.non_single_period == (drop ? 2 : 0) && !result.off_grid);
        if (hz == 60)
            assert(result.histogram[7] == result.intervals - (drop ? 2 : 0)); // (14, 17] ms
        if (hz == 120)
            assert(result.histogram[3] == result.intervals - (drop ? 2 : 0)); // (8, 9] ms
    }
}

static void check_percentiles_and_stalls(void)
{
    double timestamps[101] = {1};
    for (size_t i = 1; i < 101; ++i) {
        // Put the longest intervals first so computing ranks without sorting fails.
        const unsigned steps = i == 1 ? 4 : i == 2 ? 3 : i <= 6 ? 2 : 1;
        timestamps[i] = timestamps[i - 1] + steps / 64.0;
    }

    FtStats result;
    assert(ft_analyze(timestamps, 101, 1, 4, 64, &result));
    assert(result.intervals == 100 && result.missed_vsync == 9);
    assert(near(result.p50_ms, 15.625) && near(result.p95_ms, 31.25));
    assert(near(result.p99_ms, 46.875) && near(result.max_ms, 62.5));
    assert(result.histogram[11] == 1 && result.histogram[12] == 1);

    // The 50 ms histogram bound is inclusive. Only the 62.5 and 125 ms intervals are stalls.
    const double stalls[] = {0.0625, 0.1125, 0.175, 0.3};
    assert(ft_analyze(stalls, 4, 0, 1, 60, &result));
    assert(result.histogram[11] == 1 && result.histogram[12] == 1 && result.histogram[13] == 1);
}

static void check_numeric_limits(void)
{
    FtStats result;
    const double samples[] = {1, 2, 3, 4};
    assert(!ft_analyze(samples, 2, 1, 5, 0x1p64, &result));
    assert(ft_analyze(samples, 3, 1, 5, 0x1p63, &result));
    assert(result.missed_vsync == UINT64_MAX - 1);
    assert(!ft_analyze(samples, 4, 1, 5, 0x1p63, &result));
    const double wide[] = {1, DBL_MAX / 2};
    assert(!ft_analyze(wide, 2, 0, DBL_MAX, 1e-300, &result));
    assert(!ft_analyze(NULL, 0, -DBL_MAX, DBL_MAX, 60, &result));
    assert(!ft_analyze(NULL, 1, 0, 5, 60, &result));
    assert(!ft_analyze(samples, SIZE_MAX, 0, 5, 60, &result));
}

static void check_capture_validity(void)
{
    FtStats presented, target;
    const double targets[] = {20, 20 + 1.0 / 60, 20 + 2.0 / 60};
    const double zero[] = {0, 0, 0};
    assert(ft_analyze(targets, 3, 20, 21, 60, &target) && target.intervals == 2);
    assert(ft_analyze(zero, 3, 20, 21, 60, &presented));
    assert(presented.missing == 3 && presented.unique == 0 && presented.intervals == 0);
    assert(!ft_capture_valid(&presented, true, true, 3, 3, 0));

    const double sparse[] = {20, 20, 0, -1, INFINITY, 19, 21};
    assert(ft_analyze(sparse, 7, 20, 21, 60, &presented));
    assert(presented.unique == 1 && presented.duplicates == 1 && presented.outside == 2);
    assert(presented.missing == 3 && presented.intervals == 0 && presented.window_hz == 1);
    assert(!ft_capture_valid(&presented, true, true, 7, 7, 0));
    assert(ft_analyze(NULL, 0, 20, 21, 60, &presented));
    assert(!presented.intervals && !presented.window_hz && !presented.p99_ms);
    assert(!ft_capture_valid(&presented, true, true, 0, 0, 0));

    const double out_of_order[] = {20.125, 20.0625, 20.0625, 20, 0};
    assert(ft_analyze(out_of_order, 5, 20, 21, 60, &presented));
    assert(presented.unique == 3 && presented.intervals == 2 && presented.duplicates == 1);
    assert(presented.missing == 1 && near(presented.p50_ms, 62.5) && near(presented.cadence_hz, 16));
    assert(ft_capture_valid(&presented, true, true, 5, 5, 0));
    assert(!ft_capture_valid(&presented, false, true, 5, 5, 0));
    assert(!ft_capture_valid(&presented, true, false, 5, 5, 0));
    assert(!ft_capture_valid(&presented, true, true, 5, 4, 0));
    assert(!ft_capture_valid(&presented, true, true, 5, 5, 1));
}

int main(void)
{
    assert(ft_panel_match(0x61a9, 0x8750, 3200, 2136, false, false, 0, 0, false, false) == 2);
    assert(!ft_panel_match(0x61a9, 0xd6, 3200, 2136, false, true, 3200, 2136, false, false));
    assert(ft_panel_match(0x610, 0x756e6b6e, 3200, 2136, true, true, 3200, 2136, true, false) == 1);
    assert(!ft_panel_match(0x610, 0x756e6b6e, 3200, 2136, true, false, 3200, 2136, true, false));
    assert(!ft_panel_match(0x610, 0x756e6b6e, 3200, 2136, true, true, 3200, 2136, false, false));
    assert(!ft_panel_match(0x610, 0x756e6b6e, 3200, 2136, false, true, 3200, 2136, true, false));
    assert(!ft_panel_match(0x610, 0x756e6b6e, 1920, 1080, true, true, 3200, 2136, true, false));
    assert(!ft_panel_match(0, 0, 3200, 2136, false, true, 3200, 2136, false, false));
    assert(ft_panel_match(0, 0, 3200, 2136, false, true, 3200, 2136, false, true) == 1);
    assert(!ft_panel_match(0, 0, 3200, 2136, false, false, 3200, 2136, false, true));
    assert(!ft_panel_match(0, 0, 3200, 2136, false, true, 0, 0, false, true));
    const unsigned rates[] = {60, 90, 120, 144};
    for (size_t i = 0; i < sizeof(rates) / sizeof(*rates); ++i) {
        check_stream(rates[i], false, false);
        check_stream(rates[i], true, false);
        check_stream(rates[i], false, true);
    }

    FtStats result;
    const double partial[] = {20.125, 0, 19.9, 20, 20.125, NAN, 21};
    assert(ft_analyze(partial, sizeof(partial) / sizeof(*partial), 20, 21, 120, &result));
    assert(result.unique == 2 && result.missing == 2 && result.duplicates == 1 && result.outside == 2);
    assert(near(result.first_s, 20) && near(result.last_s, 20.125));
    assert(result.missed_vsync == 14 && result.histogram[FT_HISTOGRAM_BINS - 1] == 1);
    assert(near(result.max_ms, 125));
    assert(ft_analyze(partial, 1, 20, 21, 120, &result));
    assert(result.unique == 1 && result.intervals == 0);
    assert(ft_analyze(NULL, 0, 20, 21, 120, &result) && !result.unique);
    assert(!ft_analyze(NULL, 0, 20, 20, 120, &result));

    double half_rate[120];
    for (size_t i = 0; i < 120; ++i)
        half_rate[i] = 1.0 + i / 60.0;
    assert(ft_analyze(half_rate, 120, 1, 3, 120, &result));
    assert(near(result.cadence_hz, 60) && result.missed_vsync == 119 && !result.off_grid);
    check_percentiles_and_stalls();
    check_numeric_limits();
    check_capture_validity();
    puts("frametime stats: cadence, percentiles, stalls, numeric limits and capture validity passed");
}
