// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's run-time snapshot in the kernel (B1 milestone 1; DpuSnapshot.h is the portable part and its host tests): with the boot
// argument `piano-dpu-snapshot=1` (default 0: nothing registered, nothing mapped), a read-only string sysctl
//   sudo sysctl -n machdep.piano_dpu_snapshot
// reads the display path's registers fresh at the call and returns them as text, one line per block; the same lines go to the
// console ("PianoDpu: snap: ..."), so a run whose ssh capture fails still has them. Loads only: this file has no store at all, and
// its page maps are its own (device memory, uncached), made on first use and kept for the boot (at most kMaxPages pages).
// Root only (EPERM otherwise); at most one fresh read per 100 ms (a quicker call returns the previous text with its serial).
// The gate: the survey's rule as piano-dpu-ahb-vote has it (1, the default at piano-dpu >= 2: GCC's display AHB branch enabled and
// hardware-gated counts as on); a closed gate reads nothing of MDSS, the DPU or DSI.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include "DpuSnapshot.h"
#include "DpuFlip.h"
#include "DpuFlipKernel.h"

using namespace pianodpu;

namespace {

constexpr unsigned kMaxPages = 48;      // GCC, DISPCC, MDSS, the DPU blocks read, both DSI controllers, the TLMM pin: about 30
constexpr unsigned kLineBytes = 480;
constexpr uint64_t kMinIntervalUs = 100000;

struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
IOLock *gLock;
bool gHwGatedAhb = true, gRegistered;
uint64_t gSerial, gLastUs;
char gText[snapshot::kMaxLines * kLineBytes];
size_t gTextLen;

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

uint32_t load(uint64_t pa)
{
  const uintptr_t va = pageVa(pa);
  if (!va) {
    ++gMapFailures;
    return 0xffffffffu;   // the survey's convention: an unmapped word reads all ones (no geometry, no mode)
  }
  return rd32(va + uintptr_t(pa & 0xfff));
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

// The text: lines appended to gText (each also logged).
struct Out {
  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    if (gTextLen + 2 >= sizeof gText) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(gText + gTextLen, sizeof gText - gTextLen - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = size_t(n);
    if (len > sizeof gText - gTextLen - 2) len = sizeof gText - gTextLen - 2;
    IOLog("PianoDpu: snap: %.*s\n", int(len), gText + gTextLen);
    gTextLen += len;
    gText[gTextLen++] = '\n';
    gText[gTextLen] = 0;
  }
};

void refresh()
{
  static snapshot::Snapshot s;   // ~1 KiB, under gLock
  s = snapshot::Snapshot();
  const uint64_t at = nowUs();
  snapshot::take(s, load, gHwGatedAhb);
  gTextLen = 0;
  gText[0] = 0;
  Out out;
  snapshot::format(s, ++gSerial, at, out);
  flip::Stats stats;
  if (flipStatsSnapshot(stats)) stats.formatClean([&](auto... args) { out.line(args...); });
  if (gMapFailures) out.line("snapshot: %u page maps failed so far (those words read as ffffffff)", gMapFailures);
  gLastUs = at;
}

int snapshotSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  if (req->newptr) return EPERM;   // read only
  if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
  if (!gLock) return ENODEV;
  IOLockLock(gLock);
  if (!gSerial || nowUs() - gLastUs >= kMinIntervalUs) refresh();
  const int err = SYSCTL_OUT(req, gText, gTextLen + 1);
  IOLockUnlock(gLock);
  return err;
}

struct sysctl_oid gOid;

}  // namespace

// From PianoDpu::start: registered once for the boot with piano-dpu-snapshot=1 (a kext of the collection is never unloaded).
void dpuSnapshotStart(uint32_t level, uint32_t ahbVote)
{
  uint32_t on = 0;
  if (!PE_parse_boot_argn("piano-dpu-snapshot", &on, sizeof on) || on != 1 || gRegistered) return;
  gHwGatedAhb = ahbVote == 1;
  gLock = IOLockAlloc();
  if (!gLock) {
    IOLog("PianoDpu: snap: no lock: machdep.piano_dpu_snapshot not registered\n");
    return;
  }
  bzero(&gOid, sizeof gOid);
  gOid.oid_parent = &sysctl__machdep_children;
  gOid.oid_number = OID_AUTO;
  gOid.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gOid.oid_name = "piano_dpu_snapshot";
  gOid.oid_handler = snapshotSysctl;
  gOid.oid_fmt = "A";
  gOid.oid_descr = "PianoDpu: the display path's registers read now (root, read only)";
  gOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gOid);
  gRegistered = true;
  IOLog("PianoDpu: snap: piano-dpu-snapshot=1: machdep.piano_dpu_snapshot registered (root, read only, loads only; gate rule %s; "
        "piano-dpu=%u)\n", gHwGatedAhb ? "enabled + hardware-gated display AHB counts as on" : "strict", level);
}
