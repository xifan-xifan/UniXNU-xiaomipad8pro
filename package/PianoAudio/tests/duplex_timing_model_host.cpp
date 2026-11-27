// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -x c++ duplex_timing_model_host.cpp -x c ../AudioLive.c ../AudioSpkProt.c ../AudioSession.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c ../AudioSafety.c ../AudioDspLink.c ../../PianoPower/QcomGlink.c -lm -o /tmp/duplex_timing_model_host && /tmp/duplex_timing_model_host
// Offline Goal 6/7 regression test. Active duplex tablet qualification is HOLD.
// AudioTiming, AudioLive, AudioSpkProt, FS19xx and QcomGlink supply the real
// geometry, graph commands, gain limits and protocol. HAL deadlines, PCM,
// firmware blobs, FIFO pressure and fault responses are synthetic fixtures.
// The aud-mic4 maximum informs the wake delay, not the scheduling mechanism.
// Reuse the existing board/session simulator and supply this suite's entry point.
#define main piano_audio_live_host_main
#include "live_host.c"
#undef main
#include "../../PianoPower/QcomGlink.h"
#include <initializer_list>

namespace {

constexpr auto kHalFrames = PIANO_AUDIO_LEGACY_PERIOD_FRAMES;
constexpr uint64_t kLateWakeUs = 7374; // Synthetic delay based on the published aud-mic4 maximum.
constexpr uint32_t kSampleCycle = 12000;
constexpr int kSpeakerBase = 3000;
constexpr int kMicBase = 1000;
constexpr uint16_t kThermalStatusChange = 1u << 8; // Synthetic OTWDS change; no hardware polarity or trip temperature assumed.

struct Reader {
    uint64_t deadline;
    uint64_t frames;
    uint64_t delay_at;
    uint64_t max_late;
    unsigned late_periods;
    unsigned catchup_periods;
};

struct Duplex {
    Reader speaker, mic;
    uint64_t period_us, dsp_period_us;
    uint64_t mic_next, mic_frames, speaker_next, speaker_frames, last_played;
    uint64_t playback_periods;
    uint64_t output_overruns;
    uint32_t epoch, fades, ahead;
    uint32_t underruns, skipped, drops, repeats;
    unsigned gain_writes, power_ups, unmutes, command_start;
    unsigned phase;
    bool hal_parked, watch_output, watch_input, stamp_ahead;
};

Duplex duplex;
int16_t speaker_hal[2u * PIANO_AUDIO_RING_FRAMES];
int16_t mic_hal[PIANO_AUDIO_RING_FRAMES];

int16_t speaker_sample(uint64_t frame)
{
    const auto magnitude = static_cast<int16_t>(kSpeakerBase + frame % kSampleCycle);
    return frame % 2 ? -magnitude : magnitude;
}

int16_t mic_sample(uint64_t frame)
{
    return static_cast<int16_t>(kMicBase + frame % kSampleCycle);
}

void fill_mic(int16_t *pcm, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i, ++w.cap_k) {
        pcm[2u * i] = mic_sample(w.cap_k) + 100;
        pcm[2u * i + 1] = mic_sample(w.cap_k) - 100;
    }
}

void read_speaker(uint64_t frame, bool after_late_wake)
{
    const auto size = piano_audio_timing_profile(PIANO_AUDIO_TIMING_LEGACY)->ringFrames;
    int16_t out[2u * kHalFrames], from_ring[2u * kHalFrames];
    for (uint32_t i = 0; i < kHalFrames; ++i) {
        out[2u * i] = speaker_sample(frame + i);
        out[2u * i + 1] = -out[2u * i];
    }
    piano_audio_copy_to_ring(speaker_hal, size, frame, out, kHalFrames, 2);
    piano_audio_copy_from_ring(from_ring, speaker_hal, size, frame, kHalFrames, 2);
    piano_audio_zero_ring(speaker_hal, size, frame, kHalFrames, 2);
    int wake = 0;
    // Model the producer publishing just after the worker sampled its time.
    const bool ahead = after_late_wake && duplex.stamp_ahead;
    CHECK(audio_live_play(&sh, from_ring, kHalFrames, now_ms() + ahead, &wake) == kHalFrames);
    if (wake || ahead) w.wake = 1;
}

void read_mic(uint64_t frame, bool)
{
    const auto size = piano_audio_timing_profile(PIANO_AUDIO_TIMING_LEGACY)->ringFrames;
    int16_t in[kHalFrames]{}, from_ring[kHalFrames];
    const uint32_t received = audio_live_capture(&sh, in, kHalFrames);
    piano_audio_copy_to_ring(mic_hal, size, frame, in, kHalFrames, 1);
    piano_audio_copy_from_ring(from_ring, mic_hal, size, frame, kHalFrames, 1);
    if (!duplex.watch_input) return;
    if (sh.input_primed) CHECK(received == kHalFrames);
    for (uint32_t i = 0; i < kHalFrames; ++i) {
        const int16_t sample = from_ring[i];
        if (!duplex.mic_frames) {
            if (!sample) continue; // DSP settle and HAL FIFO prefill.
            const uint32_t unread = audio_ring_fill(&sh.in) + received;
            CHECK(w.cap_k >= unread && i < received);
            duplex.mic_next = w.cap_k - unread + i;
        }
        CHECK(sample == mic_sample(duplex.mic_next++));
        ++duplex.mic_frames;
    }
}

// PianoAudioDevice::run advances an absolute period deadline even when it
// wakes late. The simulated DSP still returns its own 10 ms periods.
void service_reader(Reader &reader, bool running, void (*period)(uint64_t, bool))
{
    if (!running || duplex.hal_parked) {
        reader.deadline = 0;
        reader.frames = 0;
        return;
    }
    if (!reader.deadline) reader.deadline = w.now_us + duplex.period_us;
    bool recovering = false;
    while (w.now_us >= reader.deadline) {
        const bool injected = reader.deadline == reader.delay_at;
        const uint64_t late = w.now_us - reader.deadline;
        if (injected && late < kLateWakeUs) break;
        if (late > reader.max_late) reader.max_late = late;
        if (late > duplex.period_us) ++reader.late_periods;
        if (recovering && late < duplex.period_us)
            ++reader.catchup_periods;
        recovering |= injected;
        period(reader.frames, recovering);
        reader.frames += kHalFrames;
        reader.deadline += duplex.period_us;
    }
}

void hal_engine()
{
    service_reader(duplex.speaker, w.out_running, read_speaker);
    service_reader(duplex.mic, w.in_running, read_mic);
}

void check_written_pcm()
{
    if (!duplex.watch_output) return;
    CHECK(live.spk_mode == SpkPlaying && live.chain.fade == AudioFadeSteady);
    if (!duplex.speaker_frames) {
        const uint32_t unread = audio_ring_fill(&sh.out) + AUDIO_PERIOD_FRAMES;
        CHECK(duplex.speaker.frames >= unread);
        duplex.speaker_next = duplex.speaker.frames - unread;
    }
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES; ++i) {
        const int16_t sample = speaker_sample(duplex.speaker_next++);
        CHECK(live.stereo[2u * i] == sample && live.stereo[2u * i + 1] == -sample);
    }
    duplex.speaker_frames += AUDIO_PERIOD_FRAMES;
}

void check_played_pcm(uint64_t due_us, int32_t peak)
{
    if (!duplex.watch_output) return;
    CHECK(peak > 1000 && all_unmuted());
    if (duplex.last_played) CHECK(due_us == duplex.last_played + duplex.dsp_period_us);
    duplex.last_played = due_us;
    ++duplex.playback_periods;
}

void check_amp_gain(unsigned gain)
{
    CHECK(live.unmuted && all_unmuted() && !live.gain_changing);
    for (const auto &amp : w.amp) CHECK((amp.reg[FS19XX_REG_VOLCTRL] >> 8) == gain);
}

void check_protected()
{
    CHECK(live.spk_state == LiveRunning && live.prot_active && live.mip_active);
    CHECK(w.vi_open && w.vi_started && w.vi_clock && !w.clock_on);
    CHECK(w.prot_dev_open && w.dev_started && w.mip_open && w.mip_started && w.str_started);
    check_amp_gain(FS19XX_GAIN_CAP_PROTECTED);
    CHECK(!live.prot_fallbacks && !live.mip_fallbacks);
}

void check_no_cpu_tdm_vote(unsigned from)
{
    CHECK(!count_cmd("PRM REQ 0x202 6144000", from));
    CHECK(!count_cmd("PRM REL 0x202", from));
}

void check_capture_run()
{
    CHECK(live.mic_state == LiveRunning && w.cap_started && w.va_on && w.map_mic == 1);
    CHECK(duplex.mic_frames > PIANO_AUDIO_RATE / 4u);
    CHECK(!sh.input_hal.missing && !sh.in.underruns && !sh.in.overruns);
    CHECK(!live.cap.errors && !live.cap.late);
    CHECK(sh.input_hal.requested == duplex.mic.frames);
    CHECK(sh.input_hal.requested == sh.input_hal.delivered + sh.input_hal.startup);
}

void duplex_transitions(uint32_t t)
{
    switch (duplex.phase) {
    case 0:
        device(1, 0);
        duplex.phase = 1;
        return;
    case 1:
        if (t < 600) return;
        check_protected();
        duplex.watch_output = true;
        duplex.epoch = sh.epoch;
        duplex.gain_writes = w.gain_writes;
        duplex.power_ups = w.power_ups;
        duplex.unmutes = w.unmutes;
        duplex.fades = live.fades_silent_total;
        duplex.ahead = live.ahead_total;
        duplex.underruns = live.underruns;
        duplex.skipped = live.skipped;
        duplex.drops = live.drops;
        duplex.repeats = live.repeats;
        duplex.output_overruns = sh.out.overruns;
        duplex.command_start = w.ncmd;
        device(1, 1);
        duplex.watch_input = true;
        duplex.phase = 2;
        break;
    case 2:
        if (t >= 1100) {
            check_capture_run();
            for (auto *reader : {&duplex.speaker, &duplex.mic}) {
                reader->delay_at = reader->deadline;
                reader->max_late = 0;
                reader->late_periods = 0;
            }
            duplex.stamp_ahead = true;
            duplex.phase = 3;
        }
        break;
    case 3:
        if (t >= 1600) {
            check_capture_run();
            CHECK(live.ahead_total > duplex.ahead); // The stale worker-time dependency was exercised.
            for (const auto &reader : {duplex.speaker, duplex.mic}) {
                CHECK(reader.late_periods == 1 && reader.catchup_periods == 1);
                CHECK(reader.max_late >= kLateWakeUs && reader.max_late < 2u * duplex.period_us);
            }
            duplex.watch_input = false;
            device(1, 0);
            duplex.phase = 4;
        }
        break;
    case 4:
        if (live.mic_state == LiveDown) {
            CHECK(!w.map_mic && !w.va_on && !w.cap_started && !w.va_votes && !w.va_mclk);
            CHECK(count_cmd("VA OFF", duplex.command_start) == 1);
            duplex.mic_frames = 0;
            device(1, 1);
            duplex.watch_input = true;
            duplex.phase = 5;
        }
        break;
    case 5:
        if (t >= 5250) {
            check_capture_run();
            CHECK(sh.input_runs == 2 && live.mic_starts == 2 && live.spk_starts == 1);
            CHECK(duplex.speaker_frames > 4u * PIANO_AUDIO_RATE && duplex.playback_periods > 400);
            CHECK(live.underruns == duplex.underruns && live.skipped == duplex.skipped);
            CHECK(live.drops == duplex.drops && live.repeats == duplex.repeats && sh.out.overruns == duplex.output_overruns);
            CHECK(count_cmd("START 0xb00000d5", duplex.command_start) == 0);
            CHECK(count_cmd("STOP 0xb00000d5", duplex.command_start) == 0);
            check_no_cpu_tdm_vote(live_from());
            duplex.watch_output = false;
            device(0, 1); // Explicit Speaker StopIO is a different transition.
            duplex.phase = 6;
            return;
        }
        break;
    case 6:
        if (t >= 5350) {
            CHECK(!live.unmuted && !sh.want_out && sh.epoch == duplex.epoch);
            duplex.underruns = live.underruns;
            device(1, 1);
            CHECK(sh.epoch == duplex.epoch + 1u);
            duplex.phase = 7;
        }
        return;
    case 7:
        if (t >= 5650) {
            check_protected();
            check_capture_run();
            CHECK(live.spk_starts == 1 && live.mic_starts == 2 && live.underruns == duplex.underruns);
            CHECK(w.gain_writes == duplex.gain_writes && w.power_ups == duplex.power_ups);
            CHECK(w.unmutes == duplex.unmutes + AUDIO_AMPS);
            CHECK(sh.epoch == duplex.epoch + 1u);
            duplex.phase = 8;
        }
        return;
    default:
        return;
    }
    check_protected();
    CHECK(sh.epoch == duplex.epoch && live.spk_mode == SpkPlaying && !live.spk_ending);
    CHECK(w.gain_writes == duplex.gain_writes && w.power_ups == duplex.power_ups && w.unmutes == duplex.unmutes);
    CHECK(live.fades_silent_total == duplex.fades);
}

// PianoPower retains the GLINK channel across audio sleep. Route real GPR
// sends through the production TX primitive during suspend and resume. The
// remote's FIFO consumption and intent returns are simulated.
struct GlinkFixture {
    QcomGlink link;
    QcomGlinkChannel *channel;
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    uint8_t tx[2048], rx[128], intent[16];
    unsigned received, sent;
    bool enabled, pressure;
};

GlinkFixture glink;

void received_glink(void *ctx, QcomGlinkChannel *channel, const uint8_t *data, uint32_t size)
{
    CHECK(ctx == &glink && channel == glink.channel && size == 4 && le32(data) == 42);
    ++glink.received;
}

void init_glink()
{
    glink = {};
    const QcomGlinkFifo tx = {&glink.tx_head, &glink.tx_tail, glink.tx, sizeof glink.tx};
    const QcomGlinkFifo rx = {&glink.rx_head, &glink.rx_tail, glink.rx, sizeof glink.rx};
    qcom_glink_init(&glink.link, tx, rx, nullptr, nullptr);
    glink.link.version_sent = glink.link.version_agreed = 1;
    glink.link.features = QCOM_GLINK_FEATURE_INTENT_REUSE;
    const QcomGlinkClient client = {nullptr, received_glink};
    auto *c = qcom_glink_channel(&glink.link, "adsp_apps", sizeof glink.intent, 1, glink.intent, &client, &glink, 1);
    CHECK(c);
    c->open_sent = c->open_acked = c->remote_open = c->up = 1;
    c->rcid = 7;
    c->local[0].advertised = 1;
    c->remote[0].valid = 1;
    c->remote[0].id = 1;
    c->remote[0].size = GPR_MAX_PACKET;
    glink.channel = c;
}

void inject_glink(uint16_t cmd, const uint8_t *payload, uint32_t size)
{
    const uint8_t header[] = {
        static_cast<uint8_t>(cmd), static_cast<uint8_t>(cmd >> 8),
        static_cast<uint8_t>(glink.channel->rcid), 0, 1, 0, 0, 0
    };
    CHECK(glink.rx_head == glink.rx_tail);
    for (uint8_t byte : header) glink.rx[glink.rx_head++ % sizeof glink.rx] = byte;
    for (uint32_t i = 0; i < size; ++i) glink.rx[glink.rx_head++ % sizeof glink.rx] = payload[i];
    glink.rx_head = ((glink.rx_head + 7u) & ~7u) % sizeof glink.rx;
}

void release_glink_pressure()
{
    glink.tx_tail = glink.tx_head;
    const uint32_t reply_at = glink.tx_tail;
    CHECK(qcom_glink_poll(&glink.link) == 0);
    CHECK(glink.tx_head == (reply_at + 8u) % sizeof glink.tx);
    CHECK(glink.tx[reply_at] == QCOM_GLINK_RX_DONE_W_REUSE);
    CHECK(glink.tx[(reply_at + 2u) % sizeof glink.tx] == glink.channel->lcid);
    CHECK(glink.tx[(reply_at + 4u) % sizeof glink.tx] == 1);
    glink.tx_tail = glink.tx_head;
    CHECK(qcom_glink_poll(&glink.link) == 0 && glink.tx_tail == glink.tx_head);
    CHECK(glink.link.done_deferred == 1 && glink.link.done_retried == 1 && glink.received == 1);
    CHECK(!glink.channel->local[0].busy && !glink.channel->local[0].done_due);
    CHECK(!glink.link.retry_pending && !glink.link.reply_due);
}

int send_during_sleep(const uint8_t *bytes, uint32_t size)
{
    if (!glink.enabled) return 0;
    CHECK(size >= 24);
    auto *c = glink.channel;
    if (!glink.pressure && le32(bytes + 20) == AR_OP_GRAPH_STOP) {
        CHECK(live.sleeping);
        // Saturate the ordinary TX allowance while a local RX_DONE is owed.
        glink.tx_head = (glink.tx_tail + sizeof glink.tx - 24u) % sizeof glink.tx;
        const uint8_t payload[] = {4, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0};
        inject_glink(QCOM_GLINK_TX_DATA, payload, sizeof payload);
        CHECK(qcom_glink_poll(&glink.link) == 1);
        CHECK(glink.link.retry_pending && c->local[0].busy && c->local[0].done_due);
        CHECK(qcom_glink_send(&glink.link, c, bytes, size) == QcomGlinkAgain);
        CHECK(glink.link.tx_blocked > 0 && !c->remote[0].busy);
        glink.pressure = true;
        return -1;
    }
    CHECK(qcom_glink_send(&glink.link, c, bytes, size) == QcomGlinkOk && c->remote[0].busy);
    ++glink.sent;
    glink.tx_tail = glink.tx_head;
    inject_glink(QCOM_GLINK_RX_DONE_W_REUSE, nullptr, 0);
    CHECK(qcom_glink_poll(&glink.link) == 1 && !c->remote[0].busy && !glink.link.retry_pending);
    return 0;
}

void check_protection_down(unsigned from)
{
    const char *steps[] = {
        "STOP 0x4001", "STOP 0xb000019f", "STOP 0xb0000002", "STOP 0xb00000d5",
        "CLOSE 0xb00000d5", "CLOSE 0x4001", "CLOSE 0xb000019f", "CLOSE 0xb0000002", "UNMAP 0x55"
    };
    for (const char *step : steps) {
        from = find_cmd(step, from);
        CHECK(from < w.ncmd);
        ++from;
    }
    CHECK(!w.vi_clock && !w.vi_open && !w.vi_started && !w.prot_dev_open && !w.mip_open && !w.mip_started);
}

unsigned parked_at, glink_count, glink_blocked;
uint64_t captured_at_park, sound_at_park;

void sleep_transitions(uint32_t t)
{
    if (duplex.phase == 0) {
        device(1, 1);
        duplex.phase = 1;
    }
    if (duplex.phase == 1 && t >= 700) {
        check_protected();
        CHECK(live.mic_state == LiveRunning && sh.input_hal.pcm.nonzero > 0);
        CHECK(live.pump.in_flight && live.cap.in_flight);
        duplex.command_start = w.ncmd;
        duplex.epoch = sh.epoch;
        duplex.hal_parked = true;
        glink.enabled = true;
        audio_live_sleep(&live, 1);
        duplex.phase = 2;
    }
    if (duplex.phase == 2 && glink.pressure && glink.channel->local[0].done_due)
        release_glink_pressure();
    if (duplex.phase == 2 && live.parked) {
        CHECK(audio_live_parked(&live) == 1 && !live.sleep_failed);
        CHECK(glink.pressure && !glink.link.retry_pending && !glink.link.reply_due);
        CHECK(glink.channel->up && !glink.channel->remote[0].busy && glink.tx_tail == glink.tx_head);
        CHECK(!w.nq && !w.outstanding && !live.pump.in_flight && !live.cap.in_flight);
        CHECK(!w.map_spk && !w.map_mic && !speaker_clock() && !any_powered() && !w.pins_on);
        CHECK(!w.va_on && !w.va_mclk && !w.va_votes);
        CHECK(sh.want_out && sh.want_in && sh.epoch == duplex.epoch);
        CHECK(!audio_ring_fill(&sh.out) && !audio_ring_fill(&sh.in));
        check_protection_down(duplex.command_start);
        glink_count = glink.sent;
        glink_blocked = glink.link.tx_blocked;
        captured_at_park = sh.captured;
        sound_at_park = w.sound_periods;
        parked_at = t;
        duplex.phase = 3;
    }
    if (duplex.phase == 3 && t >= parked_at + 50) {
        CHECK(glink.sent == glink_count);
        CHECK(audio_live_rearm(&live) == 0 && audio_live_rearm(&live) == -1);
        CHECK(!live.sleeping && w.pins_on && sh.epoch == duplex.epoch);
        duplex.hal_parked = false;
        w.wake = 1;
        duplex.phase = 4;
    }
    if (duplex.phase == 4 && t >= parked_at + 400) {
        check_protected();
        CHECK(live.mic_state == LiveRunning && live.prot_starts == 2 && live.mip_starts == 2 && live.mic_starts == 2);
        CHECK(sh.captured > captured_at_park + 4u * AUDIO_PERIOD_FRAMES);
        CHECK(w.sound_periods > sound_at_park + 4u && !live.sleep_failed);
        // tx_blocked counts lifetime refusals. The post-wake traffic adds none.
        CHECK(glink.sent > glink_count && glink.link.tx_blocked == glink_blocked);
        CHECK(glink.link.done_retried == 1 && !glink.link.retry_pending && !glink.link.reply_due);
        check_no_cpu_tdm_vote(live_from());
        duplex.phase = 5;
    }
}

void check_passthrough()
{
    CHECK(live.spk_state == LiveRunning && live.prot_failed && live.prot_fallbacks == 1);
    CHECK(!live.prot_active && !live.mip_active && live.mip_fallbacks == 1);
    CHECK(w.clock_on && !w.vi_clock && !w.vi_open && !w.vi_started && !w.prot_dev_open && !w.mip_open);
    CHECK(w.dev_started && w.str_started && w.map_spk == 1);
    CHECK(sh.amp_gain_requested == FS19XX_GAIN_CAP_PROTECTED);
    check_amp_gain(FS19XX_GAIN_CAP);
}

void vi_failure(uint32_t)
{
    if (!w.out_running) device(1, 0);
    if (duplex.phase || w.sound_periods <= 30) return;
    check_passthrough();
    const unsigned start = live_from();
    const unsigned fallback = find_cmd("OPEN 0x4005", start);
    CHECK(w.failures_injected == 2 && count_cmd("START 0xb00000d5", start) == 2);
    CHECK(count_cmd("UNMAP 0x55", start) == 2 && fallback < w.ncmd);
    const unsigned first_unmap = find_cmd("UNMAP 0x55", start);
    CHECK(find_cmd("UNMAP 0x55", first_unmap + 1u) < find_cmd("PRM REQ 0x202 6144000", start));
    CHECK(count_cmd("CLOSE 0xb00000d5", start) == 2);
    // A timed-out START may have reached the DSP, so unwind includes STOP.
    CHECK(count_cmd("STOP 0xb00000d5", start) == (w.drop ? 2u : 0u));
    CHECK(count_cmd("OPEN 0xb000019f", start) == 1 && count_cmd("CLOSE 0xb000019f", start) == 1);
    CHECK(!count_cmd("START 0xb0000002", start) && w.power_ups == AUDIO_AMPS);
    CHECK(count_cmd("PRM REQ 0x202 6144000", start) == 1);
    CHECK(!count_cmd("PRM REL 0x202", start));
    CHECK(count_cmd("START 0x4005", fallback) == 1);
    duplex.phase = 1;
}

void monitor_fault(uint32_t t)
{
    if (duplex.phase == 0) {
        device(1, 0);
        duplex.phase = 1;
    }
    if (duplex.phase == 1 && t >= 700) {
        check_protected();
        // Reject subsequent monitor reads on both protected plans.
        w.fail_match = "GET 0x4046 0x10001fab";
        w.fail_limit = 2;
        duplex.command_start = w.ncmd;
        duplex.phase = 2;
    }
    if (duplex.phase == 2 && t >= 7500) {
        check_passthrough();
        const unsigned fallback = find_cmd("OPEN 0x4005", duplex.command_start);
        CHECK(w.failures_injected == 2 && fallback < w.ncmd);
        CHECK(count_cmd("STOP 0xb00000d5", duplex.command_start) == 2);
        CHECK(count_cmd("UNMAP 0x55", duplex.command_start) == 2);
        CHECK(count_cmd("PRM REQ 0x202 6144000", duplex.command_start) == 1);
        CHECK(logged("powered up MUTED, gain 243") && logged("powered up MUTED, gain 231"));
        duplex.phase = 3;
    }
}

void thermal_fault(uint32_t t)
{
    if (duplex.phase == 0) {
        device(1, 1);
        duplex.phase = 1;
    }
    if (duplex.phase == 1 && t >= 700) {
        check_protected();
        duplex.command_start = w.ncmd;
        w.amp[1].inject = kThermalStatusChange;
        duplex.phase = 2;
    }
    if (duplex.phase == 2 && t >= 1600) {
        CHECK(live.spk_state == LiveOff && live.spk_reason == AudioGuardAmplifier && !live.unmuted);
        CHECK(!any_powered() && !w.map_spk && !speaker_clock());
        for (const auto &amp : w.amp) CHECK(!amp.released);
        CHECK(!live.prot_fallbacks && !live.mip_fallbacks && !count_cmd("OPEN 0x4005", duplex.command_start));
        check_protection_down(duplex.command_start);
        check_no_cpu_tdm_vote(live_from());
        CHECK(live.mic_state == LiveRunning && sh.input_hal.pcm.nonzero > 0);
        duplex.phase = 3;
    }
}

enum class ViFault { None, Refused, Unanswered };

void run_duplex(void (*hook)(uint32_t), uint32_t end_ms, bool strict, ViFault vi_fault = ViFault::None)
{
    duplex = {};
    duplex.period_us = piano_audio_frames_to_ns(kHalFrames) / 1000u;
    duplex.dsp_period_us = piano_audio_frames_to_ns(AUDIO_PERIOD_FRAMES) / 1000u;
    CHECK(kHalFrames == 240 && duplex.period_us == 5000 && duplex.dsp_period_us == 10000);
    spk_reset();
    spk_files();
    mip_files();
    g_spk = 3;
    g_miplay = 1;
    if (vi_fault != ViFault::None) {
        g_fail = "START 0xb00000d5";
        g_drop = vi_fault == ViFault::Unanswered;
        g_fail_limit = 2; // The initial start and the retry without MIPLAY.
    } else {
        g_fail_limit = 1;
    }
    const Scenario scenario = {10, FS19XX_GAIN_CAP_PROTECTED, 1, 0, end_ms, AudioPortsUbuntu, strict, hook};
    run(&scenario);
    CHECK(!w.violations);
}

} // namespace

int main()
{
    preset_bytes = sfsm_build(preset, sizeof preset, 0);
    CHECK(preset_bytes > 100);
    fixture_engine = hal_engine;
    fixture_capture = fill_mic;
    fixture_write = check_written_pcm;
    fixture_played = check_played_pcm;

    run_duplex(duplex_transitions, 5900, true);
    CHECK(duplex.phase == 8);
    init_glink();
    fixture_send = send_during_sleep;
    run_duplex(sleep_transitions, 2200, true);
    CHECK(duplex.phase == 5);
    fixture_send = nullptr;
    for (const auto fault : {ViFault::Refused, ViFault::Unanswered}) {
        run_duplex(vi_failure, 9500, true, fault);
        CHECK(duplex.phase == 1);
    }
    run_duplex(monitor_fault, 7800, false);
    CHECK(duplex.phase == 3);
    // FS19xx status faults are terminal in AudioLive. Only recoverable
    // graph/DSP faults may fall back to powered passthrough.
    run_duplex(thermal_fault, 1900, false);
    CHECK(duplex.phase == 3);
    printf("duplex_timing_model_host: %u checks passed (synthetic timing and faults; tablet qualification HOLD)\n", checks);
    return 0;
}
