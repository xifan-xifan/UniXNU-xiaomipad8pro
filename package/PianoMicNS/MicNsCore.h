/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. */
/* The processing core of "Mic (noise reduced)" (PianoMicNS.c; private/host-checks MIC-NS.md section 8), without Core Audio: the raw
 * mono frames from PianoAudio's tap (package/PianoAudio/AudioTap.h) through RNNoise (third_party/rnnoise, 480-frame blocks at 48 kHz)
 * into a FIFO the device's ReadInput takes from.
 *   clock    the device runs on the host clock at a nominal 48 kHz (as PianoAudio's own Mic and Speaker do); the tap fills at the DSP's
 *            rate. The frames in the pipeline (unread tap frames + the partial block + the FIFO), sampled before each read, are held
 *            near the fill the first MICNS_SETTLE primed reads settled on (their mean; then the slow average, 1/64 a read, within
 *            MICNS_BAND frames) by dropping or repeating single raw frames before RNNoise (counted), as AudioLive.c does for the
 *            Mic's own ring.
 *   start    a read delivers silence until the pipeline holds the read plus MICNS_RESERVE frames (the DSP returns its periods in
 *            batches of up to four: 40 ms), then the processed frames; an underrun delivers what there is, then silence, and primes
 *            again. A new capture run in the tap (`run` changed), a reader lapped by the writer, or a pipeline more than MICNS_EXCESS
 *            frames over its fill re-anchors at the newest frame.
 *   level    the Mic's own mute (and fault) and digital level, from the tap's header, on the output: muting "Mic" mutes this device.
 * Real-time safe after micns_start: no allocation, no lock, no system call in micns_read. Host-tested in tests/micns_host.c. */
#ifndef PIANO_MICNS_CORE_H
#define PIANO_MICNS_CORE_H
#include <stdint.h>
#include "AudioTap.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MICNS_BLOCK 480u          /* RNNoise's frame: 10 ms at 48 kHz */
#define MICNS_FIFO 16384u         /* processed frames (a power of two) */
#define MICNS_RESERVE 2400u       /* 50 ms over the read: four DSP periods in one batch plus a block */
#define MICNS_BAND 240u           /* frames the smoothed fill may stray from its setpoint before single frames slip */
#define MICNS_SETTLE 128u         /* primed reads that learn the setpoint (no slip yet): about 1.4 s of 512-frame reads */
#define MICNS_EXCESS 4800u        /* 100 ms over the setpoint: re-anchor (the reader stalled, or the writer burst) */
#define MICNS_MAX_READ 8192u      /* the largest read served whole (larger ones: the rest is silence) */

typedef struct DenoiseState DenoiseState;

typedef struct {
    uint64_t reads, delivered, startup, missing, muted;    /* frames */
    uint64_t blocks, drops, repeats, anchors, lapped, excess, runs;
    uint64_t proc_ticks, proc_max_ticks;                    /* time inside RNNoise (caller's clock ticks) */
    uint64_t read_ticks, read_max_ticks;                    /* time inside micns_read */
    uint64_t first_sound;                                   /* delivered frame index + 1 of the first non-zero output, 0 none */
    double vad_sum;                                         /* RNNoise's voice probability, summed over the blocks */
    uint64_t prime_written;                                 /* the tap's count at the last priming: the first frame then */
    uint32_t prime_target;                                  /* delivered was prime_written - prime_target (RNNoise: 480 more) */
    double in_squares, out_squares;                         /* int16-scale energies of the processed blocks: in vs out */
} MicNsStats;

typedef struct {
    const PianoTapHeader *tap;
    DenoiseState *st;              /* null: passthrough (micns_init's bypass, or no memory) */
    int bypass;
    uint64_t cursor;               /* next tap frame to take */
    uint32_t run;
    int anchored, primed;
    int32_t avg, setpoint;         /* 64ths of a frame */
    uint32_t samples;
    float block[MICNS_BLOCK];
    uint32_t block_fill;
    int16_t last;                  /* the last raw frame taken (a repeat copies it) */
    float fifo[MICNS_FIFO];
    uint64_t head, tail;           /* processed frames written / read */
    MicNsStats s;
    uint64_t (*ticks)(void);       /* the caller's clock for the timing statistics (may be null) */
} MicNs;

/* Non-real-time: the state for TAP (may be null until micns_start), RNNoise allocated unless bypass. 0, or -1 (no memory: the core
 * then passes the frames through). */
int micns_init(MicNs *m, const PianoTapHeader *tap, int bypass, uint64_t (*ticks)(void));
/* Non-real-time, at the device's StartIO: a fresh RNNoise state, an empty pipeline, the statistics zeroed; TAP the mapping now. */
int micns_start(MicNs *m, const PianoTapHeader *tap);
/* Real-time: N frames for the HAL (float, -1..1). Returns the frames that carried processed audio (the rest is silence). */
uint32_t micns_read(MicNs *m, float *out, uint32_t n);
void micns_free(MicNs *m);
/* The pipeline's fill now (frames), for tests and the log. */
uint32_t micns_fill(const MicNs *m);

#ifdef __cplusplus
}
#endif
#endif
