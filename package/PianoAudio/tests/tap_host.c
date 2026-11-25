// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host tests of the microphone tap's ABI (AudioTap.h), shared by the kext (writer) and the "Mic (noise reduced)" plug-in (reader):
// the header's offsets, the ring's wrap, a reader that keeps up, a reader that was lapped (refused), a reader whose copy the writer
// overtook while it copied (refused by the second load), and the writer's metadata.
//   cc -std=c11 -Wall -Wextra -Werror tap_host.c && ./a.out
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioTap.h"

static unsigned checks;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1);}} while (0)

static uint8_t mem[PIANO_TAP_BYTES] __attribute__((aligned(16)));

int main(void)
{
    CHECK(offsetof(PianoTapHeader, written) == 16 && offsetof(PianoTapHeader, run) == 24 && offsetof(PianoTapHeader, gain_q15) == 28);
    CHECK(offsetof(PianoTapHeader, muted) == 32 && offsetof(PianoTapHeader, clients) == 40 && offsetof(PianoTapHeader, periods) == 52);
    CHECK(PIANO_TAP_BYTES == 64u + 32768u && (PIANO_TAP_FRAMES & PIANO_TAP_MASK) == 0);
    PianoTapHeader *h = (PianoTapHeader *)(void *)mem;
    memset(mem, 0xa5, sizeof mem);
    audio_tap_init(h);
    CHECK(h->magic == PIANO_TAP_MAGIC && h->version == 1 && h->frames == 16384 && h->rate == 48000 && h->gain_q15 == 32767);
    CHECK(audio_tap_written(h) == 0 && !h->run && !h->muted && audio_tap_ring(h)[PIANO_TAP_FRAMES - 1] == 0);
    // a reader keeping up across three wraps: every frame once, in order
    int16_t period[480], got[2048];
    uint64_t rd = 0, value = 0;
    uint32_t mismatches = 0;
    for (int p = 0; p < 3 * 16384 / 480 + 5; ++p) {
        for (int i = 0; i < 480; ++i) period[i] = (int16_t)((p * 480 + i) & 0x7fff);
        audio_tap_write(h, period, 480, 16000, p & 1, (uint32_t)p * 10);
        if (p % 3 == 2) {   // the reader every 30 ms
            const uint64_t wr = audio_tap_written(h);
            const uint32_t n = (uint32_t)(wr - rd);
            CHECK(n <= 2048 && audio_tap_read(h, rd, got, n));
            for (uint32_t i = 0; i < n; ++i, ++value) mismatches += got[i] != (int16_t)(value & 0x7fff);
            rd = wr;
        }
    }
    CHECK(mismatches == 0 && rd > 3u * PIANO_TAP_FRAMES);
    CHECK(h->periods == 3 * 16384 / 480 + 5 && h->gain_q15 == 16000 && h->write_ms == (3 * 16384 / 480 + 4) * 10u);
    CHECK(h->muted == (uint32_t)((3 * 16384 / 480 + 4) & 1));
    // a reader that fell more than a ring behind: its oldest frames are gone
    const uint64_t behind = audio_tap_written(h) - PIANO_TAP_FRAMES - 1;
    CHECK(!audio_tap_read(h, behind, got, 16));
    // exactly a ring behind: still whole
    CHECK(audio_tap_read(h, audio_tap_written(h) - PIANO_TAP_FRAMES, got, 16));
    // the writer overtakes a copy in progress: the second load refuses it
    const uint64_t from = audio_tap_written(h) - 100;
    CHECK(audio_tap_read(h, from, got, 100));
    for (int p = 0; p < (int)(PIANO_TAP_FRAMES / 480) + 1; ++p) audio_tap_write(h, period, 480, 32767, 0, 0);
    CHECK(!audio_tap_read(h, from, got, 100));
    printf("PASS tap: %u checks\n", checks);
    return 0;
}
