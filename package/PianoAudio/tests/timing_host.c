/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Replay HAL's independently cached modulo-write contract against the device
 * reader, including the former small-prefix failure and both batching modes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../AudioTiming.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "timing_host:%d: %s\n", __LINE__, #x); return 1; } ++checks; } while (0)
static int16_t ring[65536u * 2u], src[1024u * 2u], dst[PIANO_AUDIO_PERIOD_FRAMES * 2u];
static int16_t sample(uint32_t frame) { return (int16_t)(1u + frame % 30000u); }

static int replay(uint32_t mode, uint32_t halRing, uint32_t io, int good)
{
    const PianoAudioTiming *tp = piano_audio_timing_profile(mode);
    const uint32_t readerRing = !good && mode == 1 ? 65536u : tp->ringFrames;
    uint32_t matched = 0, nonzero = 0, read = 0;
    memset(ring, 0, sizeof ring);
    /* HAL output time is ahead of now by safety plus the selected callback.
     * Only the advertised ring is used for its modulo, not the mapped size. */
    const uint32_t lead = tp->safetyOffset + io;
    for (uint32_t now = 0; now < 12u * PIANO_AUDIO_RATE; ++now) {
        if (now % io == 0) {
            const uint32_t first = now + lead;
            for (uint32_t i = 0; i < io; ++i) {
                src[2u*i] = sample(first + i);
                src[2u*i+1u] = (int16_t)-src[2u*i];
            }
            /* Independent of the reader helpers, as in HAL's split memcpy. */
            uint32_t at = first % halRing, n = io < halRing - at ? io : halRing - at;
            memcpy(ring + 2u*at, src, n * 4u);
            if (n < io) memcpy(ring, src + 2u*n, (io - n) * 4u);
        }
        if (now && now % tp->periodFrames == 0) {
            const uint32_t first = now - tp->periodFrames;
            piano_audio_copy_from_ring(dst, ring, readerRing, first, tp->periodFrames, 2);
            piano_audio_zero_ring(ring, readerRing, first, tp->periodFrames, 2);
            if (first >= lead) {
                for (uint32_t i = 0; i < tp->periodFrames; ++i) {
                    ++read;
                    nonzero += dst[2u*i] != 0;
                    matched += dst[2u*i] == sample(first+i) && dst[2u*i+1u] == -sample(first+i);
                }
            }
        }
    }
    if (good) CHECK(matched == read && read > 500000u);
    else CHECK(nonzero < read / 8u && matched < read / 8u);
    printf("HAL replay: mode=%u published-ring=%u callback=%u reader-ring=%u matched=%u/%u nonzero=%u (%s)\n",
           mode, halRing, io, readerRing, matched, read, nonzero, good ? "continuous" : "old failure rejected");
    return 0;
}

int main(void)
{
    const PianoAudioTiming *a = piano_audio_timing_profile(0), *b = piano_audio_timing_profile(1);
    CHECK(a->ringFrames == 7680u && b->ringFrames == a->ringFrames);
    CHECK(a->ioBufferFrames == a->ringFrames && b->ioBufferFrames == a->ioBufferFrames);
    CHECK(a->zeroTimePeriod == a->ringFrames && b->zeroTimePeriod == a->zeroTimePeriod);
    CHECK(a->safetyOffset == 480u && b->safetyOffset == a->safetyOffset);
    CHECK(a->latency == 240u && b->latency == a->latency);
    CHECK(a->periodFrames == 240u && b->periodFrames == 480u);
    CHECK(piano_audio_frames_to_ns(a->periodFrames) == 5000000u);
    CHECK(piano_audio_frames_to_ns(b->periodFrames) == 10000000u);
    const uint64_t five_days = 5ull * 24 * 3600;
    const uint64_t frames = five_days * PIANO_AUDIO_RATE + a->periodFrames;
    CHECK(piano_audio_frames_to_ns(frames) == five_days * 1000000000ull + 5000000u);
    CHECK(piano_audio_frames_to_ns(frames + 1) - piano_audio_frames_to_ns(frames) == 20833u);
    CHECK(a->ioBufferFrames * 3u / 8u >= 1024u); /* HAL's real maximum callback */
    const uint32_t callbacks[] = {64, 128, 256, 512, 1024};
    for (uint32_t mode = 0; mode < 2; ++mode)
        for (unsigned i = 0; i < sizeof callbacks / sizeof callbacks[0]; ++i)
            CHECK(replay(mode, piano_audio_timing_profile(mode)->ioBufferFrames, callbacks[i], 1) == 0);
    CHECK(replay(0, 240, 64, 0) == 0);
    CHECK(replay(1, 240, 64, 0) == 0);
    /* Restart with the other cadence: a fresh origin and cleared ring. */
    CHECK(replay(0, a->ioBufferFrames, 512, 1) == 0);
    /* An absolute HAL cursor crosses 2^32 within one batch. The physical
     * wrap is 7680 frames, which does not divide 2^32. */
    const uint32_t first = UINT32_MAX - 119u;
    for (uint32_t i = 0; i < a->periodFrames; ++i) src[i] = (int16_t)(i + 1);
    memset(ring, 0, sizeof ring);
    piano_audio_copy_to_ring(ring, a->ringFrames, first, src, a->periodFrames, 1);
    for (uint32_t i = 0; i < a->periodFrames; ++i)
        CHECK(ring[((uint64_t)first + i) % a->ringFrames] == src[i]);
    piano_audio_copy_from_ring(dst, ring, a->ringFrames, first, a->periodFrames, 1);
    CHECK(!memcmp(dst, src, a->periodFrames * sizeof *src));
    piano_audio_zero_ring(ring, a->ringFrames, first, a->periodFrames, 1);
    for (uint32_t i = 0; i < a->ringFrames; ++i) CHECK(!ring[i]);
    puts("PianoAudio HAL-ring contract and restart replay passed");
    return 0;
}
