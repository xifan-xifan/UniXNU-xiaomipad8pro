// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
#include "tier_stats.h"
#include <stdio.h>
#include <stdlib.h>

static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#define LINE(used, allocs, preq, cmd, imd) \
    "mode=1 state=ready runs=3 checked=3 pass=3 fail=0 pass-mask=0x7 free-mib=900 used-mib=" #used " refusals=0 allocs=" #allocs \
    " legacy-small=10 legacy-large=4 contiguous=0 private-req=" #preq " host-import=2 scanout-import=1 cpu-maps=5" \
    " tier-import-denied=" #imd " tier-cpu-map-denied=" #cmd

int main(void)
{
    tier_line b, d, a, bad;
    CHECK(tier_parse(LINE(4, 2, 2, 0, 0), &b) == 0);
    CHECK(b.valid && b.mode == 1 && b.used_mib == 4 && b.allocs == 2 && b.private_req == 2 && b.legacy_small == 10);
    CHECK(b.cpu_maps == 5 && b.scanout_import == 1 && b.host_import == 2);
    // "tier-import-denied" must not be read as "import" etc.; "allocs" not from another key.
    CHECK(b.import_denied == 0 && b.cpu_map_denied == 0);
    CHECK(tier_parse("mode=1 state=ready used-mib=3", &bad) == -1 && !bad.valid);
    CHECK(tier_parse(NULL, &bad) == -1);
    CHECK(tier_parse(LINE(4, 2, 2, 0, 0) " mode=2", &bad) == -1 && !bad.valid);
    CHECK(tier_parse(LINE(4, 2, 2, 0, 0) " state=closed", &bad) == -1 && !bad.valid);
    CHECK(tier_parse(LINE(4, 2, -2, 0, 0), &bad) == -1 && !bad.valid);
    CHECK(tier_parse(LINE(4, 2, 2ticks, 0, 0), &bad) == -1 && !bad.valid);
    CHECK(tier_parse(LINE(4, 2, 18446744073709551616, 0, 0), &bad) == -1 && !bad.valid);

    uint64_t sizes[] = {2u << 20, 4u << 20, 8u << 20, 16u << 20, 32u << 20, 64u << 20};
    unsigned bos;
    uint64_t bytes;
    scratch_classes(sizes, 6, &bos, &bytes);
    CHECK(bos == 6 && bytes == (uint64_t)126 << 20);
    uint64_t desc[] = {64u << 20, 2u << 20, 8u << 20};
    scratch_classes(desc, 3, &bos, &bytes);
    CHECK(bos == 1 && bytes == (uint64_t)64 << 20);
    uint64_t odd[] = {(2u << 20) + 1, 100};
    scratch_classes(odd, 2, &bos, &bytes);
    CHECK(bos == 1 && bytes == (uint64_t)4 << 20);

    CHECK(tier_parse(LINE(130, 8, 8, 0, 0), &d) == 0);
    CHECK(tier_parse(LINE(4, 8, 8, 0, 0), &a) == 0);
    CHECK(tier_live_verdict(&b, &d, 6, 126) == 0);
    CHECK(tier_verdict(&b, &d, &a, 6, 126, 2) == 0);
    CHECK(tier_verdict(&b, &d, &a, 7, 126, 2) == (TIER_FAIL_PRIVATE_REQ | TIER_FAIL_ALLOCS));
    CHECK(tier_verdict(&b, &d, &d, 6, 126, 2) == TIER_FAIL_RETURN);
    CHECK(tier_parse(LINE(4, 8, 8, 1, 0), &a) == 0);
    CHECK(tier_verdict(&b, &d, &a, 6, 126, 2) == TIER_FAIL_DENIED);
    CHECK(tier_verdict(&b, &d, &bad, 6, 126, 2) == TIER_FAIL_UNREADABLE);
    d.mode = 2;
    CHECK(tier_live_verdict(&b, &d, 6, 126) == TIER_FAIL_MODE);
    d.mode = 1;
    d.state[0] = 'R';
    CHECK(tier_live_verdict(&b, &d, 6, 126) == TIER_FAIL_MODE);
    CHECK(tier_parse(LINE(130, 8, 8, 0, 0), &d) == 0);
    a.mode = 2;
    a.cpu_map_denied = b.cpu_map_denied;
    CHECK(tier_verdict(&b, &d, &a, 6, 126, 2) == TIER_FAIL_MODE);
    CHECK(tier_live_verdict(&d, &b, 6, 126) == (TIER_FAIL_PRIVATE_REQ | TIER_FAIL_ALLOCS | TIER_FAIL_USED));
    CHECK(tier_live_verdict(&b, &d, 6, UINT64_MAX) == TIER_FAIL_USED);
    CHECK(tier_verdict(&b, &d, &d, 6, 126, UINT64_MAX) == 0);
    CHECK(tier_parse("mode=0 state=off runs=0 checked=0 pass=0 fail=0 pass-mask=0 free-mib=0 used-mib=4 refusals=0 allocs=2 "
                     "legacy-small=1 legacy-large=1 contiguous=0 private-req=8 host-import=0 scanout-import=0 cpu-maps=0 "
                     "tier-import-denied=0 tier-cpu-map-denied=0", &d) == 0);
    CHECK(tier_verdict(&b, &d, &b, 6, 126, 2) & TIER_FAIL_MODE);

    double v[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(percentile_sorted(v, 10, 50) == 5 && percentile_sorted(v, 10, 95) == 10 && percentile_sorted(v, 10, 0) == 1);
    CHECK(percentile_sorted(v, 0, 50) == 0);
    printf("tier_stats: %d passed, %d failed\n", checks - failures, failures);
    return failures != 0;
}
