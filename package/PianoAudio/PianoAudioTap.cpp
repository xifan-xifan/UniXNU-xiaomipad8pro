// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The microphone tap (AudioTap.h; private/host-checks MIC-NS.md section 8): the service "piano-mic-tap" (class PianoMicTap, on
// IOResources) and its user client PianoMicTapUserClient, through which the "Mic (noise reduced)" AudioServerPlugIn
// (userland/piano-micns) maps the tap read-only. Published at level 10 with the capture path (AudioDsp.cpp liveRun, piano-audio-tap,
// default 1).
//   open     IOServiceOpen type 0, only from a task holding com.apple.private.audio.driver-host (coreaudiod and its driver services
//            on 27.3); any other caller gets kIOReturnNotPermitted (counted in the header's `refused`). Each open client holds the
//            capture plan up (AudioLive.h audio_live_tap_client); its close (or its task's death) lets it go.
//   memory   type PIANO_TAP_MEMORY: the header and the ring, mapped read-only (kIOMapReadOnly).
//   methods  none: the reader needs nothing but the mapping.
// The plug-in names PIANO_TAP_USER_CLIENT in its Info.plist's AudioServerPlugIn_IOKitUserClients, so the driver service extends its
// sandbox for this class (com.apple.audio.driver-host.iokit-user-client-class, coreaudiod.sb).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include "AudioLog.h"
#include "AudioLive.h"

extern "C" int piano_audio_reparent_user_client(const OSMetaClass *meta);   // PianoAudioDevice.cpp

namespace {
AudioLiveShared *gTapShared;
IOBufferMemoryDescriptor *gTapMemory;
void (*gTapWake)(void);
}

class PianoMicTap : public IOService {
    OSDeclareDefaultStructors(PianoMicTap)
    typedef IOService super;
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;
};

class PianoMicTapUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PianoMicTapUserClient)
    typedef IOUserClient super;
    bool counted = false;
public:
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    void stop(IOService *provider) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    void release_hold();
};

OSDefineMetaClassAndStructors(PianoMicTap, IOService)
OSDefineMetaClassAndStructors(PianoMicTapUserClient, IOUserClient)

IOReturn PianoMicTap::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (type != 0) return kIOReturnBadArgument;
    if (!gTapShared || !gTapShared->tap) return kIOReturnNotReady;
    OSObject *entitled = IOUserClient::copyClientEntitlement(owningTask, PIANO_TAP_ENTITLEMENT);
    const bool allowed = entitled == kOSBooleanTrue;
    if (entitled) entitled->release();
    if (!allowed) {
        const uint32_t n = __atomic_add_fetch(&gTapShared->tap->refused, 1u, __ATOMIC_ACQ_REL);
        if (n <= 8) piano_audio_log("tap1: open refused (no %s), %u so far", PIANO_TAP_ENTITLEMENT, n);
        return kIOReturnNotPermitted;
    }
    PianoMicTapUserClient *client = OSTypeAlloc(PianoMicTapUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties)) {client->release(); return kIOReturnBadArgument;}
    if (!client->attach(this)) {client->release(); return kIOReturnError;}
    if (!client->start(this)) {client->detach(this); client->release(); return kIOReturnError;}
    *handler = client;
    return kIOReturnSuccess;
}

bool PianoMicTapUserClient::start(IOService *provider)
{
    if (!OSDynamicCast(PianoMicTap, provider) || !super::start(provider)) return false;
    const int open = audio_live_tap_client(gTapShared, 1);
    counted = true;
    piano_audio_log("tap1: open: %d client(s); the capture plan is held up", open);
    if (gTapWake) gTapWake();
    return true;
}

void PianoMicTapUserClient::release_hold()
{
    if (!counted) return;
    counted = false;
    const int open = audio_live_tap_client(gTapShared, -1);
    piano_audio_log("tap1: close: %d client(s) left", open);
    if (gTapWake) gTapWake();
}

IOReturn PianoMicTapUserClient::clientClose()
{
    release_hold();
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn PianoMicTapUserClient::clientDied() {return clientClose();}

void PianoMicTapUserClient::stop(IOService *provider)
{
    release_hold();
    super::stop(provider);
}

IOReturn PianoMicTapUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (type != PIANO_TAP_MEMORY || !gTapMemory) return kIOReturnBadArgument;
    gTapMemory->retain();
    *memory = gTapMemory;
    *options = kIOMapReadOnly;
    return kIOReturnSuccess;
}

IOReturn PianoMicTapUserClient::externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *)
{
    return kIOReturnUnsupported;
}

extern "C" int piano_audio_tap_publish(void *shared, void (*wake)(void))
{
    AudioLiveShared *sh = static_cast<AudioLiveShared *>(shared);
    IOService *resources = IOService::getResourceService();
    if (!sh || !resources || gTapShared) return -1;
    const int linked = piano_audio_reparent_user_client(PianoMicTapUserClient::metaClass);
    piano_audio_log("tap1: user client %s %s IOAudio2DeviceUserClient (coreaudiod.sb's static rule; the plug-in's extension either way)",
                    PIANO_TAP_USER_CLIENT, linked ? "NOT linked under" : "linked under");
    const IOOptionBits shared_options = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared);
    const vm_size_t bytes = (PIANO_TAP_BYTES + page_size - 1) & ~(vm_size_t)(page_size - 1);
    gTapMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared_options, bytes, page_size);
    if (!gTapMemory) return -1;
    auto *header = static_cast<PianoTapHeader *>(gTapMemory->getBytesNoCopy());
    bzero(header, bytes);
    audio_tap_init(header);
    gTapWake = wake;
    __atomic_store_n(&sh->tap, header, __ATOMIC_RELEASE);   // kept for the boot, as the shared state
    gTapShared = sh;
    PianoMicTap *service = OSTypeAlloc(PianoMicTap);
    if (!service || !service->init()) {OSSafeReleaseNULL(service); return -1;}
    service->setName(PIANO_TAP_SERVICE);
    service->setProperty("piano-tap-contract", "tap1:mono-s16-48k:ring16384:read-only:entitlement-driver-host");
    service->setProperty("piano-tap-user-client", PIANO_TAP_USER_CLIENT);
    if (!service->attach(resources)) {service->release(); return -1;}
    if (!service->start(resources)) {service->detach(resources); service->release(); return -1;}
    service->registerService();
    return 0;
}
