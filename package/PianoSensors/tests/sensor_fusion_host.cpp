// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Build: clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -working-directory "${fusion_objects:=$(mktemp -d "${TMPDIR:-/tmp}/sensor-fusion.XXXXXXXX")}" -I"$PWD/.." -I"$PWD/../../include" -I"$PWD/../../PianoAuto" -c "$PWD"/../Sensors{Pb,Ssc,Fixed,Orient,Policy,Stream}.c "$PWD"/../../PianoAuto/Auto{Stream,Light,Rotate}.c && trap 'rm -r "$fusion_objects"' EXIT && clang++ -std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../include -I../../PianoAuto sensor_fusion_host.cpp "$fusion_objects"/*.o -o /tmp/sensor-fusion-host && /tmp/sensor-fusion-host

#include <initializer_list>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "SensorsFusionModel.h"
#include "check.h"

using namespace sensorsfusion;

using Bytes = std::vector<uint8_t>;
using Pose = std::array<float, 3>;
constexpr float gravity = 9.80665f;
constexpr Pose top{gravity, 0, 0}, left{0, gravity, 0}, bottom{-gravity, 0, 0}, right{0, -gravity, 0};
constexpr float nanSample = std::numeric_limits<float>::quiet_NaN();
constexpr float infSample = std::numeric_limits<float>::infinity();

struct Proto {
    std::array<uint8_t, 256> storage;
    SpbOut out;
    Proto() { spb_out(&out, storage.data(), storage.size()); }

    Bytes bytes() const
    {
        const auto length = spb_done(&out);
        CHECK(length > 0);
        return Bytes(storage.begin(), storage.begin() + length);
    }
};

template <typename Values>
static Bytes floats(const Values &values, uint64_t status = 3)
{
    Proto proto;
    for (float value : values) spb_put_fixed32(&proto.out, 1, float_bits(value));
    spb_put_varint(&proto.out, 2, status);
    return proto.bytes();
}

static Bytes floats(std::initializer_list<float> values, uint64_t status = 3)
{
    return floats<std::initializer_list<float>>(values, status);
}

static Bytes proximity(uint64_t type, uint64_t adc = 123, uint64_t status = 3)
{
    Proto proto;
    spb_put_varint(&proto.out, 1, type);
    spb_put_varint(&proto.out, 2, adc);
    spb_put_varint(&proto.out, 3, status);
    return proto.bytes();
}

struct Event {
    uint32_t id;
    uint64_t ticks;
    Bytes payload;
};

static void append16(Bytes &bytes, uint16_t value)
{
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8));
}

static Bytes qmiReport(const Bytes &body)
{
    Bytes bytes = {SSC_QMI_INDICATION, 0, 0};
    append16(bytes, SSC_QMI_REPORT);
    append16(bytes, uint16_t(body.size() + 5));
    bytes.push_back(0x02);  // The report's event-message TLV. Client ID is optional.
    append16(bytes, uint16_t(body.size() + 2));
    append16(bytes, uint16_t(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

static Bytes report(Stream stream, std::initializer_list<Event> events)
{
    Proto proto;
    const auto &suid = fixtureSuids[index(stream)];
    const auto identity = spb_open(&proto.out, 1);
    spb_put_fixed64(&proto.out, 1, suid.low);
    spb_put_fixed64(&proto.out, 2, suid.high);
    spb_close(&proto.out, identity);
    for (const auto &event : events) {
        const auto entry = spb_open(&proto.out, 2);
        spb_put_fixed32(&proto.out, 1, event.id);
        if (event.ticks) spb_put_fixed64(&proto.out, 2, event.ticks);
        spb_put_bytes(&proto.out, 3, event.payload.data(), event.payload.size());
        spb_close(&proto.out, entry);
    }
    return qmiReport(proto.bytes());
}

struct Rig {
    Model model;
    uint64_t now = 1000;
    uint32_t generation = 1;

    Rig() { model.island(true, generation, now); }

    unsigned receive(Stream stream, const Bytes &packet)
    {
        return model.receive(stream, generation, packet.data(), packet.size(), now);
    }

    unsigned sample(Stream stream, const Bytes &payload, uint64_t ticks, uint32_t id = SSC_MSG_SENSOR_EVENT)
    {
        return receive(stream, report(stream, {{id, ticks, payload}}));
    }

    int pose(uint64_t elapsed, const Pose &value, bool accepted = true)
    {
        now += elapsed;
        CHECK(sample(Stream::Accel, floats(value), now * ticksPerMs) == unsigned(accepted));
        return model.tick(now);
    }

    std::vector<int> hold(const Pose &value, unsigned samples = 13, bool accepted = true)
    {
        std::vector<int> requests;
        for (unsigned i = 0; i < samples; ++i) {
            const int request = pose(100, value, accepted);
            if (request >= 0) requests.push_back(request);
        }
        return requests;
    }

    void lux(uint64_t elapsed, float value, bool accepted = true)
    {
        now += elapsed;
        CHECK(sample(Stream::Light, floats({value}), now * ticksPerMs) == unsigned(accepted));
        CHECK(model.tick(now) == -1);
    }
};

// Unit down-gravity in the panel frame, encoded as SSC up-vector in m/s².
static Pose panel(float x, float y, float z) { return {-y * gravity, -x * gravity, -z * gravity}; }

static Pose diagonal(float degrees)
{
    const float theta = degrees * std::numbers::pi_v<float> / 180;
    return panel(-std::sin(theta), -std::cos(theta), 0);
}

static Pose tilt(float degreesFromFlat, int face)
{
    const float theta = degreesFromFlat * std::numbers::pi_v<float> / 180;
    return panel(-std::sin(theta), 0, face * std::cos(theta));
}

static void orientations()
{
    Rig rig;
    CHECK(rig.hold(top).empty());
    CHECK(rig.model.orientation() == SNO_PORTRAIT && rig.model.edge() == PS_EDGE_TOP && rig.model.rotation() == 0);
    CHECK(rig.hold(left) == std::vector<int>{90});
    CHECK(rig.model.orientation() == SNO_LANDSCAPE_LEFT && rig.model.edge() == PS_EDGE_LEFT);
    CHECK(rig.hold(bottom) == std::vector<int>{180});
    CHECK(rig.model.orientation() == SNO_UPSIDE_DOWN && rig.model.edge() == PS_EDGE_BOTTOM);
    CHECK(rig.hold(right) == std::vector<int>{270});
    CHECK(rig.model.orientation() == SNO_LANDSCAPE_RIGHT && rig.model.edge() == PS_EDGE_RIGHT);

    for (int face : {-1, 1}) {
        Rig cold;
        CHECK(cold.hold(tilt(0, face)).empty() && cold.model.rotation() == 0 && cold.model.edge() == PS_EDGE_NONE);
        for (float degrees : {0, 10, -10, 31, -31}) {
            CHECK(rig.hold(tilt(degrees, face)).empty());
            CHECK(rig.model.rotation() == 270 && rig.model.edge() == PS_EDGE_NONE);
            CHECK(rig.model.orientation() == (face < 0 ? SNO_FACE_UP : SNO_FACE_DOWN));
        }
        // The production |z|² / |g|² >= .72 face cutoff is about 32 degrees from flat.
        CHECK(rig.hold(tilt(33, face)) == std::vector<int>{90});
        CHECK(rig.hold(right) == std::vector<int>{270});
    }

    CHECK(rig.hold(top) == std::vector<int>{0});
    // The 1.3 axis dominance ratio crosses at 52.43 degrees going out, 37.57 coming back.
    for (unsigned i = 0; i < 60; ++i) CHECK(rig.hold(diagonal(38 + i % 15), 3).empty());
    CHECK(rig.hold(diagonal(53)) == std::vector<int>{90});
    CHECK(rig.hold(diagonal(38)).empty() && rig.model.rotation() == 90);
    CHECK(rig.hold(diagonal(37)) == std::vector<int>{0});
}

static void debounce()
{
    Rig rig;
    CHECK(rig.hold(top).empty());
    for (unsigned i = 0; i < 300; ++i) {
        CHECK(rig.pose(50, left) == -1);
        CHECK(rig.pose(199, left) == -1);
        CHECK(rig.pose(50, top) == -1);
        CHECK(rig.model.edge() == PS_EDGE_TOP && rig.model.rotation() == 0);
    }
    CHECK(rig.pose(50, left) == -1 && rig.pose(100, left) == -1 && rig.pose(149, left) == -1);
    CHECK(rig.model.edge() == PS_EDGE_TOP);
    CHECK(rig.pose(1, left) == -1 && rig.model.edge() == PS_EDGE_LEFT);  // 250 ms of SSC dwell.
    CHECK(rig.hold(left, 5).empty() && rig.pose(99, left) == -1);
    CHECK(rig.pose(1, left) == 90);  // 600 ms display settle.

    CHECK(rig.pose(1, bottom) == -1 && rig.pose(100, bottom) == -1 && rig.pose(150, bottom) == -1);
    CHECK(rig.model.edge() == PS_EDGE_BOTTOM);
    CHECK(rig.hold(bottom, 7).empty() && rig.pose(48, bottom) == -1);
    CHECK(rig.pose(1, bottom) == 180);  // 1000 ms minimum between changes.

    // The classifier can publish an edge that never makes it through desktop settle.
    CHECK(rig.hold(right, 4).empty() && rig.model.edge() == PS_EDGE_RIGHT);
    CHECK(rig.hold(tilt(0, -1)).empty() && rig.model.edge() == PS_EDGE_NONE && rig.model.rotation() == 180);
}

static void coverGate()
{
    Rig rig;
    CHECK(rig.hold(left) == std::vector<int>{90});
    CHECK(rig.hold(left).empty());
    rig.model.cover(true, false, rig.now);
    CHECK(rig.model.edge() == PS_EDGE_NONE && rig.model.tick(rig.now) == 0);
    CHECK(rig.hold(right, 30, false).empty() && rig.model.edge() == PS_EDGE_NONE);

    rig.model.cover(true, true, rig.now);
    CHECK(rig.hold(right, 30).empty());
    CHECK(rig.model.edge() == PS_EDGE_RIGHT && rig.model.rotation() == 0);
    CHECK(rig.hold(left, 30).empty());
    CHECK(rig.model.edge() == PS_EDGE_LEFT && rig.model.rotation() == 0);

    rig.model.cover(false, false, rig.now);
    CHECK(rig.hold(left, 14).empty() && rig.pose(99, left) == -1 && rig.pose(1, left) == -1);  // Detach hold ends at 1500 ms.
    CHECK(rig.hold(left, 5).empty() && rig.pose(99, left) == -1);
    CHECK(rig.pose(1, left) == 90);  // Bench edge still has to settle for another 600 ms.
    CHECK(rig.hold(left).empty());

    rig.model.cover(true, false, rig.now);
    CHECK(rig.model.tick(rig.now) == 0 && rig.model.edge() == PS_EDGE_NONE);
    rig.model.cover(false, false, rig.now + 1);
    CHECK(rig.hold(right, 3).empty() && rig.model.edge() == PS_EDGE_NONE);
    CHECK(rig.pose(50, right) == -1 && rig.model.edge() == PS_EDGE_RIGHT);
    CHECK(rig.hold(right, 21) == std::vector<int>{270});
}

static int32_t target(uint32_t lux)
{
    return int32_t(referenceMillinits(ps_als_target_permille(PS_ALS_REFERENCE, PA_SDR_MIN_CODE, lux)));
}

static void lightCurve()
{
    // Android's pinned SDR targets in SensorsPolicy.c, before code/permille quantization.
    const std::array<std::array<uint32_t, 2>, 9> anchors = {{
        {0, 3500}, {1, 4500}, {4, 15000}, {15, 52000}, {30, 97000},
        {82, 101000}, {771, 178000}, {5100, 465000}, {11165, 600000},
    }};
    CHECK(ps_als_end_lux(PS_ALS_REFERENCE) == 11165);
    for (const auto &[lux, milli] : anchors) {
        CHECK(ps_als_target_permille(PS_ALS_REFERENCE, PA_SDR_MIN_CODE, lux) ==
              pa_sdr_permille(pa_sdr_code(uint32_t((uint64_t(milli) << 16) / 1000))));
        CHECK(std::abs(target(lux) - int32_t(milli)) <= 2000);  // Fixture tolerance for the quantized round trip.
    }
    const auto raisedFloor = pa_sdr_code(8u << 16);
    CHECK(referenceMillinits(ps_als_target_permille(PS_ALS_REFERENCE, raisedFloor, 0), raisedFloor) ==
          pa_sdr_millinits(pa_sdr_floor_fixed(raisedFloor)));
    int32_t previous = target(0);
    for (uint32_t lux = 1; lux <= 20000; ++lux) {
        const int32_t milli = target(lux);
        CHECK(milli >= previous && milli <= 600000);
        previous = milli;
    }
    CHECK(previous == 600000 && target(uint32_t(fixtureLuxLimit)) == previous && target(UINT32_MAX) == previous);
}

static void lightDynamics()
{
    Rig rig;
    const int32_t dark = target(0);
    rig.lux(250, 12000);   // An isolated first reading cannot take over at startup.
    CHECK(!rig.model.lightValid() && rig.model.brightness() == -1);
    rig.lux(250, 0);
    CHECK(!rig.model.lightValid());
    rig.lux(250, 0);
    CHECK(rig.model.lightValid() && rig.model.brightness() == dark);

    for (float bad : {nanSample, infSample, -infSample, -1.0f, std::numeric_limits<float>::max(), fixtureLuxLimit + 1}) {
        const auto last = rig.model.lastTicks(Stream::Light);
        rig.lux(250, bad, false);
        CHECK(rig.model.lastTicks(Stream::Light) == last && rig.model.brightness() == dark);
    }
    for (unsigned i = 0; i < 300; ++i) {
        rig.lux(250, fixtureLuxLimit);  // Valid outdoor reading, isolated in the dark.
        CHECK(rig.model.brightness() == dark);
        rig.lux(250, 0);
        CHECK(rig.model.brightness() == dark);
    }
    for (unsigned i = 0; i < 8; ++i) {
        rig.lux(250, i % 2 ? 0.4f : 0.0f);  // Sub-lux dither remains in the dark curve's first bin.
        CHECK(rig.model.brightness() == dark);
    }
    Rig dim;
    dim.lux(250, 82);
    dim.lux(250, 82);
    CHECK(target(100) > target(82));
    for (unsigned i = 0; i < 12; ++i) {
        dim.lux(250, 100);
        CHECK(dim.model.brightness() == target(82));  // Both reports pass confirmation, but stay inside the policy's deadband.
    }

    rig.lux(250, 12000);
    CHECK(rig.model.brightness() == dark);
    rig.lux(250, 12000);
    const int32_t peak = rig.model.brightness();
    CHECK(peak > dark && peak < 600000);
    rig.lux(250, 0);  // A lone dark reading does not reverse the brightening ramp.
    CHECK(rig.model.brightness() >= peak);
    int32_t previous = rig.model.brightness();
    for (unsigned i = 0; i < 160; ++i) {
        rig.lux(250, 0);
        CHECK(rig.model.brightness() <= previous && rig.model.brightness() >= dark);
        previous = rig.model.brightness();
    }
    CHECK(previous == dark);

    for (unsigned i = 0; i < 60; ++i) {
        rig.lux(250, 12000);
        CHECK(rig.model.brightness() >= previous);
        previous = rig.model.brightness();
    }
    CHECK(previous == 600000);
    rig.lux(250, 0);
    CHECK(rig.model.brightness() == previous);
}

static void validation()
{
    Rig rig;
    const Bytes malformed{0x0a, 0x05, 0x00};  // Truncated packed float field.
    const Bytes tooMany = floats(std::array<float, SSC_MAX_DATA + 1>{});
    for (auto [stream, limit] : {std::pair{Stream::Accel, fixtureAccelLimit}, std::pair{Stream::Gyro, fixtureGyroLimit},
                                 std::pair{Stream::Mag, fixtureMagLimit}}) {
        const uint64_t ticks = rig.now * ticksPerMs;
        const auto valid = report(stream, {{SSC_MSG_SENSOR_EVENT, ticks, floats({-limit, 0, limit}, 0)}});
        for (size_t n = 0; n < valid.size(); ++n) CHECK(rig.receive(stream, Bytes(valid.begin(), valid.begin() + n)) == 0);
        for (const Bytes &payload : {malformed, tooMany, floats({0, 0}), floats({0, 0, 0, 0}), floats({0, 0, 0}, 4),
                                    floats({limit + 1, 0, 0}), floats({0, -limit - 1, 0}),
                                    floats({0, nanSample, 0}), floats({0, 0, infSample}), floats({-infSample, 0, 0})}) {
            CHECK(rig.sample(stream, payload, ticks) == 0);
            CHECK(rig.model.lastTicks(stream) == 0);
        }
        CHECK(rig.receive(stream, valid) == 1 && rig.model.lastTicks(stream) == ticks);
        CHECK(rig.receive(stream, valid) == 0);
    }
    CHECK(rig.model.edge() == PS_EDGE_NONE && !rig.model.lightValid());

    const auto far = proximity(SSC_PROXIMITY_FAR, 0);
    const auto near = proximity(SSC_PROXIMITY_NEAR, fixtureProximityAdcLimit);
    const auto prox = SSC_MSG_PROXIMITY_EVENT;
    CHECK(rig.sample(Stream::Proximity, far, 10, prox) == 1);
    const auto packet = report(Stream::Proximity, {{prox, 20, near}});
    for (size_t n = 0; n < packet.size(); ++n) CHECK(rig.receive(Stream::Proximity, Bytes(packet.begin(), packet.begin() + n)) == 0);
    for (const Bytes &bad : {Bytes{0x08, 0x80}, proximity(2), proximity(1, uint64_t(fixtureProximityAdcLimit) + 1),
                             proximity(0, 1, UINT64_MAX), floats({nanSample, 0, 0})})
        CHECK(rig.sample(Stream::Proximity, bad, 20, prox) == 0);
    CHECK(rig.sample(Stream::Proximity, far, 20) == 0);
    CHECK(rig.receive(Stream::Mag, packet) == 0);  // Valid report on the wrong port.
    CHECK(rig.model.lastTicks(Stream::Proximity) == 10);
    CHECK(rig.receive(Stream::Proximity, packet) == 1);

    const auto body = Bytes(packet.begin() + SSC_QMI_HEADER + 5, packet.end());
    CHECK(rig.receive(Stream::Proximity, qmiReport(Bytes(body.begin(), body.end() - 1))) == 0);  // Truncated protobuf envelope.
    CHECK(rig.sample(Stream::Proximity, far, 15, prox) == 0 && rig.model.lastTicks(Stream::Proximity) == 20);

    const auto batch = report(Stream::Light, {{SSC_MSG_SENSOR_EVENT, 100, floats({0, 35})},
                                              {SSC_MSG_SENSOR_EVENT, 300, malformed},
                                              {SSC_MSG_SENSOR_EVENT, 200, floats({0})}});
    CHECK(rig.receive(Stream::Light, batch) == 2 && rig.model.lastTicks(Stream::Light) == 200);
    CHECK(rig.model.lightValid() && rig.model.brightness() == target(0));
}

static void sampleClock()
{
    Rig rig;
    const auto face = floats(left);
    const auto conflicting = floats(right);
    constexpr uint64_t period = 100 * ticksPerMs, dwell = SNO_HOLD_MS * ticksPerMs;
    const uint64_t start = UINT64_MAX - 2 * period;
    auto send = [&](uint64_t ticks, const Bytes &payload) { return rig.sample(Stream::Accel, payload, ticks); };
    CHECK(send(start, face) == 1);
    CHECK(send(start + period, face) == 1);
    CHECK(send(start + period, conflicting) == 0);
    CHECK(send(start + period - 1, conflicting) == 0);
    CHECK(send(UINT64_MAX, face) == 1 && rig.model.edge() == PS_EDGE_NONE);
    CHECK(send(0, face) == 0 && rig.model.lastTicks(Stream::Accel) == UINT64_MAX);
    CHECK(send(1, face) == 1 && rig.model.edge() == PS_EDGE_NONE);  // The pre-wrap dwell cannot carry over.
    CHECK(send(UINT64_MAX, conflicting) == 0 && rig.model.lastTicks(Stream::Accel) == 1);
    CHECK(send(1 + (uint64_t{1} << 63), conflicting) == 0);  // Ambiguous half-range distance.
    CHECK(send(1 + period, face) == 1 && rig.model.edge() == PS_EDGE_NONE);
    CHECK(send(10, conflicting) == 0 && send(1 + period, conflicting) == 0);
    CHECK(send(1 + 2 * period, face) == 1 && rig.model.edge() == PS_EDGE_NONE);
    CHECK(send(dwell, face) == 1 && rig.model.edge() == PS_EDGE_NONE);
    CHECK(send(1 + dwell, face) == 1 && rig.model.edge() == PS_EDGE_LEFT);

    const uint64_t next = 1 + dwell + period;
    CHECK(send(next, conflicting) == 1);
    CHECK(send(next + 2 * period + 1, conflicting) == 1);  // One tick above the 200 ms gap limit.
    CHECK(send(next + dwell, conflicting) == 1 && rig.model.edge() == PS_EDGE_LEFT);
    CHECK(send(next + 2 * period + 1 + dwell - period, conflicting) == 1);
    CHECK(send(next + 2 * period + 1 + dwell, conflicting) == 1 && rig.model.edge() == PS_EDGE_RIGHT);
}

static void islandRecovery()
{
    Rig rig;
    CHECK(rig.hold(left) == std::vector<int>{90});
    rig.lux(250, 12000);
    rig.lux(250, 12000);
    CHECK(rig.model.brightness() == 600000);
    CHECK(rig.hold(right, 4).empty() && rig.model.edge() == PS_EDGE_RIGHT);  // Desktop settle is still pending.
    rig.model.island(false, rig.generation, rig.now);
    CHECK(!rig.model.lightValid() && rig.model.edge() == PS_EDGE_NONE);
    CHECK(rig.hold(right, 20, false).empty());
    rig.lux(250, 0, false);
    CHECK(rig.model.rotation() == 90 && rig.model.brightness() == 600000);

    constexpr auto sampleId = SSC_MSG_SENSOR_EVENT;
    const auto old = report(Stream::Accel, {{sampleId, rig.now * ticksPerMs, floats(left)}});
    rig.model.island(true, ++rig.generation, rig.now);
    CHECK(rig.model.receive(Stream::Accel, rig.generation - 1, old.data(), old.size(), rig.now) == 0);
    CHECK(rig.model.lastTicks(Stream::Accel) == 0);
    CHECK(rig.sample(Stream::Light, floats({0}), 10) == 1);
    rig.model.island(true, rig.generation, rig.now);  // Repeated service notification is harmless.
    CHECK(!rig.model.lightValid() && rig.model.brightness() == 600000);
    CHECK(rig.sample(Stream::Light, floats({0}), 20) == 1);
    CHECK(rig.model.lightValid() && rig.model.brightness() == target(0));

    for (auto ms : {1u, 101u, 201u, 250u, 251u}) {
        rig.now += 100;
        CHECK(rig.sample(Stream::Accel, floats(right), ms * ticksPerMs) == 1);
        CHECK(rig.model.edge() == (ms == 251 ? PS_EDGE_RIGHT : PS_EDGE_NONE));
        CHECK(rig.model.tick(rig.now) == -1);
    }
    CHECK(rig.model.tick(rig.now + 599) == -1 && rig.model.tick(rig.now + 600) == 270);
    rig.model.island(true, ++rig.generation, rig.now + 600);  // Service changed without a down notification.
    CHECK(rig.model.edge() == PS_EDGE_NONE && !rig.model.lightValid());
    CHECK(rig.model.rotation() == 270 && rig.model.brightness() == target(0));
}

static uint32_t randomWord(uint32_t &seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void faultIsolation()
{
    Rig clean, noisy;
    uint32_t seed = 0x98fad623;
    Proto error;
    spb_put_varint(&error.out, 1, 3);
    const Bytes failure = error.bytes();
    struct BackgroundStream {
        Stream stream;
        uint32_t message;
        Bytes sample;
    };
    const std::array background = {
        BackgroundStream{Stream::Gyro, SSC_MSG_SENSOR_EVENT, floats({1, -2, 3})},
        BackgroundStream{Stream::Mag, SSC_MSG_SENSOR_EVENT, floats({20, -10, 40})},
        BackgroundStream{Stream::Proximity, SSC_MSG_PROXIMITY_EVENT, proximity(SSC_PROXIMITY_NEAR)},
    };
    constexpr unsigned faultsPerStream = 700;
    unsigned rotations = 0;
    for (unsigned i = 1; i <= faultsPerStream * background.size(); ++i) {
        clean.now += 100;
        noisy.now = clean.now;
        const uint64_t ticks = i * 100 * ticksPerMs;
        const Pose &pose = (i / 30) % 2 ? left : right;
        const auto accel = report(Stream::Accel, {{SSC_MSG_SENSOR_EVENT, ticks, floats(pose)}});
        const auto light = report(Stream::Light, {{SSC_MSG_SENSOR_EVENT, ticks, floats({(i / 100) % 2 ? 0.0f : 800.0f})}});
        CHECK(clean.receive(Stream::Accel, accel) == 1 && noisy.receive(Stream::Accel, accel) == 1);

        const auto &[stream, message, sample] = background[i % background.size()];
        auto corrupt = report(stream, {{message, ticks + 2, sample}});
        if (i % 2 == 0) {
            corrupt = Bytes(corrupt.begin(), corrupt.begin() + randomWord(seed) % corrupt.size());
        } else {
            // Change the sample bytes while keeping its SUID, timestamp and envelope intact.
            const auto offset = randomWord(seed) % sample.size();
            corrupt[corrupt.size() - 1 - offset] ^= uint8_t(randomWord(seed));
        }
        noisy.receive(stream, corrupt);
        CHECK(noisy.sample(stream, failure, ticks + 3, SSC_MSG_ERROR_EVENT) == 1);
        CHECK(noisy.sample(stream, sample, ticks + 4, message) == 1);
        CHECK(clean.receive(Stream::Light, light) == 1 && noisy.receive(Stream::Light, light) == 1);
        const int request = clean.model.tick(clean.now);
        rotations += request >= 0;
        CHECK(request == noisy.model.tick(noisy.now));
        CHECK(clean.model.edge() == noisy.model.edge() && clean.model.orientation() == noisy.model.orientation());
        CHECK(clean.model.lightValid() == noisy.model.lightValid() && clean.model.brightness() == noisy.model.brightness());
        CHECK(clean.model.rotation() == noisy.model.rotation());
    }
    for (const auto &entry : background) CHECK(noisy.model.faults(entry.stream) == faultsPerStream);
    CHECK(noisy.model.faults(Stream::Light) == 0 && noisy.model.faults(Stream::Accel) == 0);
    CHECK(noisy.model.lightValid() && rotations > 50);
}

int main()
{
    orientations();
    debounce();
    coverGate();
    lightCurve();
    lightDynamics();
    validation();
    sampleClock();
    islandRecovery();
    faultIsolation();
    printf("PASS B3 offline SSC, four-edge rotation, cover gate, and Android-reference ALS: %u checks\n", checks);
}
