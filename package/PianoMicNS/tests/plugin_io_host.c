// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The plug-in's IO path (PianoMicNS.c built with PIANO_MICNS_TEST_TAP: the tap is this test's memory, the core in passthrough) as
// the HAL drives it: StartIO, then ReadInput cycles of 512 frames at consecutive sample times while the tap fills 100 ppm fast in
// batches; every frame the next tap frame or a counted slip, nothing missing once primed; a second IO context reading the same
// times reads the same frames without taking more from the core; a jump in sample time re-syncs; negative times and a stopped
// device read silence; the zero time stamps sit on the 16384-frame grid.
// clang -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wno-null-dereference -I../third_party/rnnoise/include -c ../third_party/rnnoise/src/rnn.c -o /tmp/plugin_io_host.rnn.o && clang -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DPIANO_MICNS_TEST_TAP -I.. -I../../PianoAudio -I../third_party/rnnoise/include plugin_io_host.c ../PianoMicNS.c ../MicNsCore.c ../third_party/rnnoise/src/{denoise,rnn_data,pitch,kiss_fft,celt_lpc}.c /tmp/plugin_io_host.rnn.o -framework CoreAudio -framework CoreFoundation -framework IOKit -lm -o /tmp/plugin_io_host && /tmp/plugin_io_host
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MicNsCore.h"

static unsigned checks;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1);}} while (0)

static uint8_t tapmem[PIANO_TAP_BYTES] __attribute__((aligned(16)));
const PianoTapHeader *piano_micns_test_tap;
int piano_micns_test_bypass = 1;
void *PianoMicNS_Create(CFAllocatorRef allocator, CFUUIDRef type);

static AudioServerPlugInDriverRef drv;
static PianoTapHeader *tap;
static uint64_t written_index;
static double owed;
static void feed(double frames)   // the DSP's periods: whole 480-frame periods once owed, the ramp as samples
{
    owed += frames;
    int16_t p[480];
    while (owed >= 480) {
        for (int i = 0; i < 480; ++i) p[i] = (int16_t)(written_index++ & 0x3fff);
        audio_tap_write(tap, p, 480, 32767, 0, 0);
        owed -= 480;
    }
}
static void readAt(double sample, float *out, UInt32 n)
{
    AudioServerPlugInIOCycleInfo info;
    memset(&info, 0, sizeof info);
    info.mInputTime.mSampleTime = sample;
    info.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
    CHECK((*drv)->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationReadInput, n, &info, out, NULL) == 0);
}

int main(void)
{
    tap = (PianoTapHeader *)(void *)tapmem;
    audio_tap_init(tap);
    piano_micns_test_tap = tap;
    void *iface = PianoMicNS_Create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(iface);
    drv = iface;
    static const AudioServerPlugInHostInterface host = {0};
    CHECK((*drv)->Initialize(drv, (AudioServerPlugInHostRef)&host) == 0);
    CHECK((*drv)->StartIO(drv, 2, 1) == 0);
    Float64 st = -1; UInt64 ht = 0, seed = 0;
    CHECK((*drv)->GetZeroTimeStamp(drv, 2, 1, &st, &ht, &seed) == 0 && fmod(st, 16384.0) == 0 && ht > 0 && seed >= 1);
    static float a[512], b[512];
    // negative sample times (the HAL's first input times can precede the anchor): silence
    for (int i = 0; i < 512; ++i) a[i] = 1;
    readAt(-1024, a, 512);
    for (int i = 0; i < 512; ++i) CHECK(a[i] == 0.0f);
    const double bad_time[] = {NAN, INFINITY, 0x1p64};
    for (size_t j = 0; j < sizeof bad_time / sizeof bad_time[0]; ++j) {
        a[0] = a[511] = 1.0f;
        readAt(bad_time[j], a, 512);
        CHECK(a[0] == 0.0f && a[511] == 0.0f);
    }
    static float near_end[4096];
    near_end[0] = near_end[4095] = 1.0f;
    readAt(0x1.fffffffffffffp63, near_end, 4096);
    CHECK(near_end[0] == 0.0f && near_end[4095] == 0.0f);
    AudioServerPlugInIOCycleInfo oversized = {0};
    CHECK((*drv)->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationReadInput, 16385, &oversized, a, NULL)
          == kAudioHardwareBadPropertySizeError);
    // cycles: the tap 100 ppm fast, two contexts on the same times
    double S = 0;
    int prev = -1;
    uint64_t bad = 0, slips = 0, delivered = 0, primed_at = 0;
    for (int c = 0; c < 6000; ++c, S += 512) {
        feed(512 * 1.0001);
        readAt(S, a, 512);
        readAt(S, b, 512);   // the second context: the same frames (a core read here would also break a's ramp below)
        CHECK(!memcmp(a, b, sizeof a));
        int nonzero = 0;
        for (int i = 0; i < 512; ++i) nonzero |= a[i] != 0.0f;
        if (!nonzero && !primed_at) continue;
        if (!primed_at) primed_at = (uint64_t)c;
        for (int i = 0; i < 512; ++i) {
            const int v = (int)lrintf(a[i] * 32768.0f);
            if (prev >= 0) {const int d = (v - prev) & 0x3fff; if (d == 0 || d == 2) slips++; else if (d != 1) bad++;}
            prev = v;
            delivered++;
        }
    }
    CHECK(primed_at > 0 && primed_at < 20 && bad == 0 && delivered > 5900u * 512u);
    CHECK(slips > 50 && slips < 400);   // 100 ppm of 3.07 M frames: 307, less the band
    // a jump: produced from the new time on, continuity again after a re-prime
    S += 50000;
    feed(512);
    readAt(S, a, 512);
    readAt(S - 512, b, 512);
    for (int i = 0; i < 512; ++i) CHECK(b[i] == 0.0f);
    S += 512;
    prev = -1; bad = 0;
    int resumed = 0;
    for (int c = 0; c < 400; ++c, S += 512) {
        feed(512);
        readAt(S, a, 512);
        int nonzero = 0;
        for (int i = 0; i < 512; ++i) nonzero |= a[i] != 0.0f;
        if (!nonzero) continue;
        resumed++;
        for (int i = 0; i < 512; ++i) {
            const int v = (int)lrintf(a[i] * 32768.0f);
            if (prev >= 0) {const int d = (v - prev) & 0x3fff; if (d != 1 && d != 0 && d != 2) bad++;}
            prev = v;
        }
    }
    CHECK(resumed > 350 && bad <= 1);   // one discontinuity where the core re-anchored at the newest frame
    // stopped: silence
    CHECK((*drv)->StopIO(drv, 2, 1) == 0);
    for (int i = 0; i < 512; ++i) a[i] = 1;
    readAt(S, a, 512);
    for (int i = 0; i < 512; ++i) CHECK(a[i] == 0.0f);
    printf("PASS plugin IO: %u checks (primed after %llu cycles, %llu slips, two contexts identical, jump re-synced)\n", checks,
           (unsigned long long)primed_at, (unsigned long long)slips);
    return 0;
}
