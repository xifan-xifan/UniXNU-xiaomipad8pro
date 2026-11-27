// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Build: clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -working-directory "${stress_objects:=$(mktemp -d "${TMPDIR:-/tmp}/sensors-auto-stress.XXXXXXXX")}" -I"$PWD/.." -I"$PWD/../../include" -I"$PWD/../../PianoAuto" -c "$PWD"/../Sensors{Pb,Ssc,Fixed,Orient,Policy,Stream}.c "$PWD"/../../PianoAuto/Auto{Stream,Light,Rotate}.c && trap 'rm -r "$stress_objects"' EXIT && clang++ -std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../include -I../../PianoAuto sensors_auto_stress_host.cpp "$stress_objects"/*.o -o /tmp/sensors-auto-stress-host && /tmp/sensors-auto-stress-host

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <span>

#include "AmbientBacklight.h"
#include "AutoRotate.h"
#include "SensorsStream.h"
#include "check.h"

namespace {

constexpr uint64_t ticksPerMs = 19200;
constexpr std::array<SscSuid, SS_STREAMS> suids{{{1, 11}, {2, 22}}};
using Pose = std::array<float, 3>;
constexpr float gravity = 9.80665f;
constexpr Pose top{gravity, 0, 0}, left{0, gravity, 0}, right{0, -gravity, 0};
constexpr std::array<int, 4> edges{PS_EDGE_TOP, PS_EDGE_LEFT, PS_EDGE_BOTTOM, PS_EDGE_RIGHT};

Pose tilt(int degrees)
{
    const auto radians = degrees * std::numbers::pi / 180;
    return {float(gravity * std::cos(radians)), float(gravity * std::sin(radians)), 0};
}

struct Sample {
    uint64_t ticks;
    Pose values;
};

void put16(uint8_t *out, uint16_t value)
{
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
}

struct Hub {
    uint64_t now = 10000;
    SsEngine sensors{};
    SsConditions conditions{1, 1, 1, 1, 0, 1};  // Level 4, forced rotation consumer with bench cover acquisition.
    ParConfig rotation;
    ParAgent desktop;
    SsOutputs published{.target = -1, .edge = PS_EDGE_NONE};
    std::array<uint16_t, SS_STREAMS> txn{};
    std::array<unsigned, SS_STREAMS> sends{};
    unsigned errors = 0;
    unsigned leaves = 0;
    const SsOps ops{
        .context = this,
        .now_ms = [](void *ctx) {return static_cast<Hub *>(ctx)->now;},
        .send = [](void *ctx, int stream, const uint8_t *data, uint32_t length) {
            auto &h = *static_cast<Hub *>(ctx);
            SscQmi qmi;
            CHECK(ssc_qmi_parse(data, length, &qmi) == 0);
            CHECK(qmi.type == SSC_QMI_REQUEST && qmi.message == SSC_QMI_REQ);
            CHECK(qmi.transaction && qmi.transaction != h.txn[stream]);
            h.txn[stream] = qmi.transaction;
            ++h.sends[stream];
            return 0;
        },
        .may_request = [](void *) {return 1;},
        .result = [](void *ctx, int ok) {if (!ok) ++static_cast<Hub *>(ctx)->errors;},
        .trace = nullptr,
        .publish = [](void *ctx, const SsOutputs *out) {
            auto &h = *static_cast<Hub *>(ctx);
            CHECK(out->seq == h.published.seq + 1 && out->changed);
            CHECK(out->edge >= PS_EDGE_NONE && out->edge <= PS_EDGE_RIGHT);
            CHECK(!out->light_valid || (out->target >= 0 && out->target <= 1000));
            h.published = *out;
        },
        .leave = [](void *ctx) {++static_cast<Hub *>(ctx)->leaves;},
        .close_port = nullptr,
    };

    explicit Hub(bool sampleTime = false, uint16_t floor = PA_SDR_MIN_CODE)
    {
        SsConfig config;
        ss_defaults(&config);
        config.accel_sample_time = sampleTime;
        ss_init(&sensors, &ops, &config);
        CHECK(pal_select_curve(&sensors.light, PS_ALS_REFERENCE, 1, floor));
        par_defaults(&rotation);
        par_init(&desktop, &rotation);
        par_observed(&desktop, 0, now);
        par_cover(&desktop, PAR_COVER_DETACHED, now);
        ss_conditions(&sensors, &conditions);
        for (int stream : {SS_LIGHT, SS_ACCEL}) {
            ss_port_opened(&sensors, stream);
            ss_service(&sensors, stream, 1, 1);
            ss_sensor(&sensors, stream, 1, &suids[stream],
                      stream == SS_LIGHT ? SSC_STREAM_ON_CHANGE : SSC_STREAM_STREAMING, 0);
        }
        ss_tick(&sensors);
        CHECK(sensors.accel.outstanding == PAS_ACT_ENABLE && sensors.light.stream.outstanding == PAS_ACT_ENABLE);
        reply(SS_LIGHT, txn[SS_LIGHT]);
        reply(SS_ACCEL, txn[SS_ACCEL]);
    }

    void reply(int stream, uint16_t transaction)
    {
        uint8_t packet[] = {SSC_QMI_RESPONSE, 0, 0, SSC_QMI_REQ, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
        put16(packet + 1, transaction);
        ss_receive(&sensors, stream, packet, sizeof packet);
    }

    void report(int stream, std::span<const Sample> samples, bool truncated = false)
    {
        // QMI report with one event-message TLV containing its SUID and repeated SSC sample events.
        constexpr unsigned prefix = SSC_QMI_HEADER + 5;
        std::array<uint8_t, prefix + SSC_QMI_MAX_PAYLOAD> packet{SSC_QMI_INDICATION, 0, 0, SSC_QMI_REPORT, 0};
        SpbOut proto;
        spb_out(&proto, packet.data() + prefix, SSC_QMI_MAX_PAYLOAD);
        const auto suid = spb_open(&proto, 1);
        spb_put_fixed64(&proto, 1, suids[stream].low);
        spb_put_fixed64(&proto, 2, suids[stream].high);
        spb_close(&proto, suid);
        for (const auto &sample : samples) {
            const auto event = spb_open(&proto, 2);
            spb_put_fixed32(&proto, 1, SSC_MSG_SENSOR_EVENT);
            spb_put_fixed64(&proto, 2, sample.ticks);
            const auto payload = spb_open(&proto, 3);
            for (unsigned axis = 0; axis < (stream == SS_LIGHT ? 1u : 3u); ++axis)
                spb_put_fixed32(&proto, 1, float_bits(sample.values[axis]));
            spb_put_varint(&proto, 2, 3);  // high accuracy
            spb_close(&proto, payload);
            spb_close(&proto, event);
        }
        const auto length = spb_done(&proto);
        CHECK(length);
        put16(packet.data() + 5, uint16_t(length + 5));
        packet[7] = 2;
        put16(packet.data() + 8, uint16_t(length + 2));
        put16(packet.data() + 10, uint16_t(length));
        ss_receive(&sensors, stream, packet.data(), length + prefix - unsigned(truncated));
    }

    void accel(const Pose &pose)
    {
        const Sample sample{now * ticksPerMs, pose};
        report(SS_ACCEL, {&sample, 1});
    }

    void light(float lux)
    {
        const Sample sample{now * ticksPerMs, {lux, 0, 0}};
        report(SS_LIGHT, {&sample, 1});
    }

    void advance(uint64_t ms)
    {
        now += ms;
        ss_tick(&sensors);
    }

    ParDecision poll(bool active = true, bool canApply = true)
    {
        par_edge(&desktop, active ? published.edge : PS_EDGE_NONE, now);
        par_observed(&desktop, active ? display : -1, now);
        auto decision = par_step(&desktop, now, active && canApply);
        if (decision.touch >= 0) {
            CHECK(decision.touch == display);
            par_touch_done(&desktop, decision.touch, 1, now);
        }
        if (decision.apply >= 0) {
            par_applied(&desktop, decision.apply, 1, now);
            display = decision.apply;
            par_observed(&desktop, display, now);
        }
        return decision;
    }

    void desktopCover(bool attached)
    {
        par_cover(&desktop, attached ? PAR_COVER_ATTACHED : PAR_COVER_DETACHED, now);
    }

    void sensorCover(bool attached)
    {
        conditions.attached = attached;
        ss_conditions(&sensors, &conditions);
    }

    // Samples on both sides of the exact accelerometer dwell threshold.
    void settle(const Pose &pose, int before, int after)
    {
        const auto start = now;
        accel(pose);
        for (unsigned elapsed : {100u, SNO_HOLD_MS - 1u, SNO_HOLD_MS}) {
            advance(start + elapsed - now);
            CHECK(published.edge == before);
            accel(pose);
            CHECK(published.edge == (elapsed == SNO_HOLD_MS ? after : before));
        }
    }

    int display = 0;
};

void testJitter()
{
    for (bool sampleTime : {false, true}) {
        Hub h(sampleTime);
        h.light(0);
        h.settle(top, PS_EDGE_NONE, PS_EDGE_TOP);
        CHECK(h.poll().apply == -1);
        for (unsigned quarter = 0; quarter < edges.size(); ++quarter) {
            const int boundary = int(quarter) * 90 + 45;
            const int before = edges[quarter], after = edges[(quarter + 1) % edges.size()];
            // Cross the diagonal and each side's classification margin in short, irregular bursts.
            constexpr int offsets[] = {-2, 2, 12, 13, -12, -13, 0};
            for (unsigned i = 0; i < 350; ++i) {
                h.advance(7 + i % 19);
                h.accel(tilt(boundary + offsets[i % std::size(offsets)]));
                CHECK(h.published.edge == before && h.poll().apply == -1);
            }
            h.advance(20);
            h.settle(tilt(boundary + 45), before, after);
            CHECK(h.poll().apply == -1);
            h.advance(h.rotation.rot.settle_ms - 1);
            CHECK(h.poll().apply == -1);
            h.advance(1);
            const int degrees = (int(quarter) * 90 + 90) % 360;
            const auto observeOnly = h.poll(true, false);  // piano-rotate=1
            CHECK(observeOnly.want == degrees && observeOnly.apply == -1);
            CHECK(h.poll().apply == degrees && h.poll().apply == -1);
        }
        CHECK(h.desktop.applies == 4 && h.desktop.confirmed == 4);
    }
    puts("PASS four-way boundary jitter and exact sensor/desktop dwell");
}

void testCoverAndSleep()
{
    for (bool sampleTime : {false, true}) {
        Hub h(sampleTime);
        h.light(500);
        h.settle(left, PS_EDGE_NONE, PS_EDGE_LEFT);
        CHECK(h.poll().apply == -1);
        h.advance(h.rotation.rot.settle_ms);
        CHECK(h.poll().apply == 90);
        h.advance(h.rotation.rot.min_interval_ms);
        h.desktopCover(true);
        CHECK(h.published.edge == PS_EDGE_LEFT && h.poll().apply == 0);
        h.sensorCover(true);
        CHECK(h.published.edge == PS_EDGE_NONE);
        CHECK(ss_accel_wanted(&h.conditions) && h.sensors.accel.state == PAS_ON);
        h.advance(100);
        h.settle(right, PS_EDGE_NONE, PS_EDGE_RIGHT);  // bench acquisition keeps reporting while docked
        CHECK(h.poll().apply == -1 && h.display == 0);

        // The desktop can see detach before the stream thread handles the same notification.
        h.desktopCover(false);
        h.advance(250);
        h.accel(right);
        CHECK(h.published.edge == PS_EDGE_RIGHT && h.poll().apply == -1);
        h.sensorCover(false);
        CHECK(h.published.edge == PS_EDGE_NONE);
        h.desktopCover(true);
        h.sensorCover(true);
        for (unsigned bounce = 0; bounce < 32; ++bounce) {
            h.advance(20);
            h.sensorCover(false);
            h.desktopCover(false);
            CHECK(h.published.edge == PS_EDGE_NONE && h.poll().apply == -1);
            h.accel(right);
            h.advance(20);
            h.desktopCover(true);
            h.sensorCover(true);
            CHECK(h.poll().apply == -1);
        }
        h.sensorCover(false);
        h.desktopCover(false);
        h.advance(h.rotation.detach_hold_ms);
        CHECK(h.poll().apply == -1);
        h.advance(h.rotation.rot.settle_ms);
        CHECK(h.poll().apply == -1 && h.published.edge == PS_EDGE_NONE);
        h.settle(right, PS_EDGE_NONE, PS_EDGE_RIGHT);
        CHECK(h.poll().apply == -1);
        h.advance(h.rotation.rot.settle_ms);
        CHECK(h.poll().apply == 270 && h.sends[SS_ACCEL] == 1);

        // Suspend just before a new pose qualifies. A quick wake can reuse the same live QMI stream.
        h.advance(100);
        h.accel(left);
        h.advance(100);
        h.accel(left);
        ss_sleep(&h.sensors, 1);
        CHECK(h.published.edge == PS_EDGE_NONE && h.poll(false).apply == -1);
        const auto count = h.sensors.ch[SS_ACCEL].samples;
        h.accel(left);
        CHECK(h.sensors.ch[SS_ACCEL].samples == count);
        h.now += 10000;  // No tick while asleep, so no disable request has been sent.
        ss_sleep(&h.sensors, 0);
        CHECK(h.poll().apply == -1);
        h.settle(left, PS_EDGE_NONE, PS_EDGE_LEFT);
        CHECK(h.poll().apply == -1);
        h.advance(h.rotation.rot.settle_ms);
        CHECK(h.poll().apply == 90 && h.sends[SS_ACCEL] == 1);

        // A completed suspend disables both streams and wakes at the new pose and light level.
        ss_sleep(&h.sensors, 1);
        ss_tick(&h.sensors);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_DISABLE && h.poll(false).apply == -1);
        h.reply(SS_ACCEL, h.txn[SS_ACCEL]);
        CHECK(h.sensors.light.stream.outstanding == PAS_ACT_DISABLE);
        h.reply(SS_LIGHT, h.txn[SS_LIGHT]);
        CHECK(ss_idle(&h.sensors) && h.published.edge == PS_EDGE_NONE && !h.published.light_valid);
        h.now += 5000;
        ss_sleep(&h.sensors, 0);
        ss_tick(&h.sensors);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_ENABLE && h.sensors.light.stream.outstanding == PAS_ACT_ENABLE);
        h.reply(SS_ACCEL, h.txn[SS_ACCEL]);
        h.reply(SS_LIGHT, h.txn[SS_LIGHT]);
        h.light(0);
        CHECK(h.published.light_valid && h.published.lux == 0);
        h.settle(top, PS_EDGE_NONE, PS_EDGE_TOP);
        CHECK(h.poll().apply == -1);
        h.advance(h.rotation.rot.settle_ms);
        CHECK(h.poll().apply == 0);
    }
    puts("PASS cover lock, contact bounce, post-detach samples and quick sleep/wake");
}

int checkLight(const Hub &h)
{
    CHECK(h.published.light_valid && !h.published.light_fault);
    const auto target = h.published.target;
    CHECK(target >= 0 && target <= 1000);
    return target;
}

void testLight()
{
    Hub h;
    h.light(0);
    const int dark = checkLight(h);
    CHECK(dark == int(pa_sdr_permille(pa_sdr_code(7u << 15))));  // Android's 3.5-nit dark-room target
    const int sun = int(pa_sdr_permille(PA_SDR_MAX_CODE));
    int previous = dark;
    unsigned steps = 0;
    for (unsigned lux = 1; lux <= 100000; ++lux) {
        h.now += 10;
        h.light(float(lux));
        const int next = checkLight(h);
        CHECK(next >= previous && next <= sun);
        steps += next != previous;
        previous = next;
    }
    CHECK(steps > 20);
    CHECK(h.sensors.light.als.goal == unsigned(sun) && previous == sun);
    pianoauto::Ambient backlight;
    backlight.configure(true, PA_SDR_MIN_CODE, true, 16, PA_SDR_MAX_CODE);
    CHECK(backlight.target(sun, h.published.lux) && backlight.code() == PA_SDR_MAX_CODE);
    const auto ceilingLux = ps_als_end_lux(PS_ALS_REFERENCE);
    CHECK(h.published.lux == ceilingLux);  // the Android curve saturates before direct sun
    const auto seq = h.published.seq;
    const auto dropped = h.sensors.ch[SS_LIGHT].dropped;
    const auto samples = h.sensors.ch[SS_LIGHT].samples;
    const auto inf = std::numeric_limits<float>::infinity();
    for (float invalid : {-1.0f, -std::numeric_limits<float>::denorm_min(), -inf, inf,
                          bits_float(0x7fc00001u), bits_float(0xff800001u)}) {
        h.advance(PS_ALS_PUBLISH_MS);
        h.light(invalid);
        CHECK(h.published.seq == seq && h.sensors.ch[SS_LIGHT].samples == samples);
    }
    CHECK(h.sensors.ch[SS_LIGHT].dropped == dropped + 6 && h.sensors.light.invalid == 6);
    h.light(std::numeric_limits<float>::max());
    CHECK(h.sensors.ch[SS_LIGHT].samples == samples + 1 && h.published.lux == ceilingLux && checkLight(h) == sun);

    h.light(-0.0f);  // sudden shadow from the cover; negative zero is valid darkness
    CHECK(checkLight(h) == sun && h.sensors.light.lux == 0);
    for (unsigned i = 0; i < 150; ++i) {
        h.advance(PS_ALS_PUBLISH_MS);
        const int next = checkLight(h);
        CHECK(next <= previous && next >= dark);
        if (!i) CHECK(next < sun && next > dark);
        previous = next;
    }
    CHECK(h.published.lux == 0 && previous == dark && pal_settled(&h.sensors.light));

    // The dark-side Android anchors must be above the lit floor and increase through a dim room.
    struct Anchor {unsigned lux, nits;};
    for (const Anchor anchor : {Anchor{4, 15}, {10, 36}, {30, 97}}) {
        h.light(float(anchor.lux));
        for (unsigned i = 0; i < 60; ++i) h.advance(PS_ALS_PUBLISH_MS);
        CHECK(checkLight(h) > previous && h.published.lux == anchor.lux);
        CHECK(checkLight(h) == int(pa_sdr_permille(pa_sdr_code(anchor.nits << 16))));
        previous = checkLight(h);
    }

    constexpr uint16_t raisedFloor = 400;
    Hub raised(false, raisedFloor);
    raised.light(0);
    CHECK(checkLight(raised) == int(pa_sdr_permille(raisedFloor)));
    backlight.configure(true, raisedFloor, true, 16, PA_SDR_MAX_CODE);
    CHECK(backlight.target(checkLight(raised), 0) && backlight.code() == raisedFloor);
    puts("PASS every integer lux to 100,000, invalid inputs, smooth fade and Android dim-room targets");
}

void testBurstAndRollover()
{
    Hub h(true);
    constexpr uint64_t period = 100 * ticksPerMs;
    constexpr uint64_t dwell = SNO_HOLD_MS * ticksPerMs;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    const Sample wrap[] = {{max - 2 * period, left}, {max - period, left}, {max, left},
                           {0, left}, {1, left}, {1, right}, {1 + period, left}, {1 + 2 * period, left}, {dwell, left}};
    const auto seq = h.published.seq;
    h.report(SS_ACCEL, wrap, true);
    CHECK(h.sensors.ch[SS_ACCEL].malformed == 1 && h.published.seq == seq);
    h.report(SS_ACCEL, wrap);
    CHECK(h.published.edge == PS_EDGE_NONE && h.sensors.ch[SS_ACCEL].samples == std::size(wrap));
    const Sample qualified{dwell + 1, left};
    h.report(SS_ACCEL, {&qualified, 1});
    CHECK(h.published.edge == PS_EDGE_LEFT && h.published.seq == seq + 1);

    // Batched reports alternate quadrants faster than the orientation hold, with one receive time per report.
    std::array<Sample, 24> burst;
    uint64_t ticks = qualified.ticks;
    for (unsigned report = 0; report < 128; ++report) {
        for (unsigned i = 0; i < burst.size(); ++i) {
            ticks += 15 * ticksPerMs;
            burst[i] = {ticks, tilt(int(i % 4) * 90)};
        }
        h.report(SS_ACCEL, burst);
        CHECK(h.published.edge == PS_EDGE_LEFT);
    }
    CHECK(h.sensors.ch[SS_ACCEL].samples == std::size(wrap) + 1 + 128 * burst.size());

    puts("PASS SSC packet bursts and fixed64 timestamp rollover");
}

void testRecovery(bool sampleTime)
{
    Hub h(sampleTime);
    h.settle(left, PS_EDGE_NONE, PS_EDGE_LEFT);
    h.light(100000);
    const auto full = checkLight(h);
    const Sample late{h.now * ticksPerMs, left};

    for (uint32_t generation = 2; generation <= 32; generation += 2) {
        const auto oldTxn = h.txn[SS_ACCEL];
        ss_service(&h.sensors, SS_ACCEL, 0, generation);
        ss_service(&h.sensors, SS_LIGHT, 0, generation);
        const auto offSeq = h.published.seq;
        const auto oldSends = h.sends;
        h.advance(5000);
        h.report(SS_ACCEL, {&late, 1});
        h.light(1);
        h.reply(SS_ACCEL, oldTxn);
        CHECK(h.published.seq == offSeq && h.sends == oldSends && !h.published.light_valid);
        CHECK(h.published.edge == PS_EDGE_NONE && !h.sensors.last_accel_ticks && h.poll().apply == -1);

        ss_service(&h.sensors, SS_ACCEL, 1, generation + 1);
        ss_service(&h.sensors, SS_LIGHT, 1, generation + 1);
        ss_tick(&h.sensors);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_ENABLE && h.sensors.light.stream.outstanding == PAS_ACT_ENABLE);
        h.reply(SS_LIGHT, h.txn[SS_LIGHT]);
        h.light(0);
        CHECK(checkLight(h) < full && h.published.lux == 0);  // recovery opens at the new room level
        h.reply(SS_ACCEL, oldTxn);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_ENABLE);

        // SSC can report before its QMI response. If that response times out, retract the provisional edge.
        h.settle(left, PS_EDGE_NONE, PS_EDGE_LEFT);
        const auto timedOut = h.txn[SS_ACCEL];
        h.advance(h.sensors.cfg.stream.response_ms - SNO_HOLD_MS - 1);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_ENABLE);
        h.advance(1);
        CHECK(h.sensors.ch[SS_ACCEL].timeouts == generation / 2 && h.errors == generation / 2);
        CHECK(h.published.edge == PS_EDGE_NONE && h.sensors.accel.dirty);
        h.advance(h.sensors.cfg.stream.retry_first_ms - 1);
        CHECK(h.txn[SS_ACCEL] == timedOut);
        h.advance(1);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_DISABLE);
        h.reply(SS_ACCEL, timedOut);
        h.report(SS_ACCEL, {&late, 1});
        CHECK(h.sensors.accel.outstanding == PAS_ACT_DISABLE && h.published.edge == PS_EDGE_NONE);
        h.reply(SS_ACCEL, h.txn[SS_ACCEL]);
        ss_tick(&h.sensors);
        CHECK(h.sensors.accel.outstanding == PAS_ACT_ENABLE);
        h.reply(SS_ACCEL, h.txn[SS_ACCEL]);
        h.settle(right, PS_EDGE_NONE, PS_EDGE_RIGHT);
        CHECK(h.sensors.accel.state == PAS_ON && !h.sensors.accel.dirty && !h.sensors.accel.fails && !h.leaves);
        CHECK(h.poll().apply == -1);
        h.light(100000);
    }
    puts("PASS QMI timeout, late replies and 16 reconnects");
}

}  // namespace

int main()
{
    testJitter();
    testCoverAndSleep();
    testLight();
    testBurstAndRollover();
    for (bool sampleTime : {false, true}) testRecovery(sampleTime);
    printf("PASS sensors/auto stress (%u checks)\n", checks);
}
