/* SPDX-License-Identifier: MIT */
/* piano-hal-rec: record the default input through the CoreAudio HAL (AudioQueue), as an app does, to a WAV file, and report what
 * the HAL delivered: callbacks, frames, sample-time discontinuities, level, and the share of exact zeros (a TCC denial or a muted
 * path delivers zeros). Unlike piano-mic-diag (which opens PianoAudio's Mic directly), this goes through coreaudiod, so it starts and
 * stops the device the way QuickTime or Voice Memos do (DUPLEX.txt: the duplex stutter's top candidate is the HAL restarting the
 * Speaker when a recorder starts).
 *
 * Build: clang -Wall -Wextra -Werror -O2 piano-hal-rec.c -framework AudioToolbox -framework CoreFoundation -o piano-hal-rec
 * Use:   piano-hal-rec SECONDS OUT.wav [RATE]   (48000 by default; one channel, 16 bit)
 * Output (stdout, one line each): "hal-rec: start <unix ms>", "hal-rec: stop <unix ms>", then
 *   "hal-rec: callbacks N frames F expected E gaps G (largest L frames at S s) peak P dBFS rms R dBFS zeros Z%"
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "audio-cli.h"
#include "audio-wav.h"

enum {kBuffers = 4};

typedef struct {
    FILE *out;
    double rate;
    uint64_t frames, callbacks, zeros, gaps, largest_gap;
    double largest_gap_at, next_sample, sum_sq;
    int peak, have_next, write_failed;
    OSStatus enqueue_error;
} Rec;

static long long now_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void on_input(void *user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts, UInt32 packets,
                     const AudioStreamPacketDescription *desc)
{
    (void)desc;
    Rec *r = user;
    const int16_t *s = b->mAudioData;
    const uint32_t bytes = b->mAudioDataByteSize;
    const uint32_t n = bytes / sizeof *s;
    if (r->write_failed || r->enqueue_error)
        return;
    if (bytes > b->mAudioDataBytesCapacity || bytes % sizeof *s ||
        n > AUDIO_WAV_MAX_MONO_FRAMES - r->frames || (n && (!s || fwrite(s, sizeof *s, n, r->out) != n))) {
        r->write_failed = 1;
        return;
    }
    r->callbacks++;
    if (ts && (ts->mFlags & kAudioTimeStampSampleTimeValid) && isfinite(ts->mSampleTime)) {
        if (r->have_next) {
            const double d = ts->mSampleTime - r->next_sample;
            if (d > 0.5 || d < -0.5) {
                r->gaps++;
                const uint64_t g = fabs(d) >= 0x1p64 ? UINT64_MAX : (uint64_t)fabs(d);
                if (g > r->largest_gap) {r->largest_gap = g; r->largest_gap_at = (double)r->frames / r->rate;}
            }
        }
        r->next_sample = ts->mSampleTime + n;
        r->have_next = 1;
    } else {
        r->have_next = 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const int v = s[i];
        if (!v) r->zeros++;
        if (abs(v) > r->peak) r->peak = abs(v);
        r->sum_sq += (double)v * v;
    }
    r->frames += n;
    (void)packets;
    r->enqueue_error = AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {fprintf(stderr, "usage: %s SECONDS OUT.wav [RATE]\n", argv[0]); return 2;}
    double seconds, rate = 48000;
    if (!audio_parse_double(argv[1], &seconds) || seconds <= 0 || seconds > 3600 ||
        (argc == 4 && !audio_parse_double(argv[3], &rate)) ||
        rate < 8000 || rate > 192000 || rate != floor(rate)) {
        fprintf(stderr, "bad seconds or rate\n"); return 2;
    }
    Rec r;
    memset(&r, 0, sizeof r);
    r.rate = rate;
    r.out = fopen(argv[2], "wb");
    if (!r.out) {perror(argv[2]); return 1;}
    if (audio_wav_write_header(r.out, (uint32_t)rate, 0)) {
        fprintf(stderr, "hal-rec: cannot write WAV header\n");
        fclose(r.out);
        return 1;
    }
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = rate; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 1; fmt.mBitsPerChannel = 16; fmt.mBytesPerFrame = 2; fmt.mFramesPerPacket = 1; fmt.mBytesPerPacket = 2;
    AudioQueueRef q;
    OSStatus st = AudioQueueNewInput(&fmt, on_input, &r, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &q);
    if (st) {fprintf(stderr, "hal-rec: AudioQueueNewInput %d\n", (int)st); fclose(r.out); return 1;}
    const UInt32 bytes = (UInt32)(rate / 10) * 2;   /* 100 ms per buffer */
    for (int i = 0; i < kBuffers; ++i) {
        AudioQueueBufferRef b;
        if ((st = AudioQueueAllocateBuffer(q, bytes, &b)) || (st = AudioQueueEnqueueBuffer(q, b, 0, NULL))) {
            fprintf(stderr, "hal-rec: buffer %d\n", (int)st);
            goto fail;
        }
    }
    printf("hal-rec: start %lld\n", now_ms());
    fflush(stdout);
    if ((st = AudioQueueStart(q, NULL))) {fprintf(stderr, "hal-rec: AudioQueueStart %d\n", (int)st); goto fail;}
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    const OSStatus stop_error = AudioQueueStop(q, true);
    printf("hal-rec: stop %lld\n", now_ms());
    const OSStatus dispose_error = AudioQueueDispose(q, true);
    const int header_error = audio_wav_write_header(r.out, (uint32_t)rate, r.frames);
    const int close_error = fclose(r.out);
    const double rms = r.frames ? sqrt(r.sum_sq / (double)r.frames) : 0;
    printf("hal-rec: callbacks %llu frames %llu expected %.0f gaps %llu (largest %llu frames at %.2f s) peak %.1f dBFS rms %.1f dBFS "
           "zeros %.1f%%\n", (unsigned long long)r.callbacks, (unsigned long long)r.frames, seconds * rate,
           (unsigned long long)r.gaps, (unsigned long long)r.largest_gap, r.largest_gap_at,
           r.peak ? 20 * log10(r.peak / 32768.0) : -999.0, rms > 0 ? 20 * log10(rms / 32768.0) : -999.0,
           r.frames ? 100.0 * (double)r.zeros / (double)r.frames : 100.0);
    if (r.write_failed || header_error || close_error) {
        fprintf(stderr, "hal-rec: could not write complete WAV\n");
        return 1;
    }
    if (r.enqueue_error || stop_error || dispose_error) {
        fprintf(stderr, "hal-rec: queue enqueue %d stop %d dispose %d\n",
                (int)r.enqueue_error, (int)stop_error, (int)dispose_error);
        return 1;
    }
    if (!r.frames) {
        fprintf(stderr, "hal-rec: no PCM frames captured\n");
        return 1;
    }
    return 0;
fail:
    AudioQueueDispose(q, true);
    fclose(r.out);
    return 1;
}
