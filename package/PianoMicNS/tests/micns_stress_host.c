// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Offline stress for the mono tap, the route-4 stereo capture feeding it, and HAL lifecycle/format changes.
// Build: clang -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I../third_party/rnnoise/include -Dmalloc=stress_malloc -Dcalloc=stress_calloc -Dfree=stress_free -Drnnoise_process_frame=stress_real_frame -c ../third_party/rnnoise/src/denoise.c -o "${TMPDIR:-/tmp}/micns_stress_host.$$.o" && trap 'rm -f "${TMPDIR:-/tmp}/micns_stress_host.$$.o"' EXIT && clang -std=c11 -O1 -Wall -Wextra -Werror -Wno-null-dereference -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DPIANO_MICNS_TEST_TAP -I.. -I../../PianoAudio -I../third_party/rnnoise/include micns_stress_host.c ../PianoMicNS.c ../MicNsCore.c ../third_party/rnnoise/src/{rnn,rnn_data,pitch,kiss_fft,celt_lpc}.c ../../PianoAudio/{AudioSpkProt,AudioSession,AudioAmps,Fs19xx,AudioSha256,AudioSequence,PianoAudioReach,PianoGpr,AudioSafety,AudioDspLink}.c "${TMPDIR:-/tmp}/micns_stress_host.$$.o" -framework CoreAudio -framework CoreFoundation -framework IOKit -lm -o /tmp/micns_stress_host && /tmp/micns_stress_host
#include <CoreAudio/AudioServerPlugIn.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MicNsCore.h"
#include "rnnoise.h"
// Like PianoAudio's mic_fifo_host, drive the actual DSP READ return boundary with synthetic PCM.
#include "../../PianoAudio/AudioLive.c"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static unsigned checks, iteration;
static const char *scenario;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d (%s, case %u): %s\n", __FILE__, __LINE__, scenario, iteration, #c); \
    exit(1); \
} } while (0)

static const uint32_t sizes[] = {64, 128, 240, 256, 333, 512, 960, 1024, 4096};
typedef struct {
    PianoTapHeader header;
    int16_t ring[PIANO_TAP_FRAMES];
} TapMemory;
_Static_assert(sizeof(TapMemory) == PIANO_TAP_BYTES, "tap layout");
static TapMemory devices[2];
static MicNs core;
static float output[4096 + 2], saved[4096];
static float *const pcm = output + 1;
static const float sentinel = 2.0f;

typedef int16_t (*Source)(uint64_t);
static int16_t tag(uint64_t i) {return (int16_t)(1 + i % 30000u);}
static int16_t silence(uint64_t i) {(void)i; return 0;}
static int16_t high(uint64_t i) {(void)i; return INT16_MAX;}
static int16_t low(uint64_t i) {(void)i; return INT16_MIN;}
static int16_t square(uint64_t i) {return i % 97u < 48u ? INT16_MAX : INT16_MIN;}

// Track per-instance allocations even on ASan runtimes without LeakSanitizer.
// RNNoise's shared FFT table is a process-lifetime allocation in kiss_fft.c.
static unsigned allocations, allocation_calls, noise_calls;
static uint64_t tagged_frames;
static int check_tags, inject_floats;
void *stress_malloc(size_t size)
{
    ++allocation_calls;
    void *p = malloc(size);
    allocations += p != NULL;
    return p;
}
void *stress_calloc(size_t count, size_t size)
{
    ++allocation_calls;
    void *p = calloc(count, size);
    allocations += p != NULL;
    return p;
}
void stress_free(void *p)
{
    if (p) {CHECK(allocations > 0); --allocations;}
    free(p);
}
float stress_real_frame(DenoiseState *st, float *out, const float *in);
float rnnoise_process_frame(DenoiseState *st, float *out, const float *in)
{
    static const float invalid[] = {NAN, INFINITY, -INFINITY, FLT_TRUE_MIN, -FLT_TRUE_MIN, 65536.0f, -65536.0f, 16384.0f};
    ++noise_calls;
    if (check_tags)
        for (uint32_t i = 0; i < MICNS_BLOCK; ++i) CHECK(in[i] == tag(tagged_frames++));
    const float vad = stress_real_frame(st, out, in);
    // The ABI carries S16, so fault injection belongs at the processor's float output.
    if (inject_floats)
        for (uint32_t i = 0; i < MICNS_BLOCK; ++i) out[i] = invalid[i % COUNT(invalid)];
    return vad;
}

static void poison(uint32_t n)
{
    CHECK(n + 2 <= COUNT(output));
    for (uint32_t i = 0; i < n + 2; ++i) output[i] = sentinel;
}

static void check_output(uint32_t n)
{
    CHECK(output[0] == sentinel && pcm[n] == sentinel);
    for (uint32_t i = 0; i < n; ++i) CHECK(isfinite(pcm[i]) && fabsf(pcm[i]) <= 1.0f);
}

static void check_zero(uint32_t from, uint32_t n)
{
    for (uint32_t i = from; i < n; ++i) CHECK(pcm[i] == 0.0f && !signbit(pcm[i]));
}

static uint32_t read_core(uint32_t n)
{
    poison(n);
    const uint32_t got = micns_read(&core, pcm, n);
    CHECK(got <= n);
    check_output(n);
    check_zero(got, n);
    return got;
}

static void feed_mono(PianoTapHeader *tap, uint32_t n, Source source, uint32_t gain, int muted)
{
    CHECK(n <= 4096 + MICNS_RESERVE);
    int16_t samples[4096 + MICNS_RESERVE];
    const uint64_t first = audio_tap_written(tap);
    for (uint32_t i = 0; i < n; ++i) samples[i] = source(first + i);
    audio_tap_write(tap, samples, n, gain, muted, 0);
}

static AudioLive live;
static AudioLiveShared shared;
static int16_t speaker_ring[AUDIO_LIVE_RING * 2], mic_ring[AUDIO_LIVE_MIC_RING];
static uint8_t capture_dma[AUDIO_DMA_MIC + AUDIO_MIC_PERIOD_BYTES];

static void open_stereo(PianoTapHeader *tap)
{
    memset(&live, 0, sizeof live);
    audio_live_shared_init(&shared, speaker_ring, mic_ring, AUDIO_Q15_ONE);
    shared.mic_control = MicGraphReleased;
    shared.tap = tap;
    live.sh = &shared;
    live.ops.dma = capture_dma;
    CHECK(audio_live_mic_request(&shared, 0, 4) == 0);
    mic_apply_pending(&live);
    const AudioMicRouteStatus route = audio_live_mic_route_status(&shared);
    CHECK(route.applied == 4 && route.generation == 1);
    audio_live_running(&shared, 1, 1);
    CHECK(audio_live_mic_request(&shared, 0, AUDIO_MIC_ROUTE_LEGACY) == EBUSY);
    CHECK(audio_live_tap_client(&shared, 1) == 1);
}

static void feed_stereo(PianoTapHeader *tap, uint32_t n)
{
    const uint64_t first = audio_tap_written(tap);
    int16_t raw[MICNS_BLOCK];
    uint8_t *dma = capture_dma + AUDIO_DMA_MIC;
    for (uint32_t offset = 0; offset < n;) {
        const uint32_t count = n - offset > MICNS_BLOCK ? MICNS_BLOCK : n - offset;
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t left = (uint16_t)tag(first + offset + i);
            const uint16_t right = (uint16_t)~left;
            dma[4 * i] = (uint8_t)left;
            dma[4 * i + 1] = (uint8_t)(left >> 8);
            dma[4 * i + 2] = (uint8_t)right;
            dma[4 * i + 3] = (uint8_t)(right >> 8);
        }
        const ArAnswer answer = {.data_bytes = count * 4u};
        capture_period(&live, &answer, AUDIO_LIVE_MIC_SETTLE_MS);
        CHECK(audio_ring_read(&shared.in, raw, count) == count);
        for (uint32_t i = 0; i < count; ++i) CHECK(raw[i] == tag(first + offset + i));
        offset += count;
    }
    CHECK(audio_tap_written(tap) == first + n);
}

static void feed_stream(PianoTapHeader *tap, uint32_t n, int stereo)
{
    if (stereo) feed_stereo(tap, n);
    else feed_mono(tap, n, tag, 32767, 0);
}

static void test_stream(uint32_t n, int stereo, int bypass)
{
    PianoTapHeader *tap = &devices[0].header;
    audio_tap_init(tap);
    if (stereo) open_stereo(tap);
    CHECK(micns_init(&core, tap, bypass, NULL) == 0);
    check_tags = !bypass;
    tagged_frames = 0;
    CHECK(read_core(n) == 0);
    // Prime at the exact threshold so the normal initial trim discards no PCM.
    feed_stream(tap, MICNS_RESERVE + n - 1, stereo);
    CHECK(read_core(n) == 0);
    feed_stream(tap, 1, stereo);

    uint64_t consumed = 0;
    for (uint32_t read = 0; read <= MICNS_SETTLE || consumed < 2u * MICNS_FIFO; ++read) {
        CHECK(micns_fill(&core) == MICNS_RESERVE + n);
        CHECK(read_core(n) == n);
        if (bypass)
            for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == tag(consumed + i) / 32768.0f);
        consumed += n;
        CHECK(micns_fill(&core) == MICNS_RESERVE);
        // The speaker stays active while the raw Mic stops and restarts under the tap.
        if (stereo && (read == 32 || read == 64)) {
            const uint32_t epoch = shared.epoch;
            audio_live_running(&shared, 1, read == 64);
            CHECK(shared.want_out && shared.epoch == epoch && mic_wanted(&shared));
        }
        feed_stream(tap, n, stereo);
    }
    const uint64_t written = audio_tap_written(tap);
    CHECK(core.s.delivered == consumed && written == consumed + MICNS_RESERVE + n);
    CHECK(core.s.startup == 2u * n && core.s.prime_target == MICNS_RESERVE + n);
    CHECK(core.s.blocks * MICNS_BLOCK + core.block_fill == consumed + MICNS_RESERVE);
    if (check_tags) CHECK(tagged_frames == core.s.blocks * MICNS_BLOCK);
    check_tags = 0;
    CHECK(core.s.anchors == 1 && !core.s.excess && !core.s.lapped);
    CHECK(!core.s.drops && !core.s.repeats && !core.s.missing);
    if (stereo) {
        CHECK(live.mic_frames == written && !live.mic_full && !live.mic_drops && !live.mic_repeats);
        CHECK(audio_live_tap_client(&shared, -1) == 0);
        audio_live_running(&shared, 0, 0);
    }
    micns_free(&core);
    CHECK(!allocations && micns_fill(&core) == 0);
}

static void test_extremes(void)
{
    PianoTapHeader *tap = &devices[0].header;
    const uint32_t n = 333;
    audio_tap_init(tap);
    CHECK(micns_init(&core, tap, 0, NULL) == 0);
    CHECK(read_core(n) == 0);
    feed_mono(tap, MICNS_RESERVE, silence, 32767, 0);
    const Source inputs[] = {silence, high, low, square};
    for (size_t s = 0; s < COUNT(inputs); ++s) {
        for (unsigned i = 0; i < 16; ++i) {
            feed_mono(tap, n, inputs[s], 32767, 0);
            CHECK(read_core(n) == n);
            if (inputs[s] == silence) check_zero(0, n);
        }
    }
    CHECK(core.s.first_sound && isfinite(core.s.vad_sum) && isfinite(core.s.out_squares));

    // The writer stops with complete blocks and a partial RNNoise block queued.
    const uint32_t queued = MICNS_RESERVE - audio_tap_written(tap) % MICNS_BLOCK;
    CHECK(queued < MICNS_RESERVE && read_core(4096) == queued);
    CHECK(core.s.missing == 4096 - queued && read_core(n) == 0);
    micns_free(&core);
    CHECK(!allocations && read_core(n) == 0);
}

static void test_floats(void)
{
    const float expected[] = {0, 0, 0, 0, 0, 1.0f, -1.0f, 0.5f};
    PianoTapHeader *tap = &devices[0].header;
    const uint32_t n = 333;
    audio_tap_init(tap);
    CHECK(fpclassify(FLT_TRUE_MIN) == FP_SUBNORMAL);
    CHECK(micns_init(&core, tap, 0, NULL) == 0);
    CHECK(read_core(n) == 0);
    feed_mono(tap, MICNS_RESERVE, square, 32767, 0);
    inject_floats = 1;
    for (uint32_t offset = 0; offset < 8u * n; offset += n) {
        feed_mono(tap, n, square, 32767, 0);
        CHECK(read_core(n) == n);
        for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == expected[(offset + i) % COUNT(expected)]);
    }
    inject_floats = 0;
    CHECK(isfinite(core.s.out_squares));
    micns_free(&core);
    CHECK(!allocations);
}

static void test_capture_restart(void)
{
    PianoTapHeader *tap = &devices[0].header;
    const uint32_t n = 333;
    audio_tap_init(tap);
    CHECK(micns_init(&core, tap, 0, NULL) == 0);
    CHECK(read_core(n) == 0);
    feed_mono(tap, MICNS_RESERVE, square, 32767, 0);
    for (unsigned i = 0; i < 24; ++i) {
        feed_mono(tap, n, square, 32767, 0);
        CHECK(read_core(n) == n);
    }
    CHECK(core.s.first_sound && core.block_fill && micns_fill(&core) == MICNS_RESERVE);

    // A new DSP capture run on the same mapping must start with fresh denoiser history.
    const unsigned before_reset = allocation_calls;
    __atomic_add_fetch(&tap->run, 1u, __ATOMIC_RELEASE);
    CHECK(read_core(n) == 0 && micns_fill(&core) == 0);
    CHECK(core.s.runs == 1 && core.s.anchors == 2);
    feed_mono(tap, MICNS_RESERVE, silence, 32767, 0);
    for (unsigned i = 0; i < 12; ++i) {
        feed_mono(tap, n, silence, 32767, 0);
        CHECK(read_core(n) == n);
        check_zero(0, n);
    }
    CHECK(allocation_calls == before_reset);
    micns_free(&core);
    CHECK(!allocations);
}

const PianoTapHeader *piano_micns_test_tap;
int piano_micns_test_bypass;
void *PianoMicNS_Create(CFAllocatorRef allocator, CFUUIDRef type);
static AudioServerPlugInDriverRef driver;

static void read_hal(UInt32 client, uint64_t sample, uint32_t n)
{
    AudioServerPlugInIOCycleInfo info = {0};
    info.mInputTime.mSampleTime = (Float64)sample;
    info.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
    poison(n);
    CHECK((*driver)->DoIOOperation(driver, 2, 3, client, kAudioServerPlugInIOOperationReadInput, n, &info, pcm, NULL) == 0);
    check_output(n);
}

static void check_format(Float64 unsupported)
{
    const AudioObjectPropertyAddress rate = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
    const Float64 supported = PIANO_TAP_RATE;
    const OSStatus rejected = kAudioDeviceUnsupportedFormatError;
    CHECK((*driver)->SetPropertyData(driver, 2, 0, &rate, 0, NULL, sizeof unsupported, &unsupported) == rejected);
    CHECK((*driver)->SetPropertyData(driver, 2, 0, &rate, 0, NULL, sizeof supported, &supported) == 0);
    const AudioObjectPropertySelector formats[] = {kAudioStreamPropertyVirtualFormat, kAudioStreamPropertyPhysicalFormat};
    for (size_t i = 0; i < COUNT(formats); ++i) {
        const AudioObjectPropertyAddress format = {formats[i], kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioStreamBasicDescription actual;
        UInt32 size = 0;
        CHECK((*driver)->GetPropertyData(driver, 3, 0, &format, 0, NULL, sizeof actual, &size, &actual) == 0);
        CHECK(size == sizeof actual && actual.mSampleRate == supported && actual.mChannelsPerFrame == 1);
        CHECK((*driver)->SetPropertyData(driver, 3, 0, &format, 0, NULL, sizeof actual, &actual) == 0);
        AudioStreamBasicDescription request = actual;
        request.mSampleRate = unsupported;
        CHECK((*driver)->SetPropertyData(driver, 3, 0, &format, 0, NULL, sizeof request, &request) == rejected);
        request = actual;
        request.mChannelsPerFrame = 2;
        request.mBytesPerFrame = request.mBytesPerPacket = 2 * sizeof(float);
        CHECK((*driver)->SetPropertyData(driver, 3, 0, &format, 0, NULL, sizeof request, &request) == rejected);
    }
}

static void test_hal_churn(void)
{
    static const AudioServerPlugInHostInterface host = {0};
    const Float64 rates[] = {24000, 44100, 96000, NAN, INFINITY};
    driver = PianoMicNS_Create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(driver && (*driver)->Initialize(driver, (AudioServerPlugInHostRef)&host) == 0);
    for (size_t i = 0; i < COUNT(devices); ++i) audio_tap_init(&devices[i].header);

    for (iteration = 0; iteration < 128; ++iteration) {
        const uint32_t n = sizes[iteration % COUNT(sizes)];
        // The first primed read crosses the 16384-frame HAL cache boundary after 31 frames.
        const uint64_t sample = 16384u - n - 31u;
        PianoTapHeader *tap = &devices[(iteration / 2u) % COUNT(devices)].header;
        const int bypass = iteration % 2u;
        piano_micns_test_bypass = bypass;
        piano_micns_test_tap = tap;
        feed_mono(tap, MICNS_RESERVE + n, square, 32767, 0); // unread history from before StartIO
        const unsigned previous_calls = noise_calls;
        CHECK((*driver)->StartIO(driver, 2, 1) == 0 && (*driver)->StartIO(driver, 2, 2) == 0);
        CHECK(bypass ? allocations == 0 : allocations > 0);
        check_format(rates[iteration % COUNT(rates)]);
        read_hal(1, sample, n);
        check_zero(0, n);

        const uint64_t first = audio_tap_written(tap);
        const Source source = bypass ? tag : square;
        feed_mono(tap, MICNS_RESERVE + n, source, 32767, 0);
        read_hal(1, sample + n, n);
        memcpy(saved, pcm, n * sizeof *pcm);
        read_hal(2, sample + n, n);
        CHECK(!memcmp(saved, pcm, n * sizeof *pcm));
        if (bypass) for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == tag(first + i) / 32768.0f);
        CHECK(bypass ? noise_calls == previous_calls : noise_calls > previous_calls);

        CHECK((*driver)->StopIO(driver, 2, 1) == 0);
        feed_mono(tap, n, source, 32767, 1);
        read_hal(2, sample + 2u * n, n);
        check_zero(0, n);
        feed_mono(tap, n, source, 16384, 0);
        read_hal(2, sample + 3u * n, n);
        if (bypass)
            for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == tag(first + 2u * n + i) / 32768.0f * (16384.0f / 32767.0f));
        feed_mono(tap, n, source, 32767, 0);
        read_hal(2, sample + 4u * n, n);
        if (bypass) for (uint32_t i = 0; i < n; ++i) CHECK(pcm[i] == tag(first + 3u * n + i) / 32768.0f);

        CHECK((*driver)->StopIO(driver, 2, 2) == 0 && allocations == 0);
        read_hal(2, sample + 4u * n, n); // even the previously cached input time is silent after StopIO
        check_zero(0, n);
    }
    (*driver)->Release(driver);
}

int main(void)
{
    const char *streams[2][2] = {{"mono RNNoise", "mono bypass"}, {"route-4 stereo RNNoise", "route-4 stereo bypass"}};
    for (int stereo = 0; stereo < 2; ++stereo)
        for (int bypass = 0; bypass < 2; ++bypass) {
            scenario = streams[stereo][bypass];
            for (size_t i = 0; i < COUNT(sizes); ++i) {
                iteration = sizes[i];
                test_stream(sizes[i], stereo, bypass);
            }
        }
    iteration = 0;
    scenario = "full-scale input and underrun";
    test_extremes();
    scenario = "processor float output";
    test_floats();
    scenario = "tap capture restart";
    test_capture_restart();
    scenario = "HAL start/stop, format, mute and gain";
    test_hal_churn();
    printf("PASS micns stress: %u checks, 128 HAL cycles, %zu read sizes\n", checks, COUNT(sizes));
    return 0;
}
