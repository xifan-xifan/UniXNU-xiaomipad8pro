// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's writing slices in the kernel: B1c refresh (DpuRefresh.h), B1d Night Light (DpuPcc.h), B1e panel sleep (DpuPanel.h);
// docs/drivers/dpu.md §4.3-4.5 (design), §6 (arguments), §8 (test images and expected lines). Each slice is inert without its own
// argument, and all of them need piano-dpu >= 1: the read-only survey (PianoDpu.cpp) runs first and is waited for, so every boot
// that writes also logs what it found. One thread (this file's worker) does every store, one sequence at a time; the platform
// functions below only record a request and wake it.
//   piano-dpu-refresh=plan|60|120|auto   plan: the verdict and the stores a switch would make, nothing written; 60 / 120: switch
//                                        once at the start and stay; auto: the idle policy (DpuRefresh.h) fed by NativeGopDisplay's
//                                        swaps (it calls PianoDpuSwap when it has the same argument)
//   piano-dpu-modes=1|120|144            M3 fixed 60/120 Hz or the 144 Hz bandwidth qualification profile
//   piano-dpu-refresh-db=0|1|2           the DSI timing shadow: 1 SunUEFI's place (default), 2 downstream's, 0 none (TOTAL directly)
//   piano-dpu-refresh-test=N             first N round trips 120 -> 60 -> 120, one second at each rate (stops at an anomaly)
//   piano-dpu-refresh-idle=MS, -burst=N, -window=MS, -dwell=MS   the policy (1000, 2, 250, 200)
//   piano-dpu-pcc=1|2                    1: the PCC as found (the identity, normally) written once at the start, then macOS's colour
//                                        matrices (NativeGopDisplay forwards swap_set_color_matrix); 2: also its gamma tables, fitted
//   piano-dpu-pcc-encode=0               a diagonal as macOS sends it (default: from linear light to the encoded values)
//   piano-dpu-pcc-negative=1             negative coefficients as two's complement S3.15 (default: clamped to 0)
//   piano-dpu-pcc-test=R,G,B             a diagonal in thousandths written as the PCC's values after the start (e.g. 1000,1000,500)
//   native-gop-display-night-shift=1      the location-9 matrix and correction through DSPP0/1
//   piano-dpu-panel=1                    the panel's light sleep with display sleep (NativeGopDisplay calls PianoDpuPanel)
//   piano-dpu-panel-engines=0            DCS only: the timing engines keep running
//   piano-dpu-panel-hs=1                 DCS in high speed (default: low power, the panel's MIPI_DSI_MODE_LPM)
//   piano-dpu-panel-test=N               standalone level-1 DCS test, 5 s asleep and 5 s awake each
// Platform functions (IOService::callPlatformFunction on the PianoDpu service, registered when a slice is on; no link dependency):
//   "PianoDpuSwap"          no parameters: a frame was shown
//   "PianoDpuMode"          p1 = configured fixed rate, p2 = optional unsigned * for the verified rate
//                           returns Aborted for a verified restore or Offline if recovery failed
//   "PianoDpuColorMatrix"   p1 = the 72 bytes of an IOMFBColorFixedMatrix (9 uint64_t), p2 = its IOMFBColorMatrixFunction, p3 = the
//                           call's third argument (logged)
//   "PianoDpuGammaTable"    p1 = the table's 32-bit words, p2 = how many (fitted when piano-dpu-pcc=2 and 771 words)
//   "PianoDpuPanel"         p1 = on (0 / 1): with piano-dpu-panel=1 synchronous, from NativeGopDisplay's copy thread (M5a: display
//                           sleep, then the PCC / Night Shift replay inside the wake); Night Shift alone: a notify (M4)
//   "PianoDpuNightShiftMatrix"     p1 = nine S31.32 matrix words copied synchronously
//   "PianoDpuNightShiftCorrection" p1 = the unsigned Q16.16 gain
//   "PianoDpuNightShiftModeSet"    replay the colour after a display mode change
// Display stores go through DpuSlices.h's guard. The 144 Hz MM0 vote uses the shared RPMh transaction on its own mapping.
// Nothing is mapped while interrupts are off.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include "DpuRefresh.h"
#include "DpuPcc.h"
#include "DpuNightShift.h"
#include "DpuPanel.h"
#include "DpuBandwidth.h"
#include "DpuModeSwitch.h"
#include "DpuFlipKernel.h"

using namespace pianodpu;
using namespace pianodpu::slices;

extern "C" {
extern volatile int32_t piano_dpu_stage;          // PianoDpu.cpp's survey: 5 when done (B1b's steps 6..8 follow), negative when failed
volatile int32_t piano_dpu_slices_stage = 0;      // this file's breadcrumb: the step reached, negative when it failed there
}

// The interface PianoDpu.cpp calls (declared there).
bool dpuSlicesWanted();
void dpuSlicesStart(IOService *dpu);
bool dpuSlicesPlatformFunction(const OSSymbol *name, void *p1, void *p2, void *p3, IOReturn *result);

namespace {

constexpr unsigned kRefreshOff = 0, kRefreshAuto = 1, kRefreshPlan = 2;

struct SliceArgs {
  unsigned level = 0;
  unsigned refresh = kRefreshOff, refreshDb = 1, refreshTest = 0;
  unsigned modeHz = 0;
  refresh::PolicyConfig policy;
  unsigned pcc = 0, pccEncode = 1, pccNegative = 0;
  bool nightShift = false;
  bool pccTest = false;
  int32_t pccTestMilli[3] = {1000, 1000, 1000};
  unsigned panel = 0, panelEngines = 1, panelHs = 0, panelTest = 0;
  bool panelTrace = false;
  unsigned ahbVote = 0;   // piano-dpu-ahb-vote: 1 = the gate's Linux rule for GCC's display AHB (DpuAhbVote.h)
};
SliceArgs gA;
// The M5 trace ring, allocated once at the argument read when piano-dpu-panel=1: the 2048-row ring is ~131 KB, and in __DATA PianoDpu
// no longer fits the kext collection's native-kext data gaps (the y12o KC link: no room for a 0x432b0-byte __DATA, largest gap 0x29f9c).
panel::TraceRing *gPanelTrace = nullptr;
bool gArgsRead;

bool modesEnabled() { return gA.modeHz && !gA.refresh && gA.refreshDb; }

uint32_t argNumber(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v < lo ? lo : v > hi ? hi : v;
}

void readSliceArgs()
{
  if (gArgsRead) return;
  gA.level = argNumber("piano-dpu", 0, 0, 100);
  char s[32] = {};
  if (PE_parse_boot_arg_str("piano-dpu-refresh", s, sizeof s)) {
    if (!strcmp(s, "auto")) gA.refresh = kRefreshAuto;
    else if (!strcmp(s, "plan")) gA.refresh = kRefreshPlan;
    else if (!strcmp(s, "60")) gA.refresh = 60;
    else if (!strcmp(s, "120")) gA.refresh = 120;
  }
  gA.refreshDb = argNumber("piano-dpu-refresh-db", 1, 0, 2);
  gA.modeHz = modes::bootModeHz(argNumber("piano-dpu-modes", 0, 0, UINT32_MAX));
  gA.refreshTest = argNumber("piano-dpu-refresh-test", 0, 0, 1000);
  gA.policy.idleMs = argNumber("piano-dpu-refresh-idle", 1000, 100, 60000);
  gA.policy.burstSwaps = argNumber("piano-dpu-refresh-burst", 2, 1, 8);
  gA.policy.burstWindowMs = argNumber("piano-dpu-refresh-window", 250, 20, 5000);
  gA.policy.dwellMs = argNumber("piano-dpu-refresh-dwell", 200, 50, 60000);
  gA.pcc = argNumber("piano-dpu-pcc", 0, 0, 2);
  uint32_t nightShift = 0;
  PE_parse_boot_argn("native-gop-display-night-shift", &nightShift, sizeof nightShift);
  gA.nightShift = nightShift == 1;
  if (gA.nightShift) gA.pcc = 0;   // the two experiments share one PCC
  gA.ahbVote = argNumber("piano-dpu-ahb-vote", (gA.nightShift || modesEnabled()) && gA.level >= 2 ? 1 : 0, 0, 2);
  gA.pccEncode = argNumber("piano-dpu-pcc-encode", 1, 0, 1);
  gA.pccNegative = argNumber("piano-dpu-pcc-negative", 0, 0, 1);
  memset(s, 0, sizeof s);
  if (PE_parse_boot_arg_str("piano-dpu-pcc-test", s, sizeof s)) {
    unsigned k = 0, v = 0;
    bool digit = false, ok = true;
    for (const char *c = s;; ++c) {
      if (*c >= '0' && *c <= '9') {
        v = v * 10 + unsigned(*c - '0');
        digit = true;
        ok &= v <= 4000;
      } else if (*c == ',' || !*c) {
        ok &= digit && k < 3;
        if (ok) gA.pccTestMilli[k++] = int32_t(v);
        v = 0;
        digit = false;
        if (!*c) break;
      } else {
        ok = false;
        break;
      }
    }
    gA.pccTest = ok && k == 3;
  }
  const unsigned panelArg = argNumber("piano-dpu-panel", 0, 0, UINT32_MAX);
  gA.panel = panelArg != 0;
  gA.panelTrace = panelArg == 1;
  if (gA.panelTrace && !gPanelTrace) {   // zero-filled = an empty ring (total 0)
    gPanelTrace = static_cast<panel::TraceRing *>(IOMallocZero(sizeof(panel::TraceRing)));
    if (!gPanelTrace) {
      gA.panelTrace = false;
      IOLog("PianoDpu: m5: the trace ring (%lu bytes) could not be allocated: tracing off\n", (unsigned long)sizeof(panel::TraceRing));
    }
  }
  gA.panelEngines = argNumber("piano-dpu-panel-engines", 1, 0, 1);
  gA.panelHs = argNumber("piano-dpu-panel-hs", 0, 0, 1);
  gA.panelTest = argNumber("piano-dpu-panel-test", 0, 0, 1000);
  gArgsRead = true;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDpu: %s\n", line);
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}
uint64_t nowMs() { return nowUs() / 1000; }

// --------------------------------------------------------------------------------------- MMIO: this file's own page maps
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n dsb sy" ::"r"(v), "r"(a) : "memory"); }

struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 40;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures, gRefused, gIrqOffMisses;
uint64_t gLoads, gStores;

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (!ml_get_interrupts_enabled()) {   // never map with interrupts off (premap() made every page the slices touch)
    ++gIrqOffMisses;
    return 0;
  }
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

IOMemoryMap *mapVoteRange(uint64_t base, uint32_t size)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
  IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (memory) memory->release();
  return map;
}

bool voteDisplayBandwidth(const Mode &mode)
{
  // sm8750.dtsi aop-cmd-db and apps_rsc/DRV-2. Copy cmd-db once and use the
  // same validated parser and serialized ACTIVE TCS transport as PianoA830.
  IOMemoryMap *dbMap = mapVoteRange(0x81c60000, Sm8750CmdDbBytes);
  if (!dbMap) {
    note("mode: MM0 command database unavailable");
    return false;
  }
  const auto read = [](void *base, uint32_t offset) { return rd32(uintptr_t(base) + offset); };
  const Sm8750RpmhIo dbIo{reinterpret_cast<void *>(uintptr_t(dbMap->getVirtualAddress())), read, nullptr, nullptr};
  auto *db = static_cast<Sm8750CmdDb *>(IOMallocData(sizeof(Sm8750CmdDb)));
  uint32_t addr = 0, unit = 0;
  uint16_t width = 0;
  uint8_t vcd = 0;
  int rc = db ? Sm8750CmdDbCopy(&dbIo, db) : Sm8750RpmhIoError;
  if (!rc) rc = Sm8750CmdDbBcm(db, "MM0", &addr, &unit, &width, &vcd);
  if (db) IOFreeData(db, sizeof(Sm8750CmdDb));
  dbMap->release();
  const auto vote = modes::mm0Vote(mode, unit, width);
  if (rc || !vote.average || !vote.peak) {
    note("mode: MM0 missing or invalid in cmd-db (%d, unit %u, width %u)", rc, unit, unsigned(width));
    return false;
  }

  IOMemoryMap *drv = mapVoteRange(0x16520000, 0x10000);
  if (!drv) {
    note("mode: MM0 apps DRV-2 unavailable");
    return false;
  }
  const Sm8750RpmhIo rpmh{
      reinterpret_cast<void *>(uintptr_t(drv->getVirtualAddress())), read,
      [](void *base, uint32_t offset, uint32_t value) { wr32(uintptr_t(base) + offset, value); },
      [](void *, unsigned us) { if (us >= 1000) IOSleep(us / 1000); else IODelay(us); }};
  const Sm8750RpmhCmd cmd{addr, Sm8750BcmCommand(1, 1, vote.average, vote.peak)};
  Sm8750RpmhResult result;
  rc = Sm8750RpmhWriteActive(&rpmh, &cmd, 1, 1, &result);
  drv->release();
  note("mode: MM0 AB/IB %u/%u kB/s, aux %u/%u vcd %u, cmd %#x=%#x, RPMh %d step %u slot %u status %#x",
       modes::scanoutAvgKBps(mode), modes::kMm0PeakKBps, unit, unsigned(width), unsigned(vcd), addr, cmd.data,
       rc, result.step, result.tcs, result.command[0]);
  // Keep an acknowledged vote until reboot. The resource's inherited DRV-2
  // value is unavailable, so a rollback cannot put it back.
  return rc == Sm8750RpmhOk;
}

struct KernelIo {
  bool voteModeBandwidth(const Mode &mode) { return voteDisplayBandwidth(mode); }
  uint32_t rd(uint64_t pa)
  {
    const uintptr_t va = pageVa(pa);
    if (!va) {
      ++gMapFailures;
      return 0xffffffffu;
    }
    ++gLoads;
    return rd32(va + uintptr_t(pa & 0xfff));
  }
  bool wr(uint64_t pa, uint32_t v, int panelCtl = -1)
  {
    if (!storeAllowed(pa, v, modesEnabled(), gA.panel && gA.panelEngines ? panelCtl : -1)) {
      if (++gRefused <= 8) note("REFUSED store %#llx = %08x (not a register of the slices)", (unsigned long long)pa, v);
      return false;
    }
    const uintptr_t va = pageVa(pa);
    if (!va) {
      ++gMapFailures;
      return false;
    }
    ++gStores;
    wr32(va + uintptr_t(pa & 0xfff), v);
    return true;
  }
  uint32_t modeUnderruns()
  {
    const uint32_t raw = rd(kMdp + kTopIntrStatus);
    return (raw & kUnderrunBits) | flipPendingUnderruns(false);
  }
  bool clearModeUnderruns()
  {
    flipPendingUnderruns(true);
    return wr(kMdp + kTopIntrClear, kUnderrunBits);
  }
  void delayUs(uint32_t us) { IODelay(us); }
  void sleepMs(uint32_t ms) { IOSleep(ms); }
  uint64_t pollDeadline(uint32_t us)
  {
    uint64_t deadline;
    clock_interval_to_deadline(us, kMicrosecondScale, &deadline);
    return deadline;
  }
  void pollWait(uint64_t deadline, bool sleep)
  {
    if (!sleep) {
      clock_delay_until(deadline);
    } else if (mach_absolute_time() < deadline &&
               assert_wait_deadline(reinterpret_cast<event_t>(this), THREAD_UNINT, deadline) == THREAD_WAITING) {
      thread_block(THREAD_CONTINUE_NULL);
    }
  }
  uint64_t nowUs() { return ::nowUs(); }
  uintptr_t irqOff() { return uintptr_t(ml_set_interrupts_enabled(FALSE)); }
  void irqRestore(uintptr_t s) { ml_set_interrupts_enabled(boolean_t(s)); }
};
KernelIo gIo;

// Every page the slices load from or store to, mapped once before anything is written.
bool premap()
{
  const uint64_t at[] = {kGcc + kGccDispAhb, kDispCc + kCcMdssGdsc, kDispCc + kCcAhb, kDispCc + kCcNonGdscAhb, kDispCc + kCcMdp,
                         kDispCc + kCcPclk0, kDispCc + kCcMdpRcg, kMdp, intfReg(1, 0), intfReg(2, 0), pccReg(0, 0), pccReg(1, 0),
                         pccReg(2, 0), pccReg(3, 0), dsiReg(0, 0), dsiReg(1, 0)};
  for (uint64_t pa : at)
    if (!pageVa(pa)) return false;
  for (unsigned c = 0; c < kCtlCount; ++c)
    if (!pageVa(ctlReg(c, 0))) return false;
  return true;
}

// --------------------------------------------------------------------------------------------- requests and state
IOService *gService;
IOLock *gLock;
struct Shared {
  bool wake = false;
  refresh::Policy policy;
  pcc::State color;
  bool colorDirty = false;
  pcc::NightShift nightShift;
  bool nightShiftDirty = false, modeSet = false;
  uint32_t nightMatrixCalls = 0, nightCorrectionCalls = 0;
  uint32_t matrixCalls = 0, matrixRefused = 0, gammaCalls = 0, gammaFits = 0, swapCalls = 0, panelCalls = 0;
  uint64_t lastRaw[9] = {};
  uint32_t lastFunction = 0, lastArg = 0;
  const char *lastMatrixWhy = "";
  bool matrixLog = false, lastMatrixOk = false;
  const char *lastGammaWhy = "";
  uint32_t gammaWorstMilli = 0, gammaWords = 0;
  bool gammaLog = false;
  int panelWant = -1;   // -1 nothing new, 0 off, 1 on
  panel::RequestGate panelGate;
  bool panelBusy = false, panelDone = false;
  IOReturn panelResult = kIOReturnNotReady;
  unsigned modeWant = 0, modeResultHz = 0;    // modeWant stays occupied until its caller consumes the result
  bool modeReady = false, modeDone = false;
  IOReturn modeResult = kIOReturnNotReady;
} gS;

// The worker's own state (only it touches these).
LivePath gPath;
bool gAsleep, gEnginesStopped, gRefreshBroken, gPanelBroken, gPanelUnsafe, gPccBroken;
IOReturn gModeFailure = kIOReturnSuccess;
bool gBoe;
panel::Fetch gPanelFetch;
unsigned gHz = 120;
uint32_t gSwitches, gSwitchFailures, gSleeps, gPccApplies, gPccFailures;
pcc::Matrix gPccBase = pcc::identity(), gPccLast = pcc::identity();
bool gPccLastValid;
pcc::PanelPcc gNightShiftBase, gNightShiftLast;
bool gDisplayOff;
refresh::Options gRefreshOptions;
panel::Options gPanelOptions;
// The worker fills the preallocated ring (gPanelTrace, allocated with the arguments), then publishes a separate copy.
struct PanelTraceReport {
  bool pending = false, on = false, ok = false, restored = false;
  unsigned serial = 0, ctl = 0, step = 0;
  panel::Sent dcs[2];
  uint32_t dsiBeforeReset[2] = {};
} gPanelReport;

void milliHz(uint64_t mhz, unsigned *whole, unsigned *frac)
{
  *whole = unsigned(mhz / 1000);
  *frac = unsigned(mhz % 1000);
}

void publish()
{
  if (!gService) return;
  gService->setProperty("piano-dpu-refresh-hz", gHz, 32);
  if (gA.panel) gService->setProperty("piano-dpu-panel", gPanelUnsafe ? "fault" : gAsleep ? "asleep" : "awake");
  if (gA.pcc && gPccLastValid) {
    char s[96];
    snprintf(s, sizeof s, "%d %d %d", pcc::milli(gPccLast.m[0][0]), pcc::milli(gPccLast.m[1][1]), pcc::milli(gPccLast.m[2][2]));
    gService->setProperty("piano-dpu-pcc", s);
  }
}

// ------------------------------------------------------------------------------------------------------------- B1c
void nightShiftUpdate(bool replay, const char *why);

bool reread(const char *after)
{
  const LivePath p = readPath(gIo, gA.ahbVote == 1);
  if (!p.ok) {
    note("path after %s: %s", after, p.why);
    return false;
  }
  gPath = p;
  if (!modesEnabled()) gHz = p.link[0].vtotal == kVtotal60 ? 60 : 120;
  return true;
}

bool doSwitch(unsigned hz, const char *why)
{
  const unsigned vt = hz == 60 ? kVtotal60 : kVtotal120;
  const uint64_t t0 = nowUs();
  const refresh::Switch s = refresh::switchTo(gIo, gPath, vt, gRefreshOptions);
  const uint32_t us = uint32_t(nowUs() - t0);
  unsigned w, f;
  milliHz(s.rate.milliHz, &w, &f);
  ++gSwitches;
  if (s.ok) {
    if (gSwitches <= 50 || gSwitches % 100 == 0)
      note("refresh: %u -> %u lines (%u Hz, %s): ok in %u us: latched in %d us, %u stores, line window %u of 3; measured %u.%03u Hz (frames "
           "%u/%u in %llu us, lockstep); periods %08x/%08x dsi totals %08x/%08x; switch %u",
           s.from, s.to, hz, why, us, s.latchUs, s.stores, s.tries - 1, w, f, s.rate.frames[0], s.rate.frames[1],
           (unsigned long long)s.rate.us, s.period[0], s.period[1], s.dsiTotal[0], s.dsiTotal[1], gSwitches);
  } else {
    ++gSwitchFailures;
    note("refresh: %u -> %u lines (%u Hz, %s): %s: %s%s; latched %d us, %u stores (%u refused), measured %u.%03u Hz (frames %u/%u), "
         "underrun %08x, periods %08x/%08x dsi totals %08x/%08x",
         s.from, s.to, hz, why, s.reverted ? "ANOMALY" : "refused", s.why,
         s.reverted ? (s.revertOk ? "; switched back, verified" : "; SWITCHED BACK BUT THAT FAILED TOO (a reset brings the boot state)") : "",
         s.latchUs, s.stores, s.refused, w, f, s.rate.frames[0], s.rate.frames[1], s.underrun, s.period[0], s.period[1], s.dsiTotal[0],
         s.dsiTotal[1]);
    {   // any failure: no more switches this boot
      gRefreshBroken = true;
      IOLockLock(gLock);
      gS.policy.disable();
      IOLockUnlock(gLock);
      note("refresh: off for this boot");
    }
  }
  if (reread("a switch") && s.ok && gA.nightShift) nightShiftUpdate(true, "refresh mode set");
  publish();
  return s.ok;
}

void refreshStart()
{
  gRefreshOptions.db = refresh::Db(gA.refreshDb);
  const char *why = refresh::refuse(gPath);
  note("refresh: %s; now %u lines (%u Hz); DSI timing shadow: %s (mode +%#x, flush +%#x); policy idle %u ms, burst %u in %u ms, dwell "
       "%u ms; %s",
       gA.refresh == kRefreshPlan ? "plan only (nothing written)" : gA.refresh == kRefreshAuto ? "auto" : gA.refresh == 60 ? "60 Hz" : "120 Hz",
       gPath.link[0].vtotal, gHz, refresh::dbName(gRefreshOptions.db), refresh::dbWords(gRefreshOptions.db).mode,
       refresh::dbWords(gRefreshOptions.db).flush, gA.policy.idleMs, gA.policy.burstSwaps, gA.policy.burstWindowMs, gA.policy.dwellMs,
       why ? why : "the path takes a VFP switch");
  for (unsigned l = 0; l < 2; ++l)
    note("refresh: link %u: intf%u config %08x hsync %08x period %08x (htotal %u vtotal %u) counters %x; dsi%u total %08x (vtotal %u) "
         "shadow words %08x %08x %08x",
         l, l + 1, gPath.link[l].config, gPath.link[l].hsyncCtl, gPath.link[l].period, gPath.link[l].htotal, gPath.link[l].vtotal,
         gPath.link[l].countEn, l, gPath.link[l].dsiTotal, gPath.link[l].dsiVtotal(), gIo.rd(dsiReg(l, kDsiShadow[0])),
         gIo.rd(dsiReg(l, kDsiShadow[1])), gIo.rd(dsiReg(l, kDsiShadow[2])));
  if (why) {
    gRefreshBroken = true;
    return;
  }
  if (gA.refresh == kRefreshPlan) {
    const refresh::Plan pl = refresh::plan(gPath, gHz == 60 ? kVtotal120 : kVtotal60, gRefreshOptions.db);
    char s[400];
    size_t n = 0;
    for (unsigned k = 0; k < pl.frameCount && n < sizeof s; ++k)
      n += size_t(snprintf(s + n, sizeof s - n, "%s%#llx=%08x", k ? " " : "", (unsigned long long)pl.frame[k].pa, pl.frame[k].v));
    for (unsigned k = 0; k < pl.afterCount && n < sizeof s; ++k)
      n += size_t(snprintf(s + n, sizeof s - n, "%s%#llx=%08x", k ? " " : "; after the latch ", (unsigned long long)pl.after[k].pa, pl.after[k].v));
    note("refresh: plan to %u Hz, inside INTF1 lines %u..%u of one frame: %s", gHz == 60 ? 120 : 60, refresh::kWindowLo,
         refresh::kWindowHi, s);
    gRefreshBroken = true;   // nothing more: plan only
    return;
  }
  if (gA.refreshTest) {
    unsigned ok = 0;
    const uint64_t t0 = nowUs();
    for (unsigned k = 0; k < gA.refreshTest && !gRefreshBroken; ++k) {
      if (doSwitch(60, "test")) ++ok;
      IOSleep(1000);
      if (!gRefreshBroken && doSwitch(120, "test")) ++ok;
      IOSleep(1000);
    }
    note("refresh: test: %u of %u switches ok in %llu s%s", ok, 2 * gA.refreshTest, (unsigned long long)((nowUs() - t0) / 1000000),
         gRefreshBroken ? " (stopped at an anomaly)" : "");
  }
  if (gRefreshBroken) return;
  if (gA.refresh == 60 && gHz != 60) doSwitch(60, "piano-dpu-refresh=60");
  else if (gA.refresh == 120 && gHz != 120) doSwitch(120, "piano-dpu-refresh=120");
  else if (gA.refresh == 60 || gA.refresh == 120) note("refresh: already %u Hz", gHz);
  IOLockLock(gLock);
  gS.policy.applied(gHz, nowMs());
  if (gA.refresh != kRefreshAuto) gS.policy.disable();
  IOLockUnlock(gLock);
  publish();
}

// ------------------------------------------------------------------------------------------------------------- B1d
void logMatrix(const char *what, const pcc::Matrix &m)
{
  note("pcc: %s [%d %d %d | %d %d %d | %d %d %d] (thousandths, [output][input])", what, pcc::milli(m.m[0][0]), pcc::milli(m.m[0][1]),
       pcc::milli(m.m[0][2]), pcc::milli(m.m[1][0]), pcc::milli(m.m[1][1]), pcc::milli(m.m[1][2]), pcc::milli(m.m[2][0]),
       pcc::milli(m.m[2][1]), pcc::milli(m.m[2][2]));
}

bool pccApply(const pcc::Matrix &m, const char *why)
{
  const pcc::Apply a = pcc::apply(gIo, gPath, m, gA.pccNegative != 0);
  ++gPccApplies;
  if (a.ok) {
    gPccLast = m;
    gPccLastValid = true;
  } else {
    ++gPccFailures;
  }
  if (!a.ok || gPccApplies <= 50 || gPccApplies % 100 == 0) {
    note("pcc: %s -> dspp mask %x: %s; latched in %d us, %u stores (%u refused), %u negative clamped, %u saturated; words R %05x %05x %05x "
         "G %05x %05x %05x B %05x %05x %05x; apply %u",
         why, a.dspps, a.ok ? "ok" : a.why, a.latchUs, a.stores, a.refused, a.words.clampedNegative, a.words.saturated, a.words.w[0][0],
         a.words.w[0][1], a.words.w[0][2], a.words.w[1][0], a.words.w[1][1], a.words.w[1][2], a.words.w[2][0], a.words.w[2][1],
         a.words.w[2][2], gPccApplies);
  }
  if (gPccFailures >= 3 && !gPccBroken) {
    gPccBroken = true;
    note("pcc: off for this boot after %u failed writes", gPccFailures);
  }
  publish();
  return a.ok;
}

// The matrix the PCC should have now: the base (what the boot stages left, normally the identity) after macOS's.
pcc::Matrix pccWanted(bool *approximate)
{
  IOLockLock(gLock);
  const pcc::Matrix user = gS.color.pcc(gA.pccEncode != 0, approximate);
  gS.colorDirty = false;
  IOLockUnlock(gLock);
  return pcc::multiply(gPccBase, user);
}

void pccStart()
{
  // What the boot stages left: PCC enable and words on each DSPP of the path.
  pcc::Matrix found[kDsppCount];
  bool enabled[kDsppCount] = {}, sameAll = true;
  int first = -1;
  for (unsigned d = 0; d < kDsppCount; ++d) {
    if (!((gPath.dspps >> d) & 1u)) continue;
    enabled[d] = gIo.rd(pccReg(d, 0)) & 1u;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i) {
        const int32_t v = pccSignedQ15(gIo.rd(pccReg(d, kPccCoef[o][i])));
        found[d].m[o][i] = enabled[d] ? v * 2 : (o == i ? pcc::kOne : 0);
      }
    if (first < 0) first = int(d);
    else sameAll &= enabled[d] == enabled[first] && pcc::same(found[d], found[first]);
  }
  if (first < 0) {
    note("pcc: off: the path's LMs (%02x) have no DSPP", gPath.lms);
    gPccBroken = true;
    return;
  }
  if (!sameAll) {
    note("pcc: off: the boot stages left different PCCs on the halves (left as found)");
    gPccBroken = true;
    return;
  }
  gPccBase = found[first];
  note("pcc: piano-dpu-pcc=%u, dspp mask %x (LMs %02x), found %s%s; encode %s, negative %s; macOS's matrices go %s it",
       gA.pcc, gPath.dspps, gPath.lms, enabled[first] ? "on" : "off", enabled[first] && !pcc::isIdentity(gPccBase) ? " (not the identity: kept as the base)" : "",
       gA.pccEncode ? "diagonals to the encoded space" : "as sent", gA.pccNegative ? "two's complement" : "clamped to 0",
       pcc::isIdentity(gPccBase) ? "in place of" : "before");
  logMatrix("base", gPccBase);
  if (!pccApply(gPccBase, pcc::isIdentity(gPccBase) ? "the identity (no visible change)" : "the found matrix again (no visible change)"))
    return;
  if (gA.pccTest) {
    const pcc::Matrix t = pcc::multiply(gPccBase, pcc::diagonal(pcc::fromMilli(gA.pccTestMilli[0]), pcc::fromMilli(gA.pccTestMilli[1]),
                                                                 pcc::fromMilli(gA.pccTestMilli[2])));
    logMatrix("test (piano-dpu-pcc-test)", t);
    pccApply(t, "the test diagonal");
  }
}

void pccUpdate(bool force, const char *why)
{
  if (gPccBroken || gAsleep || gPanelUnsafe) return;
  bool approximate = false;
  const pcc::Matrix m = pccWanted(&approximate);
  if (!force && gPccLastValid && pcc::same(m, gPccLast)) return;
  if (approximate) note("pcc: the matrix is not a diagonal: applied to the encoded values as sent (an approximation)");
  logMatrix(why, m);
  pccApply(m, why);
}

// Caller holds the flip transaction lock through the PCC latch.
void nightShiftUpdateLocked(bool replay, const char *why)
{
  if (gPccBroken || gDisplayOff || gAsleep || gPanelUnsafe) return;
  IOLockLock(gLock);
  const pcc::NightShift state = gS.nightShift;
  IOLockUnlock(gLock);
  const pcc::PanelPcc wanted = pcc::nightShiftPrograms(gNightShiftBase, state);
  const bool changed = !pcc::same(wanted, gNightShiftLast);
  if (!replay && !changed) return;

  const LivePath path = readPath(gIo, gA.ahbVote == 1);
  const pcc::NightShiftApply update = pcc::updateNightShift(gIo, path, wanted, gNightShiftLast, replay);
  if (!update.attempted) {
    note("night shift: %s deferred (path %s, DSPP mask %x)", why, path.why, path.dspps);
    return;
  }

  const pcc::Apply &a = update.result;
  ++gPccApplies;
  if (a.ok) {
    if (changed) {
      const auto &l = a.readback[0], &r = a.readback[1];
      // Ascending offsets +0x10..+0x30, grouped by input R/G/B.
      note("night shift: PCC read back apply %u, %s, en %x/%x, %d us; DSPP0 %05x %05x %05x | %05x %05x %05x | %05x %05x %05x; "
           "DSPP1 %05x %05x %05x | %05x %05x %05x | %05x %05x %05x",
           gPccApplies, why, a.enable[0], a.enable[1], a.latchUs,
           l[0][0], l[1][0], l[2][0], l[0][1], l[1][1], l[2][1], l[0][2], l[1][2], l[2][2],
           r[0][0], r[1][0], r[2][0], r[0][1], r[1][1], r[2][1], r[0][2], r[1][2], r[2][2]);
    } else if (replay) {
      note("night shift: %s, %s; DSPP0/1 read back (enable %x/%x), one CTL flush latched in %d us, apply %u",
           why, state.neutral() ? "base" : "matrix and correction", a.enable[0], a.enable[1], a.latchUs, gPccApplies);
    }
  } else {
    ++gPccFailures;
    gPccBroken = !update.restored || gPccFailures >= 3;
    note("night shift: %s failed: %s; previous PCC restore %s%s", why, a.why, update.restored ? "verified" : "failed",
         gPccBroken ? "; disabled for this boot" : "");
  }
}

void nightShiftUpdate(bool replay, const char *why)
{
  IOLock *flipLock = flipTransactionLock();
  if (!flipLock) {
    note("night shift: %s deferred, no CTL lock", why);
    return;
  }
  IOLockLock(flipLock);
  nightShiftUpdateLocked(replay, why);
  IOLockUnlock(flipLock);
}

IOReturn doModeSwitch(unsigned hz)
{
  if (gModeFailure != kIOReturnSuccess) return gModeFailure;
  const bool panelUnavailable = gAsleep || gEnginesStopped || gPanelUnsafe || gDisplayOff;
  IOLock *flipLock = flipTransactionLock();
  if (!flipLock) return kIOReturnNoResources;
  IOLockLock(flipLock);
  const modes::Options options{.db = refresh::Db(gA.refreshDb), .boe = gBoe, .hwGatedAhb = gA.ahbVote == 1,
                               .panelUnavailable = panelUnavailable, .fixedHz = gA.modeHz};
  const auto r = modes::switchTo(gIo, hz, options, flipInvalidateCleanEpochs, [] {
    if (gA.nightShift) nightShiftUpdateLocked(true, "M3 mode switch");
  });
  IOLockUnlock(flipLock);
  if (r.why) {
    if (panelUnavailable) {
      note("mode: %u Hz refused: %s", hz, r.why);
      return kIOReturnNotReady;
    }
    ++gSwitchFailures;
    modes::logFailure(r, hz, note);
    if (hz == modes::kHighModeHz && r.underruns) note("mode: 144 Hz verification INTF underrun mask %#x", r.underruns);
    if (r.restoreTransitionUnderruns)
      note("mode: underrun status before restored-mode verification: %#x", r.restoreTransitionUnderruns);
    if (r.reverted) {
      gModeFailure = r.restoreWhy ? kIOReturnOffline : kIOReturnAborted;
      if (!r.restoreWhy) gHz = r.from;
      return gModeFailure;
    }
    return kIOReturnUnsupported;
  }
  gHz = hz;
  if (r.timingTouched) {
    reread("M3");
    ++gSwitches;
  }
  unsigned whole, frac;
  milliHz(r.rate.milliHz, &whole, &frac);
  note("mode: %u -> %u Hz verified, both links, %u.%03u Hz (%u/%u frames); switch %u",
       r.from, hz, whole, frac, r.rate.frames[0], r.rate.frames[1], gSwitches);
  publish();
  return kIOReturnSuccess;
}

// ------------------------------------------------------------------------------------------------------------- B1e
void logPanel(const char *what, const panel::Result &r)
{
  note("panel: %s: %s (step %u)%s; dcs %02x %s in %u us%s, %02x %s in %u us; engines intf1 %d us intf2 %d us status %x/%x%s; at %llu/%llu/"
       "%llu/%llu ms",
       what, r.ok ? "ok" : r.why, r.step, r.restored ? ", the panel brought back" : "", r.dcs[0].command, r.dcs[0].ok ? "sent" : "not sent",
       r.dcs[0].doneUs, r.dcs[0].waitedBlanking ? " (front porch)" : "", r.dcs[1].command, r.dcs[1].ok ? "sent" : "not sent", r.dcs[1].doneUs,
       r.engines.us[0], r.engines.us[1], r.engines.status[0], r.engines.status[1], r.engines.rate.lockstep ? ", counting in lockstep" : "",
       (unsigned long long)(r.tUs[0] / 1000), (unsigned long long)(r.tUs[1] / 1000), (unsigned long long)(r.tUs[2] / 1000),
       (unsigned long long)(r.tUs[3] / 1000));
}

panel::TraceRing *beginPanelTrace()
{
  if (!gA.panelTrace || !gPanelTrace) return nullptr;
  gPanelTrace->reset();
  return gPanelTrace;
}

void finishPanelTrace(panel::TraceRing *trace, bool on, const panel::Result &r)
{
  if (!trace) return;
  gPanelReport.pending = true;
  gPanelReport.on = on;
  gPanelReport.ok = r.ok;
  gPanelReport.restored = r.restored;
  ++gPanelReport.serial;
  gPanelReport.ctl = unsigned(gPath.ctl);
  gPanelReport.step = r.step;
  gPanelReport.dcs[0] = r.dcs[0];
  gPanelReport.dcs[1] = r.dcs[1];
  gPanelReport.dsiBeforeReset[0] = r.dsiBeforeReset[0];
  gPanelReport.dsiBeforeReset[1] = r.dsiBeforeReset[1];
}

void publishPanelTrace()
{
  PanelTraceReport &report = gPanelReport;
  if (!report.pending || !gPanelTrace) return;
  report.pending = false;
  const unsigned count = gPanelTrace->size();
  char line[224];
  OSData *dump = OSData::withCapacity((count + 6) * sizeof line);
  bool complete = dump != nullptr;
  auto emit = [&] {
    note("%s", line);
    complete = complete && dump->appendBytes(line, unsigned(strlen(line))) && dump->appendBytes("\n", 1);
  };
  snprintf(line, sizeof line, "m5 %u %s ctl%u ok=%u step=%u restored=%u rows=%u dropped=%u", report.serial,
           report.on ? "wake" : "off", report.ctl, unsigned(report.ok), report.step, unsigned(report.restored),
           count, gPanelTrace->total - count);
  emit();
  if (report.on) {
    snprintf(line, sizeof line, "m5 %u pre-reset status=%08x/%08x", report.serial, report.dsiBeforeReset[0], report.dsiBeforeReset[1]);
    emit();
  }
  for (const panel::Sent &s : report.dcs) {
    if (!s.command) continue;
    snprintf(line, sizeof line,
             "m5 %u dcs %02x trig=%u done=%x us=%u porch=%u intr=%08x/%08x ack=%x:%08x/%08x>%x:%08x/%08x fifo=%08x/%08x>%08x/%08x",
             report.serial, unsigned(s.command), unsigned(s.triggerAttempted), s.dmaDoneMask(), s.doneUs,
             unsigned(s.waitedBlanking), s.intrPoll[0], s.intrPoll[1], s.ackBeforeMask, s.before[0].ack, s.before[1].ack,
             s.ackAfterMask, s.ackAfter[0], s.ackAfter[1], s.before[0].fifo, s.before[1].fifo, s.fifoAfter[0], s.fifoAfter[1]);
    emit();
    if (!report.on) continue;
    const auto &a = s.before[0], &b = s.before[1];
    snprintf(line, sizeof line, "m5 %u host %02x ctrl=%08x/%08x status=%08x/%08x lane=%08x/%08x clk=%08x/%08x",
             report.serial, unsigned(s.command), a.ctrl, b.ctrl, a.status, b.status, a.lane, b.lane, a.clocks, b.clocks);
    emit();
  }
  for (unsigned i = 0; i < count; ++i) {
    const panel::TraceSample &s = gPanelTrace->at(i);
    const panel::TraceLink &a = s.link[0], &b = s.link[1];
    snprintf(line, sizeof line, "m5 %u %u %llu %s %c %08x/%08x/%08x/%08x %08x/%08x/%08x/%08x %08x/%08x/%08x/%08x",
             report.serial, gPanelTrace->total - count + i, (unsigned long long)s.us, panel::traceStepName(s.step), s.boundary ? 'B' : 'P',
             a.en, a.status, a.frame, a.line, b.en, b.status, b.frame, b.line, s.top, s.active, s.flush, s.intfFlush);
    emit();
  }
  if (!complete || !gService->setProperty("piano-dpu-panel-trace", dump))
    note("m5 %u: could not publish piano-dpu-panel-trace", report.serial);
  if (dump) dump->release();
}

bool panelSleep()
{
  if (gAsleep || gPanelBroken) return false;
  flipPanelBegin();
  const LivePath live = readPath(gIo, gA.ahbVote == 1);
  if (!live.ok || live.ctl != gPath.ctl) {
    flipPanelEnd(false);
    note("panel: sleep refused: scanout path changed (%s, ctl%d)", live.why, live.ctl);
    return false;
  }
  gPath = live;
  panel::TraceRing *trace = beginPanelTrace();
  const panel::Result r = panel::panelOff(gIo, gPath, gPanelOptions, trace);
  finishPanelTrace(trace, false, r);
  logPanel(gPanelOptions.engines ? "sleep (DCS 28/10, master-first CTL handoff)" : "sleep (DCS 28/10)", r);
  if (r.ok) {
    gAsleep = true;
    gEnginesStopped = r.enginesStopped;
    gPanelFetch = r.fetch;
    ++gSleeps;
  } else {
    gPanelBroken = true;
    gPanelUnsafe = !r.safeAwake && !r.restored;
    note("panel: sleep off for this boot");
  }
  flipPanelEnd(gAsleep || gPanelUnsafe);
  publish();
  return r.ok;
}

bool panelWake()
{
  if (!gAsleep || gPanelUnsafe) return !gPanelUnsafe;
  flipPanelBegin();
  const LivePath live = readPath(gIo, gA.ahbVote == 1, gEnginesStopped);
  if (live.ctl != gPath.ctl || (gEnginesStopped && live.intfActive != kDsiIntfSlave) ||
      (live.link[0].dsiCtrl & 3u) != 3u || (live.link[1].dsiCtrl & 3u) != 3u) {
    gPanelBroken = gPanelUnsafe = true;
    gAsleep = gEnginesStopped = false;
    flipPanelEnd(true);
    note("panel: wake refused: DSI video path unavailable (%s, ctl%d)", live.why, live.ctl);
    publish();
    return false;
  }
  panel::TraceRing *trace = beginPanelTrace();
  const panel::Result r = panel::panelOn(gIo, gPath, gPanelOptions, gEnginesStopped, gPanelFetch, trace);
  finishPanelTrace(trace, true, r);
  logPanel(gEnginesStopped ? "wake (DCS 11/29, bonded route and timing)" : "wake (DCS 11/29)", r);
  gAsleep = false;
  gEnginesStopped = false;
  const bool scanout = r.scanoutRestored && reread("the wake");
  const bool ok = r.ok && scanout;
  gPanelBroken = !ok;
  gPanelUnsafe = !scanout;
  if (ok && gA.pcc) pccUpdate(true, "re-applied at the wake");
  flipPanelEnd(gPanelUnsafe);
  publish();
  return ok;
}

void panelStart()
{
  gPanelOptions.lowPower = !gA.panelHs;
  gPanelOptions.engines = gA.panelEngines != 0;
  for (unsigned l = 0; l < 2; ++l) {
    const uint32_t trig = gIo.rd(dsiReg(l, kDsiTrigCtrl));
    note("panel: dsi%u command path: ctrl %08x status %08x fifo %08x cmd-dma %08x dma1 %08x length %08x trig %08x (dma trigger %u%s, %s) "
         "intr %08x tpg %08x ack %08x",
         l, gIo.rd(dsiReg(l, kDsiCtrl)), gIo.rd(dsiReg(l, kDsiStatus0)), gIo.rd(dsiReg(l, kDsiFifoStatus)), gIo.rd(dsiReg(l, kDsiCmdDmaCtrl)),
         gIo.rd(dsiReg(l, kDsiDmaCtrl1)), gIo.rd(dsiReg(l, kDsiDmaLength)), trig, trig & 7u, (trig & 7u) == 4 ? " software" : " NOT software",
         (trig >> 12) & 1u ? "DMA blocked within the frame" : "no block: triggered in the front porch", gIo.rd(dsiReg(l, kDsiIntrCtrl)),
         gIo.rd(dsiReg(l, kDsiTpgCtrl)), gIo.rd(dsiReg(l, kDsiAckErr)));
  }
  note("panel: piano-dpu-panel=1: sleep with the display (%s DCS, %s); nothing of the panel's reset or supplies is touched",
       gPanelOptions.lowPower ? "low-power" : "high-speed", gPanelOptions.engines ? "then the timing engines stopped" : "the engines keep running");
  if (gA.panelTest && gA.level >= 2) {
    note("panel: standalone test skipped with flips enabled; use display power");
    return;
  }
  for (unsigned k = 0; k < gA.panelTest && !gPanelBroken; ++k) {
    const bool slept = panelSleep();
    publishPanelTrace();
    if (!slept) break;
    IOSleep(5000);
    const bool woke = panelWake();
    if (woke && gA.nightShift) nightShiftUpdate(true, "panel wake");
    publishPanelTrace();
    if (!woke) break;
    IOSleep(5000);
    note("panel: test cycle %u of %u done", k + 1, gA.panelTest);
  }
}

// ------------------------------------------------------------------------------------------------------------- the worker
void serve()
{
  uint64_t nextAlive = nowMs() + 600000;
  uint32_t publishedMatrices = 0, publishedCorrections = 0;
  for (;;) {
    IOLockLock(gLock);
    uint64_t due = nextAlive;
    const refresh::Policy::Decision d0 = gS.policy.decide(nowMs());
    const bool autoRefresh = gA.refresh == kRefreshAuto && !gRefreshBroken && !gAsleep && !gPanelUnsafe;
    if (autoRefresh && d0.nextMs && d0.nextMs < due) due = d0.nextMs;
    if (!gS.wake && !(autoRefresh && d0.hz != gHz)) {
      const uint64_t now = nowMs();
      if (due > now) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(uint32_t(due - now), kMillisecondScale, &deadline);
        IOLockSleepDeadline(gLock, &gS, deadline, THREAD_UNINT);
      }
    }
    gS.wake = false;
    const int want = gS.panelWant;
    gS.panelWant = -1;
    const unsigned mode = gS.modeDone ? 0 : gS.modeWant;
    const bool nightShiftDirty = gS.nightShiftDirty, modeSet = gS.modeSet;
    const uint32_t nightMatrices = gS.nightMatrixCalls, nightCorrections = gS.nightCorrectionCalls;
    gS.nightShiftDirty = gS.modeSet = false;
    const bool colorDirty = gS.colorDirty, matrixLog = gS.matrixLog;
    uint64_t raw[9];
    memcpy(raw, gS.lastRaw, sizeof raw);
    const uint32_t function = gS.lastFunction, arg = gS.lastArg, calls = gS.matrixCalls;
    const bool matrixOk = gS.lastMatrixOk;
    const char *matrixWhy = gS.lastMatrixWhy;
    gS.matrixLog = false;
    const bool gammaLog = gS.gammaLog;
    const uint32_t gammaCalls = gS.gammaCalls, gammaWords = gS.gammaWords, gammaWorst = gS.gammaWorstMilli;
    const char *gammaWhy = gS.lastGammaWhy;
    gS.gammaLog = false;
    const refresh::Policy::Decision d = gS.policy.decide(nowMs());
    IOLockUnlock(gLock);

    if (matrixLog && (calls <= 30 || !matrixOk))
      note("pcc: matrix call %u: function %u arg %#x: %016llx %016llx %016llx | %016llx %016llx %016llx | %016llx %016llx %016llx: %s",
           calls, function, arg, (unsigned long long)raw[0], (unsigned long long)raw[1], (unsigned long long)raw[2],
           (unsigned long long)raw[3], (unsigned long long)raw[4], (unsigned long long)raw[5], (unsigned long long)raw[6],
           (unsigned long long)raw[7], (unsigned long long)raw[8], matrixOk ? "taken" : matrixWhy);
    if (gammaLog && gammaCalls <= 30)
      note("pcc: gamma call %u: %u words: %s (worst deviation from scaled ramps %u/1000)", gammaCalls, gammaWords, gammaWhy, gammaWorst);
    // y12m merge (M4 + M5): the panel transition first (M5a), the colour after it (M4's Night Shift replay on a good wake, before the
    // synchronous caller is released, so the display comes back with its PCC), then the result handed to the waiting copy thread
    bool panelOk = true;
    if (want >= 0 && gA.panel) panelOk = want ? panelWake() : gAsleep || panelSleep();
    if (colorDirty && gA.pcc) pccUpdate(false, "macOS's matrices");
    if (gA.nightShift) {
      if (want >= 0) gDisplayOff = !want;
      if ((want != 1 || panelOk) && (want == 1 || modeSet || nightShiftDirty))
        nightShiftUpdate(want == 1 || modeSet, want == 1 ? "display wake" : modeSet ? "display mode set" : "colour update");
      if (nightMatrices != publishedMatrices) {
        gService->setProperty("piano-dpu-night-shift-matrix-calls", nightMatrices, 32);
        publishedMatrices = nightMatrices;
      }
      if (nightCorrections != publishedCorrections) {
        gService->setProperty("piano-dpu-night-shift-correction-calls", nightCorrections, 32);
        publishedCorrections = nightCorrections;
      }
    }
    if (want >= 0 && gA.panel) {
      IOLockLock(gLock);
      const bool undoLateSleep = gS.panelGate.undoLateSleep(want, gAsleep);
      if (want && !panelOk) gS.panelGate.latchFallback();
      if (!gS.panelGate.fallback) {
        gS.panelGate.ready = !gPanelBroken;
        gS.panelGate.wakePending = gPanelBroken && !want;
      }
      gS.panelResult = panelOk ? kIOReturnSuccess : kIOReturnIOError;
      gS.panelDone = true;
      IOLockWakeup(gLock, &gS.panelDone, false);
      IOLockUnlock(gLock);
      publishPanelTrace();
      if (undoLateSleep) {
        // The copy worker is already on backlight-only power. Restore a late sleep on this worker.
        note("panel: sleep finished after its reply deadline; attempting one wake");
        panelWake();
        publishPanelTrace();
      }
    }
    if (mode) {
      const IOReturn result = doModeSwitch(mode);
      IOLockLock(gLock);
      gS.modeResult = result;
      gS.modeResultHz = result == kIOReturnSuccess || result == kIOReturnAborted ? gHz : 0;
      gS.modeDone = true;
      IOLockWakeup(gLock, &gS.modeDone, false);
      IOLockUnlock(gLock);
    }
    if (gA.refresh == kRefreshAuto && !gRefreshBroken && !gAsleep && !gPanelUnsafe && d.hz != gHz) {
      const bool ok = doSwitch(d.hz, d.hz == 60 ? "idle" : "swaps");
      IOLockLock(gLock);
      if (ok) gS.policy.applied(gHz, nowMs());
      IOLockUnlock(gLock);
    }
    if (nowMs() >= nextAlive) {
      nextAlive = nowMs() + 600000;
      IOLockLock(gLock);
      const uint64_t swaps = gS.policy.swaps();
      const uint32_t mc = gS.matrixCalls, gc = gS.gammaCalls, pc = gS.panelCalls;
      const bool refreshOff = modesEnabled() ? !gS.modeReady || gModeFailure != kIOReturnSuccess : gRefreshBroken;
      IOLockUnlock(gLock);
      note("slices: alive: refresh %u Hz%s (%u switches, %u failed, %llu swaps); pcc %u applies (%u failed, %u matrix calls, %u gamma "
           "calls); panel %s (%u sleeps, %u calls); %llu loads %llu stores %u refused %u map failures",
           gHz, refreshOff ? " (off)" : "", gSwitches, gSwitchFailures, (unsigned long long)swaps, gPccApplies, gPccFailures, mc, gc,
           gAsleep ? "asleep" : "awake", gSleeps, pc, (unsigned long long)gLoads, (unsigned long long)gStores, gRefused, gMapFailures);
    }
  }
}

void worker(void *, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-dpu-slices");
  piano_dpu_slices_stage = 1;
  // The survey first (<= 120 s): its stages 1..5; B1b (piano-dpu=2) goes on with 6..8 (negative when a flip step failed: not ours).
  const int readyStage = (gA.panel || modesEnabled()) && gA.level >= 2 ? 8 : 5;
  for (unsigned i = 0; i < 1200 && piano_dpu_stage >= 0 && piano_dpu_stage < readyStage; ++i) IOSleep(100);
  if ((piano_dpu_stage >= 0 && piano_dpu_stage < readyStage) || (piano_dpu_stage < 0 && piano_dpu_stage > -6)) {
    piano_dpu_slices_stage = -1;
    note("slices: off: the survey failed at stage %d", -piano_dpu_stage);
    return;
  }
  piano_dpu_slices_stage = 2;
  if (!premap()) {
    piano_dpu_slices_stage = -2;
    note("slices: off: a register page did not map (%u mapped)", gPageCount);
    return;
  }
  const LivePath p = readPath(gIo, gA.ahbVote == 1);
  note("slices: path: %s; ctl%d (top %08x intf %x master %x) lms %02x dspps %x; intf1 en %x status %x frame %u, intf2 en %x status %x "
       "frame %u",
       p.why, p.ctl, p.ctlTop, p.intfActive, p.intfMaster, p.lms, p.dspps, p.link[0].en, p.link[0].status, p.link[0].frame,
       p.link[1].en, p.link[1].status, p.link[1].frame);
  if (!p.ok) {
    piano_dpu_slices_stage = -2;
    note("slices: off: %s (nothing written)", p.why);
    return;
  }
  gPath = p;
  gHz = p.link[0].vtotal == kVtotal60 ? 60 : 120;
  bool modeReady = false;
  if (modesEnabled()) {
    const uint32_t id = gIo.rd(kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride + 4);
    if (id != UINT32_MAX) {
      const modes::LinkLive live[] = {modes::readTiming(gIo, gPath.link[0], 0), modes::readTiming(gIo, gPath.link[1], 1)};
      gHz = modes::classify(live).hz;
      gBoe = (id & 1u) != 0;
      modeReady = true;
      note("mode: piano-dpu-modes=%u, %s, %s, DSI timing shadow %s",
           gA.modeHz == modes::kBootModeHz ? 1u : gA.modeHz, panelModule(id), rateOrder(id),
           refresh::dbName(refresh::Db(gA.refreshDb)));
    } else {
      note("mode: panel ID unavailable; switching off");
    }
  }
  const bool refreshWrites = gA.refresh && gA.refresh != kRefreshPlan;
  bool countEn = true;
  if (gA.panel) {
    const CounterInit init = initCounters(gIo, gPath);
    countEn = init.enabled;
    gPanelBroken = !init.moving;
    note("panel: INTF counters: count-en %x/%x -> %x/%x; %s", p.link[0].countEn, p.link[1].countEn,
         gPath.link[0].countEn, gPath.link[1].countEn,
         init.moving ? "both frame and line counters advance" : "could not verify counter motion on both links; panel sleep off");
  } else if (refreshWrites) {
    countEn = countersOn(gIo, gPath);
  }
  if (refreshWrites && !countEn) {
    note("refresh: INTF counter enables did not read back (count-en %x/%x): refresh off", gPath.link[0].countEn, gPath.link[1].countEn);
    gRefreshBroken = true;
  }
  if (gA.pcc) {
    piano_dpu_slices_stage = 3;
    pccStart();
  }
  if (gA.nightShift) {
    gPccBroken = !pcc::capturePanelPcc(gIo, gPath, gNightShiftBase);
    gNightShiftLast = gNightShiftBase;
    gService->setProperty("piano-dpu-night-shift-matrix-calls", uint64_t(0), 32);
    gService->setProperty("piano-dpu-night-shift-correction-calls", uint64_t(0), 32);
    note("night shift: %s", gPccBroken ? "PCC base unavailable for DSPP0/1" : "captured base PCC for DSPP0/1");
  }
  if (gA.refresh) {
    piano_dpu_slices_stage = 4;
    refreshStart();
  } else {
    gRefreshBroken = true;
  }
  if (gA.panel && !gPanelBroken) {
    piano_dpu_slices_stage = 5;
    panelStart();
  }
  piano_dpu_slices_stage = 6;
  const char *refreshState = gRefreshBroken ? "off" : gA.refresh == kRefreshAuto ? "auto" : "fixed";
  char fixedRefresh[24];
  if (modeReady) {
    snprintf(fixedRefresh, sizeof fixedRefresh, "M3 fixed %u Hz", gA.modeHz);
    refreshState = fixedRefresh;
  }
  note("slices: serving (refresh %s, pcc %s, panel %s)", refreshState,
       gPccBroken ? "off" : gA.nightShift ? "Night Shift" : gA.pcc ? "on" : "off", gA.panel && !gPanelBroken ? "on" : "off");
  publish();
  IOLockLock(gLock);
  gS.panelGate.ready = gA.panel && !gPanelBroken;
  gS.modeReady = modeReady;
  IOLockUnlock(gLock);
  serve();
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- the interface
void dpuSlicesReadArgs() { readSliceArgs(); }

bool dpuSlicesWanted()
{
  return gA.refresh || modesEnabled() || gA.pcc || gA.panel || gA.nightShift;
}

void dpuSlicesStart(IOService *dpu)
{
  if (!dpuSlicesWanted() || gLock) return;
  note("slices: piano-dpu-refresh=%s pcc=%u panel=%u (B1c/B1d/B1e: these write; the survey above stays read only)%s",
       gA.refresh == kRefreshAuto ? "auto" : gA.refresh == kRefreshPlan ? "plan" : gA.refresh == 60 ? "60" : gA.refresh == 120 ? "120" : "0", gA.pcc,
       gA.panel, gA.level ? "" : ": piano-dpu=0, not started");
  if (!gA.level) return;
  gService = dpu;
  gLock = IOLockAlloc();
  thread_t t = nullptr;
  if (!gLock || kernel_thread_start(worker, nullptr, &t) != KERN_SUCCESS) {
    piano_dpu_slices_stage = -1;
    note("slices: FAILED: no lock or no thread; nothing written");
    return;
  }
  thread_deallocate(t);
  if (gA.level < 2) dpu->registerService();   // level 2 publishes the shared service in PianoDpu::start
}

bool dpuSlicesPlatformFunction(const OSSymbol *name, void *p1, void *p2, void *p3, IOReturn *result)
{
  if (name && name->isEqualTo("PianoDpuMode")) {
    const uintptr_t hz = uintptr_t(p1);
    if (!modesEnabled() || hz != gA.modeHz) *result = kIOReturnUnsupported;
    else if (!gLock) *result = kIOReturnNotReady;
    else {
      IOLockLock(gLock);
      if (!gS.modeReady) *result = kIOReturnNotReady;
      else if (gS.modeWant) *result = kIOReturnBusy;
      else {
        gS.modeWant = unsigned(hz);
        gS.wake = true;
        IOLockWakeup(gLock, &gS, false);
        while (!gS.modeDone) IOLockSleep(gLock, &gS.modeDone, THREAD_UNINT);
        *result = gS.modeResult;
        if (p2) *static_cast<unsigned *>(p2) = gS.modeResultHz;
        gS.modeWant = 0;
        gS.modeDone = false;
      }
      IOLockUnlock(gLock);
    }
    return true;
  }
  if (!name) return false;
  if (!gLock) {
    if (!gA.panel || !name->isEqualTo("PianoDpuPanel")) return false;
    note("panel: %s refused: panel worker unavailable", p1 ? "wake" : "sleep");
    *result = kIOReturnNotReady;
    return true;
  }
  if (gA.nightShift && name->isEqualTo("PianoDpuNightShiftMatrix")) {
    uint64_t raw[9];
    if (!p1) {
      *result = kIOReturnBadArgument;
      return true;
    }
    memcpy(raw, p1, sizeof raw);
    IOLockLock(gLock);
    ++gS.nightMatrixCalls;
    const auto input = gS.nightShift.setMatrix(raw);
    if (input == pcc::NightShift::Input::accepted) gS.nightShiftDirty = true;
    gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = input == pcc::NightShift::Input::invalid ? kIOReturnBadArgument : kIOReturnSuccess;
    return true;
  }
  if (gA.nightShift && name->isEqualTo("PianoDpuNightShiftCorrection")) {
    IOLockLock(gLock);
    ++gS.nightCorrectionCalls;
    gS.nightShift.setCorrection(uint32_t(uintptr_t(p1)));
    gS.nightShiftDirty = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = kIOReturnSuccess;
    return true;
  }
  if (gA.nightShift && name->isEqualTo("PianoDpuNightShiftModeSet")) {
    IOLockLock(gLock);
    gS.modeSet = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = kIOReturnSuccess;
    return true;
  }
  if (name->isEqualTo("PianoDpuSwap")) {
    IOLockLock(gLock);
    ++gS.swapCalls;
    const bool wake = gS.policy.swap(nowMs());
    if (wake) {
      gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
    }
    IOLockUnlock(gLock);
    *result = kIOReturnSuccess;
    return true;
  }
  if (name->isEqualTo("PianoDpuColorMatrix")) {
    if (!gA.pcc || !p1) {
      *result = kIOReturnUnsupported;
      return true;
    }
    uint64_t raw[9];
    memcpy(raw, p1, sizeof raw);
    const pcc::Decoded d = pcc::decodeS3132(raw);
    const uint32_t function = uint32_t(uintptr_t(p2));
    IOLockLock(gLock);
    ++gS.matrixCalls;
    memcpy(gS.lastRaw, raw, sizeof raw);
    gS.lastFunction = function;
    gS.lastArg = uint32_t(uintptr_t(p3));
    gS.lastMatrixWhy = d.ok && function >= pcc::State::kFunctions ? "the function is above 15" : d.why;
    gS.lastMatrixOk = d.ok && gS.color.setMatrix(function, d.m);
    if (gS.lastMatrixOk) gS.colorDirty = true;
    else ++gS.matrixRefused;
    gS.matrixLog = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = d.ok ? kIOReturnSuccess : kIOReturnBadArgument;
    return true;
  }
  if (name->isEqualTo("PianoDpuGammaTable")) {
    const uint32_t words = uint32_t(uintptr_t(p2));
    pcc::GammaFit f;
    if (gA.pcc >= 2 && p1 && words >= pcc::kGammaWords) f = pcc::fitGamma(static_cast<const uint32_t *>(p1), words);
    IOLockLock(gLock);
    ++gS.gammaCalls;
    gS.lastGammaWhy = gA.pcc < 2 ? "piano-dpu-pcc=1: logged only" : f.why;
    gS.gammaWorstMilli = f.worstMilli;
    gS.gammaWords = words;
    gS.gammaLog = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    if (f.ok) {
      ++gS.gammaFits;
      gS.color.setGamma(f.m);
      gS.colorDirty = gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
    }
    IOLockUnlock(gLock);
    *result = f.ok ? kIOReturnSuccess : kIOReturnUnsupported;
    return true;
  }
  if (name->isEqualTo("PianoDpuPanel")) {
    IOLockLock(gLock);
    if (!gA.panel && gA.nightShift) {   // M4 without M5a: the display power for the PCC replay, a notify (the worker does the rest)
      ++gS.panelCalls;
      gS.panelWant = p1 ? 1 : 0;
      gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
      *result = kIOReturnSuccess;
    } else if (!gS.panelGate.admit(p1 != nullptr, [](const char *line) { note("%s", line); })) {
      *result = kIOReturnNotReady;
    } else if (gS.panelBusy) {
      *result = kIOReturnBusy;
    } else {
      ++gS.panelCalls;
      gS.panelBusy = true;
      gS.panelDone = false;
      gS.panelWant = p1 ? 1 : 0;
      gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
      uint64_t deadline;
      clock_interval_to_deadline(panel::kPanelReplyTimeoutMs, kMillisecondScale, &deadline);
      while (!gS.panelDone && IOLockSleepDeadline(gLock, &gS.panelDone, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
      if (gS.panelDone) {
        *result = gS.panelResult;
      } else {
        // Cancel a queued sleep. A wake may already be needed to undo a completed sleep.
        if (p1) gS.panelGate.latchFallback();
        else gS.panelGate.sleepTimedOut(gS.panelWant);
        *result = kIOReturnTimeout;
      }
      gS.panelBusy = false;
    }
    IOLockUnlock(gLock);
    return true;
  }
  return false;
}
