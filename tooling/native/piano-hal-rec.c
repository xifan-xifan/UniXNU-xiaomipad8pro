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

enum {kBuffers = 4};

typedef struct {
    FILE *out;
    double rate;
    uint64_t frames, callbacks, zeros, gaps, largest_gap;
    double largest_gap_at, next_sample, sum_sq;
    int peak, have_next;
} Rec;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void put16(FILE *f, uint16_t v) {fputc(v & 0xff, f); fputc(v >> 8, f);}
static void put32(FILE *f, uint32_t v) {put16(f, (uint16_t)(v & 0xffff)); put16(f, (uint16_t)(v >> 16));}

static void wav_header(FILE *f, uint32_t rate, uint32_t frames)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * 2);
}

static void on_input(void *user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *ts, UInt32 packets,
                     const AudioStreamPacketDescription *desc)
{
    (void)desc;
    Rec *r = user;
    const int16_t *s = b->mAudioData;
    const uint32_t n = b->mAudioDataByteSize / 2;
    r->callbacks++;
    if (ts && (ts->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (r->have_next) {
            const double d = ts->mSampleTime - r->next_sample;
            if (d > 0.5 || d < -0.5) {
                r->gaps++;
                const uint64_t g = (uint64_t)fabs(d);
                if (g > r->largest_gap) {r->largest_gap = g; r->largest_gap_at = (double)r->frames / r->rate;}
            }
        }
        r->next_sample = ts->mSampleTime + n;
        r->have_next = 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const int v = s[i];
        if (!v) r->zeros++;
        if (abs(v) > r->peak) r->peak = abs(v);
        r->sum_sq += (double)v * v;
    }
    fwrite(s, 2, n, r->out);
    r->frames += n;
    (void)packets;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 3) {fprintf(stderr, "usage: %s SECONDS OUT.wav [RATE]\n", argv[0]); return 2;}
    const double seconds = atof(argv[1]);
    const double rate = argc > 3 ? atof(argv[3]) : 48000.0;
    if (seconds <= 0 || seconds > 3600 || rate < 8000 || rate > 192000) {fprintf(stderr, "bad seconds or rate\n"); return 2;}
    Rec r;
    memset(&r, 0, sizeof r);
    r.rate = rate;
    r.out = fopen(argv[2], "wb");
    if (!r.out) {perror(argv[2]); return 1;}
    wav_header(r.out, (uint32_t)rate, 0);
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = rate; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 1; fmt.mBitsPerChannel = 16; fmt.mBytesPerFrame = 2; fmt.mFramesPerPacket = 1; fmt.mBytesPerPacket = 2;
    AudioQueueRef q;
    OSStatus st = AudioQueueNewInput(&fmt, on_input, &r, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &q);
    if (st) {fprintf(stderr, "hal-rec: AudioQueueNewInput %d\n", (int)st); return 1;}
    const UInt32 bytes = (UInt32)(rate / 10) * 2;   /* 100 ms per buffer */
    for (int i = 0; i < kBuffers; ++i) {
        AudioQueueBufferRef b;
        if ((st = AudioQueueAllocateBuffer(q, bytes, &b)) || (st = AudioQueueEnqueueBuffer(q, b, 0, NULL))) {
            fprintf(stderr, "hal-rec: buffer %d\n", (int)st);
            return 1;
        }
    }
    printf("hal-rec: start %lld\n", now_ms());
    fflush(stdout);
    if ((st = AudioQueueStart(q, NULL))) {fprintf(stderr, "hal-rec: AudioQueueStart %d\n", (int)st); return 1;}
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    AudioQueueStop(q, true);
    printf("hal-rec: stop %lld\n", now_ms());
    AudioQueueDispose(q, true);
    wav_header(r.out, (uint32_t)rate, (uint32_t)r.frames);
    fclose(r.out);
    const double rms = r.frames ? sqrt(r.sum_sq / (double)r.frames) : 0;
    printf("hal-rec: callbacks %llu frames %llu expected %.0f gaps %llu (largest %llu frames at %.2f s) peak %.1f dBFS rms %.1f dBFS "
           "zeros %.1f%%\n", (unsigned long long)r.callbacks, (unsigned long long)r.frames, seconds * rate,
           (unsigned long long)r.gaps, (unsigned long long)r.largest_gap, r.largest_gap_at,
           r.peak ? 20 * log10(r.peak / 32768.0) : -999.0, rms > 0 ? 20 * log10(rms / 32768.0) : -999.0,
           r.frames ? 100.0 * (double)r.zeros / (double)r.frames : 100.0);
    return 0;
}
