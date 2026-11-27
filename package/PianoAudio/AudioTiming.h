/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* IOAudio2's `io buffer frame size` is the HAL ring modulus. It is
 * independent of both the selected HAL callback size and our kernel batching.
 * Keep the published geometry fixed across StartIO and control notifications;
 * the runtime switch changes only the reader's 5/10 ms cadence. */
#ifndef PIANO_AUDIO_TIMING_H
#define PIANO_AUDIO_TIMING_H
#include <stdint.h>

#define PIANO_AUDIO_RATE 48000u
#define PIANO_AUDIO_LEGACY_PERIOD_FRAMES 240u
#define PIANO_AUDIO_PERIOD_FRAMES 480u
#define PIANO_AUDIO_RING_FRAMES (32u * PIANO_AUDIO_LEGACY_PERIOD_FRAMES)
#define PIANO_AUDIO_ZERO_TIME_PERIOD PIANO_AUDIO_RING_FRAMES
#define PIANO_AUDIO_ENGINE_LATENCY_FRAMES PIANO_AUDIO_LEGACY_PERIOD_FRAMES
#define PIANO_AUDIO_SAFETY_FRAMES (2u * PIANO_AUDIO_LEGACY_PERIOD_FRAMES)
#define PIANO_AUDIO_IN_FLIGHT 4u
#define PIANO_AUDIO_TIMING_LEGACY 0u
#define PIANO_AUDIO_TIMING_NEW 1u
#define PIANO_AUDIO_RING_CONTRACT "fixed-ring7680:fifo4096:batch240-480"

typedef struct {
    uint32_t ringFrames, periodFrames, zeroTimePeriod, ioBufferFrames, latency, safetyOffset;
    const char *contract;
} PianoAudioTiming;

static inline const PianoAudioTiming *piano_audio_timing_profile(uint32_t mode)
{
    static const PianoAudioTiming profiles[2] = {
        {PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_LEGACY_PERIOD_FRAMES, PIANO_AUDIO_ZERO_TIME_PERIOD,
         PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_ENGINE_LATENCY_FRAMES, PIANO_AUDIO_SAFETY_FRAMES,
         "ring7680/period240/zero7680/ioring7680"},
        {PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_PERIOD_FRAMES, PIANO_AUDIO_ZERO_TIME_PERIOD,
         PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_ENGINE_LATENCY_FRAMES, PIANO_AUDIO_SAFETY_FRAMES,
         "ring7680/period480/zero7680/ioring7680"},
    };
    return &profiles[mode == PIANO_AUDIO_TIMING_NEW ? 1 : 0];
}

static inline uint64_t piano_audio_frames_to_ns(uint64_t frames)
{
    const uint64_t seconds = frames / PIANO_AUDIO_RATE;
    const uint64_t remainder = frames % PIANO_AUDIO_RATE;
    return seconds * 1000000000ull + remainder * 1000000000ull / PIANO_AUDIO_RATE;
}

static inline uint32_t piano_audio_ring_segment(uint32_t at, uint32_t count, uint32_t ring_frames)
{
    const uint32_t room = ring_frames - at;
    return count < room ? count : room;
}

static inline void piano_audio_copy_to_ring(int16_t *ring, uint32_t ring_frames, uint64_t frame,
                                            const int16_t *src, uint32_t count, uint32_t channels)
{
    uint32_t at = (uint32_t)(frame % ring_frames);
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(at, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                ring[(at + i) * channels + c] = src[(done + i) * channels + c];
        done += n;
        at = 0;
    }
}

static inline void piano_audio_copy_from_ring(int16_t *dst, const int16_t *ring, uint32_t ring_frames, uint64_t frame,
                                              uint32_t count, uint32_t channels)
{
    uint32_t at = (uint32_t)(frame % ring_frames);
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(at, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                dst[(done + i) * channels + c] = ring[(at + i) * channels + c];
        done += n;
        at = 0;
    }
}

static inline void piano_audio_zero_ring(int16_t *ring, uint32_t ring_frames, uint64_t frame,
                                         uint32_t count, uint32_t channels)
{
    uint32_t at = (uint32_t)(frame % ring_frames);
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(at, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                ring[(at + i) * channels + c] = 0;
        done += n;
        at = 0;
    }
}

#endif
