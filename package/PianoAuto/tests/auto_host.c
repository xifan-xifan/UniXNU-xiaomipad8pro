// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoSensors -I../../PianoSensors/tests -I../../include auto_host.c ../AutoRotate.c ../AutoStream.c ../AutoLight.c ../../PianoSensors/SensorsPolicy.c ../../PianoSensors/SensorsOrient.c ../../PianoSensors/SensorsFixed.c -o /tmp/piano-auto-host && /tmp/piano-auto-host
// Host tests of PianoAuto's portable C (docs/drivers/sensors.md section 5a), synthetic inputs only, ASan/UBSan:
//   touch    TouchRotation.h: the four rotations against CGDisplayRotation's definition (corners), round trips, clamping, the HID scale,
//            the latch at touch-down
//   rotate   AutoRotate.c over SensorsPolicy's ps_rotation_step: settle and interval, the cover (forced 0, held, at attach only,
//            ignored, unknown), the detach hold and contact bounce, confirmation by reading back, refusal and backoff, giving up,
//            rotations made by hand, the user's lock, observing only, touch retries, deadlines
//   stream, light (stream_tests.h): AutoStream.c and AutoLight.c, the sensor streams' off/on lifecycle and the ambient light chain
#include "check.h"
#include "TouchRotation.h"
#include "AutoRotate.h"
#include "stream_tests.h"

enum {W = 3200, H = 2136};

static void test_touch(void)
{
    // CGDisplayConfiguration.h: at 90 the display is turned clockwise, its bottom now on the left and its top on the right (so its left
    // edge is up); 270 the other way (its right edge up).
    static const struct {uint32_t deg; int32_t px, py, dx, dy, dw, dh;} corner[] = {
        {0, 0, 0, 0, 0, W, H},                   {0, W - 1, H - 1, W - 1, H - 1, W, H},
        {90, 0, H - 1, 0, 0, H, W},              // the panel's bottom-left corner is the desktop's top-left
        {90, 0, 0, H - 1, 0, H, W},              // top-left -> top-right
        {90, W - 1, H - 1, 0, W - 1, H, W},      // bottom-right -> bottom-left
        {90, W - 1, 0, H - 1, W - 1, H, W},      // top-right -> bottom-right
        {180, 0, 0, W - 1, H - 1, W, H},         {180, W - 1, H - 1, 0, 0, W, H},
        {180, W - 1, 0, 0, H - 1, W, H},
        {270, W - 1, 0, 0, 0, H, W},             // the panel's top-right corner is the desktop's top-left
        {270, 0, 0, 0, W - 1, H, W},             // top-left -> bottom-left
        {270, W - 1, H - 1, H - 1, 0, H, W},     // bottom-right -> top-right
        {270, 0, H - 1, H - 1, W - 1, H, W},     // bottom-left -> bottom-right
    };
    for (unsigned i = 0; i < sizeof corner / sizeof corner[0]; ++i) {
        const PatPoint p = pat_rotate(corner[i].deg, corner[i].px, corner[i].py, W, H);
        CHECK(p.x == corner[i].dx && p.y == corner[i].dy && p.width == corner[i].dw && p.height == corner[i].dh);
    }
    // a finger moving right on the panel (+x) moves down the desktop at 90 (the panel's left edge is up) and up it at 270
    CHECK(pat_rotate(90, 101, 500, W, H).y - pat_rotate(90, 100, 500, W, H).y == 1);
    CHECK(pat_rotate(270, 101, 500, W, H).y - pat_rotate(270, 100, 500, W, H).y == -1);
    // round trips over a grid, and every result inside the rotated desktop
    static const uint32_t rotations[] = {0, 90, 180, 270};
    for (unsigned r = 0; r < 4; ++r)
        for (int32_t y = 0; y < H; y += 97)
            for (int32_t x = 0; x < W; x += 131) {
                const PatPoint p = pat_rotate(rotations[r], x, y, W, H);
                CHECK(p.x >= 0 && p.x < p.width && p.y >= 0 && p.y < p.height);
                const PatPoint q = pat_unrotate(rotations[r], p.x, p.y, W, H);
                CHECK(q.x == x && q.y == y);
            }
    // out of range clamps (as tabletReport does); anything but the four rotations is 0
    PatPoint p = pat_rotate(90, -5, H + 40, W, H);
    CHECK(p.x == 0 && p.y == 0);
    p = pat_rotate(45, 7, 9, W, H);
    CHECK(p.x == 7 && p.y == 9 && p.width == W && p.height == H);
    CHECK(pat_valid(270) && !pat_valid(360) && !pat_valid(1));
    // the HID report's scale (TouchHid.h tabletReport: v * 32767 / (size - 1)) of the rotated size reaches both ends
    p = pat_rotate(90, 0, 0, W, H);
    CHECK((int64_t)p.x * 32767 / (p.width - 1) == 32767 && (int64_t)p.y * 32767 / (p.height - 1) == 0);
    // the latch: a request takes effect at the next touch-down only
    PatRotation r;
    pat_init(&r);
    CHECK(pat_latch(&r, 1) == 0);
    CHECK(pat_request(&r, 90) == 1);
    CHECK(pat_latch(&r, 0) == 0);                // the contact that is down keeps its mapping
    CHECK(pat_latch(&r, 1) == 90 && r.changes == 1);
    CHECK(pat_request(&r, 45) == 0 && pat_latch(&r, 1) == 90);
    CHECK(pat_request(&r, 270) == 1 && pat_request(&r, 0) == 1 && pat_latch(&r, 1) == 0 && r.changes == 2);
    printf("PASS touch: the four rotations as CGDisplayRotation defines them, round trips, clamping, the HID scale, the touch-down latch\n");
}

static ParAgent a;

static ParDecision at(uint64_t t)
{
    return par_step(&a, t, 1);
}

// The agent, its display at 0, the cover detached, the top edge up; touch told 0.
static void start(const ParConfig *c)
{
    par_init(&a, c);
    CHECK(at(0).apply == -1);                        // nothing shown yet: nothing is decided
    par_observed(&a, 0, 0);
    par_cover(&a, PAR_COVER_DETACHED, 0);
    par_edge(&a, PS_EDGE_TOP, 0);
    const ParDecision d = at(10);
    CHECK(d.apply == -1 && d.touch == 0);
    par_touch_done(&a, 0, 1, 10);
    CHECK(a.manual_changes == 0);
}

// The tablet turned to edge at t: the rotation after the settle time, read back 100 ms later; touch told.
static void turn(int edge, uint64_t t, int degrees)
{
    par_edge(&a, edge, t);
    CHECK(at(t).apply == -1);
    CHECK(par_deadline(&a, t, 1) == t + 600);
    CHECK(at(t + 599).apply == -1);
    const ParDecision d = at(t + 600);
    CHECK(d.apply == degrees && d.want == degrees);
    par_applied(&a, degrees, 1, t + 600);
    CHECK(at(t + 650).apply == -1);
    par_observed(&a, degrees, t + 700);
    const ParDecision e = at(t + 700);
    CHECK(e.apply == -1 && e.touch == degrees);
    par_touch_done(&a, degrees, 1, t + 700);
    CHECK(a.shown == degrees && at(t + 800).touch == -1);
}

static void test_rotate(void)
{
    ParConfig c;
    par_defaults(&c);
    CHECK(c.rot.degrees[PS_EDGE_LEFT] == 90 && c.rot.degrees[PS_EDGE_RIGHT] == 270 && c.cover_lock == PAR_LOCK_HOLD);
    CHECK(par_edge_of_degrees(&c, 90) == PS_EDGE_LEFT && par_edge_of_degrees(&c, 180) == PS_EDGE_BOTTOM &&
          par_edge_of_degrees(&c, 45) == PS_EDGE_NONE);

    // detached: settle, apply, wait for the reading back (the old rotation read meanwhile changes nothing), confirm, touch
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1 && par_deadline(&a, 1000, 1) == 1600);
    CHECK(at(1599).apply == -1);
    ParDecision d = at(1600);
    CHECK(d.apply == 90);
    par_applied(&a, 90, 1, 1600);
    CHECK(at(1700).apply == -1 && par_deadline(&a, 1700, 1) == 1950);
    par_observed(&a, 0, 1800);
    CHECK(a.manual == 0 && a.asked == 90);
    par_observed(&a, 90, 1900);
    d = at(1900);
    CHECK(d.apply == -1 && d.touch == 90 && a.confirmed == 1);
    par_touch_done(&a, 90, 1, 1900);
    // laid flat: kept; then another edge settles
    par_edge(&a, PS_EDGE_NONE, 2000);
    CHECK(at(2100).apply == -1 && par_deadline(&a, 2100, 1) == 0);
    par_edge(&a, PS_EDGE_BOTTOM, 2000);
    CHECK(at(2000).apply == -1 && at(2550).apply == -1);
    CHECK(at(2600).apply == 180);                    // 2600 - 1600 >= 1000
    par_applied(&a, 180, 1, 2600);
    par_observed(&a, 180, 2650);
    CHECK(at(2650).touch == 180);
    par_touch_done(&a, 180, 1, 2650);

    // the cover attached: 0 at once (the interval since the last rotation has passed); edges ignored while attached
    par_cover(&a, PAR_COVER_ATTACHED, 5000);
    d = at(5000);
    CHECK(d.apply == 0);
    par_applied(&a, 0, 1, 5000);
    par_observed(&a, 0, 5100);
    CHECK(at(5100).touch == 0);
    par_touch_done(&a, 0, 1, 5100);
    par_edge(&a, PS_EDGE_RIGHT, 6000);
    CHECK(at(6000).apply == -1 && at(7000).apply == -1 && par_deadline(&a, 7000, 1) == 0);
    // held (the default): a rotation made by hand while attached is turned back after the minimum interval
    par_observed(&a, 180, 8000);
    CHECK(a.manual == 1 && a.manual_changes == 1 && at(8000).touch == 180);
    par_touch_done(&a, 180, 1, 8000);
    CHECK(at(8500).apply == -1 && par_deadline(&a, 8500, 1) == 9000);
    CHECK(at(9000).apply == 0);
    par_applied(&a, 0, 1, 9000);
    par_observed(&a, 0, 9100);
    CHECK(a.manual == 0 && at(9100).touch == 0);
    par_touch_done(&a, 0, 1, 9100);
    // detached: the hold, then an edge that settles afresh
    par_cover(&a, PAR_COVER_DETACHED, 10000);
    par_edge(&a, PS_EDGE_LEFT, 10000);
    CHECK(at(10500).apply == -1 && par_deadline(&a, 10500, 1) == 11500);
    CHECK(at(11499).apply == -1);
    CHECK(at(11500).apply == -1 && par_deadline(&a, 11500, 1) == 12100);
    CHECK(at(12099).apply == -1 && at(12100).apply == 90);
    par_applied(&a, 90, 1, 12100);
    par_observed(&a, 90, 12200);
    par_touch_done(&a, 90, 1, 12200);

    // contact bounce while the cover is put on and taken off: no rotation until it has been off for the hold and an edge settled
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 0);
    for (uint64_t t = 1000; t < 3000; t += 300) {
        par_cover(&a, (t / 300) % 2 ? PAR_COVER_ATTACHED : PAR_COVER_DETACHED, t);
        CHECK(at(t).apply == -1 && at(t + 100).apply == -1);
    }
    par_cover(&a, PAR_COVER_DETACHED, 3000);
    CHECK(at(4499).apply == -1 && at(4500).apply == -1 && at(5099).apply == -1);
    CHECK(at(5100).apply == 90);

    // at attach only: attaching turns to 0, a rotation chosen while attached then stays
    ParConfig once = c;
    once.cover_lock = PAR_LOCK_AT_ATTACH;
    start(&once);
    turn(PS_EDGE_RIGHT, 1000, 270);
    par_cover(&a, PAR_COVER_ATTACHED, 5000);
    CHECK(at(5000).apply == 0);
    par_applied(&a, 0, 1, 5000);
    par_observed(&a, 0, 5100);
    CHECK(a.force == 0);
    par_observed(&a, 90, 7000);
    CHECK(at(7000).apply == -1 && at(9000).apply == -1 && a.shown == 90);
    // and the cover ignored: attached rotates like detached
    ParConfig ignore = c;
    ignore.cover_lock = PAR_LOCK_IGNORE;
    start(&ignore);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1);
    par_cover(&a, PAR_COVER_ATTACHED, 1200);
    CHECK(at(1200).apply == -1 && par_deadline(&a, 1200, 1) == 1600);
    CHECK(at(1600).apply == 90);
    // no PianoKeyboard: detached by default; or attached when configured so (then 0 is forced from the start)
    par_init(&a, &c);
    par_observed(&a, 0, 0);
    par_edge(&a, PS_EDGE_RIGHT, 0);
    CHECK(at(0).apply == -1 && at(1000).apply == 270);
    ParConfig cautious = c;
    cautious.unknown_cover_attached = 1;
    par_init(&a, &cautious);
    par_observed(&a, 90, 0);
    CHECK(at(1500).apply == 0);

    // refused: backoff 2, 4, 8, 16 s, then given up (5 in a row); the policy still says what it wants
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1 && at(1600).apply == 90);
    par_applied(&a, 90, 0, 1600);
    CHECK(at(1700).apply == -1 && a.retry_at == 3600 && par_deadline(&a, 1700, 1) == 3600);
    d = at(2700);                                    // past the minimum interval, inside the backoff
    CHECK(d.apply == -1 && d.want == 90 && par_deadline(&a, 2700, 1) == 3600);
    uint64_t t = 3600;
    for (uint32_t backoff = 4000; a.failures < 5; backoff *= 2) {
        CHECK(at(t).apply == 90);
        par_applied(&a, 90, 0, t);
        if (a.failures < 5) CHECK(a.retry_at == t + backoff);
        t = a.retry_at;
    }
    CHECK(a.gave_up && a.refused == 5 && at(t + 100000).apply == -1);
    // unconfirmed: taken but never shown -> a failure at the verify deadline, then tried again after the backoff
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1 && at(1600).apply == 90);
    par_applied(&a, 90, 1, 1600);
    CHECK(at(3599).apply == -1);
    d = at(3600);
    CHECK(d.apply == -1 && a.unconfirmed == 1 && a.retry_at == 5600 && a.asked == -1);
    CHECK(at(5600).apply == 90);

    // by hand while detached: adopted, automatic rotation waits until the tablet turns to another edge
    start(&c);
    CHECK(at(1000).apply == -1);
    par_observed(&a, 180, 2000);
    d = at(2000);
    CHECK(a.manual && d.touch == 180 && d.apply == -1);
    par_touch_done(&a, 180, 1, 2000);
    CHECK(at(10000).apply == -1 && par_deadline(&a, 10000, 1) == 0);     // the top edge is still up: the choice holds
    par_edge(&a, PS_EDGE_TOP, 11000);
    CHECK(a.manual && at(12000).apply == -1);                         // the same edge again: still held
    par_edge(&a, PS_EDGE_RIGHT, 13000);
    CHECK(!a.manual && at(13000).apply == -1 && at(13600).apply == 270);

    // the user's lock: nothing while locked; unlocked, the settled edge applies at once
    start(&c);
    par_lock(&a, 1);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1 && at(5000).apply == -1 && par_deadline(&a, 5000, 1) == 0);
    par_lock(&a, 0);
    CHECK(at(6000).apply == 90);

    // observing only (or the display asleep): wanted, not applied; later applied
    start(&c);
    par_edge(&a, PS_EDGE_BOTTOM, 1000);
    CHECK(par_step(&a, 1000, 0).apply == -1);
    CHECK(par_deadline(&a, 1000, 0) == 1600);
    d = par_step(&a, 1600, 0);
    CHECK(d.apply == -1 && d.want == 180);
    CHECK(par_deadline(&a, 1600, 0) == 0 && par_deadline(&a, 2600, 0) == 0);
    CHECK(at(2600).apply == 180 && at(2600).apply == 180);  // decisions alone never change the request time

    // touch: refused -> told again after touch_retry_ms
    start(&c);
    turn(PS_EDGE_LEFT, 1000, 90);
    par_observed(&a, 180, 3000);
    CHECK(at(3000).touch == 180);
    par_touch_done(&a, 180, 0, 3000);
    CHECK(at(4000).touch == -1 && par_deadline(&a, 4000, 1) == 8000 && at(8000).touch == 180);
    printf("PASS rotate: settle and interval, the cover (held, at attach, ignored, unknown), detach hold and bounce, reading back, "
           "backoff and giving up, rotations by hand, the lock, observing only, touch retries, deadlines\n");
}

static void test_orientation(void)
{
    const uint32_t gravity = sfx_float_from_ratio(981, 100);
    const uint32_t negative = 1u << 31;
    const uint32_t up[][3] = {{gravity, 0, 0}, {0, gravity, 0}, {gravity | negative, 0, 0}, {0, gravity | negative, 0}};
    const uint32_t diagonal = sfx_float_from_ratio(7, 1);
    const uint32_t diagonal_left = sfx_float_from_ratio(8, 1);
    const uint32_t reject[][3] = {
        {0, 0, 0},                                             // free fall
        {0, sfx_float_from_ratio(20, 1), 0},                   // shake in the proposed direction
        {0x7fc00000u, gravity, 0},                             // NaN cannot masquerade as a zero component
        {0, gravity, 0x7f800000u},
        {diagonal, diagonal, 0}, {diagonal, diagonal_left, 0}, // both axes inside the hysteresis margin
    };
    ParConfig c;
    const SfxAxes axes = {{1, 0, 2}, {0, 0, 0}};  // a+y+x+z
    SnoState state;
    par_defaults(&c);
    sno_init(&state);
    start(&c);
    for (int edge = PS_EDGE_TOP; edge <= PS_EDGE_RIGHT; ++edge) {
        const uint64_t t = 1000 + 3000 * (uint64_t)edge;
        CHECK(ps_accel_sample(&state, &axes, up[edge], t) == -1);
        CHECK(ps_accel_sample(&state, &axes, up[edge], t + SNO_HOLD_MS - 1) == -1);
        CHECK(ps_edge_of(ps_accel_sample(&state, &axes, up[edge], t + SNO_HOLD_MS)) == edge);
        if (edge > PS_EDGE_TOP) turn(edge, t + SNO_HOLD_MS, edge * 90);
    }
    turn(PS_EDGE_TOP, 15000, 0);
    sno_init(&state);
    CHECK(ps_accel_sample(&state, &axes, up[0], 16000) == -1);
    CHECK(ps_edge_of(ps_accel_sample(&state, &axes, up[0], 16250)) == PS_EDGE_TOP);
    for (unsigned i = 0; i < sizeof reject / sizeof reject[0]; ++i) {
        const uint64_t t = 17000 + 1000 * i;
        CHECK(ps_accel_sample(&state, &axes, up[0], t - 1) == -1);
        CHECK(ps_accel_sample(&state, &axes, up[1], t) == -1);
        CHECK(ps_accel_sample(&state, &axes, reject[i], t + 249) == -1);
        CHECK(ps_accel_sample(&state, &axes, up[1], t + 250) == -1);
        CHECK(ps_accel_sample(&state, &axes, up[1], t + 499) == -1);
        CHECK(ps_edge_of(state.state) == PS_EDGE_TOP && at(t + 499).apply == -1 && a.shown == 0);
    }
    turn(PS_EDGE_LEFT, 24000, 90);
    for (unsigned z = 0; z < 2; ++z) {
        const uint64_t t = 25000 + z * 2000;
        const uint32_t flat[3] = {sfx_float_from_ratio(1, 2), 0, gravity | (z ? negative : 0)};
        CHECK(ps_accel_sample(&state, &axes, flat, t) == -1);
        CHECK(ps_accel_sample(&state, &axes, flat, t + 250) == (z ? SNO_FACE_DOWN : SNO_FACE_UP));
        par_edge(&a, ps_edge_of(state.state), t + 250);
        CHECK(at(t + 1000).apply == -1 && a.shown == 90);
    }
    printf("PASS orientation: all SSC axis signs, sensor dwell boundary, diagonal bounce, shake/drop, nonfinite samples, flat hold\n");
}

static void test_rotation_transitions(void)
{
    ParConfig c;
    par_defaults(&c);
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1 && at(1600).apply == 90);
    par_applied(&a, 90, 1, 1600);
    par_cover(&a, PAR_COVER_ATTACHED, 1700);
    CHECK(at(1700).apply == -1 && at(1700).touch == -1);
    par_observed(&a, 180, 1750);  // an intervening read-back differs from the requested angle
    CHECK(at(1750).apply == -1 && at(1750).touch == 180 && a.asked == 90);
    par_touch_done(&a, 180, 0, 1750);
    par_observed(&a, 90, 1800);
    CHECK(at(1800).apply == -1 && at(1800).touch == 90 && a.confirmed == 1);
    par_touch_done(&a, 90, 1, 1800);
    CHECK(at(2599).apply == -1 && at(2600).apply == 0);
    par_applied(&a, 0, 1, 2600);
    par_observed(&a, 0, 2700);
    CHECK(at(2700).touch == 0);
    par_touch_done(&a, 0, 1, 2700);
    par_cover(&a, PAR_COVER_DETACHED, 3000);    // still held left-edge-up after detach
    CHECK(at(4499).apply == -1 && at(4500).apply == -1);
    CHECK(at(5099).apply == -1 && at(5100).apply == 90);
    par_observed(&a, -1, 5100);  // panel disappeared before the proposal was applied
    CHECK(at(5100).apply == -1 && at(5100).touch == -1 && par_deadline(&a, 5100, 0) == 0);
    par_observed(&a, 0, 5200);
    CHECK(at(5200).touch == 0);  // console ownership may have changed the touch angle meanwhile
    printf("PASS rotation transitions: cover during a request, touch follows read-back, detach in portrait, panel loss\n");
}

static void test_interrupted_rotation(void)
{
    ParConfig c;
    par_defaults(&c);
    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1);
    par_edge(&a, PS_EDGE_NONE, 1599);
    par_edge(&a, PS_EDGE_LEFT, 1600);
    CHECK(at(1600).apply == -1 && at(2199).apply == -1 && at(2200).apply == 90);

    start(&c);
    par_edge(&a, PS_EDGE_LEFT, 1000);
    CHECK(at(1000).apply == -1);
    par_observed(&a, -1, 1400);
    CHECK(at(1600).apply == -1);
    par_observed(&a, 0, 1700);
    CHECK(at(1700).apply == -1 && at(2299).apply == -1 && at(2300).apply == 90);

    c.rot.min_interval_ms = 0;
    par_init(&a, &c);
    par_observed(&a, 90, 1000);
    par_touch_done(&a, 90, 1, 1000);
    par_cover(&a, PAR_COVER_ATTACHED, 1000);
    CHECK(at(1000).apply == 0);
    par_applied(&a, 0, 0, 1000);
    par_cover(&a, PAR_COVER_DETACHED, 1100);
    CHECK(par_deadline(&a, 1100, 1) == 1100 + c.detach_hold_ms);
    par_cover(&a, PAR_COVER_ATTACHED, 1200);
    CHECK(at(1200).apply == -1 && par_deadline(&a, 1200, 1) == 1000 + c.retry_first_ms);
    CHECK(at(1000 + c.retry_first_ms).apply == 0);
    puts("PASS rotation interruptions: fresh dwell after edge or panel loss, cover reattach schedules its retry");
}

static void test_android_als(void)
{
    const uint32_t lux[] = {2, 4, 6, 8, 10, 15, 20, 25, 30, 82, 771, 1202, 2058, 5100, 8900, 11165};
    const uint32_t nits[] = {6, 15, 21, 27, 36, 52, 71, 87, 97, 101, 178, 236, 343, 465, 560, 600};
    for (unsigned i = 0; i < sizeof lux / sizeof lux[0]; ++i)
        CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 178, lux[i]) == pa_sdr_permille(pa_sdr_code(nits[i] << 16)));
    CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 921, 0) == pa_sdr_permille(921));   /* never below the lit floor */
    /* 2026-11-21: 0 lux is Android's 3.5 nits (above the 178 floor's 2.29), 1 lux 4.5; 56 lux halfway between 30 (97) and 82 (101) */
    CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 178, 0) == pa_sdr_permille(pa_sdr_code(7u << 15)) && pa_sdr_code(7u << 15) > 178);
    CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 178, 1) == pa_sdr_permille(pa_sdr_code(9u << 15)));
    CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 178, 56) == pa_sdr_permille(pa_sdr_code(99u << 16)));
    CHECK(ps_als_target_permille(PS_ALS_REFERENCE, 178, 99999) == pa_sdr_permille(1952));
    CHECK(ps_als_target_permille(PS_ALS_LEGACY, 921, 200) == ps_als_curve_permille(200));

    PalLight l;
    PasConfig cfg;
    pas_defaults(&cfg);
    pal_init(&l, &cfg);
    pal_conditions(&l, 1, 1, 1, 0);
    pas_sent(&l.stream, PAS_ACT_ENABLE, 1);
    pas_response(&l.stream, 1, 2);
    CHECK(pal_event(&l, sfx_float_from_ratio(15000, 1), 3) == 1000 && l.lux == 10000);
    CHECK(pal_select_curve(&l, PS_ALS_REFERENCE, 1, 921));
    CHECK(l.target == -1 && !l.als.has && l.tick_at == 0);
    CHECK(pal_event(&l, sfx_float_from_ratio(15000, 1), 4) == -1);
    CHECK(pas_action(&l.stream, 4) == PAS_ACT_DISABLE);
    pas_sent(&l.stream, PAS_ACT_DISABLE, 4);
    pas_response(&l.stream, 1, 5);
    pas_sent(&l.stream, PAS_ACT_ENABLE, 6);
    CHECK(pal_event(&l, sfx_float_from_ratio(15000, 1), 7) == (int32_t)pa_sdr_permille(1952) && l.lux == 11165);
    CHECK(l.stream.sessions == 2 && !pal_select_curve(&l, PS_ALS_REFERENCE, 1, 921));
    l.stream.state = PAS_FAILED;
    CHECK(pal_select_curve(&l, PS_ALS_LEGACY, 2, 921) && l.stream.state == PAS_FAILED);
    CHECK(pal_event(&l, 0, 8) == -1 && pal_tick(&l, 20000) == -1);
}

int main(void)
{
    test_android_als();
    test_touch();
    test_rotate();
    test_orientation();
    test_rotation_transitions();
    test_interrupted_rotation();
    stream_tests();
    printf("PASS PianoAuto C host tests (%u checks)\n", checks);
    return 0;
}
