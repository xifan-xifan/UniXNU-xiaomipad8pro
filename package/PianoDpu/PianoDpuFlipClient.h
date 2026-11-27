// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PianoDpu service's flip interface (B1b, docs/drivers/dpu.md section 4.1), header only: no link dependency. The caller finds the
// service by its class name (IOService::serviceMatching("PianoDpu"), registered only with piano-dpu=2; flips served with piano-dpu=2) and calls
//   service->callPlatformFunction("PianoDpuFlip", false, request, handoff, nullptr, nullptr)
// with a PianoDpuFlipRequest. It returns kIOReturnSuccess when the request was understood; the outcome is request->result:
//   surface != nullptr   scan that IOSurface out directly: its pages are mapped into an IOVA window of the DPU's SMMU bank (once per
//                        surface), the CPU caches over the rows the DPU fetches are cleaned, the pipes that scan the boot framebuffer
//                        are pointed at it with one CTL flush, and the call returns once the hardware latched it (at the next frame,
//                        < 8.4 ms at 120 Hz; at most piano-dpu-latch ms). kPianoDpuFlipShown: on the panel now, the previous surface
//                        free. kPianoDpuFlipPaused asks the caller to keep the frame queued for wake. Other refusals use the boot
//                        framebuffer fallback (the caller copies the frame there and asks for it back with surface = nullptr).
//   surface == nullptr   back to the boot framebuffer (the caller has just copied a frame there): returns once latched, at once when it
//                        is on the panel already. kPianoDpuFlipShown.
// A failure on the hardware side (an SMMU fault, a missed latch, an underrun) puts the boot framebuffer back on the panel and the
// translation back as found, and every later request is refused (kPianoDpuFlipOff): the copy for the rest of the boot.
// Blocking (a latch wait): call it from a thread, never with a command gate or a spinlock held, never from an interrupt.
// NativeGopDisplay calls it from its copy thread, after the GPU's fence wait (native-gop-display-flip=1).
#ifndef PIANO_DPU_FLIP_CLIENT_H
#define PIANO_DPU_FLIP_CLIENT_H
#include <stdint.h>

#define kPianoDpuServiceClass "PianoDpu"
#define kPianoDpuFlipFunction "PianoDpuFlip"
#define kPianoDpuFlipVersion 1u

// Kernel-only readout for NativeGopDisplay. The callback runs outside the flip lock while the window's
// surface, wiring and CPU map are held. A null surface selects the boot framebuffer. kIOReturnBusy
// means the read must be discarded (changing or indeterminate scanout).
#define kPianoDpuScanoutFunction "PianoDpuScanout"
class OSObject;
class IOMemoryMap;
struct PianoDpuScanoutRequest {
  int (*read)(void *context, const OSObject *surface, IOMemoryMap *map, uint64_t offset, uint32_t stride);
  void *context;
  int error;                  // callback result
};
// Optional second argument to PianoDpuScanout for the boot-gated snapshot diagnostic.
// A sample is available only when the corresponding try-lock succeeded.
enum class PianoDpuScanoutBusy : uint8_t { none, initialLock, finalLock, changed };
struct PianoDpuScanoutSample {
  bool available = false;
  bool surface = false;
  uint32_t base = 0, stride = 0, state = 0;
  uint64_t flips = 0, reshows = 0, toBoot = 0;
};
struct PianoDpuScanoutDiagnostics {
  PianoDpuScanoutBusy busy = PianoDpuScanoutBusy::none;
  PianoDpuScanoutSample before, after;
};
// Optional pointer-sized value in handoff. Legacy callers pass nullptr. Only pass this at a producer handoff with known completion.
constexpr uintptr_t kPianoDpuFlipProducerComplete = 1;

enum PianoDpuFlipResult : uint32_t {
  kPianoDpuFlipShown = 0,     // latched: on the panel
  kPianoDpuFlipOff,           // not serving: piano-dpu-flip below 4, not ready yet, or off for the boot after a failure
  kPianoDpuFlipFormat,        // not 'BGRA' (8-bit): 'RGhA' half float, 'b3a8' and the rest are copied
  kPianoDpuFlipPlanes,        // a planar surface
  kPianoDpuFlipSize,          // smaller than the panel, or larger than a 32 MiB window
  kPianoDpuFlipStride,        // row bytes above 0xffff, below the panel's 12800, or not a multiple of 64
  kPianoDpuFlipAlign,         // the pixels' offset in the memory is not a multiple of 64
  kPianoDpuFlipMemory,        // the memory could not be prepared, listed (4 KiB pages inside XNU's RAM) or mapped
  kPianoDpuFlipSlots,         // no window free (all are on screen or pending)
  kPianoDpuFlipClean,         // required pixel clean failed
  kPianoDpuFlipLatch,         // the hardware did not latch: reverted, off for the boot
  kPianoDpuFlipFault,         // an SMMU fault: reverted, off for the boot
  kPianoDpuFlipUnderrun,      // an underrun: reverted, off for the boot
  kPianoDpuFlipVersionMismatch,
  kPianoDpuFlipPaused,        // panel asleep or unsafe: keep the request and retry after wake
  kPianoDpuFlipResults
};

struct PianoDpuFlipRequest {
  uint32_t size;              // sizeof(PianoDpuFlipRequest)
  uint32_t version;           // kPianoDpuFlipVersion
  // in (surface == nullptr: back to the boot framebuffer, the rest ignored)
  void *surface;              // the IOSurface (an OSObject): the window cache's key; retained while it has a window
  void *memory;               // its IOMemoryDescriptor (retained and prepared while it has a window)
  uint64_t offset;            // the pixels' offset in that memory (IOSurface base offset)
  uint64_t rowBytes, width, height;
  uint32_t format;            // the IOSurface pixel format, e.g. 'BGRA'
  uint32_t planes;            // its plane count (0 or 1 for 'BGRA')
  uint32_t copyUs;            // the caller's mean copy time so far (0: unknown): what a flip saves, for PianoDpu's log
  // out
  uint32_t result;            // PianoDpuFlipResult
  uint32_t latchUs;           // flush to latch
  uint32_t workUs;            // the CPU time of the flip itself (map, clean, program)
};

inline const char *pianoDpuFlipResultName(uint32_t r)
{
  static const char *const names[kPianoDpuFlipResults] = {"shown", "off", "format", "planes", "size", "stride", "align", "memory",
                                                           "slots", "clean", "latch", "fault", "underrun", "version", "paused"};
  return r < kPianoDpuFlipResults ? names[r] : "?";
}

// How a surface's memory is prepared (wired) by a driver that is not its only holder: PianoDpu for a window, NativeGopDisplay's copy
// for its kept map. XNU's IOGeneralMemoryDescriptor remembers a FIRST prepare of kIODirectionOut as "prepared read only" until the last
// complete, and refuses every later prepare that may write (kIODirectionNone: the memory's own direction, InOut for an IOSurface) with
// kIOReturnNotWritable and an "IOMemoryDescriptor ... prepared read only" report. So a read-only first holder locks the others out:
// 2026-11-15 (zcflip1, zcflip-nc1) PianoDpu's windows prepared WindowServer's three frames kIODirectionOut at the first flips, kept
// them for reuse, and every later copy of a frame failed to map (black for the rest of the boot). The rule: prepare with the memory's
// own direction; read-only only when that is refused (someone else made it read-only, or it cannot be written). prepare(readOnly)
// returns true on success. pianoPrepareNone: not prepared (nothing to complete).
enum PianoPrepared : uint8_t { pianoPrepareNone = 0, pianoPrepareShared, pianoPrepareReadOnly };
template <class Prepare>
inline PianoPrepared pianoPrepareSurface(Prepare prepare)
{
  if (prepare(false)) return pianoPrepareShared;
  if (prepare(true)) return pianoPrepareReadOnly;
  return pianoPrepareNone;
}

#endif
