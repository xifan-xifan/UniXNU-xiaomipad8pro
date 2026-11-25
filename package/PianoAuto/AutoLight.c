/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, Leo Zhang. All rights reserved. */
/* The ambient light chain: see AutoLight.h. */
#include "AutoLight.h"

void pal_init(PalLight *l, const PasConfig *c)
{
    pas_init(&l->stream, c);
    ps_als_init(&l->als);
    l->enabled = l->display_on = 0;
    l->lid = -1;
    l->tick_at = 0;
    l->target = -1;
    l->lux = 0;
    l->samples = l->targets = l->restarts = 0;
    l->raw_bits = l->invalid = 0;
    l->sample_ms = l->valid_ms = 0;
    l->sample_valid = 0;
    l->curve = PS_ALS_LEGACY;
    l->revision = l->floor = 0;
    l->curve_waiting = 0;
}

int pal_select_curve(PalLight *l, PsAlsCurve curve, uint32_t revision, uint16_t floor)
{
    if (curve == l->curve && revision == l->revision && floor == l->floor) return 0;
    l->curve = curve;
    l->revision = revision;
    l->floor = floor;
    l->target = -1;
    l->tick_at = l->valid_ms = 0;
    l->sample_valid = 0;
    ps_als_init(&l->als);
    l->curve_waiting = 1;
    if (l->stream.want && (l->stream.state == PAS_ON || l->stream.state == PAS_ENABLING)) l->stream.rearm = 1;
    return 1;
}

int pal_settled(const PalLight *l)
{
    const PsAls *a = &l->als;
    return !a->has || (a->smooth_q16 == a->goal << 16 && a->published == a->goal);
}

static int32_t took(PalLight *l, int32_t target, uint64_t now)
{
    l->tick_at = pal_settled(l) ? 0 : now + PS_ALS_PUBLISH_MS;
    if (target >= 0) {
        l->target = target;
        ++l->targets;
    }
    return target;
}

void pal_conditions(PalLight *l, int enabled, int display_on, int lid, uint64_t now_ms)
{
    l->enabled = enabled != 0;
    l->display_on = display_on != 0;
    l->lid = lid == 0 || lid == 1 ? lid : -1;
    const int want = l->enabled && l->display_on && l->lid != 0;
    const int was_wanted = l->stream.want;
    pas_want(&l->stream, want, now_ms);
    if (want && !was_wanted && l->curve_waiting && (l->stream.state == PAS_ON || l->stream.state == PAS_ENABLING))
        l->stream.rearm = 1;
    if (!want) {
        l->tick_at = 0;
        if (l->revision && was_wanted) {
            l->curve_waiting = 1;
            l->target = -1;
            ps_als_init(&l->als);
        }
    }
}

int pal_lux_valid(uint32_t bits)
{
    return (bits & 0x7f800000u) != 0x7f800000u && (!(bits >> 31) || !(bits & 0x7fffffffu));
}

int32_t pal_event(PalLight *l, uint32_t lux_bits, uint64_t now_ms)
{
    l->raw_bits = lux_bits;
    l->sample_ms = now_ms;
    l->sample_valid = pal_lux_valid(lux_bits);
    if (!l->sample_valid) { ++l->invalid; return -1; }
    if (l->curve_waiting && l->stream.rearm) return -1;
    const int e = pas_event(&l->stream, now_ms);
    if (e == PAS_EV_DROP) return -1;
    l->curve_waiting = 0;
    if (e == PAS_EV_FIRST) {
        if (l->samples) ++l->restarts;
        ps_als_init(&l->als);                     /* a new session: start from its first sample */
    }
    ++l->samples;
    l->valid_ms = now_ms;
    const uint32_t end = ps_als_end_lux(l->curve);
    const int32_t milli = (int32_t)sfx_scale(lux_bits, 1000, 1, 0, 0, (int64_t)end * 1000);
    l->lux = (uint32_t)(((int64_t)milli + 500) / 1000);
    return took(l, ps_als_step_curve(&l->als, l->curve, l->floor, 1, milli, now_ms), now_ms);
}

int32_t pal_tick(PalLight *l, uint64_t now_ms)
{
    pas_tick(&l->stream, now_ms);
    if (l->curve_waiting && l->stream.state == PAS_OFF) l->stream.rearm = 0;
    if (!l->tick_at || now_ms < l->tick_at) return -1;
    if (!l->stream.want || l->stream.state != PAS_ON) {
        l->tick_at = 0;
        return -1;
    }
    return took(l, ps_als_step(&l->als, 0, 0, now_ms), now_ms);
}

uint64_t pal_deadline(const PalLight *l, uint64_t now_ms)
{
    const uint64_t s = pas_deadline(&l->stream, now_ms), t = l->tick_at;
    const uint64_t best = !s ? t : !t ? s : s < t ? s : t;
    return best && best < now_ms ? now_ms : best;
}
