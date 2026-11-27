/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The device orientation from the accelerometer: see SensorsOrient.h. */
#include "SensorsOrient.h"

#define G (1ll << 16)
#define TOP (8 * G)             /* beyond any sample that counts: the squares below stay inside 64 bits */

void sno_init(SnoState *s)
{
    s->state = SNO_UNKNOWN;
    s->candidate = SNO_UNKNOWN;
    s->since = 0;
}

static int64_t absolute(int64_t v) {return v < 0 ? -v : v;}

/* What one sample shows: a state, or the current one when it is in a dead zone. */
static int classify(const SnoState *s, int64_t x, int64_t y, int64_t z)
{
    const int64_t ax = absolute(x), ay = absolute(y), az = absolute(z);
    if (ax > TOP || ay > TOP || az > TOP) return s->state;
    const int64_t length = x * x + y * y + z * z;                    /* 32.32, at most 3 * 2^38 */
    if (length * 100 < 36 * (G * G) || length * 100 > 196 * (G * G)) return s->state;
    if (az * az * 100 >= 72 * length) return z < 0 ? SNO_FACE_UP : SNO_FACE_DOWN;      /* 0.85 squared */
    if ((ax * ax + ay * ay) * 100 < 16 * length) return s->state;    /* 0.4 squared: neither flat nor upright enough */
    const int portrait = y < 0 ? SNO_PORTRAIT : SNO_UPSIDE_DOWN, landscape = x < 0 ? SNO_LANDSCAPE_LEFT : SNO_LANDSCAPE_RIGHT;
    const int leading = ay >= ax ? portrait : landscape;
    /* Any change (and the first state) needs the margin: the diagonals keep what they have. */
    if (leading == s->state || (ay >= ax ? ay * 10 >= ax * 13 : ax * 10 >= ay * 13)) return leading;
    return s->state;
}

int sno_update_at(SnoState *s, int32_t x, int32_t y, int32_t z, uint64_t now, uint64_t hold)
{
    const int shown = classify(s, x, y, z);
    if (shown == s->state) {
        s->candidate = shown;
        return -1;
    }
    if (shown != s->candidate || now < s->since) {
        s->candidate = shown;
        s->since = now;
        return -1;
    }
    if (now - s->since < hold) return -1;
    s->state = shown;
    return shown;
}

int sno_update(SnoState *s, int32_t x, int32_t y, int32_t z, uint64_t now_ms)
{
    return sno_update_at(s, x, y, z, now_ms, SNO_HOLD_MS);
}
