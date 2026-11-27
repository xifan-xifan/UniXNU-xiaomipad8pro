/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. */
/* "Mic (noise reduced)": the tap -> RNNoise -> FIFO core (MicNsCore.h). */
#include "MicNsCore.h"
#include <math.h>
#include <string.h>
#include "rnnoise.h"

#define FIFO_MASK (MICNS_FIFO - 1u)
_Static_assert((MICNS_FIFO & FIFO_MASK) == 0, "the FIFO is a power of two");
_Static_assert(MICNS_MAX_READ + MICNS_RESERVE + MICNS_EXCESS + 2u * MICNS_BLOCK <= MICNS_FIFO, "the FIFO holds the largest target");

static uint32_t fifo_fill(const MicNs *m) {return (uint32_t)(m->head - m->tail);}
static uint64_t now(const MicNs *m) {return m->ticks ? m->ticks() : 0;}

static void prime_state(DenoiseState *st)
{
    float in[MICNS_BLOCK] = {0}, out[MICNS_BLOCK];
    (void)rnnoise_process_frame(st, out, in);
}

static void new_state(MicNs *m)
{
    if (m->st) {rnnoise_destroy(m->st); m->st = 0;}
    if (m->bypass) return;
    m->st = rnnoise_create(0);
    if (m->st) prime_state(m->st);
}

int micns_init(MicNs *m, const PianoTapHeader *tap, int bypass, uint64_t (*ticks)(void))
{
    if (!m) return -1;
    memset(m, 0, sizeof *m);
    m->tap = tap;
    m->bypass = bypass;
    m->ticks = ticks;
    new_state(m);
    return bypass || m->st ? 0 : -1;
}

int micns_start(MicNs *m, const PianoTapHeader *tap)
{
    if (!m) return -1;
    m->tap = tap;
    new_state(m);
    m->anchored = m->primed = 0;
    m->head = m->tail = 0;
    m->block_fill = 0;
    m->avg = m->setpoint = 0;
    m->samples = 0;
    m->last = 0;
    memset(&m->s, 0, sizeof m->s);
    return m->bypass || m->st ? 0 : -1;
}

void micns_free(MicNs *m)
{
    if (!m) return;
    if (m->st) rnnoise_destroy(m->st);
    m->st = NULL;
    m->tap = NULL;
    m->anchored = m->primed = 0;
    m->head = m->tail = 0;
    m->block_fill = 0;
}

uint32_t micns_fill(const MicNs *m)
{
    if (!m || !m->tap || !m->anchored) return 0;
    const uint64_t raw = audio_tap_written(m->tap) - m->cursor;
    if (raw > PIANO_TAP_FRAMES) return 0;
    return (uint32_t)raw + m->block_fill + fifo_fill(m);
}

static void anchor(MicNs *m, uint64_t written)
{
    if (m->anchored && m->st) {
        /* A discontinuity discards the denoiser's history as well as the queued PCM. */
        rnnoise_reset(m->st);
        prime_state(m->st);
    }
    m->cursor = written;
    m->run = __atomic_load_n(&m->tap->run, __ATOMIC_ACQUIRE);
    m->anchored = 1;
    m->primed = 0;
    m->head = m->tail = 0;
    m->block_fill = 0;
    m->last = 0;
    m->avg = m->setpoint = 0;
    m->samples = 0;
    m->s.anchors++;
}

static void process_block(MicNs *m)
{
    float out[MICNS_BLOCK];
    const uint64_t t0 = now(m);
    if (m->st) m->s.vad_sum += rnnoise_process_frame(m->st, out, m->block);
    else memcpy(out, m->block, sizeof out);
    if (m->ticks) {
        const uint64_t d = now(m) - t0;
        m->s.proc_ticks += d;
        if (d > m->s.proc_max_ticks) m->s.proc_max_ticks = d;
    }
    double in2 = 0, out2 = 0;
    if (fifo_fill(m) + MICNS_BLOCK > MICNS_FIFO) {m->tail += MICNS_BLOCK; m->s.excess++;}   /* never: the target fits; kept safe */
    for (uint32_t i = 0; i < MICNS_BLOCK; ++i) {
        if (!isfinite(out[i])) out[i] = 0.0f;
        in2 += (double)m->block[i] * m->block[i];
        out2 += (double)out[i] * out[i];
        float v = out[i] * (1.0f / 32768.0f);
        m->fifo[(uint32_t)(m->head + i) & FIFO_MASK] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    }
    m->s.in_squares += in2;
    m->s.out_squares += out2;
    m->head += MICNS_BLOCK;
    m->block_fill = 0;
    m->s.blocks++;
}

static void push(MicNs *m, int16_t v)
{
    m->block[m->block_fill++] = (float)v;
    m->last = v;
    if (m->block_fill == MICNS_BLOCK) process_block(m);
}

/* Process the unread frames in one snapshot so a moving writer cannot prolong the read. */
static int pull(MicNs *m)
{
    int16_t chunk[1024];
    uint64_t pending = audio_tap_written(m->tap) - m->cursor;
    while (pending) {
        const uint32_t capacity = sizeof chunk / sizeof chunk[0];
        const uint32_t n = pending > capacity ? capacity : (uint32_t)pending;
        if (!audio_tap_read(m->tap, m->cursor, chunk, n)) {m->s.lapped++; anchor(m, audio_tap_written(m->tap)); return -1;}
        m->cursor += n;
        pending -= n;
        for (uint32_t i = 0; i < n; ++i) push(m, chunk[i]);
    }
    return 0;
}

uint32_t micns_read(MicNs *m, float *out, uint32_t n)
{
    if (!out || !n) return 0;
    if (!m) {
        memset(out, 0, n * sizeof *out);
        return 0;
    }
    const uint64_t t0 = now(m);
    m->s.reads++;
    uint32_t served = n > MICNS_MAX_READ ? MICNS_MAX_READ : n, got = 0;
    if (!m->tap) {
        memset(out, 0, n * sizeof *out);
        m->s.startup += n;
        return 0;
    }
    const uint64_t w = audio_tap_written(m->tap);
    const uint32_t run = __atomic_load_n(&m->tap->run, __ATOMIC_ACQUIRE);
    if (!m->anchored) anchor(m, w);
    else if (run != m->run) {m->s.runs++; anchor(m, w);}
    else if (w - m->cursor > PIANO_TAP_FRAMES - 2u * MICNS_BLOCK) {m->s.lapped++; anchor(m, w);}
    uint32_t total = micns_fill(m);
    const uint32_t target = served + MICNS_RESERVE;
    if (m->primed) {
        if (total > (uint32_t)(m->setpoint / 64) + MICNS_EXCESS) {   /* far over: drop the oldest unread frames down to the setpoint */
            const uint64_t raw = audio_tap_written(m->tap) - m->cursor;
            const uint64_t skip = total - (uint32_t)(m->setpoint / 64);
            m->cursor += skip < raw ? skip : raw;
            m->s.excess++;
            total = micns_fill(m);
        }
        /* the first MICNS_SETTLE reads learn where the fill settles (the DSP's batches make it sit below the priming point by up to
         * a batch); then the slow average is held there */
        int slip = 0;
        if (m->samples < MICNS_SETTLE) {
            m->avg += ((int32_t)total * 64 - m->avg) / (int32_t)(m->samples + 1);
            if (++m->samples == MICNS_SETTLE) m->setpoint = m->avg;
        } else {
            m->avg += ((int32_t)total * 64 - m->avg) / 64;
            slip = m->avg > m->setpoint + (int32_t)MICNS_BAND * 64 ? 1 : m->avg < m->setpoint - (int32_t)MICNS_BAND * 64 ? -1 : 0;
        }
        if (slip > 0 && audio_tap_written(m->tap) != m->cursor) {m->cursor++; m->s.drops++;}   /* the device reads slower: one less */
        if (pull(m) == 0 && slip < 0) {push(m, m->last); m->s.repeats++;}                    /* it reads faster: one repeated */
    } else {
        (void)pull(m);
        total = micns_fill(m);
        if (total >= target && fifo_fill(m) >= served) {
            const uint32_t over = total - target;   /* start at the target: the oldest processed frames beyond it go */
            const uint32_t trim = over < fifo_fill(m) - served ? over : fifo_fill(m) - served;
            m->tail += trim;
            m->primed = 1;
            m->s.prime_written = m->cursor;                      /* the newest frame taken; the FIFO's oldest is this far back */
            m->s.prime_target = m->block_fill + fifo_fill(m);
            m->setpoint = m->avg = (int32_t)target * 64;   /* refined over the first MICNS_SETTLE reads */
            m->samples = 0;
        }
    }
    if (m->primed) {
        const uint32_t have = fifo_fill(m);
        got = have < served ? have : served;
        const int muted = __atomic_load_n(&m->tap->muted, __ATOMIC_RELAXED) != 0;
        const float gain = (float)__atomic_load_n(&m->tap->gain_q15, __ATOMIC_RELAXED) * (1.0f / 32767.0f);
        for (uint32_t i = 0; i < got; ++i) {
            const float v = muted ? 0.0f : m->fifo[(uint32_t)(m->tail + i) & FIFO_MASK] * gain;
            out[i] = v;
            if (v != 0.0f && !m->s.first_sound) m->s.first_sound = m->s.delivered + i + 1;
        }
        m->tail += got;
        m->s.delivered += got;
        if (muted) m->s.muted += got;
        if (got < served) {m->s.missing += served - got; m->primed = 0;}   /* an underrun: prime again */
    } else {
        m->s.startup += served;
    }
    if (got < n) memset(out + got, 0, (n - got) * sizeof *out);
    if (n > served) m->s.missing += n - served;
    if (m->ticks) {
        const uint64_t d = now(m) - t0;
        m->s.read_ticks += d;
        if (d > m->s.read_max_ticks) m->s.read_max_ticks = d;
    }
    return got;
}
