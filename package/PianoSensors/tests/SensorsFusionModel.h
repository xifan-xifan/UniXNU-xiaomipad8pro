// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
#ifndef PIANO_SENSORS_FUSION_MODEL_H
#define PIANO_SENSORS_FUSION_MODEL_H

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>

#include "../SensorsStream.h"
#include "../../PianoAuto/AutoRotate.h"
#include "../../PianoBacklight/NativeBrightness.h"

namespace sensorsfusion {

enum class Stream { Accel, Gyro, Mag, Light, Proximity, Count };
constexpr unsigned index(Stream stream) { return static_cast<unsigned>(stream); }

// SSC sample timestamps are fixed64 QTimer ticks at 19.2 MHz (SensorsSsc.h).
constexpr uint64_t ticksPerMs = 19200;

// Synthetic SUIDs and plausibility bounds for this fixture. The vector units are
// m/s², rad/s and µT respectively. Use the sensor attributes for real ranges.
inline constexpr std::array<SscSuid, index(Stream::Count)> fixtureSuids = {{
    {1, 11}, {2, 22}, {3, 33}, {4, 44}, {5, 55},
}};
constexpr float fixtureAccelLimit = 80.0f;
constexpr float fixtureGyroLimit = 35.0f;
constexpr float fixtureMagLimit = 2000.0f;
constexpr float fixtureLuxLimit = 120000.0f;
constexpr uint32_t fixtureProximityAdcLimit = 65535;

inline uint32_t referenceMillinits(uint32_t permille, uint16_t floor = PA_SDR_MIN_CODE)
{
    const auto raw = pianoauto::Ambient::codeForPermille(permille, floor);
    const auto code = raw > PA_SDR_MAX_CODE ? uint16_t(PA_SDR_MAX_CODE) : raw;
    return pianobacklight::NativeBrightness(floor, PA_SDR_MAX_CODE, true).milliNits(code);
}

// Host fixture for a recoverable SSC service/island outage. The caller supplies
// a transport generation and a monotonic receive clock in ms. Actual DSP
// guard strikes are boot-latched (SensorsHealth.h / SensorsGuard.h).
class Model {
public:
    Model()
    {
        SsConfig config;
        ss_defaults(&config);
        axes_ = config.axes;
        ParConfig rotation;
        par_defaults(&rotation);
        par_init(&agent_, &rotation);
        par_observed(&agent_, 0, 0);  // Synthetic display starts in normal landscape.
        sno_init(&orient_);
        ps_als_init(&als_);
    }

    void island(bool up, uint32_t generation, uint64_t now)
    {
        if (up == up_ && generation == generation_) return;
        up_ = up;
        generation_ = generation;
        channels_ = {};
        resetStream(Stream::Accel, now);
        resetStream(Stream::Light, now);
    }

    void cover(bool attached, bool bench, uint64_t now)
    {
        conditions_.attached = attached;
        conditions_.accel_with_cover = bench;
        if (!ss_accel_wanted(&conditions_)) resetStream(Stream::Accel, now);
        par_cover(&agent_, attached ? PAR_COVER_ATTACHED : PAR_COVER_DETACHED, now);
    }

    // The return value counts accepted sample/error events. Bad sample payloads
    // are local to their event. A report must match its source port's SUID.
    unsigned receive(Stream stream, uint32_t generation, const uint8_t *packet, uint32_t length, uint64_t now)
    {
        if (!up_ || generation != generation_ || index(stream) >= channels_.size()) return 0;
        if (stream == Stream::Accel && !ss_accel_wanted(&conditions_)) return 0;

        SscQmi qmi;
        int hasClient;
        uint64_t client;
        const uint8_t *message;
        uint32_t size;
        SscEvents events;
        if (ssc_qmi_parse(packet, length, &qmi) || qmi.type != SSC_QMI_INDICATION ||
            (qmi.message != SSC_QMI_REPORT && qmi.message != SSC_QMI_JUMBO_REPORT) ||
            ssc_qmi_report(&qmi, &hasClient, &client, &message, &size) ||
            ssc_events_open(&events, message, size) || !events.has_suid ||
            !ssc_suid_equal(&events.suid, &fixtureSuids[index(stream)])) return 0;

        SscEvent event;
        unsigned accepted = 0;
        while (ssc_events_next(&events, &event) == 1) accepted += accept(stream, event, now);
        return accepted;
    }

    // A tick runs the real policies. The fixture immediately confirms requests
    // as if WindowServer accepted them. Returns the requested angle or -1.
    int tick(uint64_t now)
    {
        if (up_) publish(ps_als_step_curve(&als_, PS_ALS_REFERENCE, PA_SDR_MIN_CODE, 0, 0, now));
        const int request = par_step(&agent_, now, 1).apply;
        if (request >= 0) {
            par_applied(&agent_, request, 1, now);
            par_observed(&agent_, request, now);
        }
        return request;
    }

    int orientation() const { return orient_.state; }
    int edge() const { return agent_.edge; }
    int rotation() const { return agent_.shown; }
    int32_t brightness() const { return millinits_; }
    bool lightValid() const { return als_.has != 0; }
    uint64_t lastTicks(Stream stream) const { return channels_[index(stream)].ticks; }
    unsigned faults(Stream stream) const { return channels_[index(stream)].faults; }

private:
    struct Channel {
        uint64_t ticks = 0;
        unsigned faults = 0;
    };

    static bool validStatus(int64_t status) { return status >= 0 && status <= 3; }

    static bool validFloats(Stream stream, const SscSensorEvent &sample)
    {
        const bool light = stream == Stream::Light;
        if (!validStatus(sample.status) || sample.more || (light ? sample.count < 1 : sample.count != 3)) return false;
        if (light) {
            const float lux = std::bit_cast<float>(sample.data[0]);
            return lux >= 0 && lux <= fixtureLuxLimit;  // The comparisons also reject NaN and infinity.
        }
        const float limit = stream == Stream::Accel ? fixtureAccelLimit : stream == Stream::Gyro ? fixtureGyroLimit : fixtureMagLimit;
        for (unsigned i = 0; i < 3; ++i)
            if (!(std::fabs(std::bit_cast<float>(sample.data[i])) <= limit)) return false;
        return true;
    }

    void resetStream(Stream stream, uint64_t now)
    {
        if (stream == Stream::Accel) {
            sno_init(&orient_);
            par_edge(&agent_, PS_EDGE_NONE, now);
        } else if (stream == Stream::Light) {
            ps_als_init(&als_);
            previousLightTarget_ = -1;
        }
    }

    void publish(int32_t permille)
    {
        if (permille >= 0) millinits_ = int32_t(referenceMillinits(uint32_t(permille)));
    }

    void light(uint32_t bits, uint64_t now)
    {
        const int32_t milliLux = sfx_milli(bits);
        const auto target = int32_t(ps_als_target_permille(PS_ALS_REFERENCE, PA_SDR_MIN_CODE, uint32_t((milliLux + 500) / 1000)));
        // Synthetic confirmation rule: two successive valid readings in the same
        // target band. This also applies at startup and after a service restart.
        if (previousLightTarget_ >= 0 && std::abs(target - previousLightTarget_) < int32_t(PS_ALS_HYSTERESIS))
            publish(ps_als_step_curve(&als_, PS_ALS_REFERENCE, PA_SDR_MIN_CODE, 1, milliLux, now));
        previousLightTarget_ = target;
    }

    bool accept(Stream stream, const SscEvent &event, uint64_t now)
    {
        auto &channel = channels_[index(stream)];
        const uint64_t last = channel.ticks;
        const uint64_t delta = event.timestamp - last;
        // Fixture serial ordering: zero is missing, and a forward modular
        // distance below half the counter space is newer. Replays can't move
        // the watermark. A true wrap starts fresh orientation dwell.
        if (!event.timestamp || (last && (!delta || delta >= (uint64_t{1} << 63)))) return false;

        SscSensorEvent sample;
        if (event.msg_id == SSC_MSG_ERROR_EVENT) {
            int64_t error;
            if (ssc_parse_error_event(event.payload, event.length, &error) || error < 0) return false;
            ++channel.faults;
            resetStream(stream, now);
        } else if (stream == Stream::Proximity) {
            SscProximityEvent proximity;
            if (event.msg_id != SSC_MSG_PROXIMITY_EVENT || ssc_parse_proximity_event(event.payload, event.length, &proximity) ||
                (proximity.type != SSC_PROXIMITY_FAR && proximity.type != SSC_PROXIMITY_NEAR) ||
                proximity.raw_adc > fixtureProximityAdcLimit || !validStatus(proximity.status)) return false;
        } else {
            if (event.msg_id != SSC_MSG_SENSOR_EVENT || ssc_parse_sensor_event(event.payload, event.length, &sample) ||
                !validFloats(stream, sample)) return false;
            if (stream == Stream::Accel) {
                constexpr uint64_t maxGap = 2 * 1000 * ticksPerMs / SS_ACCEL_HZ;
                if (last && (event.timestamp < last || delta > maxGap)) orient_.candidate = orient_.state;
                const int state = ps_accel_sample_at(&orient_, &axes_, sample.data, event.timestamp, SNO_HOLD_MS * ticksPerMs);
                if (state >= 0) par_edge(&agent_, ps_edge_of(state), now);
            } else if (stream == Stream::Light) {
                light(sample.data[0], now);
            }
        }
        channel.ticks = event.timestamp;
        return true;
    }

    std::array<Channel, index(Stream::Count)> channels_{};
    SsConditions conditions_{.light_consumer = 1, .accel_consumer = 1, .display = 1, .lid = 1, .attached = 0, .accel_with_cover = 0};
    SfxAxes axes_;
    SnoState orient_;
    ParAgent agent_;
    PsAls als_;
    int32_t previousLightTarget_ = -1;
    int32_t millinits_ = -1;
    uint32_t generation_ = 0;
    bool up_ = false;
};

}  // namespace sensorsfusion
#endif
