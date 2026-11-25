// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-vpio-probe: does macOS's own CPU voice processing (VPIO: AUVoiceIO, kAudioUnitSubType_VoiceProcessingIO, the unit FaceTime,
// Messages, Safari WebRTC and AVAudioEngine voice-processing apps run) cancel echo and suppress noise on this tablet? It plays a
// far-end WAV (music) and records the microphone at the same time, in one of three modes:
//   vpio    VPIO with its processing on (echo cancellation, noise suppression, AGC)
//   bypass  the same VPIO unit with kAUVoiceIOProperty_BypassVoiceProcessing = 1: same devices, aggregate and resampling, no
//           processing (the control)
//   raw     no VPIO: one AUHAL unit on the default output device, one on the default input device (the second control)
//   avrec   (mic-ns3) a plain recorder: AVAudioRecorder on the default input, no far end (FAR.wav ignored), 48 kHz into OUT.wav
// --input UID (mic-ns3): that HAL device ("PianoMicNS" is "Mic (noise reduced)") as the system default input for the run, restored at
// the end, as a user picks it in Sound settings for Voice Memos or FaceTime. --list: the HAL's devices (name, UID, input channels,
// transport) and AVFoundation's audio capture devices (QuickTime Player's microphone menu).
// It writes the recording (mono, 16 bit) and reports, like piano-hal-rec, frames, sample-time gaps, level and the share of exact
// zeros: a process the microphone grant does not cover records zeros while the HAL runs IO, so a ~100 % zeros run is a TCC denial
// or a muted path, never a result.
//
//   piano-vpio-probe --tcc
//       mic_status=N (AVCaptureDevice audio authorization: 0 not determined, 1 restricted, 2 denied, 3 authorized), the microphone
//       modes (0 standard, 1 wide spectrum, 2 voice isolation; the last two override the bypass) and the default devices. Opens no
//       audio device.
//   piano-vpio-probe MODE SECONDS FAR.wav|none OUT.wav [--rate HZ] [--far-delay S] [--duck default|min|mid|max|unset]
//                    [--advanced-duck 0|1] [--agc 0|1] [--ready FILE] [--done FILE] [--stop FILE]
//       FAR.wav: 16-bit PCM, mono or stereo (mixed to mono), played once from the first output buffer after --far-delay seconds of
//       silence, then silence; none = silence only. --rate: the far end's and the recording's client rate (default: the WAV's, else
//       48000; the WAV must have it). raw records at the input device's rate and decimates to --rate (integer ratios) at the end.
//       --duck: kAUVoiceIOProperty_OtherAudioDuckingConfiguration (macOS 14), default min with advanced ducking off, so other
//       apps' audio stays as loud as VPIO allows; unset leaves the unit's default. --agc: kAUVoiceIOProperty_VoiceProcessingEnableAGC
//       (default: the unit's own). --ready is written when the first input buffer arrived, --done when OUT.wav is complete; the run
//       ends early when the --stop file exists.
// Output, one line each, "vpio-probe: ...": the setup calls with their OSStatus, the devices and formats, "io-start <unix ms>",
// "stop <unix ms>", the properties read back while running (bypass, AGC, mute, ducking), "timing far-start-in-rec-s T" (the first
// far-end frame's output time stamp minus the first input frame's capture time stamp; the acoustic echo delay adds the device
// latencies), "overloads ...", "output ...", and the summary
//   "vpio-probe: callbacks N frames F expected E gaps G (largest L frames at S s) peak P dBFS rms R dBFS zeros Z% clipped C
//    render-errors X (last S)".
// Exit: 0 done (also when the input was all zeros: the summary says so), 1 setup failed, 2 usage.
// Build: make -C tooling/native/piano-vpio-probe (arm64, macOS 14, ad-hoc signed, Info.plist with the microphone purpose string
// embedded).
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

enum Mode {MODE_VPIO, MODE_BYPASS, MODE_RAW, MODE_AVREC};
enum {kMaxSlice = 16384, kVpioSlice = 4096};
static const char *const mode_names[] = {"vpio", "bypass", "raw", "avrec"};

typedef struct {
    enum Mode mode;
    AudioUnit vpio, out_unit, in_unit;
    double rate, rec_rate;          // client rate of the far end; rate the input callback delivers (raw: the device's)
    const float *far;
    uint64_t far_frames, far_delay_frames, out_frames, out_callbacks;
    uint64_t out_first_host, far_host, in_first_host;
    int out_first_valid, far_valid, far_marked, in_first_valid;
    int16_t *rec;
    uint64_t rec_cap;
    _Atomic uint64_t rec_frames;
    float *slice;
    uint64_t in_callbacks, zeros, gaps, largest_gap, clipped, render_errors, oversize;
    OSStatus last_render_error;
    double largest_gap_at, next_sample, sum_sq;
    float peak;
    int have_next;
    atomic_int started;
    atomic_uint overloads_in, overloads_out, overloads_unit;
} Probe;

static mach_timebase_info_data_t timebase;

static double host_s(uint64_t t) {return (double)t * timebase.numer / timebase.denom / 1e9;}
static uint64_t host_ticks(double s) {return (uint64_t)(s * 1e9 * timebase.denom / timebase.numer);}

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static void put16(FILE *f, uint16_t v) {fputc(v & 0xff, f); fputc(v >> 8, f);}
static void put32(FILE *f, uint32_t v) {put16(f, (uint16_t)(v & 0xffff)); put16(f, (uint16_t)(v >> 16));}

static int write_wav(const char *path, uint32_t rate, const int16_t *s, uint64_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) {perror(path); return -1;}
    fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + frames * 2)); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)(frames * 2));
    for (uint64_t i = 0; i < frames; ++i) put16(f, (uint16_t)s[i]);
    return fclose(f) ? -1 : 0;
}

static uint32_t le32(const uint8_t *b) {return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;}
static uint16_t le16(const uint8_t *b) {return (uint16_t)(b[0] | b[1] << 8);}

// 16-bit PCM (also WAVE_FORMAT_EXTENSIBLE), any channel count, mixed to mono float.
static int load_wav(const char *path, float **out, uint64_t *frames, double *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) {perror(path); return -1;}
    uint8_t h[12];
    int rc = -1, have_fmt = 0;
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "vpio-probe: %s: not a RIFF/WAVE file\n", path);
        goto out;
    }
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) {fprintf(stderr, "vpio-probe: %s: no data chunk\n", path); goto out;}
        const uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t b[40] = {0};
            const size_t want = len < sizeof b ? len : sizeof b;
            if (len < 16 || fread(b, 1, want, f) != want) {fprintf(stderr, "vpio-probe: %s: bad fmt chunk\n", path); goto out;}
            if (fseek(f, (long)(len - want + (len & 1)), SEEK_CUR)) goto out;
            fmt = le16(b); ch = le16(b + 2); sr = le32(b + 4); bits = le16(b + 14);
            if (fmt == 0xfffe && len >= 26) fmt = le16(b + 24);
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt || fmt != 1 || bits != 16 || !ch || !sr) {
                fprintf(stderr, "vpio-probe: %s: 16-bit PCM only (format %u, %u bit, %u channels)\n", path, fmt, bits, ch);
                goto out;
            }
            const uint64_t n = len / (2u * ch);
            int16_t *raw = malloc(n * ch * 2 + 2);
            float *mono = malloc(n * sizeof(float) + sizeof(float));
            if (!raw || !mono || fread(raw, 2, n * ch, f) != n * ch) {
                fprintf(stderr, "vpio-probe: %s: short data\n", path);
                free(raw); free(mono);
                goto out;
            }
            for (uint64_t i = 0; i < n; ++i) {
                float acc = 0;
                for (uint16_t k = 0; k < ch; ++k) acc += (float)(int16_t)le16((const uint8_t *)&raw[i * ch + k]) / 32768.0f;
                mono[i] = acc / ch;
            }
            free(raw);
            *out = mono; *frames = n; *rate = sr;
            rc = 0;
            goto out;
        } else if (fseek(f, (long)(len + (len & 1)), SEEK_CUR)) {
            goto out;
        }
    }
out:
    fclose(f);
    return rc;
}

static OSStatus on_render(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n,
                          AudioBufferList *io)
{
    Probe *p = user;
    (void)flags; (void)bus;
    const int host_ok = ts && (ts->mFlags & kAudioTimeStampHostTimeValid);
    if (!p->out_callbacks && host_ok) {p->out_first_host = ts->mHostTime; p->out_first_valid = 1;}
    if (!p->far_marked && p->out_frames + n > p->far_delay_frames) {
        if (host_ok) {
            p->far_host = ts->mHostTime + host_ticks((double)(p->far_delay_frames - p->out_frames) / p->rate);
            p->far_valid = 1;
        }
        p->far_marked = 1;
    }
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
        float *o = io->mBuffers[b].mData;
        const UInt32 cap = io->mBuffers[b].mDataByteSize / sizeof(float);
        for (UInt32 i = 0; i < n && i < cap; ++i) {
            const uint64_t k = p->out_frames + i;
            o[i] = p->far && k >= p->far_delay_frames && k - p->far_delay_frames < p->far_frames ? p->far[k - p->far_delay_frames]
                                                                                                 : 0.0f;
        }
    }
    p->out_frames += n;
    p->out_callbacks++;
    return noErr;
}

static OSStatus on_input(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n,
                         AudioBufferList *unused)
{
    Probe *p = user;
    (void)unused;
    if (n > kMaxSlice) {p->oversize++; n = kMaxSlice;}
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize = n * (UInt32)sizeof(float);
    abl.mBuffers[0].mData = p->slice;
    const OSStatus st = AudioUnitRender(p->mode == MODE_RAW ? p->in_unit : p->vpio, flags, ts, bus, n, &abl);
    if (st) {p->render_errors++; p->last_render_error = st; return noErr;}
    const uint32_t got = abl.mBuffers[0].mDataByteSize / sizeof(float);
    uint64_t frames = atomic_load_explicit(&p->rec_frames, memory_order_relaxed);
    if (ts && (ts->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (p->have_next) {
            const double d = ts->mSampleTime - p->next_sample;
            if (d > 0.5 || d < -0.5) {
                p->gaps++;
                const uint64_t g = (uint64_t)fabs(d);
                if (g > p->largest_gap) {p->largest_gap = g; p->largest_gap_at = (double)frames / p->rec_rate;}
            }
        }
        p->next_sample = ts->mSampleTime + got;
        p->have_next = 1;
    }
    if (!p->in_callbacks && ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) {p->in_first_host = ts->mHostTime; p->in_first_valid = 1;}
    p->in_callbacks++;
    for (uint32_t i = 0; i < got && frames < p->rec_cap; ++i) {
        const float v = p->slice[i], a = fabsf(v);
        if (v == 0.0f) p->zeros++;
        if (a > p->peak) p->peak = a;
        if (a >= 0.999f) p->clipped++;
        p->sum_sq += (double)v * v;
        const float s = v * 32768.0f;
        p->rec[frames++] = (int16_t)(s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : lrintf(s));
    }
    atomic_store_explicit(&p->rec_frames, frames, memory_order_release);
    atomic_store(&p->started, 1);
    return noErr;
}

static OSStatus on_overload(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress *a, void *user)
{
    (void)id; (void)n; (void)a;
    atomic_fetch_add((atomic_uint *)user, 1);
    return noErr;
}

static const AudioObjectPropertyAddress overload_addr = {kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                                         kAudioObjectPropertyElementMain};

static void listen_overload(AudioObjectID id, atomic_uint *counter)
{
    if (id != kAudioObjectUnknown) AudioObjectAddPropertyListener(id, &overload_addr, on_overload, counter);
}

static void unlisten_overload(AudioObjectID id, atomic_uint *counter)
{
    if (id != kAudioObjectUnknown) AudioObjectRemovePropertyListener(id, &overload_addr, on_overload, counter);
}

static AudioObjectID default_device(int input)
{
    const AudioObjectPropertyAddress a = {input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
                                          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &id) ? kAudioObjectUnknown : id;
}

static void device_string(AudioObjectID id, AudioObjectPropertySelector sel, char *buf, size_t n);
static long device_u32(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope);

// mic-ns3: the HAL's devices by UID, and the system default input set for one run (restored at the end).
static AudioObjectID device_by_uid(const char *uid)
{
    const AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    const OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof u, &u, &size, &id);
    CFRelease(u);
    return st ? kAudioObjectUnknown : id;
}

static OSStatus set_default_input(AudioObjectID id)
{
    const AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof id, &id);
}

static long input_channels(AudioObjectID id)
{
    const AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                          kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &size) || !size) return 0;
    AudioBufferList *l = malloc(size);
    long n = 0;
    if (l && !AudioObjectGetPropertyData(id, &a, 0, NULL, &size, l))
        for (UInt32 i = 0; i < l->mNumberBuffers; ++i) n += l->mBuffers[i].mNumberChannels;
    free(l);
    return n;
}

static void list_devices(void)
{
    const AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    AudioObjectID ids[64];
    if (size > sizeof ids) size = sizeof ids;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids)) size = 0;
    for (UInt32 i = 0; i < size / sizeof ids[0]; ++i) {
        char name[256], uid[256];
        device_string(ids[i], kAudioObjectPropertyName, name, sizeof name);
        device_string(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof uid);
        const long t = device_u32(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
        printf("hal-device id %u name \"%s\" uid \"%s\" input-channels %ld transport '%c%c%c%c' alive %ld\n", (unsigned)ids[i], name, uid,
               input_channels(ids[i]), (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t,
               device_u32(ids[i], kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal));
    }
    @autoreleasepool {   // what QuickTime Player's microphone menu lists (AVFoundation's audio capture devices)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        for (AVCaptureDevice *d in [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio])
            printf("av-device name \"%s\" uid \"%s\"\n", d.localizedName.UTF8String, d.uniqueID.UTF8String);
#pragma clang diagnostic pop
    }
}

static void device_string(AudioObjectID id, AudioObjectPropertySelector sel, char *buf, size_t n)
{
    const AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = NULL;
    UInt32 size = sizeof s;
    snprintf(buf, n, "?");
    if (id == kAudioObjectUnknown || AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &s) || !s) return;
    if (!CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8)) snprintf(buf, n, "?");
    CFRelease(s);
}

static long device_u32(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof v;
    return AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &v) ? -1 : (long)v;
}

static double device_rate(AudioObjectID id)
{
    const AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    Float64 v = 0;
    UInt32 size = sizeof v;
    return AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &v) ? 0 : v;
}

static long stream_latency(AudioObjectID id, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    AudioStreamID streams[8];
    UInt32 size = sizeof streams;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, streams) || size < sizeof streams[0]) return -1;
    return device_u32(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
}

static void print_device(const char *what, AudioObjectID id, int input)
{
    char name[256], uid[256];
    device_string(id, kAudioObjectPropertyName, name, sizeof name);
    device_string(id, kAudioDevicePropertyDeviceUID, uid, sizeof uid);
    const AudioObjectPropertyScope sc = input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    printf("vpio-probe: device %s id %u \"%s\" uid \"%s\" nominal %.0f latency %ld safety %ld buffer %ld stream-latency %ld "
           "clock-domain %ld\n", what, (unsigned)id, name, uid, device_rate(id), device_u32(id, kAudioDevicePropertyLatency, sc),
           device_u32(id, kAudioDevicePropertySafetyOffset, sc),
           device_u32(id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal), stream_latency(id, sc),
           device_u32(id, kAudioDevicePropertyClockDomain, kAudioObjectPropertyScopeGlobal));
}

static void print_format(const char *what, AudioUnit u, AudioUnitScope sc, AudioUnitElement el)
{
    AudioStreamBasicDescription f;
    UInt32 size = sizeof f;
    const OSStatus st = AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat, sc, el, &f, &size);
    if (st) {printf("vpio-probe: format %s rc %d\n", what, (int)st); return;}
    printf("vpio-probe: format %s %.0f Hz %u ch %u bit flags 0x%x\n", what, f.mSampleRate, (unsigned)f.mChannelsPerFrame,
           (unsigned)f.mBitsPerChannel, (unsigned)f.mFormatFlags);
}

static OSStatus set_u32(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, UInt32 v)
{
    return AudioUnitSetProperty(u, id, sc, el, &v, sizeof v);
}

static long get_u32(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el)
{
    UInt32 v = 0, size = sizeof v;
    return AudioUnitGetProperty(u, id, sc, el, &v, &size) ? -1 : (long)v;
}

static AudioStreamBasicDescription mono_float(double rate)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 1;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    return f;
}

static AudioUnit new_unit(OSType subtype, OSStatus *st)
{
    const AudioComponentDescription d = {kAudioUnitType_Output, subtype, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AudioUnit u = NULL;
    *st = c ? AudioComponentInstanceNew(c, &u) : kAudioUnitErr_InvalidElement;
    if (!c) printf("vpio-probe: no audio component '%c%c%c%c'\n", (char)(subtype >> 24), (char)(subtype >> 16), (char)(subtype >> 8),
                   (char)subtype);
    return *st ? NULL : u;
}

static void print_tcc(const char *prefix)
{
    @autoreleasepool {
        printf("%smic_status=%ld\n", prefix, (long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]);
        printf("%smic_mode active=%ld preferred=%ld\n", prefix, (long)AVCaptureDevice.activeMicrophoneMode,
               (long)AVCaptureDevice.preferredMicrophoneMode);
    }
}

static const char *duck_name(UInt32 level)
{
    switch (level) {
    case kAUVoiceIOOtherAudioDuckingLevelDefault: return "default";
    case kAUVoiceIOOtherAudioDuckingLevelMin: return "min";
    case kAUVoiceIOOtherAudioDuckingLevelMid: return "mid";
    case kAUVoiceIOOtherAudioDuckingLevelMax: return "max";
    default: return "other";
    }
}

static void print_readback(AudioUnit u)
{
    AUVoiceIOOtherAudioDuckingConfiguration dc = {0};
    UInt32 size = sizeof dc;
    const OSStatus st = AudioUnitGetProperty(u, kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0, &dc, &size);
    printf("vpio-probe: readback bypass %ld agc %ld mute-output %ld ducking advanced %d level %u (%s) rc %d\n",
           get_u32(u, kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, 0),
           get_u32(u, kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, 0),
           get_u32(u, kAUVoiceIOProperty_MuteOutput, kAudioUnitScope_Global, 0), st ? -1 : (int)dc.mEnableAdvancedDucking,
           st ? 0u : (unsigned)dc.mDuckingLevel, st ? "?" : duck_name(dc.mDuckingLevel), (int)st);
}

// Windowed-sinc (Blackman) decimation by an integer factor, cutoff at 0.45 of the output rate, into a new buffer.
static int16_t *decimate(const int16_t *s, uint64_t n, unsigned m, uint64_t *frames)
{
    const int taps = 16 * (int)m + 1, half = taps / 2;
    double *h = malloc(sizeof(double) * (size_t)taps), sum = 0;
    int16_t *d = malloc(sizeof(int16_t) * (size_t)(n / m + 2));
    if (!h || !d) {free(h); free(d); return NULL;}
    for (int k = 0; k < taps; ++k) {
        const double x = k - half, fc = 0.45 / m;
        const double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * cos(2 * M_PI * k / (taps - 1)) + 0.08 * cos(4 * M_PI * k / (taps - 1));
        h[k] = sinc * w;
        sum += h[k];
    }
    uint64_t out = 0;
    for (uint64_t j = 0; j * m < n; ++j) {
        double acc = 0;
        for (int k = 0; k < taps; ++k) {
            const long long i = (long long)(j * m) + k - half;
            if (i >= 0 && (uint64_t)i < n) acc += h[k] / sum * s[i];
        }
        d[out++] = (int16_t)(acc >= 32767 ? 32767 : acc <= -32768 ? -32768 : lrint(acc));
    }
    free(h);
    *frames = out;
    return d;
}

// avrec: a plain recorder, as QuickTime Player or a simple app records: AVAudioRecorder on the system default input, 48 kHz mono
// 16-bit linear PCM into OUT.wav, then the same summary line (read back from the file).
static int avrec(double seconds, const char *out_path, const char *ready, const char *done, const char *stop)
{
    @autoreleasepool {
        NSDictionary *settings = @{AVFormatIDKey: @(kAudioFormatLinearPCM), AVSampleRateKey: @48000.0, AVNumberOfChannelsKey: @1,
                                   AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO};
        NSError *err = nil;
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:out_path]];
        AVAudioRecorder *r = [[AVAudioRecorder alloc] initWithURL:url settings:settings error:&err];
        if (!r) {printf("vpio-probe: avrec init failed: %s\n", err.localizedDescription.UTF8String); return 1;}
        char in[256];
        device_string(default_device(1), kAudioObjectPropertyName, in, sizeof in);
        const BOOL prepared = [r prepareToRecord];
        const BOOL started = [r recordForDuration:seconds];
        const long long t0 = now_ms();
        printf("vpio-probe: avrec input \"%s\" prepare %d record %d at %lld\n", in, prepared, started, t0);
        if (!started) return 1;
        if (ready) {FILE *f = fopen(ready, "w"); if (f) {fprintf(f, "ready %lld\n", t0); fclose(f);}}
        while (r.recording && now_ms() - t0 < (long long)(seconds * 1000) + 5000) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            if (exists(stop)) {[r stop]; break;}
        }
        if (r.recording) [r stop];
    }
    float *pcm = NULL;
    uint64_t frames = 0;
    double rate = 0;
    if (load_wav(out_path, &pcm, &frames, &rate)) {printf("vpio-probe: avrec wrote no readable WAV\n"); return 1;}
    double sum = 0, peak = 0;
    uint64_t zeros = 0;
    for (uint64_t i = 0; i < frames; ++i) {
        const double v = pcm[i];
        sum += v * v;
        if (fabs(v) > peak) peak = fabs(v);
        zeros += v == 0;
    }
    char summary[512];
    snprintf(summary, sizeof summary, "vpio-probe: callbacks 0 frames %llu expected %.0f gaps 0 (largest 0 frames at 0.00 s) peak %.1f dBFS "
             "rms %.1f dBFS zeros %.1f%% clipped 0 render-errors 0 (last 0) oversize 0 wav %s %.0f Hz %llu frames",
             (unsigned long long)frames, seconds * rate, peak > 0 ? 20 * log10(peak) : -999.0,
             frames && sum > 0 ? 10 * log10(sum / (double)frames) : -999.0, frames ? 100.0 * (double)zeros / (double)frames : 100.0,
             out_path, rate, (unsigned long long)frames);
    printf("%s\n", summary);
    if (done) {FILE *f = fopen(done, "w"); if (f) {fprintf(f, "%s\n", summary); fclose(f);}}
    free(pcm);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --tcc | --list\n"
                    "       %s vpio|bypass|raw|avrec SECONDS FAR.wav|none OUT.wav [--rate HZ] [--far-delay S] "
                    "[--duck default|min|mid|max|unset] [--advanced-duck 0|1] [--agc 0|1] [--input UID] [--ready FILE] [--done FILE] "
                    "[--stop FILE]\n",
            argv0, argv0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--tcc")) {
        print_tcc("");
        char in[256], out[256];
        device_string(default_device(1), kAudioObjectPropertyName, in, sizeof in);
        device_string(default_device(0), kAudioObjectPropertyName, out, sizeof out);
        printf("default_input=\"%s\" default_output=\"%s\"\n", in, out);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--list")) {list_devices(); return 0;}
    if (argc < 5) {usage(argv[0]); return 2;}
    Probe p;
    memset(&p, 0, sizeof p);
    if (!strcmp(argv[1], "vpio")) p.mode = MODE_VPIO;
    else if (!strcmp(argv[1], "bypass")) p.mode = MODE_BYPASS;
    else if (!strcmp(argv[1], "raw")) p.mode = MODE_RAW;
    else if (!strcmp(argv[1], "avrec")) p.mode = MODE_AVREC;
    else {usage(argv[0]); return 2;}
    const double seconds = atof(argv[2]);
    const char *far_path = argv[3], *out_path = argv[4], *ready = NULL, *done = NULL, *stop = NULL, *duck = "min", *input_uid = NULL;
    double opt_rate = 0, far_delay = 0;
    int agc = -1, advanced = 0;
    for (int i = 5; i < argc; i += 2) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) {usage(argv[0]); return 2;}
        if (!strcmp(o, "--rate")) opt_rate = atof(v);
        else if (!strcmp(o, "--far-delay")) far_delay = atof(v);
        else if (!strcmp(o, "--duck")) duck = v;
        else if (!strcmp(o, "--advanced-duck")) advanced = atoi(v) != 0;
        else if (!strcmp(o, "--agc")) agc = atoi(v) != 0;
        else if (!strcmp(o, "--ready")) ready = v;
        else if (!strcmp(o, "--done")) done = v;
        else if (!strcmp(o, "--stop")) stop = v;
        else if (!strcmp(o, "--input")) input_uid = v;
        else {usage(argv[0]); return 2;}
    }
    UInt32 duck_level = 0;
    const int duck_set = strcmp(duck, "unset") != 0;
    if (!strcmp(duck, "default")) duck_level = kAUVoiceIOOtherAudioDuckingLevelDefault;
    else if (!strcmp(duck, "min")) duck_level = kAUVoiceIOOtherAudioDuckingLevelMin;
    else if (!strcmp(duck, "mid")) duck_level = kAUVoiceIOOtherAudioDuckingLevelMid;
    else if (!strcmp(duck, "max")) duck_level = kAUVoiceIOOtherAudioDuckingLevelMax;
    else if (duck_set) {usage(argv[0]); return 2;}
    if (seconds <= 0 || seconds > 1800 || far_delay < 0 || far_delay > 600) {fprintf(stderr, "vpio-probe: bad seconds or far delay\n"); return 2;}
    mach_timebase_info(&timebase);

    float *far = NULL;
    uint64_t far_frames = 0;
    double far_rate = 0;
    if (strcmp(far_path, "none") && load_wav(far_path, &far, &far_frames, &far_rate)) return 1;
    const double rate = opt_rate > 0 ? opt_rate : far ? far_rate : 48000.0;
    if (rate < 8000 || rate > 192000) {fprintf(stderr, "vpio-probe: bad rate\n"); return 2;}
    if (far && far_rate != rate) {fprintf(stderr, "vpio-probe: %s is %.0f Hz, the client rate is %.0f Hz\n", far_path, far_rate, rate); return 1;}
    p.rate = p.rec_rate = rate;
    p.far = far;
    p.far_frames = far_frames;
    p.far_delay_frames = (uint64_t)llround(far_delay * rate);
    printf("vpio-probe: mode %s seconds %.1f rate %.0f far %s frames %llu (%.1f s) far-delay %.3f duck %s advanced %d agc %s\n",
           mode_names[p.mode], seconds, rate, far ? far_path : "none", (unsigned long long)far_frames, far_frames / rate, far_delay,
           duck, advanced, agc < 0 ? "unit-default" : agc ? "1" : "0");
    print_tcc("vpio-probe: tcc ");
    // --input UID: that device as the system default input for this run (VPIO and AVAudioRecorder take the default), restored at exit
    AudioObjectID previous_input = kAudioObjectUnknown;
    if (input_uid) {
        const AudioObjectID want = device_by_uid(input_uid);
        if (want == kAudioObjectUnknown) {printf("vpio-probe: input %s: no such device\n", input_uid); return 1;}
        previous_input = default_device(1);
        const OSStatus dst = want == previous_input ? 0 : set_default_input(want);
        for (int i = 0; i < 50 && default_device(1) != want; ++i) usleep(20000);
        printf("vpio-probe: input %s device %u set-default rc %d now %u (was %u)\n", input_uid, (unsigned)want, (int)dst,
               (unsigned)default_device(1), (unsigned)previous_input);
        if (default_device(1) != want) return 1;
    }
    if (p.mode == MODE_AVREC) {
        const int rc = avrec(seconds, out_path, ready, done, stop);
        if (previous_input != kAudioObjectUnknown && previous_input != default_device(1)) set_default_input(previous_input);
        return rc;
    }
    const AudioObjectID din = default_device(1), dout = default_device(0);
    print_device("input", din, 1);
    print_device("output", dout, 0);
    listen_overload(din, &p.overloads_in);
    listen_overload(dout, &p.overloads_out);
    AudioObjectID unit_dev = kAudioObjectUnknown;
    const AudioStreamBasicDescription cf = mono_float(rate);
    const AURenderCallbackStruct render_cb = {on_render, &p}, input_cb = {on_input, &p};
    OSStatus st;

    if (p.mode != MODE_RAW) {
        p.vpio = new_unit(kAudioUnitSubType_VoiceProcessingIO, &st);
        if (!p.vpio) {printf("vpio-probe: unit vpio new %d\n", (int)st); return 1;}
        const OSStatus ein = set_u32(p.vpio, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, 1);
        const OSStatus eout = set_u32(p.vpio, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, 1);
        const OSStatus slice = set_u32(p.vpio, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, kVpioSlice);
        const OSStatus fo = AudioUnitSetProperty(p.vpio, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &cf, sizeof cf);
        const OSStatus fi = AudioUnitSetProperty(p.vpio, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &cf, sizeof cf);
        const OSStatus rcb = AudioUnitSetProperty(p.vpio, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render_cb,
                                                  sizeof render_cb);
        const OSStatus icb = AudioUnitSetProperty(p.vpio, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &input_cb,
                                                  sizeof input_cb);
        const UInt32 bypass = p.mode == MODE_BYPASS;
        const OSStatus byp = set_u32(p.vpio, kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, 0, bypass);
        const OSStatus agc_st = agc < 0 ? 0 : set_u32(p.vpio, kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, 0,
                                                      (UInt32)agc);
        const AUVoiceIOOtherAudioDuckingConfiguration dc = {(Boolean)advanced, duck_level};
        OSStatus duck_st = duck_set ? AudioUnitSetProperty(p.vpio, kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
                                                           kAudioUnitScope_Global, 0, &dc, sizeof dc) : 0;
        printf("vpio-probe: unit vpio enable-in %d enable-out %d max-slice %d fmt-out %d fmt-in %d render-cb %d input-cb %d "
               "bypass %u rc %d agc rc %d ducking %s rc %d\n", (int)ein, (int)eout, (int)slice, (int)fo, (int)fi, (int)rcb, (int)icb,
               (unsigned)bypass, (int)byp, (int)agc_st, duck_set ? duck_name(duck_level) : "unset", (int)duck_st);
        st = AudioUnitInitialize(p.vpio);
        printf("vpio-probe: init %d%s\n", (int)st, st == kAUVoiceIOErr_UnexpectedNumberOfInputChannels ? " (unexpected number of input channels)" : "");
        if (st) return 1;
        if (duck_set && duck_st) {   // some properties are only accepted on an initialized unit
            duck_st = AudioUnitSetProperty(p.vpio, kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0, &dc,
                                           sizeof dc);
            printf("vpio-probe: ducking set after init rc %d\n", (int)duck_st);
        }
        const long cur = get_u32(p.vpio, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0);
        if (cur > 0) {
            unit_dev = (AudioObjectID)cur;
            print_device("vpio-current", unit_dev, 1);
            if (unit_dev != din && unit_dev != dout) listen_overload(unit_dev, &p.overloads_unit);
        }
        print_format("hw-in", p.vpio, kAudioUnitScope_Input, 1);
        print_format("hw-out", p.vpio, kAudioUnitScope_Output, 0);
        print_format("client-in", p.vpio, kAudioUnitScope_Output, 1);
        print_format("client-out", p.vpio, kAudioUnitScope_Input, 0);
        Float64 lat = 0;
        UInt32 size = sizeof lat;
        st = AudioUnitGetProperty(p.vpio, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &lat, &size);
        printf("vpio-probe: unit latency %.4f s rc %d\n", st ? 0 : lat, (int)st);
        print_readback(p.vpio);
    } else {
        p.out_unit = new_unit(kAudioUnitSubType_HALOutput, &st);
        p.in_unit = p.out_unit ? new_unit(kAudioUnitSubType_HALOutput, &st) : NULL;
        if (!p.out_unit || !p.in_unit) {printf("vpio-probe: unit raw new %d\n", (int)st); return 1;}
        const OSStatus odev = AudioUnitSetProperty(p.out_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dout,
                                                   sizeof dout);
        const OSStatus ofmt = AudioUnitSetProperty(p.out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &cf, sizeof cf);
        const OSStatus orcb = AudioUnitSetProperty(p.out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render_cb,
                                                   sizeof render_cb);
        const OSStatus oinit = AudioUnitInitialize(p.out_unit);
        // AUHAL converts formats on input but not the sample rate: record at the device's rate, decimate at the end.
        const OSStatus ein = set_u32(p.in_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, 1);
        const OSStatus eout = set_u32(p.in_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, 0);
        const OSStatus idev = AudioUnitSetProperty(p.in_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &din,
                                                   sizeof din);
        AudioStreamBasicDescription hw;
        UInt32 size = sizeof hw;
        const OSStatus hwst = AudioUnitGetProperty(p.in_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw, &size);
        p.rec_rate = !hwst && hw.mSampleRate > 0 ? hw.mSampleRate : device_rate(din) > 0 ? device_rate(din) : rate;
        const AudioStreamBasicDescription cfin = mono_float(p.rec_rate);
        const OSStatus ifmt = AudioUnitSetProperty(p.in_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &cfin,
                                                   sizeof cfin);
        const OSStatus icb = AudioUnitSetProperty(p.in_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                                  &input_cb, sizeof input_cb);
        const OSStatus iinit = AudioUnitInitialize(p.in_unit);
        printf("vpio-probe: unit raw out device %d fmt %d render-cb %d init %d; in enable-in %d enable-out %d device %d hw %d fmt %d "
               "input-cb %d init %d; record rate %.0f\n", (int)odev, (int)ofmt, (int)orcb, (int)oinit, (int)ein, (int)eout, (int)idev,
               (int)hwst, (int)ifmt, (int)icb, (int)iinit, p.rec_rate);
        if (oinit || iinit || ifmt || icb || orcb) return 1;
        print_format("hw-in", p.in_unit, kAudioUnitScope_Input, 1);
        print_format("hw-out", p.out_unit, kAudioUnitScope_Output, 0);
    }

    p.rec_cap = (uint64_t)llround(seconds * p.rec_rate);
    p.rec = calloc(p.rec_cap + 1, sizeof(int16_t));
    p.slice = calloc(kMaxSlice, sizeof(float));
    if (!p.rec || !p.slice) {fprintf(stderr, "vpio-probe: out of memory\n"); return 1;}
    const long long t_start = now_ms();
    if (p.mode != MODE_RAW) st = AudioOutputUnitStart(p.vpio);
    else if (!(st = AudioOutputUnitStart(p.in_unit))) st = AudioOutputUnitStart(p.out_unit);
    printf("vpio-probe: start %d at %lld\n", (int)st, t_start);
    if (st) return 1;
    int ready_done = 0, why = 0;
    for (;;) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
        if (!ready_done && atomic_load(&p.started)) {
            const long long io_ms = p.in_first_valid ? now_ms() - (long long)llround((host_s(mach_absolute_time()) - host_s(p.in_first_host)) * 1000)
                                                     : now_ms();
            printf("vpio-probe: io-start %lld (%lld ms after start)\n", io_ms, io_ms - t_start);
            if (ready) {
                FILE *f = fopen(ready, "w");
                if (f) {fprintf(f, "ready %lld\n", io_ms); fclose(f);}
            }
            ready_done = 1;
            if (p.vpio) print_readback(p.vpio);
        }
        if (atomic_load_explicit(&p.rec_frames, memory_order_acquire) >= p.rec_cap) break;
        if (exists(stop)) {why = 1; break;}
        if (now_ms() - t_start > (long long)(seconds * 1000) + 20000) {why = 2; break;}
    }
    if (p.vpio) print_readback(p.vpio);
    if (p.vpio) {AudioOutputUnitStop(p.vpio); AudioUnitUninitialize(p.vpio); AudioComponentInstanceDispose(p.vpio);}
    if (p.out_unit) {AudioOutputUnitStop(p.out_unit); AudioUnitUninitialize(p.out_unit); AudioComponentInstanceDispose(p.out_unit);}
    if (p.in_unit) {AudioOutputUnitStop(p.in_unit); AudioUnitUninitialize(p.in_unit); AudioComponentInstanceDispose(p.in_unit);}
    unlisten_overload(din, &p.overloads_in);
    unlisten_overload(dout, &p.overloads_out);
    if (unit_dev != kAudioObjectUnknown && unit_dev != din && unit_dev != dout) unlisten_overload(unit_dev, &p.overloads_unit);
    printf("vpio-probe: stop %lld%s\n", now_ms(), why == 1 ? " (stop file)" : why == 2 ? " (timeout: the input fell short)" : "");

    if (p.far_valid && p.in_first_valid)
        printf("vpio-probe: timing far-start-in-rec-s %.4f out-first-in-rec-s %.4f\n", host_s(p.far_host) - host_s(p.in_first_host),
               p.out_first_valid ? host_s(p.out_first_host) - host_s(p.in_first_host) : NAN);
    else
        printf("vpio-probe: timing far-start-in-rec-s n/a (host time stamps: output %d far %d input %d)\n", p.out_first_valid,
               p.far_valid, p.in_first_valid);
    const uint64_t far_played = p.out_frames > p.far_delay_frames ? p.out_frames - p.far_delay_frames : 0;
    printf("vpio-probe: output callbacks %llu frames %llu far-played %llu of %llu\n", (unsigned long long)p.out_callbacks,
           (unsigned long long)p.out_frames, (unsigned long long)(far_played < p.far_frames ? far_played : p.far_frames),
           (unsigned long long)p.far_frames);
    printf("vpio-probe: overloads input %u output %u unit %u\n", atomic_load(&p.overloads_in), atomic_load(&p.overloads_out),
           atomic_load(&p.overloads_unit));

    uint64_t frames = atomic_load(&p.rec_frames);
    const uint64_t raw_frames = frames;
    uint32_t out_rate = (uint32_t)llround(p.rec_rate);
    int16_t *wav = p.rec;
    if (p.rec_rate != rate) {
        const double ratio = p.rec_rate / rate;
        int16_t *d = NULL;
        if (fabs(ratio - llround(ratio)) < 1e-9 && llround(ratio) > 1 &&
            (d = decimate(p.rec, raw_frames, (unsigned)llround(ratio), &frames))) {
            wav = d;
            out_rate = (uint32_t)llround(rate);
            printf("vpio-probe: decimated %.0f -> %.0f Hz (%llu -> %llu frames)\n", p.rec_rate, rate, (unsigned long long)raw_frames,
                   (unsigned long long)frames);
        } else {
            printf("vpio-probe: recorded at %.0f Hz (no integer ratio to %.0f Hz)\n", p.rec_rate, rate);
        }
    }
    if (write_wav(out_path, out_rate, wav, frames)) {fprintf(stderr, "vpio-probe: cannot write %s\n", out_path); return 1;}
    if (wav != p.rec) free(wav);
    const double rms = raw_frames ? sqrt(p.sum_sq / (double)raw_frames) : 0;
    const double zeros = raw_frames ? 100.0 * (double)p.zeros / (double)raw_frames : 100.0;
    char summary[512];
    snprintf(summary, sizeof summary,
             "vpio-probe: callbacks %llu frames %llu expected %.0f gaps %llu (largest %llu frames at %.2f s) peak %.1f dBFS rms %.1f dBFS "
             "zeros %.1f%% clipped %llu render-errors %llu (last %d) oversize %llu wav %s %u Hz %llu frames",
             (unsigned long long)p.in_callbacks, (unsigned long long)raw_frames, seconds * p.rec_rate, (unsigned long long)p.gaps,
             (unsigned long long)p.largest_gap, p.largest_gap_at, p.peak > 0 ? 20 * log10(p.peak) : -999.0,
             rms > 0 ? 20 * log10(rms) : -999.0, zeros, (unsigned long long)p.clipped, (unsigned long long)p.render_errors,
             (int)p.last_render_error, (unsigned long long)p.oversize, out_path, out_rate, (unsigned long long)frames);
    printf("%s\n", summary);
    if (zeros >= 99.0)
        printf("vpio-probe: all-zero input: a TCC denial (an ssh-started process needs a kTCCServiceMicrophone grant for its responsible "
               "process) or a muted path; not a result\n");
    if (done) {
        FILE *f = fopen(done, "w");
        if (f) {fprintf(f, "%s\n", summary); fclose(f);}
    }
    free(p.rec); free(p.slice); free(far);
    if (previous_input != kAudioObjectUnknown && previous_input != default_device(1)) {
        const OSStatus r = set_default_input(previous_input);
        printf("vpio-probe: default input restored to %u rc %d\n", (unsigned)previous_input, (int)r);
    }
    return 0;
}
