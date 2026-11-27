/* SPDX-License-Identifier: BSD-2-Clause-Patent */
// cc -std=c11 -Wall -Wextra -Werror -I.. duplex_schedule_host.c ../AudioSpkProt.c ../AudioSession.c ../AudioAmps.c ../Fs19xx.c ../AudioSha256.c ../AudioSequence.c ../PianoAudioReach.c ../PianoGpr.c ../AudioSafety.c ../AudioDspLink.c -o /tmp/duplex_schedule_host && /tmp/duplex_schedule_host
/* Offline analysis model. No IOKit, sleeping, hardware, or wall-clock timing.
 * See docs/drivers/audio-duplex.md. Use the actual FIFO/capture implementation;
 * model only PianoAudioDevice.cpp:532-540,557-558's absolute deadline loop.
 * Delays are synthetic injections of the published aud-mic4 maximum magnitudes,
 * NOT a reconstruction of unrecorded individual wake times or their cause. */
/* As in mic_fifo_host.c, including production C also exposes the exact
 * speaker refill boundary without inventing a DSP scheduling implementation. */
#include "../AudioLive.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} ++checks; } while (0)

enum DelaySite {WaitReturn, LockAcquire, PreviousBody};
typedef struct {
    unsigned late_periods, pre_wait_late;
    uint64_t max_us, delivered, startup, missing, produced, remaining;
} Result;

static AudioLiveShared sh;
static int16_t out[AUDIO_LIVE_RING * 2], in[AUDIO_LIVE_MIC_RING];

static int16_t sample(uint64_t index)
{
    return (int16_t)(1u + index % 30000u);
}

static void produce(uint64_t until, unsigned burst, uint64_t *next, uint64_t *frames)
{
    int16_t pcm[AUDIO_PERIOD_FRAMES];
    while (*next <= until) {
        for (unsigned b = 0; b < burst; ++b) {
            for (unsigned i = 0; i < AUDIO_PERIOD_FRAMES; ++i)
                pcm[i] = sample(*frames + i);
            CHECK(audio_ring_write(&sh.in, pcm, AUDIO_PERIOD_FRAMES) == AUDIO_PERIOD_FRAMES);
            *frames += AUDIO_PERIOD_FRAMES;
        }
        *next += (uint64_t)burst * 10000u;
    }
}

static int injection(uint64_t deadline)
{
    return deadline == 400000u || deadline == 800000u || deadline == 1200000u;
}

static Result run(unsigned batch, unsigned burst, unsigned delay, enum DelaySite site)
{
    audio_live_shared_init(&sh, out, in, AUDIO_Q15_ONE);
    const uint32_t origin = UINT32_MAX - 1023u;
    sh.in.head = sh.in.tail = origin;
    audio_live_running(&sh, 0, 1);
    sh.capturing = 1;
    const uint64_t period_us = (uint64_t)batch * 1000000u / PIANO_AUDIO_RATE;
    uint64_t now = 0, done = 0, next = (uint64_t)burst * 10000u, produced = 0, read = 0;
    Result r = {0};
    int16_t pcm[PIANO_AUDIO_PERIOD_FRAMES];
    while (done < PIANO_AUDIO_RATE * 2u) {
        const uint64_t deadline = (done + batch) * 1000000u / PIANO_AUDIO_RATE;
        /* The global driver counter is sampled before waiting. */
        if (now > deadline + period_us) ++r.pre_wait_late;
        else if (now < deadline) now = deadline;
        /* Either wait return or lock acquisition can add the SAME measured
         * delay. The driver samples only after both boundaries. */
        if (site != PreviousBody && injection(deadline)) now += delay;
        const uint64_t late = now > deadline ? now - deadline : 0;
        if (late > r.max_us) r.max_us = late;
        if (late > period_us) ++r.late_periods;
        produce(now, burst, &next, &produced);
        const unsigned n = audio_live_capture(&sh, pcm, batch);
        if (sh.input_primed) CHECK(n == batch);
        for (unsigned i = 0; i < n; ++i) CHECK(pcm[i] == sample(read++));
        done += batch;
        /* Slow processing in a previous pass can yield the same next-pass
         * lateness. This is an alternative cause, not an added second delay. */
        if (site == PreviousBody && injection(deadline + period_us))
            now = deadline + period_us + delay;
    }
    r.delivered = sh.input_hal.delivered;
    r.startup = sh.input_hal.startup;
    r.missing = sh.input_hal.missing;
    r.produced = produced;
    r.remaining = audio_ring_fill(&sh.in);
    CHECK(read == r.delivered && r.delivered > PIANO_AUDIO_RATE);
    CHECK(sh.input_hal.requested == done);
    CHECK(r.delivered + r.startup + r.missing == done);
    CHECK(r.missing == 0 && sh.in.overruns == 0 && sh.in.underruns == 0);
    CHECK(r.produced == r.delivered + r.remaining);
    CHECK(sh.in.head == origin + (uint32_t)r.produced && sh.in.tail == origin + (uint32_t)r.delivered);
    return r;
}

static void metric_ambiguity(void)
{
    for (unsigned burst = 1; burst <= 4; burst += 3) {
        const unsigned delays[] = {7374, 8293};
        for (unsigned i = 0; i < sizeof delays / sizeof delays[0]; ++i) {
            const unsigned delay = delays[i];
            const Result wait = run(240, burst, delay, WaitReturn);
            const Result lock = run(240, burst, delay, LockAcquire);
            const Result body = run(240, burst, delay, PreviousBody);
            CHECK(wait.late_periods == 3 && wait.max_us == delay);
            CHECK(wait.late_periods == lock.late_periods && wait.max_us == lock.max_us);
            CHECK(wait.late_periods == body.late_periods && wait.max_us == body.max_us);
            CHECK(wait.delivered == lock.delivered && wait.delivered == body.delivered);
            CHECK(wait.pre_wait_late == 0 && body.pre_wait_late == 3);
            /* Same physical delay but a doubled counter threshold: zero
             * late-periods does not prove better scheduling in batch480. */
            const Result ten = run(480, burst, delay, WaitReturn);
            CHECK(ten.max_us == delay && ten.late_periods == 0);
            printf("burst %u delay %u us: batch240 late %u; batch480 late %u; missing 0, contiguous PCM\n",
                   burst, delay, wait.late_periods, ten.late_periods);
        }
    }
}

static void boundaries(void)
{
    /* Counter uses strict >, not >=. A 10 ms delay at a 5 ms cadence
     * counts once, then catches up; 10.001 ms counts twice. */
    Result r = run(240, 4, 5000, WaitReturn);
    CHECK(r.late_periods == 0 && r.max_us == 5000);
    r = run(240, 4, 5001, WaitReturn);
    CHECK(r.late_periods == 3 && r.max_us == 5001);
    r = run(240, 4, 10000, WaitReturn);
    CHECK(r.late_periods == 3 && r.max_us == 10000);
    r = run(240, 4, 10001, WaitReturn);
    CHECK(r.late_periods == 6 && r.max_us == 10001);
}

static void four_slot_refill(void)
{
    static AudioLive live;
    int16_t pcm[AUDIO_LIVE_START_FRAMES * 2];
    for (unsigned i = 0; i < sizeof pcm / sizeof pcm[0]; ++i) pcm[i] = 1000;
    for (unsigned mode = 0; mode < 2; ++mode) {
        memset(&live, 0, sizeof live);
        audio_live_shared_init(&sh, out, in, AUDIO_Q15_ONE);
        sh.output_frames = piano_audio_timing_profile(mode)->periodFrames;
        live.sh = &sh;
        /* This is the post-unmute prime_left selection in production. */
        live.prime_left = mode == 0 ? AUDIO_LIVE_DEPTH : 0;
        CHECK(audio_ring_write(&sh.out, pcm, AUDIO_LIVE_START_FRAMES) == AUDIO_LIVE_START_FRAMES);
        for (unsigned burst = AUDIO_LIVE_DEPTH; burst; --burst) take_period(&live, burst);
        if (mode == 0) {
            CHECK(live.underruns == 0 && sh.out.underruns == 0 && live.skipped == 0);
            CHECK(audio_ring_fill(&sh.out) == AUDIO_LIVE_TARGET);
        } else {
            /* Actual mode480 threshold 2400 trims 2880 to 960 BEFORE a
             * 1920-frame refill. Two exact 480-frame short reads follow. */
            CHECK(live.skipped == 1 && live.underruns == 2 && sh.out.underruns == 960);
            CHECK(audio_ring_fill(&sh.out) == 0);
        }
        printf("four-slot refill mode %u: skipped %u short-reads %u missing %llu frames\n",
               mode, live.skipped, live.underruns, (unsigned long long)sh.out.underruns);
    }
}

int main(void)
{
    metric_ambiguity();
    boundaries();
    four_slot_refill();
    printf("duplex_schedule_host: %u checks passed (synthetic timing, real capture FIFO)\n", checks);
    return 0;
}
