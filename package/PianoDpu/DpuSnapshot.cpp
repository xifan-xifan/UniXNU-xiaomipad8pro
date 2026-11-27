// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's run-time snapshot in the kernel (B1 milestone 1; DpuSnapshot.h is the portable part and its host tests): with the boot
// argument `piano-dpu-snapshot=1` (default 0: nothing registered, nothing mapped), a read-only string sysctl
//   sudo sysctl -n machdep.piano_dpu_snapshot
// reads the display path's registers fresh at the call and returns them as text, one line per block; the same lines go to the
// console ("PianoDpu: snap: ..."), so a run whose ssh capture fails still has them. The ordinary snapshot uses only loads.
// Its page maps are device memory, made on first use and kept for the boot (at most kMaxPages pages).
// piano-dpu-clkread=1 also registers this sysctl and appends clock and CRM readback. RPMh readback uses the ACTIVE TCS transport
// and is available only when piano-dpu-snapshot is off.
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
#include "DpuClockRead.h"
#include "DpuFlip.h"
#include "DpuFlipKernel.h"

using namespace pianodpu;

namespace {

constexpr unsigned kMaxPages = 48;      // GCC, DISPCC, MDSS, the DPU blocks read, both DSI controllers, the TLMM pin: about 30
constexpr unsigned kLineBytes = 480;
constexpr uint64_t kMinIntervalUs = 100000;
constexpr uint64_t kCmdDb = 0x81c60000, kAppsDrv2 = 0x16520000;

struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures;
IOLock *gLock;
bool gHwGatedAhb = true, gRegistered, gClockRead, gSnapshotOn;
uint64_t gSerial, gLastUs;
char gText[(snapshot::kMaxLines + 10) * kLineBytes];
size_t gTextLen;

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}

IOMemoryMap *mapRange(uint64_t base, uint32_t bytes)
{
  IODeviceMemory *m = IODeviceMemory::withRange(base, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  return map;
}

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (gPageCount == kMaxPages) return 0;
  IOMemoryMap *map = mapRange(base, 0x1000);
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

uint32_t load(uint64_t pa)
{
  const uintptr_t va = pa & 3u ? 0 : pageVa(pa);
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

struct BcmResource {
  const char *name;
  uint32_t addr = 0, unit = 0;
  uint16_t width = 0;
  uint8_t vcd = 0;
  int rc = 0;
};
struct ArcResource {
  const char *name;
  uint32_t addr = 0;
  unsigned count = 0;
  uint16_t levels[Sm8750RpmhArcMaxLevels] = {};
  int rc = 0;
};
BcmResource gBcms[] = {{"MM0"}, {"SH0"}, {"ACV"}, {"MC0"}};
ArcResource gArcs[] = {{"mmcx.lvl"}, {"mxc.lvl"}};
bool gResourcesLoaded;
int gReadTransportError;
IOMemoryMap *gDrvMap;

uint32_t loadWord(void *base, uint32_t offset) { return rd32(uintptr_t(base) + offset); }

int loadResources()
{
  if (gResourcesLoaded) return 0;
  auto *db = static_cast<Sm8750CmdDb *>(IOMallocData(sizeof(Sm8750CmdDb)));
  if (!db) return ENOMEM;
  IOMemoryMap *map = mapRange(kCmdDb, Sm8750CmdDbBytes);
  int rc = ENXIO;
  if (map) {
    const Sm8750RpmhIo io{reinterpret_cast<void *>(uintptr_t(map->getVirtualAddress())), loadWord, nullptr, nullptr};
    rc = Sm8750CmdDbCopy(&io, db);
    map->release();
  }
  if (!rc) {
    for (auto &bcm : gBcms) bcm.rc = Sm8750CmdDbBcm(db, bcm.name, &bcm.addr, &bcm.unit, &bcm.width, &bcm.vcd);
    for (auto &arc : gArcs) arc.rc = Sm8750CmdDbArc(db, arc.name, &arc.addr, arc.levels, &arc.count);
    gResourcesLoaded = true;
  }
  IOFreeData(db, sizeof(Sm8750CmdDb));
  return rc;
}

// Calls share gLock with the snapshot and use the RPMh transport's own DRV-2 lock.
void formatVotes(Out &out)
{
  out.line("clkread_rpmh voter=apps-drv2 aggregate=unavailable");
  const int dbError = loadResources();
  if (!dbError && !gReadTransportError && !gDrvMap) gDrvMap = mapRange(kAppsDrv2, 0x10000);
  const Sm8750RpmhIo io{
      gDrvMap ? reinterpret_cast<void *>(uintptr_t(gDrvMap->getVirtualAddress())) : nullptr, loadWord,
      [](void *base, uint32_t offset, uint32_t word) {
        const uintptr_t at = uintptr_t(base) + offset;
        __asm__ volatile("str %w0, [%1]" :: "r"(word), "r"(at) : "memory");
      },
      [](void *, unsigned us) { if (us >= 1000) IOSleep(us / 1000); else IODelay(us); }};

  int sampleError = gReadTransportError;
  if (!sampleError) sampleError = dbError;
  if (!sampleError && !gDrvMap) sampleError = ENXIO;
  auto read = [&](uint32_t addr, int lookupError, uint32_t &word, Sm8750RpmhResult &result) {
    if (sampleError) return sampleError;
    if (lookupError) return lookupError;
    const int rc = Sm8750RpmhReadActive(&io, addr, &word, &result);
    // A timeout retains its slot. Stop querying for the boot so polling this
    // sysctl cannot strand additional ACTIVE slots. A busy slot can clear.
    if (rc) sampleError = rc;
    if (rc == Sm8750RpmhTimeout || rc == Sm8750RpmhIoError) gReadTransportError = rc;
    return rc;
  };

  for (const auto &bcm : gBcms) {
    uint32_t word = clkread::kUnread;
    Sm8750RpmhResult result{};
    const int rc = read(bcm.addr, bcm.rc, word, result);
    const bool ok = !rc && word != clkread::kUnread;
    out.line("clkread_bcm %s status=%s addr=%08x raw=%08x valid=%u x=%u y=%u unit=%u width=%u vcd=%u rc=%d step=%u cmd_status=%08x",
             bcm.name, ok ? "ok" : "unavailable", bcm.addr, word, unsigned(ok && clkread::bcmValid(word)),
             ok ? clkread::bcmX(word) : 0, ok ? clkread::bcmY(word) : 0, bcm.unit, unsigned(bcm.width), unsigned(bcm.vcd),
             rc, result.step, result.command[0]);
  }
  for (const auto &arc : gArcs) {
    uint32_t word = clkread::kUnread;
    Sm8750RpmhResult result{};
    const int rc = read(arc.addr, arc.rc, word, result);
    const int level = rc ? -1 : clkread::arcLevel(arc.levels, arc.count, word);
    out.line("clkread_arc %s status=%s addr=%08x raw=%08x hlvl=%d vlvl=%d rc=%d step=%u cmd_status=%08x",
             arc.name, level < 0 ? "unavailable" : "ok", arc.addr, word, level < 0 ? -1 : int(word), level,
             rc, result.step, result.command[0]);
  }
}

void refresh()
{
  static snapshot::Snapshot s;   // ~1 KiB, under gLock
  s = snapshot::Snapshot();
  const uint64_t at = nowUs();
  snapshot::take(s, load, gHwGatedAhb, gSnapshotOn);
  const auto clocks = gClockRead ? clkread::capture(s.path.clocks, load) : clkread::Clocks{};
  gTextLen = 0;
  gText[0] = 0;
  Out out;
  snapshot::format(s, ++gSerial, at, out);
  flip::Stats stats;
  flip::CleanQualification clean;
  if (flipStatsSnapshot(stats, clean)) {
    stats.formatClean([&](auto... args) { out.line(args...); });
    if (clean.policy >= 0)
      out.line("clean: m2 policy %d writes %llu latched %llu full %llu to-boot %llu underruns %llu pending %08x serving %u",
               clean.policy, (unsigned long long)stats.cleanPolicyWrites, (unsigned long long)stats.flips,
               (unsigned long long)stats.cleanFull, (unsigned long long)stats.toBoot, (unsigned long long)stats.cleanUnderruns,
               clean.pendingUnderruns, unsigned(clean.serving));
  }
  if (gClockRead) clkread::formatReadback(clocks, gSnapshotOn, out, formatVotes);
  if (gMapFailures) out.line("snapshot: %u page maps failed so far (those words read as ffffffff)", gMapFailures);
  gLastUs = gClockRead ? nowUs() : at;
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

// From PianoDpu::start: registered once for the boot (a kext of the collection is never unloaded).
void dpuSnapshotStart(uint32_t level, uint32_t ahbVote)
{
  if (gRegistered) return;
  uint32_t on = 0, clockRead = 0;
  gSnapshotOn = PE_parse_boot_argn("piano-dpu-snapshot", &on, sizeof on) && on == 1;
  gClockRead = PE_parse_boot_argn("piano-dpu-clkread", &clockRead, sizeof clockRead) && clockRead == 1;
  if (!gSnapshotOn && !gClockRead) return;
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
  IOLog("PianoDpu: snap: %s=1: machdep.piano_dpu_snapshot registered (root, read only, %s; gate rule %s; piano-dpu=%u)\n",
        gSnapshotOn ? "piano-dpu-snapshot" : "piano-dpu-clkread", gClockRead && !gSnapshotOn ? "clock/CRM loads and RPMh queries" : "loads only",
        gHwGatedAhb ? "enabled + hardware-gated display AHB counts as on" : "strict", level);
}
