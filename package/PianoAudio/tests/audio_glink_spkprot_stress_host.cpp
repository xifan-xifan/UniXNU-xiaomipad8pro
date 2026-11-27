// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -working-directory "${audio_stress_objects:=$(mktemp -d "${TMPDIR:-/tmp}/audio-stress.XXXXXXXX")}" -c "$PWD"/../{AudioLive,AudioSpkProt,AudioSession,AudioAmps,Fs19xx,AudioSha256,AudioSequence,PianoAudioReach,PianoGpr,AudioSafety,AudioDspLink}.c "$PWD"/../../PianoPower/QcomGlink.c && trap 'rm -r "$audio_stress_objects"' EXIT && clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread audio_glink_spkprot_stress_host.cpp "$audio_stress_objects"/*.o -lm -o /tmp/audio_glink_spkprot_stress_host && /tmp/audio_glink_spkprot_stress_host
#define main piano_audio_live_host_main
#include "live_host.c"
#undef main
#include "../../PianoPower/QcomGlink.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <deque>
#include <thread>

namespace {

constexpr unsigned kProbeCycles = 512;
constexpr unsigned kIntents = 20;
constexpr unsigned kFifoBytes = 2048;
constexpr uint32_t kThermalQ12 = 90u * 4096u;
constexpr uint16_t kOtwdStatus = 1u << 8;

struct Packet {
    std::array<uint8_t, GPR_MAX_PACKET> bytes{};
    uint32_t size;

    Packet(const uint8_t *data, uint32_t n) : size(n)
    {
        CHECK(n <= bytes.size());
        memcpy(bytes.data(), data, n);
    }
};

struct Ring {
    uint32_t head{}, tail{};
    uint8_t bytes[kFifoBytes]{};

    QcomGlinkFifo fifo() { return {&head, &tail, bytes, sizeof bytes}; }
};

struct Wire {
    Ring tx, rx;
    QcomGlink host, dsp;
    QcomGlinkChannel *host_channel, *dsp_channel;
    uint8_t host_storage[kIntents * GPR_MAX_PACKET], dsp_storage[sizeof host_storage];
    AudioInbox inbox;
    std::deque<Packet> requests, replies;
    unsigned host_filler{}, dsp_filler{};
    bool enabled{};
};

Wire wire;
bool block_next_request;

unsigned fill_fifo(QcomGlink &link, QcomGlinkChannel *channel);

void host_receive(void *, QcomGlinkChannel *channel, const uint8_t *data, uint32_t size)
{
    CHECK(channel == wire.host_channel && size > 0);
    if (data[0] == 0xff) {
        ++wire.host_filler;
        return;
    }
    CHECK(audio_inbox_put(&wire.inbox, data, size) == 0);
}

void dsp_receive(void *, QcomGlinkChannel *channel, const uint8_t *data, uint32_t size)
{
    CHECK(channel == wire.dsp_channel && size > 0);
    if (data[0] == 0xff) {
        ++wire.dsp_filler;
        return;
    }
    CHECK(!wire.requests.empty());
    const auto &request = wire.requests.front();
    CHECK(size == request.size && !memcmp(data, request.bytes.data(), size));
    wire.requests.pop_front();
}

void poll_wire()
{
    CHECK(qcom_glink_poll(&wire.host) >= 0);
    CHECK(qcom_glink_poll(&wire.dsp) >= 0);
}

void start_wire()
{
    wire.tx = {};
    wire.rx = {};
    wire.requests.clear();
    wire.replies.clear();
    wire.host_filler = wire.dsp_filler = 0;
    audio_inbox_init(&wire.inbox);
    qcom_glink_init(&wire.host, wire.tx.fifo(), wire.rx.fifo(), nullptr, nullptr);
    qcom_glink_init(&wire.dsp, wire.rx.fifo(), wire.tx.fifo(), nullptr, nullptr);
    const QcomGlinkClient host_client{nullptr, host_receive}, dsp_client{nullptr, dsp_receive};
    wire.host_channel = qcom_glink_channel(&wire.host, "adsp_apps", GPR_MAX_PACKET, kIntents, wire.host_storage, &host_client, nullptr, 0);
    wire.dsp_channel = qcom_glink_channel(&wire.dsp, "adsp_apps", GPR_MAX_PACKET, kIntents, wire.dsp_storage, &dsp_client, nullptr, 1);
    CHECK(wire.host_channel && wire.dsp_channel);
    for (unsigned i = 0; i < 8; ++i) poll_wire();
    CHECK(wire.host_channel->up && wire.dsp_channel->up);
    wire.enabled = true;
}

int send_request(const uint8_t *data, uint32_t size)
{
    if (!wire.enabled) return 0;  // The level 4–7 setup has its own simulator loop.
    if (block_next_request) fill_fifo(wire.host, wire.host_channel);
    const int rc = qcom_glink_send(&wire.host, wire.host_channel, data, size);
    if (block_next_request) {
        CHECK(rc == QcomGlinkAgain);
        block_next_request = false;
    }
    if (rc == QcomGlinkOk) wire.requests.emplace_back(data, size);
    else CHECK(rc == QcomGlinkAgain);
    return rc;
}

void queue_reply(const uint8_t *data, uint32_t size)
{
    wire.replies.emplace_back(data, size);
}

void service_wire()
{
    while (!wire.replies.empty()) {
        const auto &reply = wire.replies.front();
        const int rc = qcom_glink_send(&wire.dsp, wire.dsp_channel, reply.bytes.data(), reply.size);
        if (rc == QcomGlinkAgain) break;
        CHECK(rc == QcomGlinkOk);
        wire.replies.pop_front();
    }
    poll_wire();
    uint8_t packet[GPR_MAX_PACKET];
    while (const auto size = audio_inbox_get(&wire.inbox, packet, sizeof packet))
        audio_live_receive(&live, packet, size, now_ms());
}

void check_intents(const QcomGlink &link, const QcomGlinkChannel &channel)
{
    CHECK(channel.up && !channel.intent_asked && !link.retry_pending && !link.reply_due);
    CHECK(!link.dropped && !link.dynamic_bytes && link.done_deferred == link.done_retried);
    for (const auto &intent : channel.local) CHECK(!intent.busy && !intent.done_due && !intent.fill);
    unsigned advertised = 0;
    for (const auto &intent : channel.remote) {
        advertised += intent.valid;
        CHECK(!intent.busy);
    }
    CHECK(advertised == kIntents);
}

void check_wire_drained()
{
    CHECK(wire.tx.head == wire.tx.tail && wire.rx.head == wire.rx.tail);
    CHECK(wire.requests.empty() && wire.replies.empty());
    CHECK(wire.inbox.head == wire.inbox.tail && !wire.inbox.dropped && !wire.inbox.too_long);
    check_intents(wire.host, *wire.host_channel);
    check_intents(wire.dsp, *wire.dsp_channel);
}

unsigned fill_fifo(QcomGlink &link, QcomGlinkChannel *channel)
{
    uint8_t filler[GPR_MAX_PACKET]{0xff};
    unsigned sent = 0;
    // GLINK messages cost 16 header bytes. Pick lengths that leave no normal TX room.
    while (const uint32_t room = qcom_glink_tx_avail(&link.tx)) {
        CHECK(room > 16);
        uint32_t size = std::min(room - 16, static_cast<uint32_t>(sizeof filler));
        if (room > size + 16 && room - size - 16 <= 16) size -= 24;
        CHECK(qcom_glink_send(&link, channel, filler, size) == QcomGlinkOk);
        ++sent;
    }
    CHECK(qcom_glink_send(&link, channel, filler, sizeof filler) == QcomGlinkAgain);
    CHECK(link.retry_pending && link.tx_blocked > 0);
    return sent;
}

void saturate_wire()
{
    for (unsigned i = 0; i < 4; ++i) service_wire();
    check_wire_drained();
    const auto host_before = wire.host_filler, dsp_before = wire.dsp_filler;
    const auto deferred = wire.host.done_deferred;
    const auto to_dsp = fill_fifo(wire.host, wire.host_channel);
    const auto to_host = fill_fifo(wire.dsp, wire.dsp_channel);
    CHECK(qcom_glink_rx_avail(&wire.host.rx) == kFifoBytes - 16);
    CHECK(qcom_glink_rx_avail(&wire.dsp.rx) == kFifoBytes - 16);
    CHECK(qcom_glink_poll(&wire.host) > 0 && wire.host.done_deferred == deferred + to_host);
    CHECK(wire.host.retry_pending && wire.host_channel->local[0].done_due);
    for (unsigned i = 0; i < 4; ++i) poll_wire();
    CHECK(wire.host_filler == host_before + to_host && wire.dsp_filler == dsp_before + to_dsp);
    check_wire_drained();
}

class ProbeWriters {
public:
    ProbeWriters() : first([this] { writer(0); }), second([this] { writer(1); }) {}
    ~ProbeWriters()
    {
        stopping = true;
        start.arrive_and_wait();
        first.join();
        second.join();
    }

    std::array<int, 2> overlap(uint32_t request, bool sleep = false)
    {
        value = request;
        start.arrive_and_wait();
        if (sleep) audio_live_sleep(&live, 1);
        done.arrive_and_wait();
        return result;
    }

private:
    void writer(unsigned index)
    {
        for (;;) {
            start.arrive_and_wait();
            if (stopping) return;
            result[index] = audio_live_spk_probe(&sh, value);
            done.arrive_and_wait();
        }
    }

    std::barrier<> start{3}, done{3};
    std::array<int, 2> result{};
    uint32_t value{};
    bool stopping{};
    std::thread first, second;
};

ProbeWriters *writers;
unsigned cycle, phase, route_applies_stress;
unsigned amp_fault_mask, temperature_mask, mapped_before_sleep;
std::array<unsigned, 4> bus_errors_before;
uint64_t mic_frames_before_fault;
uint32_t trigger_at;
Answer old_map{};
uint32_t previous_pump_base, reads_before_rearm;
bool checked_replay, reordered, saw_prime;
uint64_t delayed_until;

void setup_session(AudioSessionConfig *config)
{
    CHECK(config->tone == 0);
    config->level = 7;  // The level-10 boot also walks through level 7 with tone disabled.
}

void apply_route(int route, uint32_t generation)
{
    CHECK(route == 4 && generation == 1 && !w.va_on && !w.map_mic);
    ++route_applies_stress;
}

void setup_live(AudioLiveConfig *config)
{
    config->spk_probe = 1;
    config->mic_route = 4;
    config->mic_route_apply = apply_route;
    start_wire();
}

void set_live_words(unsigned amp, uint32_t *words)
{
    words[0] = g_re25_q12[amp] + g_re25_q12[amp] * (amp + 1) / 100u;
    words[1] = temperature_mask & (1u << amp) ? kThermalQ12 + amp * 4096u : (25u + amp) * 4096u;
}

void fill_route_pcm(int16_t *pcm, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i, ++w.cap_k) {
        const int sign = w.cap_k % 96 < 48 ? 1 : -1;
        pcm[2u * i] = static_cast<int16_t>(sign * 6000);  // Route 4 selects DEC0.
        pcm[2u * i + 1] = static_cast<int16_t>(-sign * 4000);
    }
}

void check_telemetry()
{
    AudioLiveSpkStatus status;
    audio_live_spk_status(&live, &status);
    CHECK(status.live_reads && w.vbat_sets && w.vbat_uv == g_battery_uv && live.vbat_uv == g_battery_uv);
    char text[480];
    const auto length = audio_live_spk_live_text(&status, now_ms(), text, sizeof text);
    CHECK(length == strlen(text) && length < sizeof text - 1);
    const char *next = strstr(text, "; amp");
    for (unsigned amp = 0; amp < 4; ++amp) {
        const unsigned temp = (temperature_mask & (1u << amp) ? 90u : 25u) + amp;
        const unsigned r0 = g_re25_q12[amp] * (101u + amp) / 100u;
        CHECK(status.re25[amp] == g_re25_q12[amp] && status.live_data.re[amp] == r0);
        CHECK(status.live_data.temp[amp] == temp * 4096u);
        CHECK(status.live_data.f0[amp] == (800u + 10u * amp) * 256u && status.live_data.q[amp] == 3u + amp);
        char expected[65];
        const auto n = snprintf(expected, sizeof expected, "; amp%u r0-permille %u f0 %u q %u t-dC %u ok",
                                amp, r0 * 1000u / g_re25_q12[amp], 800u + 10u * amp, 3u + amp, temp * 10u);
        CHECK(n > 0 && static_cast<size_t>(n) < sizeof expected && next && !strncmp(next, expected, n));
        next += n;
    }
    CHECK(next && !*next);
}

void check_no_audio()
{
    CHECK(!w.sound_periods && !w.peak && !live.sound_periods && !w.unmutes);
    CHECK(!live.chain.clamped && !w.violations);
}

void check_unmapped()
{
    CHECK(!w.vi_open && !w.vi_started && !w.prot_dev_open && !w.mip_open && !w.mip_started);
    CHECK(!w.str_open && !w.str_started && !w.dev_started && !speaker_clock() && !w.map_spk);
    CHECK(!any_powered());
    for (const auto busy : w.slot_busy) CHECK(!busy);
}

void check_park()
{
    CHECK(audio_live_parked(&live) == 1 && !w.pins_on && !w.va_on && !w.va_votes && !w.va_mclk && !w.map_mic);
    CHECK(!live.splan.in_flight && !live.mplan.in_flight && !live.xplan.in_flight && !live.hplan.in_flight);
    CHECK(!live.pump.in_flight && !live.cap.in_flight && !live.stream_unsent && !live.cap_unsent);
    CHECK(!w.nq && !w.outstanding && !audio_ring_fill(&sh.out) && !audio_ring_fill(&sh.in));
    check_unmapped();
    for (unsigned i = 0; i < 3; ++i) service_wire();
    check_wire_drained();
}

void check_mic()
{
    CHECK(route_applies_stress == 1 && live.mic_route == 4 && live.mic_route_generation == 1);
    CHECK(live.mic_state == LiveRunning && w.va_on && w.cap_started && w.map_mic == 1);
    CHECK(w.mic_ok > AUDIO_PERIOD_FRAMES && !w.mic_bad && !live.cap.errors && !live.cap.late);
}

void delay_first_write()
{
    for (unsigned i = 0; i < w.nq; ++i) {
        auto &answer = w.q[i];
        if (answer.read_slot > -2) continue;
        answer.due_us += 13000;
        delayed_until = answer.due_us;
        return;
    }
    CHECK(false);
}

void probe_cycle(uint32_t t)
{
    if (!phase) {
        const uint32_t request = cycle % 2 ? 102 : 2;
        block_next_request = cycle % 4 == 0;
        const auto results = writers->overlap(request);
        CHECK(std::count(results.begin(), results.end(), 0) == 1);
        CHECK(std::count(results.begin(), results.end(), EBUSY) == 1);
        CHECK(audio_live_spk_probe(&sh, 30) == EBUSY);
        phase = 1;
        return;
    }
    if (phase == 1 && live.splan.in_flight && live.splan.seq.pending_opcode == AR_OP_MEM_MAP && cycle % 4 == 0) {
        // Keep the active MAP token pinned across a backpressure pulse and sleep.
        CHECK(live.queue_full == 1 && w.nq == 1 && w.q[0].plan);
        old_map = w.q[0];
        w.q[0].due_us += 12000 + cycle % 11 * 1000u;
        saturate_wire();
        mapped_before_sleep = w.map_spk;
    } else if (phase == 1 && live.live_reads && cycle % 4 != 0) {
        check_telemetry();
        CHECK(live.probing && live.probe_runs == 1 && w.played_periods > 0 && w.vbat_sets == 1);
        CHECK(w.power_ups == (cycle % 2 ? 4u : 0u));
        CHECK((writers->overlap(130) == std::array{EBUSY, EBUSY}));
        if (cycle % 4 == 3) {
            phase = 2;  // Let some probes expire on their own.
            return;
        }
    } else if (phase == 2 && live.spk_state == LiveDown && !live.probing) {
        CHECK(live.probe_runs == 1 && live.probe_reads >= 1);
        check_unmapped();
    } else if (phase == 3 && live.parked) {
        check_park();
        CHECK(audio_live_spk_probe(&sh, 1) == ENXIO);
        CHECK(audio_live_rearm(&live) == 0);
        CHECK(!sh.have_sound && !sh.sound && !live.pump.in_flight && !live.cap.in_flight);
        reads_before_rearm = live.live_reads;
        checked_replay = false;
        CHECK(audio_live_spk_probe(&sh, 1) == 0);
        w.wake = 1;  // The sysctl wakes the worker after accepting a request.
        trigger_at = t;
        phase = 4;
        return;
    } else if (phase == 4 && mapped_before_sleep && !checked_replay && live.splan.in_flight &&
               live.splan.seq.pending_opcode == AR_OP_MEM_MAP) {
        const auto token = live.splan.seq.pending_token;
        CHECK(token != le32(old_map.b + 16));
        const auto stray = live.stray;
        queue_reply(old_map.b, old_map.n);
        service_wire();
        CHECK(live.splan.in_flight && live.splan.seq.pending_token == token && live.stray == stray + 1);
        checked_replay = true;
        return;
    } else if (phase == 4 && live.live_reads > reads_before_rearm) {
        check_telemetry();
        CHECK(live.probe_runs == 2 && live.probing && checked_replay == (mapped_before_sleep != 0));
        audio_live_sleep(&live, 2);
        phase = 5;
        return;
    } else if (phase == 5 && live.parked) {
        check_park();
        CHECK(!live.timeouts && !live.prot_fallbacks && !live.mip_fallbacks);
        check_no_audio();
        phase = 6;
        return;
    } else {
        if (phase == 4) CHECK(t - trigger_at < 1000);
        return;
    }
    const auto results = writers->overlap(130, true);
    CHECK(std::all_of(results.begin(), results.end(), [](int rc) { return rc == EBUSY || rc == ENXIO || rc == 0; }));
    // An expired probe may leave the mailbox idle just before sleep takes it.
    if (phase == 1) CHECK(std::count(results.begin(), results.end(), 0) == 0);
    else CHECK(std::count(results.begin(), results.end(), 0) <= 1);
    CHECK(audio_live_spk_probe(&sh, 1) == ENXIO && !live.probing);
    if (mapped_before_sleep) CHECK(live.splan.in_flight && audio_live_parked(&live) == 0);
    w.wake = 1;
    phase = 3;
}

void awake_duplex(uint32_t t)
{
    if (!phase) {
        w.sound_on = 1;
        w.amplitude = 32767;
        device(1, 1);
        phase = 1;
    }
    if (phase == 1 && t >= 600) {
        check_mic();
        CHECK(live.prot_active && live.mip_active && live.unmuted && w.sound_periods);
        previous_pump_base = live.pump.token_base;
        audio_live_sleep(&live, 1);
        w.sound_on = 0;
        phase = 2;
    } else if (phase == 2 && live.parked) {
        check_park();
        CHECK(audio_live_rearm(&live) == 0 && !sh.have_sound && !sh.sound);
        w.sound_on = 1;
        phase = 3;
    } else if (phase == 3 && live.pump.in_flight == AUDIO_LIVE_DEPTH) {
        CHECK(live.spk_mode == SpkPriming && !live.spk_primed && !live.unmuted);
        CHECK(live.pump.token_base != previous_pump_base);
        delay_first_write();
        phase = 4;
    } else if (phase == 4) {
        if (w.now_us < delayed_until && live.pump.returned) reordered = true;
        if (live.spk_primed && !saw_prime) {
            CHECK(live.pump.returned >= AUDIO_UNMUTE_PERIODS && live.unmuted);
            saw_prime = true;
        }
        if (t >= 1300) {
            CHECK(reordered && saw_prime && live.unmuted && !live.pump.errors && !live.pump.late);
            check_mic();
            phase = 5;
        }
    }
}

void fault_during_playback(uint32_t t)
{
    if (!phase) {
        w.sound_on = 1;
        w.amplitude = 32767;
        device(1, 1);
        phase = 1;
    }
    if (phase == 1 && t >= 600) {
        check_mic();
        CHECK(live.prot_active && live.mip_active && live.unmuted);
        check_telemetry();
        mic_frames_before_fault = w.mic_ok;
        for (unsigned amp = 0; amp < 4; ++amp) {
            bus_errors_before[amp] = live.amps->amp[amp].bus_errors;
            if (amp_fault_mask & (1u << amp)) w.amp[amp].present = 0;
            // The FourSemi model asserts OTWDS at the injected threshold. The
            // DSP temperature report is independent of the I2C status read.
            if (live.live_data.temp[amp] >= kThermalQ12) w.amp[amp].inject = kOtwdStatus;
        }
        phase = 2;
    } else if (phase == 2 && live.spk_state == LiveOff) {
        check_unmapped();
        CHECK(live.spk_reason == AudioGuardAmplifier && !live.unmuted && !sh.accepting);
        for (const auto &amp : w.amp) CHECK(!amp.released);
        for (unsigned amp = 0; amp < 4; ++amp)
            CHECK((live.amps->amp[amp].bus_errors > bus_errors_before[amp]) == bool(amp_fault_mask & (1u << amp)));
        CHECK(live.amps->off_errors == (amp_fault_mask ? 1u : 0u));
        if (temperature_mask) CHECK((live.amp_bad & temperature_mask) == temperature_mask);
        CHECK(!count_cmd("OPEN 0x4005", live_from()) && !live.prot_fallbacks && !live.mip_fallbacks);
        CHECK(audio_live_spk_probe(&sh, 1) == 0);  // Consumed and refused by the worker.
        phase = 3;
    } else if (phase == 3 && live.probe_refused) {
        CHECK(live.probe_refused == 1 && !live.probe_runs);
        CHECK(!w.violations && !live.chain.clamped && !live.timeouts);
        CHECK(w.mic_ok > mic_frames_before_fault && live.mic_starts == 1);
        check_mic();
        phase = 4;
    }
}

void nack_during_probe_start(uint32_t t)
{
    if (!phase) {
        device(0, 1);
        phase = 1;
    } else if (phase == 1 && t >= 350) {
        check_mic();
        mic_frames_before_fault = w.mic_ok;
        CHECK(audio_live_spk_probe(&sh, 102) == 0);
        w.wake = 1;
        phase = 2;
    } else if (phase == 2 && live.splan.in_flight && live.splan.seq.pending_opcode == AR_OP_MEM_MAP) {
        CHECK(live.probing && live.probe_only && !live.probe_noamps);
        for (unsigned amp = 0; amp < 4; ++amp)
            if (amp_fault_mask & (1u << amp)) w.amp[amp].present = 0;
        phase = 3;
    } else if (phase == 3 && live.spk_state == LiveOff && !live.probing) {
        CHECK(live.probe_runs == 1 && live.amps->off_errors == 1 && !live.splan.seq.undo_errors);
        CHECK(logged("ended: protection unavailable") && !count_cmd("OPEN 0x4005", live_from()));
        CHECK(w.mic_ok > mic_frames_before_fault && live.mic_starts == 1);
        check_mic();
        check_unmapped();
        check_no_audio();
        phase = 4;
    }
}

void fallback(uint32_t t)
{
    if (!phase) {
        w.sound_on = 1;
        w.amplitude = 32767;
        device(1, 1);
        phase = 1;
    }
    if (phase == 1 && w.failures_injected == 1) {
        w.fail_match = "START 0xb0000002";  // Also refuse the retry without MIPLAY.
        phase = 2;
    }
    if (phase == 2 && t >= 650) {
        CHECK(w.failures_injected == 2 && live.mip_fallbacks == 1 && live.prot_fallbacks == 1);
        CHECK(logged("status 0x1") && count_cmd("UNMAP 0x55", live_from()) == 2);
        const unsigned passthrough = find_cmd("OPEN 0x4005", live_from());
        CHECK(passthrough < w.ncmd && count_cmd("OPEN 0x4005", live_from()) == 1);
        const unsigned first_unmap = find_cmd("UNMAP 0x55", live_from());
        CHECK(first_unmap < passthrough && find_cmd("UNMAP 0x55", first_unmap + 1) < passthrough);
        CHECK(count_cmd("CLOSE 0xb0000002", live_from()) == 2);
        CHECK(!w.vi_open && !w.prot_dev_open && !w.mip_open && !w.vi_clock);
        CHECK(live.spk_state == LiveRunning && !live.prot_active && !live.mip_active && all_unmuted());
        CHECK(live.cfg.gain == 231 && w.unmutes == 4 && w.sound_periods && w.peak <= live.cfg.ceiling);
        for (const auto &amp : w.amp) CHECK((amp.reg[FS19XX_REG_VOLCTRL] >> 8) == 231);
        CHECK(!live.chain.clamped && !w.violations && !live.timeouts);
        check_mic();
        phase = 3;
    }
}

void restart(uint32_t)
{
    if (!phase) {
        CHECK(audio_live_spk_probe(&sh, 102) == 0);
        phase = 1;
    }
    if (phase == 1 && live.splan.in_flight && live.splan.seq.pending_opcode == AR_OP_MEM_MAP) {
        CHECK(w.nq == 1 && w.q[0].plan && w.outstanding);
        old_map = w.q[0];  // Late MAP ACK from the retired generation.
        w.q[0].due_us += 25000;
        fill_fifo(wire.host, wire.host_channel);
        fill_fifo(wire.dsp, wire.dsp_channel);
        CHECK(qcom_glink_poll(&wire.host) > 0 && wire.host_channel->local[0].done_due);
        ++w.o.link_generation;
        w.off = 1;
        w.wake = 1;
        phase = 2;
    }
}

void scenario(uint32_t end, void (*hook)(uint32_t), bool strict = true)
{
    phase = route_applies_stress = 0;
    wire.enabled = false;
    block_next_request = false;
    g_setup = setup_live;
    const Scenario config{10, 243, 1, 0, end, AudioPortsUbuntu, strict, hook};
    run(&config);
    CHECK(session.result[7] == AudioLevelSkipped && logged("piano-audio-tone=1 not given: no sound"));
    CHECK(!w.violations);
    for (unsigned i = 0; i < 3; ++i) service_wire();
    check_wire_drained();
}

} // namespace

int main()
{
    preset_bytes = sfsm_build(preset, sizeof preset, 0);
    CHECK(preset_bytes > 0);
    fixture_session_setup = setup_session;
    fixture_poll = service_wire;
    fixture_send = send_request;
    fixture_receive = queue_reply;
    fixture_live_data = set_live_words;
    fixture_capture = fill_route_pcm;
    spk_reset();
    spk_files();
    mip_files();
    g_spk = 3;
    g_miplay = 1;

    ProbeWriters contenders;
    writers = &contenders;
    for (cycle = 0; cycle < kProbeCycles; ++cycle) {
        g_battery_uv = 3900000u + cycle * 1000u;
        mapped_before_sleep = 0;
        scenario(cycle % 4 == 3 ? 3300 : 1600, probe_cycle);
        CHECK(phase == 6 && live.probe_runs == 2 && !live.probe_refused);
        CHECK(mapped_before_sleep == (cycle % 4 == 0 ? 1u : 0u));
    }
    scenario(1450, awake_duplex);
    CHECK(phase == 5);

    // All possible partial failures as well as the whole bus/group.
    for (unsigned mask = 1; mask < (1u << 4); ++mask) {
        amp_fault_mask = mask;
        scenario(1800, fault_during_playback, false);
        CHECK(phase == 4);
        scenario(850, nack_during_probe_start);
        CHECK(phase == 4);
        amp_fault_mask = 0;
        temperature_mask = mask;
        scenario(1800, fault_during_playback, false);
        CHECK(phase == 4);
        temperature_mask = 0;
    }

    g_fail = "START 0xb000019f";
    g_fail_limit = 2;
    scenario(850, fallback);
    CHECK(phase == 3);
    g_fail = nullptr;

    scenario(1000, restart);
    CHECK(phase == 2 && live.finished && live.off && guard.reason == AdgReasonLink);
    CHECK(!w.sends_after_off && !live.splan.in_flight && !live.mplan.in_flight && !live.xplan.in_flight &&
          !live.hplan.in_flight && !live.splan.unsent && !w.pins_on && !any_powered());
    CHECK(w.nq == 1 && w.outstanding);  // DSP-owned work lost with SSR.
    const auto received = live.received;
    queue_reply(old_map.b, old_map.n);
    for (unsigned i = 0; i < 3; ++i) service_wire();
    CHECK(live.received == received && live.off && audio_live_rearm(&live) == -1);
    check_wire_drained();
    check_no_audio();
    mapped_before_sleep = 0;
    scenario(1650, probe_cycle);  // A new boot advertises fresh intents and runs probes again.
    CHECK(phase == 6);

    printf("audio_glink_spkprot_stress: %u silent probe/sleep cycles, 45 amp fault cases, "
           "GLINK saturation, stale/reordered GPR, SSR and gain-231 fallback (%u checks)\n", kProbeCycles, checks);
    return 0;
}
