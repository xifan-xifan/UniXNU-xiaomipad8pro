/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang.
 * CoreAudio input/output control and timing snapshot; optional explicit control write. */
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioHardwareService.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audio-cli.h"

static OSStatus get(AudioObjectID id, AudioObjectPropertySelector key, AudioObjectPropertyScope scope,
                    AudioObjectPropertyElement element, UInt32 *size, void *out)
{
    const AudioObjectPropertyAddress a = {key, scope, element};
    return AudioObjectGetPropertyData(id, &a, 0, NULL, size, out);
}
static OSStatus u32(AudioObjectID id, AudioObjectPropertySelector key, UInt32 *out)
{
    UInt32 n = sizeof *out;
    return get(id, key, kAudioObjectPropertyScopeGlobal, 0, &n, out);
}
static void fourcc(UInt32 v, char out[5])
{
    for (int i = 0; i < 4; ++i) {
        const unsigned c = (v >> (24 - 8*i)) & 255;
        out[i] = c >= 32 && c <= 126 ? (char)c : '.';
    }
    out[4] = 0;
}
static int named(AudioObjectID id, AudioObjectPropertySelector key, const char *want)
{
    CFStringRef str = NULL; UInt32 n = sizeof str;
    if (get(id, key, kAudioObjectPropertyScopeGlobal, 0, &n, &str) || !str) return 0;
    char buf[256] = "";
    const int ok = CFStringGetCString(str, buf, sizeof buf, kCFStringEncodingUTF8) && !strcmp(buf, want);
    CFRelease(str); return ok;
}
static AudioDeviceID mic(void)
{
    const AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 n = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &n)) return 0;
    const UInt32 capacity = n;
    AudioDeviceID *ids = n ? calloc(1, n) : NULL; AudioDeviceID found = 0;
    if (ids && !AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &n, ids) && n <= capacity)
        for (UInt32 i = 0; i < n / sizeof *ids; ++i)
            if (named(ids[i], kAudioDevicePropertyDeviceUID, "Mic")) {found = ids[i]; break;}
    free(ids); return found;
}
int main(int argc, char **argv)
{
    const char *phase = argc > 1 ? argv[1] : "snapshot";
    const int input = argc > 2 && !strcmp(argv[2], "input");
    const AudioObjectPropertyScope direction = input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    long level_arg = -1, mute_arg = -1;
    if (argc > 5 ||
        (argc > 3 && !audio_parse_long(argv[3], -1, 100, &level_arg)) ||
        (argc > 4 && !audio_parse_long(argv[4], -1, 1, &mute_arg)))
        return 2;
    const int level = (int)level_arg, mute = (int)mute_arg;
    AudioDeviceID dev = 0;
    const OSStatus ds = u32(kAudioObjectSystemObject, input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice, &dev);
    printf("default_%s phase=%s status=%d device=%u\n", input ? "input" : "output", phase, (int)ds, (unsigned)dev);
    if (input) dev = mic();
    if (ds || !dev) return 1;
    printf("selected_device phase=%s direction=%s device=%u exact_mic_uid=%d\n", phase, input ? "input" : "output", dev,
           named(dev, kAudioDevicePropertyDeviceUID, "Mic"));
    const AudioObjectPropertyAddress a = {kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 bytes = 0;
    OSStatus st = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &bytes);
    AudioObjectID *ids = bytes ? calloc(1, bytes) : NULL;
    if (!st && bytes && !ids) return 2;
    if (!st && bytes) {
        const UInt32 capacity = bytes;
        st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &bytes, ids);
        if (!st && (bytes > capacity || bytes % sizeof *ids)) st = kAudioHardwareBadPropertySizeError;
    }
    const UInt32 count = st ? 0 : bytes / sizeof *ids;
    printf("control_list_read phase=%s status=%d count=%u\n", phase, (int)st, (unsigned)count);
    int wrote_level = 0, wrote_mute = 0, invalid = 0, has_level = 0, has_mute = 0;
    for (UInt32 i = 0; i < count; ++i) {
        UInt32 cls = 0, base = 0, scope = 0, element = 0;
        const OSStatus cs = u32(ids[i], kAudioObjectPropertyClass, &cls);
        const OSStatus bs = u32(ids[i], kAudioObjectPropertyBaseClass, &base);
        const OSStatus ss = u32(ids[i], kAudioControlPropertyScope, &scope);
        const OSStatus es = u32(ids[i], kAudioControlPropertyElement, &element);
        if (!cs && !ss && scope == direction && !es && element == 0 &&
            ((cls == kAudioVolumeControlClassID && level >= 0) || (cls == kAudioMuteControlClassID && mute >= 0))) {
            const AudioObjectPropertyAddress write = {cls == kAudioVolumeControlClassID ? kAudioLevelControlPropertyScalarValue : kAudioBooleanControlPropertyValue,
                                                       kAudioObjectPropertyScopeGlobal, 0};
            Float32 value = (Float32)level / 100; UInt32 muted = (UInt32)mute;
            const OSStatus ws = AudioObjectSetPropertyData(ids[i], &write, 0, NULL,
                cls == kAudioVolumeControlClassID ? sizeof value : sizeof muted,
                cls == kAudioVolumeControlClassID ? (void *)&value : (void *)&muted);
            printf("control_write phase=%s id=%u status=%d level=%d mute=%d\n", phase, ids[i], (int)ws, level, mute);
            if (ws) {free(ids); return 3;}
            if (cls == kAudioVolumeControlClassID) ++wrote_level; else ++wrote_mute;
        }
        Float32 scalar = 0, db = 0;
        UInt32 n = sizeof scalar;
        const OSStatus vs = get(ids[i], kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0, &n, &scalar);
        n = sizeof db;
        const OSStatus dbs = get(ids[i], kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0, &n, &db);
        UInt32 muted = 0; n = sizeof muted;
        const OSStatus ms = get(ids[i], kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, 0, &n, &muted);
        printf("control_state phase=%s id=%u mute_status=%d mute=%u finite_scalar=%d finite_db=%d\n", phase, ids[i], (int)ms, muted,
               !vs && isfinite(scalar), !dbs && isfinite(db));
        if (!ss && scope == direction && !es && element == 0) {
            if (cls == kAudioVolumeControlClassID) {
                has_level = 1;
                invalid |= vs || dbs || !isfinite(scalar) || !isfinite(db) ||
                    (level >= 0 && (fabs(scalar - level / 100.0) > .011 || fabs(db - (-36 + .36 * level)) > .03));
            }
            if (cls == kAudioMuteControlClassID) {has_mute = 1; invalid |= ms || (mute >= 0 && muted != (UInt32)mute);}
        }
        char c[5], b[5], sc[5]; fourcc(cls, c); fourcc(base, b); fourcc(scope, sc);
        printf("control phase=%s id=%u class=0x%08x(%s) base=0x%08x(%s) scope_status=%d scope=0x%08x(%s) element_status=%d element=%u class_status=%d base_status=%d scalar_status=%d scalar=%.9g db_status=%d db=%.9g\n",
               phase, (unsigned)ids[i], (unsigned)cls, c, (unsigned)base, b, (int)ss, (unsigned)scope, sc,
               (int)es, (unsigned)element, (int)cs, (int)bs, (int)vs, (double)scalar, (int)dbs, (double)db);
    }
    free(ids);
    if (st || (level >= 0 && !wrote_level) || (mute >= 0 && !wrote_mute) || invalid || (input && (!has_level || !has_mute))) {
        printf("control_validation phase=%s FAIL wrote_level=%d wrote_mute=%d invalid=%d\n", phase, wrote_level, wrote_mute, invalid);
        return 4;
    }
    printf("control_validation phase=%s PASS wrote_level=%d wrote_mute=%d\n", phase, wrote_level, wrote_mute);
    for (UInt32 element = 0; element < 3; ++element) {
        Float32 v = 0; UInt32 n = sizeof v;
        st = get(dev, kAudioDevicePropertyVolumeScalar, direction, element, &n, &v);
        printf("device_volume phase=%s element=%u status=%d size=%u scalar=%.9g\n", phase, (unsigned)element, (int)st, (unsigned)n, (double)v);
    }
    Float32 vm = 0; UInt32 n = sizeof vm;
    const AudioObjectPropertyAddress va = {kAudioHardwareServiceDeviceProperty_VirtualMainVolume, direction, 0};
    st = AudioObjectGetPropertyData(dev, &va, 0, NULL, &n, &vm);
    printf("virtual_main phase=%s status=%d size=%u scalar=%.9g\n", phase, (int)st, (unsigned)n, (double)vm);
    UInt32 cycle = 0, safety = 0; n = sizeof cycle;
    const OSStatus fs = get(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, 0, &n, &cycle);
    n = sizeof safety;
    const OSStatus so = get(dev, kAudioDevicePropertySafetyOffset, direction, 0, &n, &safety);
    printf("hal_timing phase=%s callback_status=%d callback_frames=%u safety_status=%d safety_frames=%u\n", phase, (int)fs, (unsigned)cycle, (int)so, (unsigned)safety);
    UInt32 latency = 0; n = sizeof latency;
    st = get(dev, kAudioDevicePropertyLatency, direction, 0, &n, &latency);
    printf("hal_latency phase=%s status=%d frames=%u\n", phase, (int)st, latency);
    AudioStreamBasicDescription fmt = {0}; n = sizeof fmt;
    st = get(dev, kAudioDevicePropertyStreamFormat, direction, 0, &n, &fmt);
    printf("stream_format phase=%s status=%d rate=%.0f channels=%u bits=%u bytes_per_frame=%u\n", phase, (int)st, fmt.mSampleRate,
           (unsigned)fmt.mChannelsPerFrame, (unsigned)fmt.mBitsPerChannel, (unsigned)fmt.mBytesPerFrame);
    return 0;
}
