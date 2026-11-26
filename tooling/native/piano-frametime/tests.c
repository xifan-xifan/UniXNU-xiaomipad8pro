// SPDX-License-Identifier: MIT
#include "frametime_stats.h"
#include "panel_match.h"

#include <assert.h>
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
    }
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
    puts("frametime stats: clean, drops, judder, half rate and missing/duplicate samples passed");
}
