// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Native V5a bounded transport adapted from the approved UniXNU frontend (UPSTREAM).
// Apple's 27.3 PV support opens this exact service/type0. Selectors:0 shared
// send/queue buffers,1/2 UUID register/deregister,3 copied vtpv message plus up
// to5 IOSurface IDs,4 queue-drained; notification type0. Replies are enqueued
// asynchronously; no guest-dependent wait is allowed inside selector3.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSharedDataQueue.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <kern/task.h>
#include "PianoVideoVTContract.h"

extern kmod_info_t g_kernel_kmod_info;
static IOBufferMemoryDescriptor *capture;
extern "C" {
void *droidvm_vthost_client_open(void *transport, bool (*send)(void *, const uint8_t *, uint32_t), task_t task, int pid);
bool droidvm_vthost_available(void);
void droidvm_vthost_client_close(void *host);
int droidvm_vthost_message(void *host, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount);
volatile uint32_t droidvm_vtpv_sent, droidvm_vtpv_dropped;
}

namespace {
constexpr uint32_t FirstSendBytes = 0x4000;       // the framework's first request
constexpr uint64_t MaxSendBytes = pianovideo::vtcontract::kMaxMessageBytes;    // a message's limit (a compressed frame and its attachments)
constexpr uint32_t QueueBytes = 256 * 1024;       // the host's messages waiting for the guest
constexpr uint32_t HeaderBytes = 48;              // the 'vtpv' atom
constexpr uint32_t MaxSurfaces = 5;
constexpr unsigned LogEntries = 32, CaptureEntries = 32, CaptureBytes = 8192;

uint32_t big32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t big64(const uint8_t *p) { return uint64_t(big32(p)) << 32 | big32(p + 4); }
}

extern "C" {
// One message from the guest: who sent it, its header, its IOSurfaces and its first payload bytes (after the header).
struct droidvm_vtpv_record {
    uint32_t seq, pid, length, surface_count;
    uint32_t type, flags;
    uint64_t request;
    uint8_t uuid[16];
    uint32_t surfaces[MaxSurfaces];
    uint32_t reserved;
    uint8_t payload[64];
};
// For the host rig: 1 the class registered, 2 the service published; selector calls by selector; opens; refused messages.
volatile uint32_t droidvm_vtpv_stage, droidvm_vtpv_opens, droidvm_vtpv_messages, droidvm_vtpv_refused, droidvm_vtpv_uuids;
volatile uint32_t droidvm_vtpv_selectors[8];
// registerNotificationPort: calls, the last type and result; the step buffers() last failed at (1 the queue, 2 its mapping,
// 3 the send buffer, 4 its mapping, 5 too large); clients closed.
volatile uint32_t droidvm_vtpv_notify_calls, droidvm_vtpv_notify_type, droidvm_vtpv_notify_result, droidvm_vtpv_buffer_fail;
volatile uint32_t droidvm_vtpv_closes;
struct droidvm_vtpv_record droidvm_vtpv_log[LogEntries];
volatile uint64_t droidvm_vtpv_capture_pa;
int piano_video_vt_attach(void);
}

// The first message of each type and direction, whole, into the capture region.
static IOLock *captureLock;
static void captureMessage(const uint8_t *bytes, uint32_t length, uint32_t direction) {
    if (!capture || !captureLock || length < HeaderBytes) return;
    const uint32_t type = big32(bytes + 8);
    IOLockLock(captureLock);
    uint8_t *region = static_cast<uint8_t *>(capture->getBytesNoCopy());
    for (unsigned i = 0; i < CaptureEntries; ++i) {
        uint8_t *slot = region + i * CaptureBytes;
        uint32_t used, dir;
        memcpy(&used, slot, 4);
        memcpy(&dir, slot + 4, 4);
        if (used && dir == direction && big32(slot + 8 + 8) == type) break;   // that type is kept already
        if (used) continue;
        const uint32_t kept = length < CaptureBytes - 8 ? length : CaptureBytes - 8;
        memcpy(slot + 4, &direction, 4);
        memcpy(slot + 8, bytes, kept);
        memcpy(slot, &length, 4);
        break;
    }
    IOLockUnlock(captureLock);
}

// LT-B5 B1 (private/host-checks/20261124-browser-gpu ANALYSIS section 12): Safari's GPU process may open this service only on a virtual
// device (GPUProcess.sb (system-attribute virtual-device)), which piano is not. With boot-arg piano-video-vt-grant=1 the service names
// "com.apple.WebKit" in NativeSandboxGrantProcess (the k27 kernel's per-service grant: is_io_service_open_extended skips the MACF open
// checks for a process whose first 16 name characters match). The kernel compares only 16 characters, which also covers WebContent and
// Networking, so the user client then admits a com.apple.WebKit* process only if it holds the GPU process's own entitlement
// (com.apple.rootless.storage.WebKitGPUSandbox: on WebKit.GPU.xpc, not on WebContent / Networking, checked on 27.2).
static bool pianoVtGrant() {
    static int grant = -1;
    // default ON since y12i (Safari hardware decode qualified: v131y12i-qual1/qual3); piano-video-vt-grant=0 turns it off
    if (grant < 0) { uint32_t v = 1; PE_parse_boot_argn("piano-video-vt-grant", &v, sizeof v); grant = v == 1; }
    return grant == 1;
}
static constexpr const char PianoVtGrantPrefix[] = "com.apple.WebKit";            // 16 characters: the kernel's compare length
static constexpr const char PianoVtGrantEntitlement[] = "com.apple.rootless.storage.WebKitGPUSandbox";

class AppleVideoToolboxParavirtualizationDriver : public IOService {
    OSDeclareDefaultStructors(AppleVideoToolboxParavirtualizationDriver)
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        setProperty("IOUserClientClass", "AppleVideoToolboxParavirtualizationUserClient");
        // The 27.3 support framework's guest gate matches this IOClass and then calls IOServiceOpen(type 0).
        // It does not read kern.hv_vmm_present or a guest boolean property.  Keep a diagnostic property so the
        // registry records exactly what this service implements; IOUserClientClass is consumed by IOServiceOpen.
        setProperty("piano-video-vt-guest-gate", "IOServiceMatching(AppleVideoToolboxParavirtualizationDriver)+IOServiceOpen(type=0)");
        if (pianoVtGrant()) {
            setProperty("NativeSandboxGrantProcess", PianoVtGrantPrefix);
            IOLog("PianoVideoVT G1: sandbox grant %s (16-char kernel match), admitted only with %s\n", PianoVtGrantPrefix,
                  PianoVtGrantEntitlement);
        }
        IOLog("PianoVideo: vt guest property published: gate=IOServiceMatching(AppleVideoToolboxParavirtualizationDriver)+IOServiceOpen(type=0), kern.hv_vmm_present untouched\n");
        // Synchronously: a process that writes kern.piano_video_vt finds the service as soon as the write returns.
        registerService(kIOServiceSynchronous);
        IOLog("PianoVideo: vt service started: AppleVideoToolboxParavirtualizationDriver user-client AppleVideoToolboxParavirtualizationUserClient\n");
        return true;
    }
};
OSDefineMetaClassAndStructors(AppleVideoToolboxParavirtualizationDriver, IOService)

class AppleVideoToolboxParavirtualizationUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AppleVideoToolboxParavirtualizationUserClient)
    typedef IOUserClient super;
    task_t owner;
    int ownerPid;
    char ownerName[32];
    IOLock *lock;
    IOBufferMemoryDescriptor *send;
    IOMemoryMap *sendMap;
    IOSharedDataQueue *queue;
    IOMemoryMap *queueMap;
    uint64_t capacity;
    IOLock *sendLock;     // the queue has one producer at a time: the guest's thread and the host's workers both send
    void *host;
    uint8_t uuids[32][16];
    unsigned uuidCount;
    static bool sendToGuest(void *transport, const uint8_t *bytes, uint32_t size);

    IOReturn buffers(uint64_t size, uint64_t *out);
    IOReturn message(uint64_t length, const uint64_t *surfaces, unsigned count, uint64_t *status);
public:
    bool initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) override;
};
OSDefineMetaClassAndStructors(AppleVideoToolboxParavirtualizationUserClient, IOUserClient)

bool AppleVideoToolboxParavirtualizationUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) {
    if (type != 0 || !super::initWithTask(task, token, type, properties)) return false;
    owner = task;
    // start()/worker context need not be the process that opened the IOUC.
    ownerPid = -1;
    strlcpy(ownerName,"?",sizeof ownerName);
    proc_t process = task ? (proc_t)get_bsdtask_info(task) : nullptr;
    if (process) {
        ownerPid = proc_pid(process);
        proc_name(ownerPid,ownerName,sizeof ownerName);
    }
    if (pianoVtGrant() && !strncmp(ownerName, PianoVtGrantPrefix, sizeof PianoVtGrantPrefix - 1)) {
        OSObject *entitled = task ? IOUserClient::copyClientEntitlement(task, PianoVtGrantEntitlement) : nullptr;
        const bool gpu = entitled == kOSBooleanTrue;
        if (entitled) entitled->release();
        IOLog("PianoVideoVT G1: %s pid %d name %s (WebKit GPU entitlement %d)\n", gpu ? "admitted" : "refused", ownerPid, ownerName,
              gpu);
        if (!gpu) return false;
    }
    setProperty("piano-video-owner-pid",uint64_t(ownerPid),32);
    setProperty("piano-video-owner-name",ownerName);
    lock = IOLockAlloc();
    sendLock = IOLockAlloc();
    return lock && sendLock;
}

bool AppleVideoToolboxParavirtualizationUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    droidvm_vtpv_opens = droidvm_vtpv_opens + 1;
    IOLog("PianoVideo: vt user client opened by pid %d (count %u)\n", ownerPid, droidvm_vtpv_opens);
    IOLog("PianoVideoVT V9c: owner pid %d name %s source owning-task\n",ownerPid,ownerName);
    host = droidvm_vthost_client_open(this, sendToGuest, owner, ownerPid);
    return host != nullptr;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::clientClose() {
    droidvm_vtpv_closes = droidvm_vtpv_closes + 1;
    IOLockLock(lock);
    void *closing = host;
    host = nullptr;
    droidvm_vthost_client_close(closing);
    IOLockUnlock(lock);
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void AppleVideoToolboxParavirtualizationUserClient::free() {
    if (host) { droidvm_vthost_client_close(host); host = nullptr; }
    OSSafeReleaseNULL(sendMap);
    OSSafeReleaseNULL(send);
    OSSafeReleaseNULL(queueMap);
    OSSafeReleaseNULL(queue);
    if (lock) IOLockFree(lock);
    if (sendLock) IOLockFree(sendLock);
    super::free();
}

// Selector 0: the send buffer at least `size` bytes (a new one, mapped anew, when it must grow; the framework stops using the
// old one as it takes the answer) and the queue, made and mapped once.
IOReturn AppleVideoToolboxParavirtualizationUserClient::buffers(uint64_t size, uint64_t *out) {
    if (size > MaxSendBytes) { droidvm_vtpv_buffer_fail = 5; return kIOReturnNoMemory; }
    size = size < FirstSendBytes ? FirstSendBytes : (size + page_size - 1) & ~uint64_t(page_size - 1);
    if (size > MaxSendBytes + page_size) { droidvm_vtpv_buffer_fail = 5; return kIOReturnNoMemory; }
    if (!queue) {
        queue = IOSharedDataQueue::withCapacity(QueueBytes);
        if (!queue) { droidvm_vtpv_buffer_fail = 1; return kIOReturnNoMemory; }
        IOMemoryDescriptor *memory = queue->getMemoryDescriptor();
        queueMap = memory ? memory->createMappingInTask(owner, 0, kIOMapAnywhere) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!queueMap) { droidvm_vtpv_buffer_fail = 2; return kIOReturnNoMemory; }
    }
    if (!send || size > capacity) {
        IOBufferMemoryDescriptor *buffer =
            IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared),
                                                      size, page_size);
        IOMemoryMap *map = buffer ? buffer->createMappingInTask(owner, 0, kIOMapAnywhere) : nullptr;
        if (!map) {
            droidvm_vtpv_buffer_fail = buffer ? 4 : 3;
            OSSafeReleaseNULL(buffer);
            return kIOReturnNoMemory;
        }
        OSSafeReleaseNULL(sendMap);
        OSSafeReleaseNULL(send);
        send = buffer;
        sendMap = map;
        capacity = size;
    }
    out[0] = capacity;
    out[1] = sendMap->getAddress();
    out[2] = queueMap->getAddress();
    IOLog("PianoVideoVT V5a: selector0 pid %d capacity %llu shared-send-and-queue 1\n", ownerPid, capacity);
    return kIOReturnSuccess;
}

// Selector3 snapshots a bounded message and its IOSurface IDs, then dispatches
// to the native frontend without waiting for any guest reply.
IOReturn AppleVideoToolboxParavirtualizationUserClient::message(uint64_t length, const uint64_t *surfaces, unsigned count,
                                                               uint64_t *status) {
    IOLog("PianoVideoVT V5c: send-event pid %d bytes %llu capacity %llu surfaces %u decision received\n",ownerPid,length,capacity,count);
    if (!send || length < HeaderBytes || length > capacity || length > MaxSendBytes) {
        droidvm_vtpv_refused = droidvm_vtpv_refused + 1;
        IOLog("PianoVideoVT V5c: send-event decision envelope-rejected status %08x\n",kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
    // The caller controls this shared mapping. Snapshot once and parse only the
    // private copy; a concurrent userspace mutation cannot change checked lengths.
    uint8_t *bytes = static_cast<uint8_t *>(IOMalloc(length));
    if (!bytes) { IOLog("PianoVideoVT V5c: send-event decision snapshot-no-memory status %08x\n",kIOReturnNoMemory);return kIOReturnNoMemory; }
    memcpy(bytes, send->getBytesNoCopy(), length);
    vtwire::Header checked;
    if (!vtwire::header(bytes, uint32_t(length), checked)) {
        IOLog("PianoVideoVT V5c: send-event decision header-rejected status %08x\n",kIOReturnBadArgument);
        IOFree(bytes, length); return kIOReturnBadArgument;
    }
    IOLog("PianoVideoVT V5c: send-event type %08x flags %u request %llu bytes %llu decision snapshot\n",checked.type,checked.flags,checked.request,length);
    if (!pianovideo::vtcontract::message(bytes, uint32_t(length))) {
        IOLog("PianoVideoVT V5c: send-event request %llu decision atom-layout-rejected status %08x\n",checked.request,kIOReturnBadArgument);
        IOFree(bytes, length); return kIOReturnBadArgument;
    }
    bool owned = checked.type == vtwire::cc("decs") || checked.type == vtwire::cc("encs");
    for (unsigned i = 0; i < uuidCount; ++i) if (!memcmp(uuids[i], checked.uuid, 16)) owned = true;
    if (!owned) { IOLog("PianoVideoVT V5c: send-event request %llu decision uuid-rejected status %08x\n",checked.request,kIOReturnNotPermitted);IOFree(bytes, length); return kIOReturnNotPermitted; }
    const uint32_t seq = droidvm_vtpv_messages;
    droidvm_vtpv_record &r = droidvm_vtpv_log[seq % LogEntries];
    r.seq = seq;
    r.pid = uint32_t(ownerPid);
    r.length = uint32_t(length);
    r.type = big32(bytes + 8);
    r.flags = big32(bytes + 12);
    memcpy(r.uuid, bytes + 16, sizeof r.uuid);
    r.request = big64(bytes + 32);
    r.surface_count = 0;
    for (unsigned i = 0; i < count && i < MaxSurfaces && surfaces[i]; ++i) r.surfaces[r.surface_count++] = uint32_t(surfaces[i]);
    memset(r.payload, 0, sizeof r.payload);
    memcpy(r.payload, bytes + HeaderBytes, length - HeaderBytes < sizeof r.payload ? length - HeaderBytes : sizeof r.payload);
    captureMessage(bytes, uint32_t(length), 0);
    droidvm_vtpv_messages = seq + 1;
    IOLog("PianoVideo: vt message %c%c%c%c received count %u\n", char(r.type >> 24), char(r.type >> 16), char(r.type >> 8), char(r.type), droidvm_vtpv_messages);
    uint32_t ids[MaxSurfaces];
    for (unsigned i = 0; i < r.surface_count; ++i) ids[i] = r.surfaces[i];
    const int rc = host ? droidvm_vthost_message(host, bytes, uint32_t(length), ids, r.surface_count) : -1;
    IOLog("PianoVideoVT V5c: send-event request %llu flags %u decision %s status %d\n",checked.request,checked.flags,host?(rc?"host-transport-rejected":"host-dispatched"):"host-closed",rc);
    IOFree(bytes, length);
    *status = uint64_t(int64_t(rc));
    if (rc) droidvm_vtpv_refused = droidvm_vtpv_refused + 1;
    return kIOReturnSuccess;
}

// A message to the guest, into its queue (whose notification wakes the framework's dequeue loop).
bool AppleVideoToolboxParavirtualizationUserClient::sendToGuest(void *transport, const uint8_t *bytes, uint32_t size) {
    auto *self = static_cast<AppleVideoToolboxParavirtualizationUserClient *>(transport);
    captureMessage(bytes, size, 1);
    IOLockLock(self->sendLock);
    const bool sent = self->queue && self->queue->enqueue(const_cast<uint8_t *>(bytes), size);
    IOLockUnlock(self->sendLock);
    vtwire::Header h;const bool valid=vtwire::header(bytes,size,h);
    IOLog("PianoVideoVT V5c: reply-queue type %08x flags %u request %llu bytes %u decision %s\n",valid?h.type:0,valid?h.flags:0,valid?h.request:0,size,sent?"enqueued":"dropped");
    if (sent) droidvm_vtpv_sent = droidvm_vtpv_sent + 1;
    else { droidvm_vtpv_dropped = droidvm_vtpv_dropped + 1; IOLog("PianoVideoVT V5a: transport DROP size %u\n", size); }
    return sent;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                                       IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector < 8) droidvm_vtpv_selectors[selector] = droidvm_vtpv_selectors[selector] + 1;
    IOReturn result = kIOReturnUnsupported;
    IOLockLock(lock);
    switch (selector) {
    case 0:
        if (a->scalarInputCount != 1 || a->scalarOutputCount < 3) { result = kIOReturnBadArgument; break; }
        result = buffers(a->scalarInput[0], a->scalarOutput);
        if (!result) a->scalarOutputCount = 3;
        break;
    case 1: case 2:
        if (a->structureInputSize != 16 || !a->structureInput) { result = kIOReturnBadArgument; break; }
        { unsigned found = uuidCount;
          for (unsigned i = 0; i < uuidCount; ++i) if (!memcmp(uuids[i], a->structureInput, 16)) { found = i; break; }
          if (selector == 1) {
            if (found == uuidCount) {
              if (uuidCount == 32) { result = kIOReturnNoResources; break; }
              memcpy(uuids[uuidCount++], a->structureInput, 16);
              droidvm_vtpv_uuids = droidvm_vtpv_uuids + 1;
            }
          } else {
            if (found == uuidCount) { result = kIOReturnBadArgument; break; }
            memcpy(uuids[found], uuids[--uuidCount], 16);
          }
          result = kIOReturnSuccess;
        }
        break;
    case 3:
        if (a->scalarInputCount < 1 || a->scalarInputCount > 1 + MaxSurfaces || a->scalarOutputCount < 1) {
            IOLog("PianoVideoVT V5c: send-event decision scalar-shape-rejected inputs %u outputs %u status %08x\n",a->scalarInputCount,a->scalarOutputCount,kIOReturnBadArgument);
            result = kIOReturnBadArgument;
            break;
        }
        result = message(a->scalarInput[0], a->scalarInput + 1, a->scalarInputCount - 1, a->scalarOutput);
        if (!result) a->scalarOutputCount = 1;
        break;
    case 4:
        IOLog("PianoVideoVT V5c: reply-queue event drained-ack status 0\n");
        result = kIOReturnSuccess;
        break;
    }
    IOLockUnlock(lock);
    return result;
}

IOReturn AppleVideoToolboxParavirtualizationUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t) {
    droidvm_vtpv_notify_calls = droidvm_vtpv_notify_calls + 1;
    droidvm_vtpv_notify_type = type;
    if (type != 0) { droidvm_vtpv_notify_result = kIOReturnBadArgument; return kIOReturnBadArgument; }
    IOLockLock(lock);
    IOReturn result = kIOReturnNotReady;
    if (!queue) {
        uint64_t out[3];
        result = buffers(FirstSendBytes, out);
    }
    if (queue) {
        queue->setNotificationPort(port);
        result = kIOReturnSuccess;
    }
    IOLockUnlock(lock);
    droidvm_vtpv_notify_result = uint32_t(result);
    IOLog("PianoVideoVT V5a: notification type %u pid %d status %08x\n", type, ownerPid, result);
    return result;
}

#define PIANO_VIDEO_LATE_INIT(file) extern "C" void piano_video_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
PIANO_VIDEO_LATE_INIT(PianoVideoVTParavirt)

// Publishes the service on IOResources, once.
static int publish(void) {
    if (droidvm_vtpv_stage != 1) return droidvm_vtpv_stage == 2 ? 0 : -1;
    if (!droidvm_vthost_available()) return -1;   // no codec to stand for: VideoToolbox keeps its own
    capture = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, IOOptionBits(kIODirectionInOut) |
                                                               IOOptionBits(kIOMemoryPhysicallyContiguous),
                                                               CaptureEntries * CaptureBytes, 0x0000fffffffff000ULL);
    captureLock = IOLockAlloc();
    if (capture) {
        bzero(capture->getBytesNoCopy(), CaptureEntries * CaptureBytes);
        droidvm_vtpv_capture_pa = capture->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    IOService *resources = IOService::getResourceService();
    AppleVideoToolboxParavirtualizationDriver *driver = OSTypeAlloc(AppleVideoToolboxParavirtualizationDriver);
    if (!driver || !resources || !driver->init() || !driver->attach(resources)) {
        OSSafeReleaseNULL(driver);
        return -1;
    }
    if (!driver->start(resources)) {
        driver->detach(resources);
        driver->release();
        return -1;
    }
    driver->release();
    droidvm_vtpv_stage = 2;
    return 0;
}

static IOLock *publishLock;

static int droidvm_vtpv_sysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    int value = int(droidvm_vtpv_stage);
    const int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value != 1) return EINVAL;
    IOLockLock(publishLock);
    const int published = publish();
    IOLockUnlock(publishLock);
    return published ? EIO : 0;
}

static struct sysctl_oid droidvm_vtpv_oid;

// Registers the classes in their own OSMetaClass module load (the static initializer staged for this call, as the other board
// classes are) and the sysctl, and with piano-video-vt=1 publishes the service.
int piano_video_vt_attach(void) {
    if (droidvm_vtpv_stage) return -1;
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) return -1;
    piano_video_late_init_PianoVideoVTParavirt();
    if (OSMetaClass::postModLoad(module)) return -1;
    droidvm_vtpv_stage = 1;
    if (!(publishLock = IOLockAlloc())) return -1;
    droidvm_vtpv_oid.oid_parent = &sysctl__kern_children;
    droidvm_vtpv_oid.oid_number = OID_AUTO;
    // CTLFLAG_OID2: the structure carries oid_descr and oid_version (SYSCTL_OID sets it; arm64 panics without it).
    droidvm_vtpv_oid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    droidvm_vtpv_oid.oid_name = "piano_video_vt";
    droidvm_vtpv_oid.oid_handler = droidvm_vtpv_sysctl;
    droidvm_vtpv_oid.oid_fmt = "I";
    droidvm_vtpv_oid.oid_descr = "VideoToolbox paravirtualization: 1 classes registered, 2 service published; write 1 to publish";
    droidvm_vtpv_oid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&droidvm_vtpv_oid);
    uint32_t enabled = 0;
    PE_parse_boot_argn("piano-video-vt", &enabled, sizeof enabled);
    return enabled ? publish() : 0;
}
