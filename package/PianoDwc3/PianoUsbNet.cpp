// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbNet: the tablet's end of PianoDwc3's CDC-NCM function (piano-usb-net=1) as an IOEthernetController. macOS gets an
// Ethernet interface whose frames travel in NTB16s over the USB-C cable; the Mac's own NCM driver shows the other end as an
// Ethernet interface (docs/drivers/usb-device-debug.md section 14).
//
// Not built-in, and not the primary interface: PianoNullEthernet stays the built-in en0 whose MAC TimeSync takes at boot (with or
// without a cable); configd names this one (en1, or the next free unit). MAC 02:50:49:41:4e:02, fixed (UsbDescriptors.h).
//
// The controller pattern is PianoNullEthernet's (see UPSTREAM), with an optional family output queue for TX backpressure.
// The data path: outputPacket copies the frame into a queue the USB thread empties into the NTB IN buffer; the USB thread puts the
// datagrams of each NTB OUT into the other queue, this file's thread turns them into mbufs. Only counts are logged.
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOWorkLoop.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5 (outputStart ... networkInterfaceNotification)
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOBasicOutputQueue.h>
#pragma clang diagnostic pop
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/kpi_mbuf.h>
#include "UsbNet.h"
#include "UsbDescriptors.h"

using namespace pianousb;

namespace {
constexpr uint32_t kQueueBytes = 64 * 1024;   // each way: about 40 full frames
constexpr uint32_t kMinWireFrame = 60;        // shorter frames go out zero-padded, as a NIC sends them
constexpr UInt32 kOutputPackets = 1024;      // pending mbufs in IONetworkingFamily

IOLock *gNetLock;
ncm::FrameRing gToHost, gFromHost;
ncm::TxGate gTxGate;            // under gNetLock
const ncm::NetStats *gGadget;
pianonet::Counters gCount;
bool gTxFlow;                  // immutable after init
bool gLinkUp, gLinkChanged;     // under gNetLock
uint32_t gLinkBps;
bool gEnabled;                  // written under gNetLock, read atomically outside it
uint8_t *gFrame;                // the receive thread's frame buffer

uint64_t nowNs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}
}  // namespace

// Use the k27 family's exported implementations directly. The build headers need not match the queue's virtual slots.
bool oqStart(IOBasicOutputQueue *) __asm("__ZN18IOBasicOutputQueue5startEv");
bool oqStop(IOBasicOutputQueue *) __asm("__ZN18IOBasicOutputQueue4stopEv");
bool oqService(IOBasicOutputQueue *, IOOptionBits) __asm("__ZN18IOBasicOutputQueue7serviceEj");
UInt32 oqFlush(IOBasicOutputQueue *) __asm("__ZN18IOBasicOutputQueue5flushEv");
bool oqSetCapacity(IOBasicOutputQueue *, UInt32) __asm("__ZN18IOBasicOutputQueue11setCapacityEj");
UInt32 oqDropCount(IOBasicOutputQueue *) __asm("__ZN18IOBasicOutputQueue12getDropCountEv");

class PianoUsbNet : public IOEthernetController {
  OSDeclareDefaultStructors(PianoUsbNet)

  IOEthernetInterface *fInterface = nullptr;
  IOWorkLoop *fWorkLoop = nullptr;
  IONetworkMedium *fMedium = nullptr;
  IOBasicOutputQueue *fQueue = nullptr;   // retained by IONetworkController

  UInt32 sendFrame(mbuf_t packet, bool flow);
  UInt32 outputQueuedPacket(mbuf_t packet, void *) { return sendFrame(packet, true); }
  void startTxQueue();
  void resetTxQueue();
  static IOReturn serviceTxAction(OSObject *, void *, void *, void *, void *);

protected:
  bool createWorkLoop() override
  {
    fWorkLoop = IOWorkLoop::workLoop();
    return fWorkLoop != nullptr;
  }
  IOOutputQueue *createOutputQueue() override
  {
    if (gTxFlow) {
      // Only this handler may return the retry status. A direct outputPacket call consumes the mbuf as before.
      fQueue = IOBasicOutputQueue::withTarget(this, static_cast<IOOutputAction>(&PianoUsbNet::outputQueuedPacket), 0);
    }
    return fQueue;
  }

public:
  bool start(IOService *provider) override;
  IOWorkLoop *getWorkLoop() const override { return fWorkLoop; }
  IOReturn getHardwareAddress(IOEthernetAddress *address) override
  {
    for (unsigned i = 0; i < 6; ++i) address->bytes[i] = kNetDeviceMac[i];
    return kIOReturnSuccess;
  }
  IOReturn enable(IONetworkInterface *) override
  {
    IOLockLock(gNetLock);
    __atomic_store_n(&gEnabled, true, __ATOMIC_RELAXED);
    IOLockUnlock(gNetLock);
    startTxQueue();
    return kIOReturnSuccess;
  }
  IOReturn disable(IONetworkInterface *) override
  {
    IOLockLock(gNetLock);
    __atomic_store_n(&gEnabled, false, __ATOMIC_RELAXED);
    gToHost.clear();
    gFromHost.clear();
    IOLockUnlock(gNetLock);
    resetTxQueue();
    return kIOReturnSuccess;
  }
  // The thread that starts us waited for IOBSD already; the family's own wait would only cost the same check again.
  UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
  UInt32 outputPacket(mbuf_t packet, void *) override { return sendFrame(packet, false); }
  IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }   // a point-to-point link: the Mac sends what is ours
  IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
  IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
  const OSString *newVendorString() const override { return OSString::withCString("UniXNU"); }
  const OSString *newModelString() const override { return OSString::withCString("piano USB network (CDC-NCM)"); }

  void applyLink(bool up, uint32_t bps)
  {
    setLinkStatus(up ? kIONetworkLinkValid | kIONetworkLinkActive : kIONetworkLinkValid, up ? fMedium : nullptr, up ? bps : 0);
  }
  void deliver(const uint8_t *frame, uint32_t length);
  void serviceTx() { fWorkLoop->runAction(&PianoUsbNet::serviceTxAction, this); }
  UInt32 queueDrops() const { return fQueue ? oqDropCount(fQueue) : 0; }
  IOEthernetInterface *interface() const { return fInterface; }
};

OSDefineMetaClassAndStructors(PianoUsbNet, IOEthernetController)

bool PianoUsbNet::start(IOService *provider)
{
  if (!IOEthernetController::start(provider)) return false;
  if (gTxFlow && (!fQueue || getOutputQueue() != fQueue)) {
    IOLog("PianoDwc3: net: cannot install the TX flow-control queue\n");
    return false;
  }
  OSDictionary *media = OSDictionary::withCapacity(1);
  fMedium = IONetworkMedium::medium(IOMediumType(kIOMediumEthernetAuto) | IOMediumType(kIOMediumOptionFullDuplex), 480 * 1000000ULL);
  if (!media || !fMedium || !IONetworkMedium::addMedium(media, fMedium) || !publishMediumDictionary(media)) {
    OSSafeReleaseNULL(media);
    IOLog("PianoDwc3: net: cannot publish the medium\n");
    return false;
  }
  media->release();
  setSelectedMedium(fMedium);
  setLinkStatus(kIONetworkLinkValid, nullptr);   // no Mac yet: the USB side reports the link
  if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&fInterface), false)) {
    IOLog("PianoDwc3: net: cannot attach the interface\n");
    return false;
  }
  fInterface->setProperty(kIOBuiltin, false);
  fInterface->setProperty(kIOPrimaryInterface, false);
  fInterface->registerService();
  IOLog("PianoDwc3: net: piano-usb-net-txflow=%u\n", unsigned(gTxFlow));
  return true;
}

// Queue lifecycle and restart run under the controller work loop, as do enable and disable. No family queue call holds gNetLock.
void PianoUsbNet::startTxQueue()
{
  if (!fQueue) return;
  // Start at zero capacity after creation or a flush so start() cannot drain on this thread.
  oqStart(fQueue);
  oqSetCapacity(fQueue, kOutputPackets);
}

void PianoUsbNet::resetTxQueue()
{
  if (fQueue) {
    oqSetCapacity(fQueue, 0);
    oqStop(fQueue);
    oqFlush(fQueue);
  }
  IOLockLock(gNetLock);
  gTxGate.resetDone();
  IOLockUnlock(gNetLock);
}

IOReturn PianoUsbNet::serviceTxAction(OSObject *owner, void *, void *, void *, void *)
{
  auto *net = static_cast<PianoUsbNet *>(owner);
  IOLockLock(gNetLock);
  const bool reset = gTxGate.needsReset(), restart = gTxGate.takeRestart(), enabled = gEnabled;
  if (restart && !reset && enabled) ++gCount.txRestarts;
  IOLockUnlock(gNetLock);
  if (reset) {
    // A down/up pair can arrive before the net thread sees it. Flush the old mbufs before reopening TX.
    net->resetTxQueue();
    if (enabled) net->startTxQueue();
  } else if (restart && enabled) {
    // Let the family drain TX on its callout thread so this thread can keep delivering RX.
    // Service still invalidates a stall from an in-flight callback.
    oqService(net->fQueue, IOBasicOutputQueue::kServiceAsync);
  }
  return kIOReturnSuccess;
}

UInt32 PianoUsbNet::sendFrame(mbuf_t packet, bool flow)
{
  using Result = ncm::TxGate::Result;
  const size_t length = mbuf_pkthdr_len(packet);
  IOLockLock(gNetLock);
  const Result result = gTxGate.offer(gToHost, length, gLinkUp && gEnabled, flow,
      [packet](uint8_t *d, uint32_t off, uint32_t n) { return mbuf_copydata(packet, off, n, d) == 0; });
  switch (result) {
  case Result::badLength:
    ++gCount.sendRefused;
    break;
  case Result::queued:
    ++gCount.sent;
    gCount.sentBytes += length;
    break;
  case Result::drop:
    ++gCount.sendDrops;
    break;
  case Result::stall:
    ++gCount.txStalls;
    break;
  }
  IOLockUnlock(gNetLock);
  if (result == Result::stall) {
    pianonet::usbKick();
    return kIOReturnOutputStall;   // the family keeps the mbuf at the head of its queue
  }
  freePacket(packet);
  if (result != Result::queued) return kIOReturnOutputDropped;
  pianonet::usbKick();
  return kIOReturnOutputSuccess;
}

// The receive thread only (inputPacket calls must be serialized).
void PianoUsbNet::deliver(const uint8_t *frame, uint32_t length)
{
  if (!fInterface || !__atomic_load_n(&gEnabled, __ATOMIC_RELAXED)) { ++gCount.notUp; return; }
  mbuf_t m = allocatePacket(length);
  if (!m || mbuf_copyback(m, 0, length, frame, MBUF_DONTWAIT) != 0) {
    if (m) freePacket(m);
    ++gCount.receiveDrops;
    return;
  }
  fInterface->inputPacket(m, length);
  ++gCount.received;
  gCount.receivedBytes += length;
}

namespace {
PianoUsbNet *gNet;

PianoUsbNet *publish(IOService *owner)
{
  PianoUsbNet *net = OSTypeAlloc(PianoUsbNet);
  if (!net || !net->init(nullptr)) {
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: init failed\n");
    return nullptr;
  }
  if (!net->attach(owner)) {
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: attach failed\n");
    return nullptr;
  }
  if (!net->start(owner)) {
    net->detach(owner);
    OSSafeReleaseNULL(net);
    IOLog("PianoDwc3: net: the controller: start failed\n");
    return nullptr;
  }
  const uint8_t *m = kNetDeviceMac;
  IOLog("PianoDwc3: net: interface published (MAC %02x:%02x:%02x:%02x:%02x:%02x, not built-in; configd names it), registry ID %#llx\n",
        m[0], m[1], m[2], m[3], m[4], m[5], net->getRegistryEntryID());
  return net;
}

const char *bsdName()
{
  IOEthernetInterface *i = gNet ? gNet->interface() : nullptr;
  OSString *name = i ? OSDynamicCast(OSString, i->getProperty(kIOBSDNameKey)) : nullptr;
  return name ? name->getCStringNoCopy() : nullptr;
}

void counts(char *line, unsigned size)
{
  const ncm::NetStats z = {}, &g = gGadget ? *gGadget : z;
  snprintf(line, size, "link %s, %s %s; to the Mac %llu frames %llu bytes (%u NTBs), from the Mac %llu frames %llu bytes (%u NTBs); "
           "NTB errors %u%s%s, bad datagrams %u; drops: send %llu (+%llu bad length), receive %u (queue) %llu (no mbuf) %llu (not up); "
           "TX stalls %llu, restarts %llu, queue drops %u",
           g.alt == 1 ? "up" : "down", bsdName() ? bsdName() : "(no name yet)",
           __atomic_load_n(&gEnabled, __ATOMIC_RELAXED) ? "up" : "down", gCount.sent, gCount.sentBytes,
           g.txNtbs, gCount.received, gCount.receivedBytes, g.rxNtbs, g.ntbErrors, g.lastError ? ", last: " : "",
           g.lastError ? g.lastError : "", g.badDatagrams, gCount.sendDrops, gCount.sendRefused, g.rxDrops, gCount.receiveDrops, gCount.notUp,
           gCount.txStalls, gCount.txRestarts, gNet ? gNet->queueDrops() : 0);
}

// Publishes the controller once IOBSD is up (the family's interface needs BSD), then: link changes and received frames to macOS,
// one line of counts at most every 10 s while they change.
void netThread(void *arg, wait_result_t)
{
  IOService *owner = static_cast<IOService *>(arg);
  if (OSDictionary *matching = IOService::resourceMatching(kIOBSDKey)) {
    if (IOService *bsd = IOService::waitForMatchingService(matching)) bsd->release();
    matching->release();
  }
  gNet = publish(owner);
  bool named = false, first = true;
  uint64_t reported = 0, last[3] = {};
  for (;;) {
    IOLockLock(gNetLock);
    if (!gFromHost.used() && !gLinkChanged && !(gNet && gTxGate.needsService()) && !first) {
      uint64_t deadline = 0;
      clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gNetLock, &gFromHost, deadline, THREAD_UNINT);
    }
    const bool changed = gLinkChanged || first, up = gLinkUp;
    const bool serviceTx = gTxGate.needsService();
    const uint32_t bps = gLinkBps;
    gLinkChanged = first = false;
    IOLockUnlock(gNetLock);
    if (serviceTx && gNet) gNet->serviceTx();
    if (changed && gNet) {
      gNet->applyLink(up, bps);
      IOLog("PianoDwc3: net: link %s%s\n", up ? "up: the Mac runs the NCM data interface" : "down", up ? (bps > 100000000 ? " (480 Mb/s)" : " (12 Mb/s)") : "");
    }
    unsigned taken = 0;
    for (unsigned k = 0; k < 256; ++k, ++taken) {   // a batch, then the link again
      IOLockLock(gNetLock);
      const uint32_t n = gFromHost.get(gFrame, ncm::kMaxFrame);
      IOLockUnlock(gNetLock);
      if (!n) break;
      if (gNet) gNet->deliver(gFrame, n);
      else ++gCount.notUp;
    }
    if (taken) pianonet::usbKick();   // room for the next NTB from the Mac
    if (!named && bsdName()) {
      named = true;
      IOLog("PianoDwc3: net: macOS named the interface %s (en0 is the built-in one: PianoNullEthernet's, or Wi-Fi's with piano-wifi-en0)\n", bsdName());
    }
    const uint64_t now = nowNs(), sum[3] = {gCount.sent + gCount.sendDrops, gCount.received + gCount.receiveDrops + gCount.notUp,
                                            gGadget ? uint64_t(gGadget->ntbErrors) + gGadget->badDatagrams + gGadget->rxDrops : 0};
    if ((sum[0] != last[0] || sum[1] != last[1] || sum[2] != last[2]) && now - reported > 10000000000ull) {
      reported = now;
      for (unsigned i = 0; i < 3; ++i) last[i] = sum[i];
      char line[512];
      counts(line, sizeof line);
      IOLog("PianoDwc3: net: %s\n", line);
    }
  }
}
}  // namespace

namespace pianonet {

bool init(const ncm::NetStats *gadget, bool txFlow)
{
  gGadget = gadget;
  gTxFlow = txFlow;
  gNetLock = IOLockAlloc();
  gToHost.buf = static_cast<uint8_t *>(IOMallocData(kQueueBytes));
  gFromHost.buf = static_cast<uint8_t *>(IOMallocData(kQueueBytes));
  gFrame = static_cast<uint8_t *>(IOMallocData(ncm::kMaxFrame));
  gToHost.size = gFromHost.size = kQueueBytes;
  return gNetLock && gToHost.buf && gFromHost.buf && gFrame;
}

bool startThread(IOService *owner)
{
  thread_t thread = nullptr;
  if (kernel_thread_start(netThread, owner, &thread) != KERN_SUCCESS) return false;
  thread_deallocate(thread);
  return true;
}

unsigned txNext(uint8_t *dst, unsigned room)
{
  IOLockLock(gNetLock);
  const uint32_t length = gToHost.peek(), wire = length < kMinWireFrame ? kMinWireFrame : length;
  const uint32_t n = length && wire <= room ? gToHost.get(dst, length) : 0;
  if (n && gTxGate.afterDrain(gToHost)) IOLockWakeup(gNetLock, &gFromHost, false);
  IOLockUnlock(gNetLock);
  if (!n) return 0;
  for (uint32_t i = n; i < wire; ++i) dst[i] = 0;
  return wire;
}

unsigned rxSpace()
{
  IOLockLock(gNetLock);
  const unsigned n = gFromHost.space();
  IOLockUnlock(gNetLock);
  return n;
}

bool rxFrame(const uint8_t *src, unsigned length)
{
  IOLockLock(gNetLock);
  const bool ok = gLinkUp && gEnabled && gFromHost.put(src, length);
  IOLockUnlock(gNetLock);
  return ok;
}

void rxDone()
{
  IOLockLock(gNetLock);
  IOLockWakeup(gNetLock, &gFromHost, false);
  IOLockUnlock(gNetLock);
}

void link(bool up, uint32_t bitsPerSecond)
{
  IOLockLock(gNetLock);
  gLinkUp = up;
  gLinkBps = bitsPerSecond;
  gLinkChanged = true;
  if (!up) {
    gToHost.clear();   // nothing stale for the next session
    gFromHost.clear();
    if (gTxFlow) gTxGate.requestReset();
  }
  IOLockWakeup(gNetLock, &gFromHost, false);
  IOLockUnlock(gNetLock);
}

void status(char *line, unsigned size) { counts(line, size); }

}  // namespace pianonet
