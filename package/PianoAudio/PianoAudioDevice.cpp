// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// piano's speaker and microphone as the built-in audio devices macOS's HAL takes: IOAudio2Devices (HALS_IOA2Manager in audiomxd /
// coreaudiod matches IOServiceMatching("IOAudio2Device") + IOObjectConformsTo), opened through an IOAudio2DeviceUserClient.
// Everything above is stock: the HAL's HALS_IOA2Device/Engine, VirtualAudio (routes, ports, volume).
// The IOAudio2Device top half of the VM-era audio kext (provenance: UPSTREAM), its virtio-snd transport replaced by AudioPcm.h's
// interface: the null transport at levels 3-7 (the devices run on their own clock into silence), the DSP's from level 8 (AudioDsp.cpp,
// AudioLive.h: the Speaker's periods to the speakers, the Mic's from the DMICs at level 10; the devices' clock stays the host's). The ABI notes below
// come with it.
//
// VirtualAudio's DeviceFactory picks the physical-device class by the device UID string, with a single-transducer class per kind. Two
// devices: UID "Speaker" (a routable speaker output port) and UID "Mic" (a routable built-in-mic input port); the default VirtualAudio
// device aggregates the two as a duplex route.
//
// The original IOAudio2Family is in the kernel collection (a dependency of IOAVFamily), so its two class names are taken and its
// subclass contract is closed. These classes have their own names and implement the whole user-client ABI themselves; at attach their
// metaclasses are linked under the family's (PianoAudioDevice -> IOAudio2Device, PianoAudioUserClient -> IOAudio2DeviceUserClient),
// which is what IOService matching and IOObjectConformsTo test. No family code runs on these objects. The family's classes derive
// directly from IOService and IOUserClient (checked in the KC base by tooling/k27/check-audio2-family.py), the C++ bases used here.
//
// The ABI, as the source read it off 21H461's AudioServerDriverTransports_IOA2 (ASDT::IOA2UserClient) and CoreAudio (HALS_IOA2UCDevice):
//   registry   the description, as dictionary keys with spaces ("device UID", "output streams"/"input streams", ...); sample rates
//              are Fixed64 (32.32) SInt64 numbers
//   open       IOServiceOpen type 0; IOConnectSetNotificationPort type 0 (IOAudio2Notification)
//   memory     type 0: the IOAudio2EngineStatus (>= 0x28 bytes: +0 zero sample time, +8 its host time, +0x10 seed; read twice until
//              equal; +0x20 the HAL's write end). type 0x10000000 + stream ID: the stream's ring buffer
//   selectors  0 StartIO, 1 StopIO, 2 SetControlValue, 3 PerformConfigChange, 4 SetNominalSampleRate(Fixed64), 5 SetStreamCurrentFormat
//              (id + 40-byte ASBD), 6 SetStreamActive, 10 StartIOWithFlags, 11 StopIOWithFlags, 12 SetClientDescription; the HAL also
//              writes "exclusive access owner" (hog mode) through setProperties.
//   I/O        no trap: the HAL reads/writes the ring at the zero-timestamp position. The per-device engine moves one period at a time
//              between its ring and the transport: the speaker device plays, the mic device captures.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/OSKextLib.h>
#include <kern/clock.h>
#include <kern/ipc_mig.h>
#include <kern/thread.h>
#include <sys/sysctl.h>
#include "AudioPcm.h"
#include "AudioControl.h"
#include "AudioControlNotification.h"
#include "AudioCapture.h"
#include "AudioLevelDictionary.h"
#include "AudioTiming.h"
#include "AudioPower.h"

extern "C" {
// Post-mortem and the summary line: the engines' state and their counters.
volatile uint32_t piano_audio_dev_out_running, piano_audio_dev_in_running, piano_audio_dev_clients;
volatile uint64_t piano_audio_dev_periods, piano_audio_dev_late, piano_audio_dev_played, piano_audio_dev_captured;
volatile uint64_t piano_audio_dev_last_selector, piano_audio_dev_selectors, piano_audio_dev_bad_selectors, piano_audio_dev_maps;
volatile int piano_audio_dev_family_status = -1, piano_audio_dev_hog_owner = -1, piano_audio_dev_stage = 0;
}

SYSCTL_DECL(_machdep);

namespace {
volatile uint32_t gTimingMode = PIANO_AUDIO_TIMING_LEGACY;
struct sysctl_oid gTimingModeOid;
bool gTimingModeRegistered;

static int pianoAudioTimingSysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    int value = int(__atomic_load_n(&gTimingMode, __ATOMIC_ACQUIRE));
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (value != int(PIANO_AUDIO_TIMING_LEGACY) && value != int(PIANO_AUDIO_TIMING_NEW)) return EINVAL;
    __atomic_store_n(&gTimingMode, uint32_t(value), __ATOMIC_RELEASE);
    IOLog("PianoAudio: machdep.piano_audio_timing=%d (%s; read at next StartIO)\n", value,
          value ? "10 ms batching" : "legacy");
    return 0;
}

static void registerPianoAudioTimingSysctl()
{
    if (gTimingModeRegistered) return;
    bzero(&gTimingModeOid, sizeof gTimingModeOid);
    gTimingModeOid.oid_parent = &sysctl__machdep_children;
    gTimingModeOid.oid_number = OID_AUTO;
    gTimingModeOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gTimingModeOid.oid_name = "piano_audio_timing";
    gTimingModeOid.oid_handler = pianoAudioTimingSysctl;
    gTimingModeOid.oid_fmt = "I";
    gTimingModeOid.oid_descr = "PianoAudio batching at next StartIO: 0 = 5 ms, 1 = 10 ms; fixed 7680-frame ring";
    gTimingModeOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gTimingModeOid);
    gTimingModeRegistered = true;
}

using TimingProfile = PianoAudioTiming;
static const TimingProfile &timingProfile(uint32_t mode)
{
    return *piano_audio_timing_profile(mode);
}
static uint32_t selectedTimingMode()
{
    const uint32_t mode = __atomic_load_n(&gTimingMode, __ATOMIC_ACQUIRE);
    return mode == PIANO_AUDIO_TIMING_NEW ? PIANO_AUDIO_TIMING_NEW : PIANO_AUDIO_TIMING_LEGACY;
}

enum : uint32_t {
    Rate = PIANO_AUDIO_RATE,
    OutputChannels = 2, InputChannels = 1, StreamID = 1,
    TransportBuiltIn = 0x626c746e,                        // 'bltn'
    FormatLinearPCM = 0x6c70636d,                         // 'lpcm'
    FlagsS16 = 0x4 | 0x8,                                 // signed integer, packed (little endian)
    TerminalSpeaker = 0x73706b72, TerminalMicrophone = 0x6d696372,   // 'spkr', 'micr'
    StatusType = 0, StreamMemory = 0x10000000,
    ControlVolume = 1, ControlMute = 2,
    LevelBaseClass = PIANO_AUDIO_CONTROL_BASE_LEVEL,
    LevelClass = PIANO_AUDIO_CONTROL_CLASS_VOLUME,
    MuteBaseClass = PIANO_AUDIO_CONTROL_BASE_TOGGLE,
    MuteClass = PIANO_AUDIO_CONTROL_CLASS_MUTE,
    OutputScope = PIANO_AUDIO_CONTROL_SCOPE_OUTPUT,
};
struct EngineStatus { volatile uint64_t sampleTime, hostTime, seed, reserved, writeEnd; };
static_assert(sizeof(EngineStatus) >= 0x28, "the HAL refuses a smaller engine status");
static_assert(PIANO_AUDIO_RING_FRAMES % PIANO_AUDIO_PERIOD_FRAMES == 0, "both batching modes divide the ring");
constexpr int64_t fixed64(uint32_t hz) { return int64_t(hz) << 32; }
// Wired kernel memory a client maps: the engine status and the ring.
const IOOptionBits Shared = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared);

// The two devices share one transport, which starts/stops each direction independently: a combined view so either device starting or
// stopping tells the transport the right {output, input} pair.
const PianoPcmTransport *gTransport;
IOLock *gTransportLock;
bool gOutRunning, gInRunning;
void transportSet(bool output, bool running) {
    IOLockLock(gTransportLock);
    if (output) gOutRunning = running; else gInRunning = running;
    piano_audio_dev_out_running = gOutRunning; piano_audio_dev_in_running = gInRunning;
    gTransport->running(gTransport->context, gOutRunning, gInRunning);
    IOLockUnlock(gTransportLock);
}
}  // namespace

class PianoAudioDevice : public IOService {
    OSDeclareDefaultStructors(PianoAudioDevice)
    typedef IOService super;
public:
    bool isInput;
    const char *uid;
    IOBufferMemoryDescriptor *statusMemory, *ring;
    IOLock *lock;
    thread_t engine;
    bool running, stopping, powerParked, reanchor;
    uint64_t startTime;
    uint32_t seed;
    int32_t hogOwner = -1;
    uint32_t timingMode = PIANO_AUDIO_TIMING_LEGACY;
    uint32_t volumePercent = 100;
    bool masterMute = false;

    bool initRole(bool input, const char *deviceUID);
    void free() override;
    bool start(IOService *provider) override;
    EngineStatus *status() { return static_cast<EngineStatus *>(statusMemory->getBytesNoCopy()); }
    uint32_t channels() const { return isInput ? InputChannels : OutputChannels; }
    IOMemoryDescriptor *memory(uint32_t type);
    IOReturn startIO();
    IOReturn stopIO();
    void publish(IORegistryEntry *entry);
    OSArray *controls() const;
    IOReturn setProperties(OSObject *properties) override;
    IOReturn apply(OSObject *properties, IORegistryEntry *client);
    static void engineMain(void *, wait_result_t);
    void run();
};

namespace {
PianoAudioDevice *gPowerDevices[2];
int gDeviceSleep;
}

extern "C" int piano_audio_devices_power(int sleep)
{
    if (sleep) __atomic_store_n(&gDeviceSleep,1,__ATOMIC_RELEASE);
    for (unsigned i=0;i<2;++i) {
        auto *d=__atomic_load_n(&gPowerDevices[i],__ATOMIC_ACQUIRE);
        if (!d) continue;
        IOLockLock(d->lock);
        uint64_t deadline; clock_interval_to_deadline(250,kMillisecondScale,&deadline);
        while (sleep && d->running && !d->powerParked) {
            if (IOLockSleepDeadline(d->lock,&d->powerParked,deadline,THREAD_UNINT)==THREAD_TIMED_OUT) {
                if (d->running && !d->powerParked) {IOLockUnlock(d->lock); return -1;}
                break;
            }
        }
        if (!sleep) {
            bzero(d->ring->getBytesNoCopy(),PIANO_AUDIO_RING_FRAMES*2u*d->channels());
            d->startTime=mach_absolute_time();
            auto *s=d->status();
            s->sampleTime=0; s->hostTime=d->startTime; s->writeEnd=0;
            __asm__ volatile("dmb ish" ::: "memory");
            s->seed=++d->seed;
            d->reanchor=true;
            IOLockWakeup(d->lock,&gDeviceSleep,false);
            IOLog("PianoAudio: sleep-contract1: HAL reanchor %s seed %u intent %u\n",d->uid,d->seed,d->running?1:0);
        }
        IOLockUnlock(d->lock);
    }
    if (!sleep) {
        __atomic_store_n(&gDeviceSleep,0,__ATOMIC_RELEASE);
        for (unsigned i=0;i<2;++i) {
            auto *d=__atomic_load_n(&gPowerDevices[i],__ATOMIC_ACQUIRE);
            if (!d) continue;
            IOLockLock(d->lock); IOLockWakeup(d->lock,&gDeviceSleep,false); IOLockUnlock(d->lock);
        }
    }
    return 0;
}

class PianoAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PianoAudioUserClient)
    typedef IOUserClient super;
    PianoAudioDevice *device;
    bool startedIO;
    mach_port_t notificationPort = MACH_PORT_NULL;
    io_user_reference_t notificationRefCon = 0;
public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn clientClose() override;
    void stop(IOService *provider) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) override;
    IOReturn setProperties(OSObject *properties) override;
    uint32_t controlLogCount = 0;
    mach_msg_return_t sendControlNotification(uint32_t controlID, uint32_t applied);
};

OSDefineMetaClassAndStructors(PianoAudioDevice, IOService)
OSDefineMetaClassAndStructors(PianoAudioUserClient, IOUserClient)

namespace {
void setNumber(OSDictionary *d, const char *key, uint64_t value, unsigned bits = 32) {
    if (OSNumber *n = OSNumber::withNumber(value, bits)) { d->setObject(key, n); n->release(); }
}
void setNumber(IORegistryEntry *e, const char *key, uint64_t value, unsigned bits = 32) {
    if (OSNumber *n = OSNumber::withNumber(value, bits)) { e->setProperty(key, n); n->release(); }
}
void setBool(OSDictionary *d, const char *key, bool value) { d->setObject(key, value ? kOSBooleanTrue : kOSBooleanFalse); }
OSDictionary *format(uint32_t channels) {
    OSDictionary *f = OSDictionary::withCapacity(8);
    if (!f) return nullptr;
    setNumber(f, "sample rate", uint64_t(fixed64(Rate)), 64);
    setNumber(f, "format ID", FormatLinearPCM);
    setNumber(f, "format flags", FlagsS16);
    setNumber(f, "bytes per packet", 2 * channels);
    setNumber(f, "frames per packet", 1);
    setNumber(f, "bytes per frame", 2 * channels);
    setNumber(f, "channels per frame", channels);
    setNumber(f, "bits per channel", 16);
    return f;
}
OSArray *streams(uint32_t id, uint32_t channels, uint32_t terminal) {
    OSDictionary *stream = OSDictionary::withCapacity(8), *current = format(channels), *range = format(channels);
    OSArray *available = OSArray::withCapacity(1), *list = OSArray::withCapacity(1);
    if (stream && current && range && available && list) {
        setNumber(range, "min sample rate", uint64_t(fixed64(Rate)), 64);
        setNumber(range, "max sample rate", uint64_t(fixed64(Rate)), 64);
        available->setObject(range);
        setNumber(stream, "stream ID", id);
        setNumber(stream, "starting channel", 1);
        setNumber(stream, "latency", 0);
        setNumber(stream, "terminal type", terminal);
        stream->setObject("current format", current);
        stream->setObject("available formats", available);
        list->setObject(stream);
    }
    OSSafeReleaseNULL(stream); OSSafeReleaseNULL(current); OSSafeReleaseNULL(range); OSSafeReleaseNULL(available);
    return list;
}
OSArray *controlSelectors() {
    OSArray *a = OSArray::withCapacity(2);
    if (!a) return nullptr;
    if (OSNumber *n = OSNumber::withNumber(0x6376616c, 32)) { a->setObject(n); n->release(); } // 'cval'
    if (OSNumber *n = OSNumber::withNumber(0x63726e67, 32)) { a->setObject(n); n->release(); } // 'crng'
    return a;
}
OSDictionary *makeControl(uint32_t id, uint32_t baseClass, uint32_t cls, uint32_t value, bool mute, bool input) {
    OSDictionary *d = OSDictionary::withCapacity(32);
    if (!d) return nullptr;
    setNumber(d, "control ID", id);
    setNumber(d, "UsageCase", mute ? 2 : 1);
    setNumber(d, "SampleRate", uint64_t(fixed64(Rate)), 64);
    setNumber(d, "IOAudioEngineDescription", 0);
    setNumber(d, "base class", baseClass);
    setNumber(d, "class", cls);
    setNumber(d, "scope", piano_audio_control_scope(input));
    setNumber(d, "element", 0);
    setBool(d, "read only", false);
    setNumber(d, "value", value);
    if (OSArray *a = controlSelectors()) { d->setObject("property selectors", a); a->release(); }
    if (mute) {
        setNumber(d, "minimum value", 0);
        setNumber(d, "maximum value", 1);
        setNumber(d, "integer steps", 1);
        setNumber(d, "left value", value);
        setNumber(d, "center value", value);
        setNumber(d, "right value", value);
        setNumber(d, "left channel", 1);
        setNumber(d, "right channel", input ? 1 : 2);
    } else {
        if (!piano_audio_level_dictionary<OSDictionary, OSArray>(d, value,
                [](OSDictionary *dict, const char *key, uint64_t v, unsigned bits) { setNumber(dict, key, v, bits); }, input ? 1 : 2)) {
            d->release();
            return nullptr;
        }
    }
    setNumber(d, "selectors", 0);
    setNumber(d, "mapping options", 0);
    setNumber(d, "descriptor", 0);
    return d;
}
uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(piano_audio_frames_to_ns(frames), &t); return t; }
}  // namespace

bool PianoAudioDevice::initRole(bool input, const char *deviceUID) {
    if (!super::init(nullptr)) return false;
    isInput = input;
    uid = deviceUID;
    lock = IOLockAlloc();
    statusMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, page_size, page_size);
    /* Both batching modes share this fixed ring and the same HAL contract. */
    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, PIANO_AUDIO_RING_FRAMES * 2 * channels(), page_size);
    if (!lock || !statusMemory || !ring) return false;
    bzero(statusMemory->getBytesNoCopy(), page_size);
    bzero(ring->getBytesNoCopy(), ring->getLength());
    return true;
}

void PianoAudioDevice::free() {
    OSSafeReleaseNULL(statusMemory); OSSafeReleaseNULL(ring);
    if (lock) IOLockFree(lock);
    lock = nullptr;
    super::free();
}

// The description the HAL caches (HALB_UCObject::CacheProperties) and VirtualAudio classifies the device by. Published on the device
// and copied onto each user client, since the HAL reads some keys through the service and some through the connection.
void PianoAudioDevice::publish(IORegistryEntry *e) {
    e->setProperty("device UID", uid);
    e->setProperty("device name", uid);
    e->setProperty("device manufacturer", "Apple Inc.");
    e->setProperty("model UID", uid);
    setNumber(e, "transport type", TransportBuiltIn);
    setNumber(e, "clock domain", 1);   // the HAL warns that a built-in device in clock domain 0 is unusual
    const TimingProfile &tp = timingProfile(timingMode);
    const uint32_t outLatency = tp.latency + (gTransport ? gTransport->output_latency : 0);
    const uint32_t inLatency = tp.latency + (gTransport ? gTransport->input_latency : 0);
    setNumber(e, "output latency", outLatency);
    setNumber(e, "input latency", inLatency);
    setNumber(e, "output safety offset", tp.safetyOffset);
    setNumber(e, "input safety offset", tp.safetyOffset);
    /* HAL writes modulo this field, regardless of the mapped byte capacity. */
    setNumber(e, "io buffer frame size", tp.ioBufferFrames);
    setNumber(e, "sample rate", uint64_t(fixed64(Rate)), 64);
    setNumber(e, "exclusive access owner", uint32_t(hogOwner));
    e->setProperty("is running", running ? kOSBooleanTrue : kOSBooleanFalse);
    e->setProperty("is private", kOSBooleanFalse);
    e->setProperty("supports prewarming", kOSBooleanFalse);
    e->setProperty("supports client description", kOSBooleanFalse);
    e->setProperty("wants output trap", kOSBooleanFalse);
    e->setProperty("wants input trap", kOSBooleanFalse);
    /* HALS_Device::CanBeDefaultDevice asks these through the IOAudio2
     * property path. Keep the direction-specific answers explicit. */
    e->setProperty("can be system default", kOSBooleanTrue);
    e->setProperty("can be content default", kOSBooleanTrue);
    e->setProperty("can be default", kOSBooleanTrue);
    // One direction each: the speaker device has an output stream, the mic device an input stream.
    const char *key = isInput ? "input streams" : "output streams";
    if (OSArray *a = streams(StreamID, channels(), isInput ? TerminalMicrophone : TerminalSpeaker)) { e->setProperty(key, a); a->release(); }
    if (OSArray *a = controls()) { e->setProperty("controls", a); a->release(); }
    e->setProperty("piano-audio-transport", gTransport->name);
    e->setProperty("piano-audio-control-contract", "levl/vlme");
    e->setProperty("piano-audio-level-map-contract", "range-array:raw0-100:db-fixed32.32:-36-0");
    e->setProperty("piano-audio-notification-contract", "counted-cval64:raw-integer");
    e->setProperty("piano-audio-timing-contract", tp.contract);
    e->setProperty("piano-audio-ring-contract", PIANO_AUDIO_RING_CONTRACT);
    setNumber(e, piano_audio_level_key(isInput), volumePercent);
    e->setProperty(piano_audio_mute_key(isInput), masterMute ? kOSBooleanTrue : kOSBooleanFalse);
    if (isInput) {
        e->setProperty("piano-audio-input-contract", "mic-delivery1:inpt/main:mono:db-linear:pre-HAL");
        const uint32_t inputGain = masterMute ? 0u : uint32_t(piano_audio_input_q15(int(volumePercent)));
        setNumber(e, "piano-audio-input-gain-q15", inputGain);
        e->setProperty("piano-audio-input-zero", "raw0=silence;finite-db-floor=-36;raw1-100=db-linear");
    }
}

OSArray *PianoAudioDevice::controls() const {
    OSArray *a = OSArray::withCapacity(2);
    if (!a) return a;
    OSDictionary *level = makeControl(ControlVolume, LevelBaseClass, LevelClass, volumePercent, false, isInput);
    OSDictionary *mute = makeControl(ControlMute, MuteBaseClass, MuteClass, masterMute ? 1 : 0, true, isInput);
    if (level) { a->setObject(level); level->release(); }
    if (mute) { a->setObject(mute); mute->release(); }
    return a;
}

bool PianoAudioDevice::start(IOService *provider) {
    if (!super::start(provider)) return false;
    publish(this);
    setProperty("IOUserClientClass", "PianoAudioUserClient");
    registerService();
    __atomic_store_n(&gPowerDevices[isInput?1:0],this,__ATOMIC_RELEASE);
    return true;
}

// The one property the HAL writes (HALB_UCObject::SetProperty, through the connection): "exclusive access owner", the pid holding
// hog mode, or -1. VirtualAudio takes hog mode on the device before it builds its ports, and gives up ("Unable to take hog mode") if
// it is refused. On the Speaker, the kernel's own master volume and mute of level 9 (the transport's control; root through
// IORegistryEntrySetCFProperties on the device): "piano-audio-volume" (0..100) and "piano-audio-mute" (a boolean). Other keys are
// accepted and ignored.
IOReturn PianoAudioDevice::apply(OSObject *properties, IORegistryEntry *client) {
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnBadArgument;
    bool controlsChanged = false;
    {
        OSNumber *volume = OSDynamicCast(OSNumber, d->getObject(piano_audio_level_key(isInput)));
        OSBoolean *mute = OSDynamicCast(OSBoolean, d->getObject(piano_audio_mute_key(isInput)));
        const int v = volume && volume->unsigned32BitValue() <= 100 ? int(volume->unsigned32BitValue()) : -1;
        const int m = mute ? (mute->isTrue() ? 1 : 0) : -1;
        if (v >= 0 || m >= 0) {
            auto control = isInput ? gTransport->input_control : gTransport->control;
            if (control) control(gTransport->context, v, m);
            if (v >= 0) { volumePercent = uint32_t(v); setNumber(this, piano_audio_level_key(isInput), volumePercent); controlsChanged = true; }
            if (m >= 0) { masterMute = m != 0; setProperty(piano_audio_mute_key(isInput), masterMute ? kOSBooleanTrue : kOSBooleanFalse); controlsChanged = true; }
        }
    }
    if (OSNumber *owner = OSDynamicCast(OSNumber, d->getObject("exclusive access owner"))) {
        IOLockLock(lock);
        hogOwner = int32_t(owner->unsigned32BitValue());
        IOLockUnlock(lock);
        setNumber(this, "exclusive access owner", uint32_t(hogOwner));
        if (client) setNumber(client, "exclusive access owner", uint32_t(hogOwner));
        piano_audio_dev_hog_owner = hogOwner;
    }
    if (controlsChanged) {
        /* setProperty emits the IORegistry change; rebuilding the dictionary
         * makes the new scalar/mute values visible to HAL property reads. */
        publish(this);
        if (client) publish(client);
    }
    return kIOReturnSuccess;
}

IOReturn PianoAudioDevice::setProperties(OSObject *properties) { return apply(properties, nullptr); }

IOMemoryDescriptor *PianoAudioDevice::memory(uint32_t type) {
    if (type == StatusType) return statusMemory;
    if (type == StreamMemory + StreamID) return ring;
    return nullptr;
}

IOReturn PianoAudioDevice::startIO() {
    IOLockLock(lock);
    if (!running) {
        while (stopping) IOLockSleep(lock, &stopping, THREAD_UNINT);
        reanchor=false; powerParked=false;
        /* Only kernel batching changes at StartIO. HAL has already cached the
         * fixed ring/safety/latency tuple before calling this selector. */
        timingMode = selectedTimingMode();
        const TimingProfile &tp = timingProfile(timingMode);
        bzero(ring->getBytesNoCopy(), tp.ringFrames * 2u * channels());
        EngineStatus *s = status();
        startTime = mach_absolute_time();
        s->sampleTime = 0; s->hostTime = startTime; s->seed = ++seed; s->writeEnd = 0;
        IOLog("PianoAudio: HAL ring: %s mode %u ring %u io-ring %u period %u zero %u safety %u seed %u\n",
              isInput ? "input" : "output", timingMode, tp.ringFrames, tp.ioBufferFrames,
              tp.periodFrames, tp.zeroTimePeriod, tp.safetyOffset, seed);
        __asm__ volatile("dmb ish" ::: "memory");
        running = true;
        setProperty("is running", kOSBooleanTrue);
        transportSet(!isInput, true);
        if (kernel_thread_start(engineMain, this, &engine) == KERN_SUCCESS) thread_deallocate(engine);
        else {running = false; transportSet(!isInput, false);}
    }
    IOLockUnlock(lock);
    return running ? kIOReturnSuccess : kIOReturnNoResources;
}

IOReturn PianoAudioDevice::stopIO() {
    IOLockLock(lock);
    if (running) {
        running = false; stopping = true;
        IOLockWakeup(lock,&gDeviceSleep,false);
        while (stopping) IOLockSleep(lock, &stopping, THREAD_UNINT);   // the engine thread clears it as it leaves
        setProperty("is running", kOSBooleanFalse);
        transportSet(!isInput, false);
    }
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

void PianoAudioDevice::engineMain(void *self, wait_result_t) { static_cast<PianoAudioDevice *>(self)->run(); }

// One period per pass, on the device's own clock: period n ends at startTime + n periods. The speaker device hands the period the
// hardware position just passed to the transport, then erases the ring so a client that stops writing plays silence. The mic device
// fills the period the HAL is about to read (safety-offset behind) with the transport's frames, or silence. The zero time stamp moves
// at each wrap of the ring, as IOAudioFamily's engines do.
void PianoAudioDevice::run() {
    retain();
    thread_set_thread_name(current_thread(), isInput ? "piano-audio-in" : "piano-audio-out");
    int16_t *buffer = static_cast<int16_t *>(ring->getBytesNoCopy());
    const uint32_t ch = channels();
    const TimingProfile &tp = timingProfile(timingMode);
    int16_t period[PIANO_AUDIO_PERIOD_FRAMES * OutputChannels];
    uint64_t done = 0, soundPeriods = 0, latePeriods = 0, maxLate = 0;
    uint64_t inputDelivered = 0, firstInput = 0;
    for (;;) {
        IOLockLock(lock);
        while (running && __atomic_load_n(&gDeviceSleep,__ATOMIC_ACQUIRE)) {
            powerParked=true;
            IOLockWakeup(lock,&powerParked,false);
            IOLockSleep(lock,&gDeviceSleep,THREAD_UNINT);
        }
        powerParked=false;
        if (reanchor) {done=0; reanchor=false;}
        const bool live = running;
        IOLockUnlock(lock);
        if (!live) break;
        const uint64_t deadline = startTime + absolute(done + tp.periodFrames);
        if (mach_absolute_time() > deadline + absolute(tp.periodFrames)) piano_audio_dev_late = piano_audio_dev_late + 1;
        else clock_delay_until(deadline);
        IOLockLock(lock);
        if (!running || __atomic_load_n(&gDeviceSleep,__ATOMIC_ACQUIRE)) {IOLockUnlock(lock); continue;}
        const uint64_t woke = mach_absolute_time();
        const uint64_t late = woke > deadline ? woke - deadline : 0;
        if (late > maxLate) maxLate = late;
        if (late > absolute(tp.periodFrames)) ++latePeriods;
        if (isInput) {
            const uint32_t captured = gTransport->capture(gTransport->context, period, tp.periodFrames);
            if (captured < tp.periodFrames) bzero(period + captured * ch, (tp.periodFrames - captured) * 2 * ch);
            piano_audio_copy_to_ring(buffer, tp.ringFrames, done, period, tp.periodFrames, ch);
            inputDelivered += captured;
            if (captured && !firstInput) firstInput = done + 1;
            piano_audio_dev_captured = piano_audio_dev_captured + captured;
        } else {
            piano_audio_copy_from_ring(period, buffer, tp.ringFrames, done, tp.periodFrames, ch);
            int any = 0;
            for (uint32_t i = 0; i < tp.periodFrames * ch; ++i) any |= period[i];
            if (any) ++soundPeriods;
            const uint32_t played = gTransport->play(gTransport->context, period, tp.periodFrames);
            piano_audio_zero_ring(buffer, tp.ringFrames, done, tp.periodFrames, ch);
            piano_audio_dev_played = piano_audio_dev_played + played;
        }
        const uint64_t before = done;
        done += tp.periodFrames;
        if (before / tp.zeroTimePeriod != done / tp.zeroTimePeriod) {
            EngineStatus *s = status();
            const uint64_t anchor = (done / tp.zeroTimePeriod) * tp.zeroTimePeriod;
            s->hostTime = startTime + absolute(anchor);
            __asm__ volatile("dmb ish" ::: "memory");
            s->sampleTime = anchor;
            __asm__ volatile("dmb ish" ::: "memory");
        }
        if (!isInput) {
            uint64_t ns; absolutetime_to_nanoseconds(late,&ns);
            piano_audio_producer_trace(tp.periodFrames,uint32_t(ns/1000u),status()->sampleTime,status()->hostTime,status()->writeEnd);
        }
        piano_audio_dev_periods = piano_audio_dev_periods + 1;
        IOLockUnlock(lock);
    }
    IOLog("PianoAudio: HAL ring read: %s mode %u periods %llu with sound %llu frames %llu write-end %llu\n",
          isInput ? "input" : "output", timingMode, (unsigned long long)(done / tp.periodFrames),
          (unsigned long long)soundPeriods, (unsigned long long)done, (unsigned long long)status()->writeEnd);
    if (isInput) IOLog("PianoAudio: mic HAL stop: seed %u requested %llu delivered %llu startup-or-missing %llu first-frame %llu read-end %llu\n",
          seed, (unsigned long long)done, (unsigned long long)inputDelivered, (unsigned long long)(done - inputDelivered),
          (unsigned long long)firstInput, (unsigned long long)status()->reserved);
    uint64_t maxLateNs;
    absolutetime_to_nanoseconds(maxLate, &maxLateNs);
    IOLog("PianoAudio: HAL wake: %s mode %u late-periods %llu max-us %llu\n", isInput ? "input" : "output", timingMode,
          (unsigned long long)latePeriods, (unsigned long long)(maxLateNs / 1000u));
    IOLockLock(lock);
    stopping = false;
    powerParked=false;
    IOLockWakeup(lock, &powerParked, false);
    IOLockWakeup(lock, &stopping, false);
    IOLockUnlock(lock);
    release();
}

bool PianoAudioUserClient::start(IOService *provider) {
    device = OSDynamicCast(PianoAudioDevice, provider);
    if (!device || !super::start(provider)) return false;
    device->publish(this);
    piano_audio_dev_clients = piano_audio_dev_clients + 1;
    return true;
}

IOReturn PianoAudioUserClient::clientClose() {
    if (startedIO && device) device->stopIO();
    startedIO = false;
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void PianoAudioUserClient::stop(IOService *provider) {
    piano_audio_dev_clients = piano_audio_dev_clients - 1;
    super::stop(provider);
}

void PianoAudioUserClient::free() {
    if (notificationPort != MACH_PORT_NULL) {
        IOUserClient::releaseNotificationPort(notificationPort);
        notificationPort = MACH_PORT_NULL;
    }
    notificationRefCon = 0;
    super::free();
}

IOReturn PianoAudioUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
                                              OSObject *, void *) {
    piano_audio_dev_last_selector = selector;
    piano_audio_dev_selectors = piano_audio_dev_selectors + 1;
    switch (selector) {
    case 0: case 10: {                                     // StartIO, StartIOWithFlags
        IOReturn r = device->startIO();
        if (!r) startedIO = true;
        if (selector == 10 && a->scalarOutputCount >= 1) { a->scalarOutput[0] = 0; a->scalarOutputCount = 1; }
        return r;
    }
    case 1: case 11:                                       // StopIO, StopIOWithFlags
        startedIO = false;
        return device->stopIO();
    case 2: {                                              // SetControlValue: control UCID, integer raw value
        if (!device || a->scalarInputCount < 2 || a->scalarInput[0] > UINT32_MAX) return kIOReturnBadArgument;
        const uint32_t id = uint32_t(a->scalarInput[0]);
        const uint64_t raw = a->scalarInput[1];
        OSDictionary *d = OSDictionary::withCapacity(1);
        if (!d) return kIOReturnNoMemory;
        IOReturn result = kIOReturnSuccess;
        if (id == ControlVolume) {
            const int p = piano_audio_control_decode(device->isInput, id, raw);
            if (p < 0 || p > 100) result = kIOReturnBadArgument;
            else if (OSNumber *n = OSNumber::withNumber(uint32_t(p), 32)) { d->setObject(piano_audio_level_key(device->isInput), n); n->release(); }
            else result = kIOReturnNoMemory;
        } else if (id == ControlMute) {
            const int m = piano_audio_control_decode(device->isInput, id, raw);
            if (m < 0) result = kIOReturnBadArgument;
            else d->setObject(piano_audio_mute_key(device->isInput), m ? kOSBooleanTrue : kOSBooleanFalse);
        } else {
            result = kIOReturnBadArgument;
        }
        if (result == kIOReturnSuccess) result = device->apply(d, this);
        d->release();
        if (!result) {
            const uint32_t applied = id == ControlMute ? (device->masterMute ? 1U : 0U) : device->volumePercent;
            const mach_msg_return_t sent = sendControlNotification(id, applied);
            // Bounded receipt/apply/send witness; no changes to transport timing.
            if (__sync_fetch_and_add(&controlLogCount, 1) < 32)
                IOLog("PianoAudio: counted-cval64:raw-integer UCID %u input %llu applied %u count 1 bytes 64 send 0x%x\n",
                      id, (unsigned long long)raw, applied, unsigned(sent));
            if (a->scalarOutputCount >= 1) {
                a->scalarOutput[0] = applied;
                a->scalarOutputCount = 1;
            }
        }
        return result;
    }
    case 3:                                                // PerformConfigChange: none is ever requested
        return kIOReturnSuccess;
    case 4: {                                              // SetNominalSampleRate: the one rate there is
        int64_t rate = 0;
        if (a->structureInputSize == sizeof rate) memcpy(&rate, a->structureInput, sizeof rate);
        return rate == fixed64(Rate) ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    case 5: {                                              // SetStreamCurrentFormat: only the published format
        struct { int64_t rate; uint32_t id, flags, bytesPerPacket, framesPerPacket, bytesPerFrame, channels, bits, reserved; } f;
        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) return kIOReturnBadArgument;
        memcpy(&f, a->structureInput, sizeof f);
        return f.rate == fixed64(Rate) && f.id == FormatLinearPCM && f.channels == device->channels() && f.bits == 16
            ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    case 6: case 12:                                       // SetStreamActive, SetClientDescription
        return kIOReturnSuccess;
    default:
        piano_audio_dev_bad_selectors = piano_audio_dev_bad_selectors + 1;
        return kIOReturnUnsupported;
    }
}

IOReturn PianoAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOMemoryDescriptor *m = device ? device->memory(type) : nullptr;
    if (!m) return kIOReturnBadArgument;
    m->retain();
    *memory = m;
    *options = 0;
    piano_audio_dev_maps = piano_audio_dev_maps + 1;
    return kIOReturnSuccess;
}

IOReturn PianoAudioUserClient::setProperties(OSObject *properties) {
    return device ? device->apply(properties, this) : kIOReturnNotReady;
}

mach_msg_return_t PianoAudioUserClient::sendControlNotification(uint32_t controlID, uint32_t applied) {
    mach_port_t port = notificationPort;
    if (port == MACH_PORT_NULL) return MACH_SEND_INVALID_DEST;
    PianoAudio2NotificationMessage message;
    piano_audio_control_notification(&message, port, controlID, applied);
    return mach_msg_send_from_kernel(&message.header, message.header.msgh_size);
}

IOReturn PianoAudioUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) {
    if (type != 0) return kIOReturnBadArgument;
    if (port != MACH_PORT_NULL) {
        if (notificationPort != MACH_PORT_NULL) IOUserClient::releaseNotificationPort(notificationPort);
        notificationRefCon = refCon;
        __sync_synchronize();
        notificationPort = port;
    } else {
        mach_port_t old = notificationPort;
        notificationRefCon = 0;
        __sync_synchronize();
        notificationPort = MACH_PORT_NULL;
        if (old != MACH_PORT_NULL) IOUserClient::releaseNotificationPort(old);
    }
    return kIOReturnSuccess;
}

// OSMetaClass keeps its superclass after the vtable pointer and the reserved expansion pointer (libkern/c++/OSMetaClass.h: reserved,
// superClassLink, className). Checked against the superclass the class was defined with before it is changed, and the new parent must
// itself derive from that class, so every cast the C++ class allows stays true.
static bool reparent(const OSMetaClass *meta, const OSMetaClass *defined, const char *family) {
    const OSSymbol *name = OSSymbol::withCString(family);
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    const OSMetaClass **link = reinterpret_cast<const OSMetaClass **>(const_cast<OSMetaClass *>(meta)) + 2;
    bool conforms = false;
    for (const OSMetaClass *c = parent; c && !conforms; c = c->getSuperClass()) conforms = c == defined;
    if (!parent || *link != defined || !conforms) return false;
    *link = parent;
    return true;
}

// Publishes one device of a role (input/output) with its UID on a built-in nub of IOResources.
static bool publishDevice(IOService *resources, bool input, const char *uid, const char *nubName) {
    IOService *nub = OSTypeAlloc(IOService);
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); return false; }
    nub->setName(nubName);
    nub->setProperty("built-in", kOSBooleanTrue);
    if (!nub->attach(resources)) { nub->release(); return false; }
    nub->registerService();
    PianoAudioDevice *device = OSTypeAlloc(PianoAudioDevice);
    bool ok = device && device->initRole(input, uid) && device->attach(nub) && device->start(nub);
    if (!ok && device) { if (device->getProvider()) device->detach(nub); device->release(); }
    return ok;
}

extern "C" int piano_audio_devices_stage(void) { return piano_audio_dev_stage; }

// PianoAudioTap.cpp: the tap's user client linked under IOAudio2DeviceUserClient as the Mic's is, so coreaudiod's sandbox rule for
// that class (com.apple.audio.coreaudiod.sb: iokit-open iokit-user-client-class "IOAudio2DeviceUserClient", which the Mic's user
// client passes on the tablet) admits it too, besides the extension the driver service issues for the plug-in's
// AudioServerPlugIn_IOKitUserClients. Only after the family's classes were found (piano_audio_devices_attach); 0 done, -1 not.
extern "C" int piano_audio_reparent_user_client(const OSMetaClass *meta) {
    return piano_audio_dev_stage >= 3 && reparent(meta, IOUserClient::metaClass, "IOAudio2DeviceUserClient") ? 0 : -1;
}

// Links the two classes under the family's (a native kext's classes are registered as it loads) and publishes a speaker and a
// microphone device on the transport.
extern "C" int piano_audio_devices_attach(const PianoPcmTransport *transport) {
    IOService *resources = IOService::getResourceService();
    if (!resources || !transport || piano_audio_dev_stage) return -1;
    registerPianoAudioTimingSysctl();
    gTransportLock = IOLockAlloc();
    if (!gTransportLock) { piano_audio_dev_stage = -1; return -1; }
    gTransport = transport;
    piano_audio_dev_stage = 1;
    // Normally loaded already, for IOAVFamily; a no-op then.
    piano_audio_dev_family_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOAudio2Family");
    piano_audio_dev_stage = 2;
    if (!reparent(PianoAudioDevice::metaClass, IOService::metaClass, "IOAudio2Device") ||
        !reparent(PianoAudioUserClient::metaClass, IOUserClient::metaClass, "IOAudio2DeviceUserClient")) {
        piano_audio_dev_stage = -2;
        return -1;
    }
    piano_audio_dev_stage = 3;
    const bool speaker = publishDevice(resources, false, "Speaker", "piano-audio-out");
    const bool mic = publishDevice(resources, true, "Mic", "piano-audio-in");
    if (!speaker && !mic) { piano_audio_dev_stage = -3; return -1; }
    piano_audio_dev_stage = 4;
    return (speaker ? 1 : 0) | (mic ? 2 : 0);
}
