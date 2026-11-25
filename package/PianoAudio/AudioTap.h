/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. */
/* PianoAudio's microphone tap (private/host-checks MIC-NS.md section 8): the raw mono Mic frames for the "Mic (noise reduced)"
 * AudioServerPlugIn (userland/piano-micns), which runs a noise suppressor on them in the HAL's driver service. The live thread writes
 * each capture period (AudioLive.c capture_period: the mono frames after the first 100 ms of zeros, before the Mic ring's drift slip
 * and before the Mic's digital level) into a ring in one shared allocation; the plug-in maps it read-only (PianoMicTapUserClient,
 * clientMemoryForType PIANO_TAP_MEMORY) and keeps its own read position. While at least one tap client is open the capture plan runs
 * as if the Mic device did (and goes down idle_ms after the last one closed and the Mic stopped). Only a process holding the audio
 * driver host entitlement (coreaudiod, its driver service) may open the tap: a recording app reaches these frames only through the
 * HAL, which enforces the microphone grant.
 *
 * The layout is the ABI between the kext and the plug-in (both include this file). One writer (the live thread), any number of
 * readers. The writer stores the frames, then publishes `written` with release semantics; a reader loads `written` (acquire), copies
 * frames [c, c + n) with c + n <= written, loads `written` again and keeps the copy only if written - c <= PIANO_TAP_FRAMES (nothing
 * it copied was overwritten meanwhile: audio_tap_valid). `run` changes when a capture run starts (a reader re-anchors its position);
 * `gain_q15` / `muted` are the Mic's own level and mute (AudioCapture.h's map), copied at every period: a reader applies them to its
 * output so that muting "Mic" mutes the processed device too. Portable C: host-tested in tests/tap_host.c and tests/live_host.c. */
#ifndef PIANO_AUDIO_TAP_H
#define PIANO_AUDIO_TAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_TAP_MAGIC 0x50415450u            /* "PTAP" */
#define PIANO_TAP_VERSION 1u
#define PIANO_TAP_RATE 48000u
#define PIANO_TAP_FRAMES 16384u                /* 341 ms of mono S16; a power of two */
#define PIANO_TAP_MASK (PIANO_TAP_FRAMES - 1u)
#define PIANO_TAP_HEADER_BYTES 64u
#define PIANO_TAP_BYTES (PIANO_TAP_HEADER_BYTES + PIANO_TAP_FRAMES * 2u)
#define PIANO_TAP_MEMORY 0u                     /* IOConnectMapMemory64 type: the header and the ring, read-only */
#define PIANO_TAP_SERVICE "piano-mic-tap"       /* the IOService's name (IOServiceNameMatching) */
#define PIANO_TAP_USER_CLIENT "PianoMicTapUserClient"   /* its user client's class (the plug-in's AudioServerPlugIn_IOKitUserClients) */
#define PIANO_TAP_ENTITLEMENT "com.apple.private.audio.driver-host"   /* coreaudiod's and both driver services' (27.3, fact) */

typedef struct {
    uint32_t magic, version, frames, rate;     /* +0 */
    uint64_t written;                          /* +16 frames written since the tap was made; ring index = written & PIANO_TAP_MASK */
    uint32_t run;                              /* +24 capture runs since the tap was made */
    uint32_t gain_q15;                         /* +28 the Mic's digital level (Q15, 32767 unity) at the last period */
    uint32_t muted;                            /* +32 the Mic is muted or faulted: deliver silence */
    uint32_t write_ms;                         /* +36 the live thread's clock at the last period */
    uint32_t clients;                          /* +40 tap user clients open now */
    uint32_t opens;                            /* +44 tap user clients opened since the tap was made */
    uint32_t refused;                          /* +48 opens refused (no entitlement) */
    uint32_t periods;                          /* +52 capture periods written */
    uint32_t reserved[2];                      /* +56 */
} PianoTapHeader;

#ifndef __cplusplus
_Static_assert(sizeof(PianoTapHeader) == PIANO_TAP_HEADER_BYTES, "the tap header is the ABI's 64 bytes");
#else
static_assert(sizeof(PianoTapHeader) == PIANO_TAP_HEADER_BYTES, "the tap header is the ABI's 64 bytes");
#endif

static inline int16_t *audio_tap_ring(PianoTapHeader *h) {return (int16_t *)(void *)((uint8_t *)h + PIANO_TAP_HEADER_BYTES);}
static inline const int16_t *audio_tap_ring_ro(const PianoTapHeader *h)
{
    return (const int16_t *)(const void *)((const uint8_t *)h + PIANO_TAP_HEADER_BYTES);
}

static inline void audio_tap_init(PianoTapHeader *h)
{
    uint8_t *p = (uint8_t *)h;
    for (uint32_t i = 0; i < PIANO_TAP_BYTES; ++i) p[i] = 0;
    h->magic = PIANO_TAP_MAGIC;
    h->version = PIANO_TAP_VERSION;
    h->frames = PIANO_TAP_FRAMES;
    h->rate = PIANO_TAP_RATE;
    h->gain_q15 = 32767u;
}

/* The writer (the live thread): n mono frames, then the new count with release semantics. */
static inline void audio_tap_write(PianoTapHeader *h, const int16_t *mono, uint32_t n, uint32_t gain_q15, int muted, uint32_t now_ms)
{
    int16_t *ring = audio_tap_ring(h);
    const uint64_t w = __atomic_load_n(&h->written, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; ++i) ring[(uint32_t)(w + i) & PIANO_TAP_MASK] = mono[i];
    __atomic_store_n(&h->gain_q15, gain_q15, __ATOMIC_RELAXED);
    __atomic_store_n(&h->muted, muted ? 1u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&h->write_ms, now_ms, __ATOMIC_RELAXED);
    __atomic_store_n(&h->periods, __atomic_load_n(&h->periods, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&h->written, w + n, __ATOMIC_RELEASE);
}

static inline uint64_t audio_tap_written(const PianoTapHeader *h) {return __atomic_load_n(&h->written, __ATOMIC_ACQUIRE);}

/* A reader: copies frames [from, from + n) (the caller has from + n <= the written count it loaded); 1 when the copy is whole
 * (nothing in it was overwritten while it was copied), 0 when the writer lapped it (the caller re-anchors). */
static inline int audio_tap_read(const PianoTapHeader *h, uint64_t from, int16_t *out, uint32_t n)
{
    const int16_t *ring = audio_tap_ring_ro(h);
    for (uint32_t i = 0; i < n; ++i) out[i] = ring[(uint32_t)(from + i) & PIANO_TAP_MASK];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return audio_tap_written(h) - from <= PIANO_TAP_FRAMES;
}

#ifdef __cplusplus
}
#endif
#endif
