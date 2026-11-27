// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors (the AudioServerPlugIn skeleton: UniXNU-sheng hal/ShengAudioPlugIn.c) and Leo Zhang.
// "Mic (noise reduced)": a Core Audio AudioServerPlugIn with one input device that runs RNNoise (third_party/rnnoise, BSD-3-Clause)
// on the tablet's raw microphone (private/host-checks MIC-NS.md section 8). Any recording app can pick it as its input (QuickTime,
// Voice Memos through the default input, FaceTime-style VPIO apps on top of it); PianoAudio's own "Mic" stays raw and the default.
// Objects: the plug-in (1), the device (2), its mono input stream (3); no controls (the Mic's own mute and level apply, see below).
//   frames   PianoAudio's microphone tap (package/PianoAudio/AudioTap.h): the service "piano-mic-tap", opened at the first StartIO
//            and closed at the last StopIO (an open tap holds the kernel's capture plan up, so the microphone runs only while
//            someone records from this device), mapped read-only; MicNsCore.c takes its frames through RNNoise in 10 ms blocks
//            into a FIFO held near a fixed fill (single raw frames slip against the DSP's clock), and applies the Mic's own mute
//            and digital level.
//   clock    the host clock at a nominal 48 kHz, as PianoAudio's Mic and Speaker run (so VPIO's aggregate of this device and the
//            Speaker sees one rate); time stamps every kZeroPeriod frames from the first StartIO.
//   IO       ReadInput is served from an output ring indexed by sample time: the frames past what was produced come from the core,
//            so two IO contexts reading the same times read the same frames.
//   sandbox  the driver service extends its sandbox for the tap's user-client class named in Info.plist's
//            AudioServerPlugIn_IOKitUserClients; the kernel admits only a task with com.apple.private.audio.driver-host.
//   log      "PianoMicNS: ..." (os_log, subsystem com.unixnu.piano.micns): the load, each open of the tap with its result, the first
//            frames of a start, every ~30 s while running and each stop: the core's counters, RNNoise's time per block and its CPU
//            share; every property asked for and not answered, once.
// The time stamps, property answers and the factory follow the sheng skeleton (UPSTREAM); the tap, the core and the IO are new.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include "MicNsCore.h"

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kInStream = 3};
#define kPlugInBundleID "com.unixnu.piano.micns"
#define kDeviceUID "PianoMicNS"
#define kModelUID "PianoMicNS:model"
#define kDeviceName "Mic (noise reduced)"
static const Float64 kRate = PIANO_TAP_RATE;
static const UInt32 kZeroPeriod = 16384;          // frames between time stamps (more than a safety offset plus an IO cycle)
enum { kRingFrames = 16384, kProduceFrames = 4096 };  // output ring indexed by sample time, and the largest core read
static const UInt32 kLatency = MICNS_RESERVE + 2 * MICNS_BLOCK;   // the FIFO's reserve, a block in RNNoise, a block of slack

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static os_log_t gLog;
static Float64 gHostTicksPerFrame;
static atomic_uint gIoUsers;
static uint64_t gAnchorHost, gSeed;
#ifndef PIANO_MICNS_TEST_TAP
static io_connect_t gConnect;
static mach_vm_address_t gTapAddress;
static mach_vm_size_t gTapSize;
#endif
static const PianoTapHeader *gTap;
static MicNs gCore;
static int gCoreReady;
static float gRing[kRingFrames + kProduceFrames - 1];  // extra space for a core read crossing the ring boundary
static uint64_t gProducedStart, gProducedEnd;
static int gProducedValid;
static UInt64 gIoCycles, gNextTick;
static uint64_t gStartHost;

static uint64_t ticksNow(void) {return mach_absolute_time();}
static double ticksToUs(uint64_t t) {return (double)t / (gHostTicksPerFrame * kRate) * 1e6;}   // ticks a second: per frame x rate

// ---- The tap ---------------------------------------------------------------------------------------------------------------------
#ifdef PIANO_MICNS_TEST_TAP
// tests/plugin_io_host.c: the tap is the test's memory, the core runs in passthrough when it asks (the IO path's integrity is visible).
extern const PianoTapHeader *piano_micns_test_tap;
extern int piano_micns_test_bypass;
static kern_return_t tapOpen(void) {gTap = piano_micns_test_tap; return gTap ? KERN_SUCCESS : kIOReturnNotFound;}
static void tapClose(void) {gTap = NULL;}
#define MICNS_BYPASS piano_micns_test_bypass
#else
#define MICNS_BYPASS 0
static kern_return_t tapOpen(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(PIANO_TAP_SERVICE));
    if (!service) {os_log_error(gLog, "PianoMicNS: tap: no service %{public}s (PianoAudio level 10 with piano-audio-tap=1 not up yet?)", PIANO_TAP_SERVICE); return kIOReturnNotFound;}
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {os_log_error(gLog, "PianoMicNS: tap: IOServiceOpen 0x%x (sandbox or entitlement: %{public}s)", kr, PIANO_TAP_USER_CLIENT); gConnect = 0; return kr;}
    kr = IOConnectMapMemory64(gConnect, PIANO_TAP_MEMORY, mach_task_self(), &gTapAddress, &gTapSize, kIOMapAnywhere | kIOMapReadOnly);
    if (kr != KERN_SUCCESS || gTapSize < PIANO_TAP_BYTES) {
        os_log_error(gLog, "PianoMicNS: tap: map 0x%x size %llu", kr, (unsigned long long)gTapSize);
        if (kr == KERN_SUCCESS) IOConnectUnmapMemory64(gConnect, PIANO_TAP_MEMORY, mach_task_self(), gTapAddress);
        IOServiceClose(gConnect); gConnect = 0; return kr ? kr : kIOReturnNoSpace;
    }
    const PianoTapHeader *h = (const PianoTapHeader *)(uintptr_t)gTapAddress;
    if (h->magic != PIANO_TAP_MAGIC || h->version != PIANO_TAP_VERSION || h->frames != PIANO_TAP_FRAMES || h->rate != PIANO_TAP_RATE) {
        os_log_error(gLog, "PianoMicNS: tap: header magic 0x%x version %u frames %u rate %u: not this ABI", h->magic, h->version, h->frames, h->rate);
        IOConnectUnmapMemory64(gConnect, PIANO_TAP_MEMORY, mach_task_self(), gTapAddress);
        IOServiceClose(gConnect); gConnect = 0; return kIOReturnUnsupported;
    }
    gTap = h;
    os_log_error(gLog, "PianoMicNS: tap: open: clients %u opens %u run %u written %llu periods %u", h->clients, h->opens, h->run,
                 (unsigned long long)audio_tap_written(h), h->periods);
    return KERN_SUCCESS;
}
static void tapClose(void) {
    if (!gConnect) return;
    if (gTapAddress) IOConnectUnmapMemory64(gConnect, PIANO_TAP_MEMORY, mach_task_self(), gTapAddress);
    IOServiceClose(gConnect);
    gConnect = 0; gTapAddress = 0; gTapSize = 0; gTap = NULL;
}
#endif

static void report(const char *event) {
    const MicNsStats *s = &gCore.s;
    const double us = s->blocks ? ticksToUs(s->proc_ticks) / (double)s->blocks : 0;
    const double audio_s = (double)s->blocks * MICNS_BLOCK / kRate;
    const double cpu = audio_s > 0 ? ticksToUs(s->proc_ticks) / 1e6 / audio_s * 100.0 : 0;
    const double io_us = s->reads ? ticksToUs(s->read_ticks) / (double)s->reads : 0;
    const double nr = s->out_squares > 0 && s->in_squares > 0 ? 10.0 * __builtin_log10(s->in_squares / s->out_squares) : 0;
    os_log_error(gLog, "PianoMicNS: %{public}s: reads %llu delivered %llu startup %llu missing %llu muted %llu blocks %llu drops %llu "
                 "repeats %llu anchors %llu runs %llu lapped %llu excess %llu fill %u first-sound %llu rnnoise-us %.1f max %.1f "
                 "cpu %.2f%% read-us %.1f max %.1f vad %.3f in-over-out-db %.1f cycles %llu", event,
                 s->reads, s->delivered, s->startup, s->missing, s->muted, s->blocks, s->drops, s->repeats, s->anchors, s->runs, s->lapped,
                 s->excess, gTap ? micns_fill(&gCore) : 0, s->first_sound, us, ticksToUs(s->proc_max_ticks), cpu, io_us,
                 ticksToUs(s->read_max_ticks), s->blocks ? s->vad_sum / (double)s->blocks : 0, nr, gIoCycles);
}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    if (!gCoreReady) gCoreReady = micns_init(&gCore, NULL, MICNS_BYPASS, ticksNow) == 0 ? 1 : -1;
    pthread_mutex_unlock(&gLock);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(PIANO_TAP_SERVICE));
    if (service) IOObjectRelease(service);
    os_log_error(gLog, "PianoMicNS: loaded: device \"%{public}s\" (%{public}s), RNNoise %{public}s, tap service %{public}s, latency %u frames",
                 kDeviceName, kDeviceUID, gCoreReady > 0 ? "ready" : "NOT ready (passthrough)", service ? "present" : "absent (yet)", kLatency);
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 1; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
#define REPLY_LIST(n, ...) do { const AudioObjectID l_[] = {__VA_ARGS__}; UInt32 c_ = (n); if (c_ * sizeof(AudioObjectID) > inDataSize) c_ = inDataSize / (UInt32)sizeof(AudioObjectID); \
                               memcpy(outData, l_, c_ * sizeof(AudioObjectID)); *outDataSize = c_ * (UInt32)sizeof(AudioObjectID); return 0; } while (0)
#define REPLY_NONE() do { *outDataSize = 0; return 0; } while (0)

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    const int input = a->mScope != kAudioObjectPropertyScopeOutput;
    if (o <= kInStream && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) || (s == 'taps' && o == kInStream))) REPLY_NONE();
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: REPLY_LIST(1, kDevice);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = q && qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR(kDeviceName);
        case kAudioObjectPropertyModelName: REPLY_STR(kDeviceName);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case 'accs': REPLY(UInt32, 0);   // private device properties the audio server reads for each new device (sheng i526)
        case 'auto': REPLY(UInt32, 0);
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.audio.AudioMIDISetup");
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices: REPLY_LIST(1, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, input ? 1 : 0);   // the user may pick it as the default input
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 0);
        case kAudioDevicePropertyLatency: REPLY(UInt32, input ? kLatency : 0);
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, 0);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);   // exact host-clock time stamps
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = 1;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = 1;
            l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeOutput) REPLY_NONE();
            REPLY_LIST(1, kInStream);
        case kAudioObjectPropertyControlList: REPLY_NONE();
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kInStream:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: REPLY_STR(kDeviceName);
        case kAudioObjectPropertyOwnedObjects: REPLY_NONE();
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, 1);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format());
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            const AudioStreamRangedDescription r = {format(), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d;
    const OSStatus r = propertyData(o, a, qs, q, inDataSize, outDataSize, outData);
    if (r == kAudioHardwareUnknownPropertyError) {   // each unanswered property once: what a client that drops the device wanted
        static UInt64 seen[128];
        static atomic_uint count;
        const UInt64 key = (UInt64)o << 56 ^ (UInt64)a->mSelector << 24 ^ a->mScope;
        unsigned n = atomic_load(&count);
        for (unsigned i = 0; i < n && i < 128; ++i) if (seen[i] == key) return r;
        n = atomic_fetch_add(&count, 1);
        if (n < 128) seen[n] = key;
        const UInt32 v = a->mSelector, sc = a->mScope;
        os_log_error(gLog, "PianoMicNS: unknown property object %u selector '%c%c%c%c' scope '%c%c%c%c' element %u pid %d", o,
                     (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8),
                     (char)sc, a->mElement, pid);
    }
    return r;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    _Alignas(AudioStreamRangedDescription) UInt8 scratch[512];
    UInt32 written = 0;
    const OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    _Alignas(AudioStreamRangedDescription) UInt8 scratch[512];
    UInt32 written = 0;
    (void)d; (void)pid;
    return propertyData(o, a, 0, NULL, sizeof scratch, &written, scratch) == 0;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    *out = ((o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                 a->mSelector == kAudioStreamPropertyIsActive)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    if (o == kInStream && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        const AudioStreamBasicDescription supported = format();
        return f && size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == 1 && f->mFormatID == kAudioFormatLinearPCM &&
               (f->mFormatFlags & ~kAudioFormatFlagIsNonInterleaved) == supported.mFormatFlags && f->mBitsPerChannel == 32 &&
               f->mBytesPerFrame == supported.mBytesPerFrame && f->mBytesPerPacket == supported.mBytesPerPacket &&
               f->mFramesPerPacket == supported.mFramesPerPacket ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return data && size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    if (o == kInStream && a->mSelector == kAudioStreamPropertyIsActive) return data && size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnknownPropertyError;
}

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (gIoUsers == 0) {
        const kern_return_t kr = tapOpen();
        if (kr != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
        else {
            gCore.bypass = MICNS_BYPASS;
            if (micns_start(&gCore, gTap)) os_log_error(gLog, "PianoMicNS: start: RNNoise state NOT allocated: passthrough");
            gProducedValid = 0; gProducedStart = gProducedEnd = 0;
            memset(gRing, 0, sizeof gRing);
            gIoCycles = 0;
            gNextTick = 0;
            gAnchorHost = gStartHost = mach_absolute_time();
            gSeed++;
        }
    }
    if (!r) gIoUsers++;
    os_log_error(gLog, "PianoMicNS: start: client %u users %u result %d", client, gIoUsers, (int)r);
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {
        report("stop");
        micns_free(&gCore);
        tapClose();
    }
    os_log_error(gLog, "PianoMicNS: stop: client %u users %u", client, gIoUsers);
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)o; (void)client;
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * kZeroPeriod;
    const uint64_t wraps = now > gAnchorHost ? (uint64_t)((Float64)(now - gAnchorHost) / period) : 0;
    *sampleTime = (Float64)(wraps * kZeroPeriod);
    *hostTime = gAnchorHost + (uint64_t)((Float64)wraps * period);
    *seed = gSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationReadInput;
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
// Produces sample times [gProducedEnd, end) from the core into the ring.
static void produce(uint64_t end) {
    while (gProducedEnd < end) {
        const uint32_t at = (uint32_t)gProducedEnd & (kRingFrames - 1);
        const uint64_t remaining = end - gProducedEnd;
        const uint32_t n = remaining > kProduceFrames ? kProduceFrames : (uint32_t)remaining;
        // Keep the core read whole so priming uses the full request when the cache wraps.
        (void)micns_read(&gCore, gRing + at, n);
        if (at + n > kRingFrames)
            memcpy(gRing, gRing + kRingFrames, (at + n - kRingFrames) * sizeof *gRing);
        gProducedEnd += n;
    }
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)o; (void)client; (void)secondary;
    if (op != kAudioServerPlugInIOOperationReadInput || stream != kInStream || !main || !frames) return 0;
    if (frames > kRingFrames) return kAudioHardwareBadPropertySizeError;
    Float32 *dst = main;
    if (!gTap || !info) {memset(dst, 0, frames * sizeof *dst); return 0;}
    const Float64 t = info->mInputTime.mSampleTime;
    if (!(t >= 0 && t < 0x1p64)) {memset(dst, 0, frames * sizeof *dst); return 0;}
    const uint64_t start = (uint64_t)t;
    if (start > UINT64_MAX - frames) {memset(dst, 0, frames * sizeof *dst); return 0;}
    const uint64_t end = start + frames;
    // a first read, or one far from what was produced (the HAL skipped or went back): produce from here on
    if (!gProducedValid || (start > gProducedEnd && start - gProducedEnd > kRingFrames / 2) ||
        (end < gProducedEnd && gProducedEnd - end > kRingFrames / 2)) {
        gProducedStart = gProducedEnd = start;
        gProducedValid = 1;
    }
    if (end > gProducedEnd) produce(end);
    if (gProducedEnd - gProducedStart > kRingFrames) gProducedStart = gProducedEnd - kRingFrames;
    for (UInt32 i = 0; i < frames; ++i) {
        const uint64_t s = start + i;
        dst[i] = s >= gProducedStart ? gRing[(uint32_t)s & (kRingFrames - 1)] : 0.0f;
    }
    ++gIoCycles;
    const uint64_t now = mach_absolute_time();
    if (!gNextTick) gNextTick = now + (uint64_t)(gHostTicksPerFrame * kRate * 2.0);   // the first frames: 2 s after the start
    else if (now >= gNextTick) {
        report(gCore.s.reads < 400 ? "first" : "tick");
        gNextTick = now + (uint64_t)(gHostTicksPerFrame * kRate * 30.0);
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *PianoMicNS_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gLog) gLog = os_log_create("com.unixnu.piano.micns", "plugin");
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    return &gInterfacePtr;
}
