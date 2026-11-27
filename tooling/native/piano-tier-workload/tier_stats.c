// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
#include "tier_stats.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

int tier_decimal(const char *s, const char **end, uint64_t *value)
{
    uint64_t n = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (n > (UINT64_MAX - digit) / 10)
            return -1;
        n = n * 10 + digit;
        p++;
    }
    if (p == s)
        return -1;
    *end = p;
    *value = n;
    return 0;
}

static const char *field(const char *line, const char *key)
{
    size_t k = strlen(key);
    const char *value = NULL;
    for (const char *p = line; (p = strstr(p, key)) != NULL; p += k) {
        if ((p != line && !isspace((unsigned char)p[-1])) || p[k] != '=')
            continue;
        if (value)
            return NULL;
        value = p + k + 1;
    }
    return value;
}

int tier_parse(const char *line, tier_line *t)
{
    memset(t, 0, sizeof *t);
    if (!line)
        return -1;
    uint64_t mode = 0;
    struct { const char *key; uint64_t *v; } f[] = {
        {"mode", &mode}, {"free-mib", &t->free_mib}, {"used-mib", &t->used_mib}, {"refusals", &t->refusals},
        {"allocs", &t->allocs}, {"legacy-small", &t->legacy_small}, {"legacy-large", &t->legacy_large},
        {"contiguous", &t->contiguous}, {"private-req", &t->private_req}, {"host-import", &t->host_import},
        {"scanout-import", &t->scanout_import}, {"cpu-maps", &t->cpu_maps}, {"tier-import-denied", &t->import_denied},
        {"tier-cpu-map-denied", &t->cpu_map_denied},
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        const char *value = field(line, f[i].key), *end;
        if (!value || tier_decimal(value, &end, f[i].v) || (*end && !isspace((unsigned char)*end)))
            return -1;
    }
    const char *s = field(line, "state");
    if (!s || mode > UINT_MAX)
        return -1;
    size_t n = strcspn(s, " \t\r\n\v\f");
    if (!n || n >= sizeof t->state)
        return -1;
    t->mode = (unsigned)mode;
    memcpy(t->state, s, n);
    t->state[n] = 0;
    t->valid = 1;
    return 0;
}

void scratch_classes(const uint64_t *staging_bytes, size_t n, unsigned *new_bos, uint64_t *bytes)
{
    unsigned have = 0; // log2 of the largest class allocated so far, 0 = none
    *new_bos = 0;
    *bytes = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned log2 = 12; // MIN_SCRATCH_BO_SIZE_LOG2
        while (log2 < 63 && (UINT64_C(1) << log2) < staging_bytes[i])
            log2++;
        if (have >= log2)
            continue; // the fast path hands out an initialized class at or above the request
        have = log2;
        (*new_bos)++;
        *bytes += UINT64_C(1) << log2;
    }
}

unsigned tier_snapshot_failure(const tier_line *sample)
{
    if (!sample->valid)
        return TIER_FAIL_UNREADABLE;
    return sample->mode == 1 && !strcmp(sample->state, "ready") ? 0 : TIER_FAIL_MODE;
}

static int roseBy(uint64_t before, uint64_t after, uint64_t amount)
{
    return after >= before && after - before >= amount;
}

static int denialsChanged(const tier_line *b, const tier_line *sample)
{
    return sample->cpu_map_denied != b->cpu_map_denied || sample->import_denied != b->import_denied;
}

unsigned tier_live_verdict(const tier_line *b, const tier_line *d, unsigned expected_bos, uint64_t expected_mib)
{
    unsigned fail = tier_snapshot_failure(b) | tier_snapshot_failure(d);
    if (fail & TIER_FAIL_UNREADABLE)
        return TIER_FAIL_UNREADABLE;
    if (!roseBy(b->private_req, d->private_req, expected_bos))
        fail |= TIER_FAIL_PRIVATE_REQ;
    if (!roseBy(b->allocs, d->allocs, expected_bos))
        fail |= TIER_FAIL_ALLOCS;
    if (!roseBy(b->used_mib, d->used_mib, expected_mib))
        fail |= TIER_FAIL_USED;
    if (denialsChanged(b, d))
        fail |= TIER_FAIL_DENIED;
    return fail;
}

unsigned tier_verdict(const tier_line *b, const tier_line *d, const tier_line *a, unsigned expected_bos, uint64_t expected_mib,
                      uint64_t slack_mib)
{
    if (!b->valid || !d->valid || !a->valid)
        return TIER_FAIL_UNREADABLE;
    unsigned fail = tier_live_verdict(b, d, expected_bos, expected_mib) | tier_snapshot_failure(a);
    if (a->used_mib > b->used_mib && a->used_mib - b->used_mib > slack_mib)
        fail |= TIER_FAIL_RETURN;
    if (denialsChanged(b, a))
        fail |= TIER_FAIL_DENIED;
    return fail;
}

double percentile_sorted(const double *v, size_t n, double p)
{
    if (!n)
        return 0;
    size_t rank = (size_t)ceil(p / 100.0 * (double)n);
    if (rank < 1)
        rank = 1;
    if (rank > n)
        rank = n;
    return v[rank - 1];
}
