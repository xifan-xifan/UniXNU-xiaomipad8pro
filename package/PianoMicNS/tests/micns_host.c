// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host tests of "Mic (noise reduced)"'s core (MicNsCore.c) against PianoAudio's tap (AudioTap.h) in simulated time: a writer at the
// DSP's rate (480-frame periods, returned in batches of one to four, its clock +-200 ppm off the reader's) and a reader at the host
// clock's nominal 48 kHz (the HAL's reads of 512, 480 or 4096 frames):
//   - passthrough (bypass): every output frame is the next input frame, or one skipped or repeated (counted as the core's slips), no
//     frame missing once primed, the delay held near the reserve whatever the drift;
//   - RNNoise: pink noise suppressed by >= 20 dB (RNNoise leaves flat white noise mostly alone: ~4 dB), silence stays silence;
//   - a new capture run, a writer that stops (underrun, then primed again), a reader that stalls (lapped), the Mic's mute and level.
//   micns_host --wav IN.wav OUT.wav [--bypass]: a recording (mono 16-bit, 48 kHz or 24 kHz, the latter upsampled x2 and the output
//   decimated back) through the same tap/core path in simulated real time, for the offline measurement (piano-vpio-measure.py block).
//   cc -std=c11 -Wall -Wextra -Werror -I .. -I ../../PianoAudio -I ../third_party/rnnoise/include micns_host.c ../MicNsCore.c \
//      <rnnoise sources> -lm
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MicNsCore.h"

static unsigned checks;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1);}} while (0)

static uint8_t tapmem[PIANO_TAP_BYTES] __attribute__((aligned(16)));
static PianoTapHeader *tap;
static MicNs core;
static uint64_t sim_us;
#include <time.h>
static uint64_t ticks(void) {struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;}   // us

// ---- the writer: the DSP's periods at (1 + ppm) x 48 kHz, returned in batches
typedef int16_t (*Source)(uint64_t index);
typedef struct {
    double ppm, next_us;
    uint32_t pending, batch;
    uint64_t index;
    Source src;
    int stopped;
    unsigned seed;
    uint32_t gain;                 /* the Mic's level and mute the kernel copies into every period (0: unity) */
    int muted;
} Writer;
static unsigned rnd(unsigned *s) {*s = *s * 1103515245u + 12345u; return (*s >> 16) & 0x7fff;}
static void writer_step(Writer *w)
{
    if (w->stopped) return;
    while (sim_us >= w->next_us) {
        w->pending++;
        w->next_us += 10000.0 / (1.0 + w->ppm * 1e-6);
        if (w->pending >= w->batch) {
            int16_t p[480];
            for (uint32_t k = 0; k < w->pending; ++k) {
                for (int i = 0; i < 480; ++i) p[i] = w->src(w->index++);
                audio_tap_write(tap, p, 480, w->gain ? w->gain : 32767u, w->muted, (uint32_t)(sim_us / 1000));
            }
            w->pending = 0;
            w->batch = 1 + rnd(&w->seed) % 4;   // the next completion: one to four periods at once
        }
    }
}

static int16_t ramp(uint64_t i) {return (int16_t)(i & 0x3fff);}
static int16_t silence(uint64_t i) {(void)i; return 0;}
static int16_t constant(uint64_t i) {(void)i; return 8192;}
static unsigned noise_seed = 7;
static double pk0, pk1, pk2;
static int16_t noise(uint64_t i)   // pink (Paul Kellet's filter on white), about -40 dBFS: the measurement's kind of noise
{
    (void)i;
    const double w = ((int)rnd(&noise_seed) - 16384) / 16384.0;
    pk0 = 0.99765 * pk0 + w * 0.0990460; pk1 = 0.96300 * pk1 + w * 0.2965164; pk2 = 0.57000 * pk2 + w * 1.0526913;
    return (int16_t)lrint((pk0 + pk1 + pk2 + w * 0.1848) / 3.0 * 1024.0);
}

// One run: SECONDS of simulated time, reads of N frames every N/48000 s; checks the passthrough ramp when ramp_check.
typedef struct {uint64_t frames, ramp_bad, ramp_slips, max_lag, min_lag; double out2;} Result;
static Result run(Writer *w, uint32_t n, double seconds, int ramp_check, int stall_ms_at, int stall_ms)
{
    Result r = {0, 0, 0, 0, ~0ull, 0};
    static float out[8192];
    double next_read = sim_us + n * 1e6 / 48000.0;
    const uint64_t end = sim_us + (uint64_t)(seconds * 1e6);
    int16_t prev = -1;
    while (sim_us < end) {
        writer_step(w);
        if (sim_us >= next_read) {
            if (stall_ms && sim_us >= (uint64_t)stall_ms_at * 1000u && sim_us < (uint64_t)(stall_ms_at + stall_ms) * 1000u) {
                next_read += n * 1e6 / 48000.0;   // the HAL's thread stalled: the reads of this window never happen
            } else {
                const uint32_t got = micns_read(&core, out, n);
                for (uint32_t i = 0; i < n; ++i) r.out2 += (double)out[i] * out[i];
                if (ramp_check && got == n) {
                    for (uint32_t i = 0; i < n; ++i) {
                        const int16_t v = (int16_t)lrintf(out[i] * 32768.0f);
                        if (prev >= 0) {
                            const int d = (v - prev) & 0x3fff;
                            if (d == 0 || d == 2) r.ramp_slips++;
                            else if (d != 1) r.ramp_bad++;
                        }
                        prev = v;
                    }
                    // the delay: frames written minus the frame just delivered
                    const uint64_t lag = (w->index - (uint64_t)((int16_t)lrintf(out[n - 1] * 32768.0f))) & 0x3fff;
                    if (lag > r.max_lag) r.max_lag = lag;
                    if (lag < r.min_lag) r.min_lag = lag;
                } else {
                    prev = -1;
                }
                r.frames += got;
                next_read += n * 1e6 / 48000.0;
            }
        }
        sim_us += 250;
    }
    return r;
}

static void fresh(int bypass)
{
    tap = (PianoTapHeader *)(void *)tapmem;
    audio_tap_init(tap);
    micns_free(&core);
    CHECK(micns_init(&core, tap, bypass, ticks) == 0);
    CHECK(micns_start(&core, tap) == 0);
}

static void testPassthrough(double ppm, uint32_t n, double seconds)
{
    fresh(1);
    Writer w = {ppm, (double)sim_us, 0, 1, 0, ramp, 0, 1, 0, 0};
    Result r = run(&w, n, seconds, 1, 0, 0);
    const MicNsStats *s = &core.s;
    if (getenv("MICNS_DEBUG")) fprintf(stderr, "ppm %+.0f n %u: anchors %llu missing %llu drops %llu repeats %llu bad %llu slips %llu lag %llu..%llu excess %llu\n",
        ppm, n, (unsigned long long)s->anchors, (unsigned long long)s->missing, (unsigned long long)s->drops, (unsigned long long)s->repeats,
        (unsigned long long)r.ramp_bad, (unsigned long long)r.ramp_slips, (unsigned long long)r.min_lag, (unsigned long long)r.max_lag,
        (unsigned long long)s->excess);
    if (getenv("MICNS_DEBUG")) fprintf(stderr, "  avg %d setpoint %d samples %u fill %u\n", core.avg / 64, core.setpoint / 64, core.samples, micns_fill(&core));
    // primed once, no frame missing after it, every step +1 or a counted slip
    CHECK(s->anchors == 1 && s->missing == 0 && s->runs == 0 && s->lapped == 0);
    CHECK(r.ramp_bad == 0 && r.ramp_slips <= s->drops + s->repeats && r.ramp_slips + 2 >= s->drops + s->repeats);   // the last may be queued
    // the drift's frames (200 ppm over 60 s: 576) slip, of the right sign, less what the band and a period's quantization hold
    const double drift = 48000.0 * fabs(ppm) * 1e-6 * seconds;
    const uint64_t right = ppm > 0 ? s->drops : s->repeats, wrong = ppm > 0 ? s->repeats : s->drops;
    CHECK(wrong <= 40 && right <= drift + 100);
    if (ppm != 0) CHECK((double)right >= drift - MICNS_BAND - 480 - 100);
    else CHECK(s->drops + s->repeats <= 40);
    // the delay stays within the reserve, the read and the band, plus the batch
    CHECK(r.max_lag <= MICNS_RESERVE + n + MICNS_BAND + 4 * 480 && r.min_lag >= 480);
    printf("passthrough ppm %+.0f read %u: drops %llu repeats %llu lag %llu..%llu frames, startup %llu\n", ppm, n,
           (unsigned long long)s->drops, (unsigned long long)s->repeats, (unsigned long long)r.min_lag, (unsigned long long)r.max_lag,
           (unsigned long long)s->startup);
}

static void testRnnoise(void)
{
    fresh(0);
    Writer w = {0, (double)sim_us, 0, 1, 0, noise, 0, 3, 0, 0};
    (void)run(&w, 512, 10.0, 0, 0, 0);
    const MicNsStats *s = &core.s;
    const double nr = 10.0 * log10(s->in_squares / s->out_squares);
    printf("rnnoise: pink noise %.1f dB lower over %llu blocks, vad %.3f, %.1f us a block (host)\n", nr, (unsigned long long)s->blocks,
           s->vad_sum / (double)s->blocks, (double)s->proc_ticks / (double)s->blocks);
    CHECK(s->blocks > 900 && nr >= 20.0 && s->missing == 0);   // offline on the tablet's recording: 32 dB
    fresh(0);
    w = (Writer){0, (double)sim_us, 0, 1, 0, silence, 0, 3, 0, 0};
    Result r = run(&w, 512, 3.0, 0, 0, 0);
    CHECK(r.out2 == 0.0 && core.s.blocks > 250);
}

static void testEvents(void)
{
    fresh(1);
    Writer w = {0, (double)sim_us, 0, 1, 0, ramp, 0, 5, 0, 0};
    (void)run(&w, 512, 2.0, 1, 0, 0);
    CHECK(core.s.anchors == 1 && core.s.missing == 0);
    // a new capture run: re-anchored, primed again, no stale frame
    __atomic_add_fetch(&tap->run, 1u, __ATOMIC_RELEASE);
    Result r = run(&w, 512, 2.0, 1, 0, 0);
    CHECK(core.s.runs == 1 && core.s.anchors == 2 && r.ramp_bad == 0);
    // the writer stops for 300 ms: an underrun, silence, then primed again
    const uint64_t missing = core.s.missing;
    w.stopped = 1;
    (void)run(&w, 512, 0.3, 0, 0, 0);
    CHECK(core.s.missing > missing && !core.primed);
    w.stopped = 0; w.next_us = (double)sim_us;
    r = run(&w, 512, 2.0, 1, 0, 0);
    CHECK(core.primed && r.ramp_bad == 0);
    // the reader stalls 500 ms (more than the tap's 341 ms): lapped, re-anchored
    const uint64_t anchors = core.s.anchors;
    r = run(&w, 512, 3.0, 0, (int)(sim_us / 1000u) + 500, 500);
    CHECK(core.s.lapped >= 1 && core.s.anchors > anchors && core.primed);
    // the Mic muted: silence out (counted), then its level
    static float out[512];
    w.muted = 1;
    for (int i = 0; i < 60; ++i) {for (int k = 0; k < 43; ++k) {sim_us += 250; writer_step(&w);} (void)micns_read(&core, out, 512);}
    for (int i = 0; i < 512; ++i) CHECK(out[i] == 0.0f);
    CHECK(core.s.muted >= 512u * 40u && core.primed);
    w.muted = 0; w.gain = 16384; w.src = constant;
    for (int i = 0; i < 60; ++i) {for (int k = 0; k < 43; ++k) {sim_us += 250; writer_step(&w);} (void)micns_read(&core, out, 512);}
    int halves = 0;   // 8192 / 32768 at half level
    for (int i = 0; i < 512; ++i) halves += fabsf(out[i] - 0.125f) < 1e-3f;
    CHECK(halves == 512);
    // no tap: silence, counted as startup
    MicNs none;
    CHECK(micns_init(&none, NULL, 1, NULL) == 0);
    for (int i = 0; i < 512; ++i) out[i] = 1.0f;
    CHECK(micns_read(&none, out, 512) == 0 && out[0] == 0.0f && out[511] == 0.0f && none.s.startup == 512);
    micns_free(&none);
}

// ---- a recording through the tap and the core
static int16_t *wav_in;
static uint64_t wav_frames;
static int16_t from_wav(uint64_t i) {return i < wav_frames ? wav_in[i] : 0;}
static uint32_t le32(const uint8_t *b) {return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;}
static int wavMode(const char *in, const char *out_path, int bypass)
{
    FILE *f = fopen(in, "rb");
    if (!f) {perror(in); return 1;}
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) {fprintf(stderr, "read %s\n", in); return 1;}
    fclose(f);
    uint32_t rate = 0, at = 12, data = 0, bytes = 0;
    uint16_t ch = 0, bits = 0;
    while (at + 8 <= (uint32_t)size) {
        const uint32_t len = le32(b + at + 4);
        if (!memcmp(b + at, "fmt ", 4)) {ch = (uint16_t)(b[at + 10] | b[at + 11] << 8); rate = le32(b + at + 12); bits = (uint16_t)(b[at + 22] | b[at + 23] << 8);}
        if (!memcmp(b + at, "data", 4)) {data = at + 8; bytes = len; break;}
        at += 8 + len + (len & 1);
    }
    if (ch != 1 || bits != 16 || !data || (rate != 48000 && rate != 24000)) {fprintf(stderr, "%s: mono 16-bit 48/24 kHz only\n", in); return 1;}
    const uint64_t n_in = bytes / 2, up = rate == 24000 ? 2 : 1;
    const int16_t *pcm = (const int16_t *)(const void *)(b + data);
    wav_frames = n_in * up;
    wav_in = malloc(wav_frames * sizeof *wav_in);
    for (uint64_t i = 0; i < n_in; ++i) {   // x2: linear interpolation (the offline method's)
        if (up == 1) {wav_in[i] = pcm[i]; continue;}
        wav_in[2 * i] = pcm[i];
        wav_in[2 * i + 1] = (int16_t)(((int32_t)pcm[i] + (i + 1 < n_in ? pcm[i + 1] : pcm[i])) / 2);
    }
    fresh(bypass);
    Writer w = {0, (double)sim_us, 0, 1, 0, from_wav, 0, 11, 0, 0};
    // the reader's output, aligned back to the input by the core's own delay (the first delivered frames follow the priming)
    float *outbuf = calloc(wav_frames + 48000, sizeof *outbuf);
    uint64_t produced = 0;
    static float blk[512];
    double next_read = sim_us + 512e6 / 48000.0;
    while (produced < wav_frames + 24000) {
        writer_step(&w);
        if (sim_us >= next_read) {
            (void)micns_read(&core, blk, 512);
            for (int i = 0; i < 512 && produced < wav_frames + 48000; ++i) outbuf[produced++] = blk[i];
            next_read += 512e6 / 48000.0;
        }
        sim_us += 250;
    }
    // the delay: the first delivered frame (output index = the startup frames) is input frame prime_written - prime_target, and
    // RNNoise's output lags its input by one block
    const uint64_t first_in = core.s.prime_written - core.s.prime_target - (bypass ? 0 : MICNS_BLOCK);
    const uint64_t delay = core.s.startup - first_in;
    CHECK(core.s.anchors == 1 && core.s.runs == 0 && core.s.startup >= first_in);
    const uint64_t out_n = wav_frames / up;
    int16_t *o16 = malloc(out_n * sizeof *o16);
    for (uint64_t i = 0; i < out_n; ++i) {
        const uint64_t k = delay + i * up;
        const float v = k < produced ? outbuf[k] * 32768.0f : 0;
        o16[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
    }
    FILE *g = fopen(out_path, "wb");
    if (!g) {perror(out_path); return 1;}
    uint8_t h[44];
    memcpy(h, b, 44 < data ? 44 : data);
    const uint32_t ob = (uint32_t)(out_n * 2);
    memcpy(h, "RIFF", 4); h[4] = (uint8_t)(36 + ob); h[5] = (uint8_t)((36 + ob) >> 8); h[6] = (uint8_t)((36 + ob) >> 16); h[7] = (uint8_t)((36 + ob) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8); h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    for (int i = 0; i < 4; ++i) h[24 + i] = (uint8_t)(rate >> (8 * i)), h[28 + i] = (uint8_t)((rate * 2) >> (8 * i));
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; ++i) h[40 + i] = (uint8_t)(ob >> (8 * i));
    fwrite(h, 1, 44, g); fwrite(o16, 2, out_n, g); fclose(g);
    const MicNsStats *s = &core.s;
    printf("wav: %s -> %s (%s): %llu frames at %u Hz, delay %llu frames at 48 kHz, blocks %llu drops %llu repeats %llu missing %llu "
           "in/out %.1f dB, vad %.3f, %.1f us a block\n", in, out_path, bypass ? "bypass" : "RNNoise", (unsigned long long)out_n, rate,
           (unsigned long long)delay, (unsigned long long)s->blocks, (unsigned long long)s->drops, (unsigned long long)s->repeats,
           (unsigned long long)s->missing, 10.0 * log10(s->in_squares / (s->out_squares + 1e-9)), s->vad_sum / (double)(s->blocks + !s->blocks),
           (double)s->proc_ticks / (double)(s->blocks + !s->blocks));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "--wav")) return wavMode(argv[2], argv[3], argc >= 5 && !strcmp(argv[4], "--bypass"));
    testPassthrough(0, 512, 60.0);
    testPassthrough(200, 512, 60.0);
    testPassthrough(-200, 512, 60.0);
    testPassthrough(150, 480, 150.0);   // reads on the DSP's period grid: the fill moves only by whole periods
    testPassthrough(-150, 4096, 150.0);
    testRnnoise();
    testEvents();
    micns_free(&core);
    printf("PASS micns: %u checks\n", checks);
    return 0;
}
