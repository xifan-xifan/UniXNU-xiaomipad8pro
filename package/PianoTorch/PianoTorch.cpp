// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTorch: piano's rear torch (the PM8550 flash LED module at SID 1 0xee00, both factory-wired channels together) for macOS 27,
// native at EL1. Design, facts, the control path and the test plan: docs/drivers/torch.md. Facts and sources: FlashHw.h.
//   piano-torch=1  a read-only survey, logged as "PianoTorch: ..." lines: the SPMI arbiter, the flash module's APID and its owner
//                  EE, identity, enables, per-channel current/strobe/timer, clamp, thermal thresholds, status bytes, raw bytes.
//                  The only store is the SPMI observer channel's read command (package/PianoPower/SpmiObserver.h).
//   piano-torch=2  the survey, then torch control: writes only to that peripheral, only the registers of TorchCore.h's
//                  allowlist, each read back; a time limit (piano-torch-timeout), the module's own fault and over-temperature
//                  bits, the SoC (PianoThermal) and battery (PianoPower) temperatures; off on system sleep and shutdown. Control:
//                  an administrator's IORegistry property (tooling/native/piano-torch) or another driver's callPlatformFunction
//                  (PianoTorchClient.h); piano-torch-test=S lights it once for S seconds at boot (the attended first test).
// With piano-torch omitted or set to 0, the driver declines in probe() and maps nothing.
// Other arguments: piano-torch-ma (default 400 mA total, 2 x 200 mA, the lamp current of an on without its own),
// piano-torch-max-ma (default 400 mA total, hard cap 400 mA total, 2 x 200 mA),
// piano-torch-timeout=S (default 300, 10..1800), piano-torch-soc-max=C (default 85), piano-torch-batt-max=C (default 45),
// piano-torch-delay=S (default 0), and piano-torch-probe=1 to replace the boot test with T4. The probe can add a spare-pair
// window (piano-torch-probe-spare=1) or a timed FLASH pulse (piano-torch-flash=1). The separate piano-torch-ladder=1 mode
// runs 100/200/300/400 mA total steps within the configured cap.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
#include "TorchSpmi.h"
#include "PianoTorchClient.h"

using namespace pianotorch;
using pianopower::Window;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last stage reached, negative when it failed there.
volatile int32_t piano_torch_stage = 0;
}

namespace {

struct Args {
  uint32_t level, ma, maxMa, timeout, test, delay, socMax, battMax, probe, probeSpare, flash, ladder;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  gArgs.level = bootArg("piano-torch", kDefaultLevel);
  gArgs.maxMa = clampMa(bootArg("piano-torch-max-ma", kDefaultMaxMa), kHardMaxMa);
  gArgs.ma = clampMa(bootArg("piano-torch-ma", kDefaultOnMa), gArgs.maxMa);
  gArgs.timeout = clampArg(bootArg("piano-torch-timeout", 300), 10, 1800);
  gArgs.test = clampArg(bootArg("piano-torch-test", 0), 0, kProbeMaxSeconds);
  gArgs.delay = clampArg(bootArg("piano-torch-delay", 0), 0, 600);
  gArgs.socMax = clampArg(bootArg("piano-torch-soc-max", 85), 40, 105);
  gArgs.battMax = clampArg(bootArg("piano-torch-batt-max", 45), 30, 60);
  const char *args = PE_boot_args();
  gArgs.probe = bootSwitchEnabled(args, "piano-torch-probe");
  gArgs.probeSpare = bootSwitchEnabled(args, "piano-torch-probe-spare");
  gArgs.flash = bootSwitchEnabled(args, "piano-torch-flash");
  gArgs.ladder = bootSwitchEnabled(args, "piano-torch-ladder");
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoTorch: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_torch_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------ SPMI windows
struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};
Region gCore, gCnfg, gObsv, gChnl;

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}
void unmapRegion(Region &r)
{
  if (r.map) r.map->release();
  r = {};
}

uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
// The observer channel's command word (a read request). No PMIC register is written through it.
void ioWr(void *, Window w, uint32_t offset, uint32_t value) { wr32(windowBase(w) + offset, value); }
void ioDelay(void *, unsigned us) { IODelay(us); }
const pianopower::Io gIo = {nullptr, ioRd, ioWr, ioDelay};

// The flash APID's owner channel (level 2): the one page mapped, the only PMIC write path of this driver.
uint32_t chRd(void *, uint32_t offset) { return rd32(gChnl.va + offset); }
void chWr(void *, uint32_t offset, uint32_t value) { wr32(gChnl.va + offset, value); }
const ChnlIo gChnlIo = {nullptr, chRd, chWr, ioDelay};

SpmiPort gPort;
pianopower::Arbiter gArbiter;
pianopower::Apid gApid;
Survey gSurvey;

// ------------------------------------------------------------------------------------------------------ the control state
struct Request {
  bool pending;
  unsigned ma, seconds;
  const char *source;
};
IOLock *gLock;
Request gReq;
Torch gTorch;          // under gLock
bool gReady;           // level 2 control is up (under gLock)
bool gBootDiagnostic;  // the boot diagnostic owns the lamp until its final read-back (under gLock)
bool gPowerJoined;
char gState[128] = "starting";

// The temperatures other drivers publish, looked up every 10 s until found (they never unload).
IOService *gThermal, *gBattery;
uint64_t gNextLookupMs;

IOService *lookup(const char *name)
{
  OSDictionary *matching = IOService::serviceMatching(name);
  IOService *s = matching ? IOService::copyMatchingService(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (s) note("found %s (its temperature is part of the guard)", name);
  return s;
}

Temperatures readTemperatures(uint64_t now)
{
  if ((!gThermal || !gBattery) && now >= gNextLookupMs) {
    gNextLookupMs = now + 10000;
    if (!gThermal) gThermal = lookup("PianoThermal");
    if (!gBattery) gBattery = lookup("AppleSmartBattery");
  }
  Temperatures t;
  if (gThermal) {
    OSObject *o = gThermal->copyProperty("TemperaturesDeciC");   // retained: PianoThermal replaces the dictionary each update
    if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
      if (OSCollectionIterator *it = OSCollectionIterator::withCollection(d)) {
        while (OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject())) {
          OSNumber *n = OSDynamicCast(OSNumber, d->getObject(key));
          if (!n) continue;
          const int32_t deci = int32_t(n->unsigned32BitValue());
          if (!t.socValid || deci > t.socDeci) t.socDeci = deci;
          t.socValid = true;
        }
        it->release();
      }
    }
    OSSafeReleaseNULL(o);
    // B6's PMIC ADC publishes the flash thermistor separately from the TSENS hottest-per-controller map.
    o = gThermal->copyProperty("BoardTemperaturesDeciC");
    if (OSDictionary *d = OSDynamicCast(OSDictionary, o)) {
      if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("flash"))) {
        t.flashDeci = int32_t(n->unsigned32BitValue());
        t.flashValid = true;
      }
    }
    OSSafeReleaseNULL(o);
  }
  if (gBattery) {
    OSObject *o = gBattery->copyProperty("Temperature");
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
      t.batteryCenti = int32_t(n->unsigned32BitValue());
      t.batteryValid = true;
    }
    OSSafeReleaseNULL(o);
  }
  return t;
}

void temperatureText(const Temperatures &t, char *flash, char *soc, char *battery, unsigned size)
{
  if (t.flashValid) snprintf(flash, size, "%d.%dC", t.flashDeci / 10, t.flashDeci % 10);
  else strlcpy(flash, "none", size);
  if (t.socValid) snprintf(soc, size, "%d.%dC", t.socDeci / 10, t.socDeci % 10);
  else strlcpy(soc, "none", size);
  if (t.batteryValid) snprintf(battery, size, "%d.%02dC", t.batteryCenti / 100, t.batteryCenti % 100);
  else strlcpy(battery, "none", size);
}

// ------------------------------------------------------------------------------------------------------------ the report
void reportSurvey(const Survey &s)
{
  note("flash: revision %#04x type %#04x subtype %#04x: %s%s", s.revision, s.type, s.subtype,
       !s.flash ? "NOT a flash LED module (type 0x18 expected); nothing more read" : subtypeName(s.subtype),
       s.flash && !s.fourChannel ? " (not decoded: this driver knows the four-channel layout only)" : "");
  if (!s.flash) return;
  for (unsigned row = 0; row < 256; row += 16) {
    char line[64];   // "raw xx:" and 16 bytes of " xx"
    const int prefix = snprintf(line, sizeof line, "raw %02x:", row);
    if (prefix < 0) continue;
    size_t at = size_t(prefix);
    for (unsigned i = row; i < row + 16; ++i) {
      const int wrote = s.valid[i] ? snprintf(line + at, sizeof line - at, " %02x", s.reg[i])
                                  : snprintf(line + at, sizeof line - at, " --");
      if (wrote > 0) at += size_t(wrote);
    }
    note("%s", line);
  }
  if (!s.fourChannel) return;
  const uint8_t clamp = s.reg[kRegTorchClamp] & kClampMask;
  note("flash: module %s (%02x), channels on %02x, 5 mA steps %02x, torch clamp %u (%u mA a channel), otst thresholds %u/%u",
       s.moduleEn() ? "ENABLED" : "off", s.reg[kRegModuleEn], s.chanEn(), s.reg[kRegIres] & 0x0f, clamp, (clamp + 1u) * 5u,
       s.reg[kRegThermThrsh1] & kThrshMask, s.reg[kRegThermThrsh2] & kThrshMask);
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    const ChannelView v = s.channel(ch);
    const unsigned ua = v.ua();
    note("flash ch%u%s: itarget %u (%u.%u mA in %s steps) strobe %02x (%s) timer %02x (%s)%s", ch,
         (kPianoChannelMask & (1u << ch)) ? " (piano's lamp)" : " (not wired on piano)", v.itarget, ua / 1000, ua % 1000 / 100,
         v.torchSteps ? "5 mA" : "12.5 mA", v.strobe, strobeName(v.strobe), v.timer,
         (v.timer & kTimerEnable) ? "safety timer on" : "safety timer off", v.on ? ", ON" : "");
  }
  const Faults f = s.faults();
  note("flash status1 %02x status2 %02x status3 %02x rt %02x: %s%s%s%s%s%s%s", s.reg[kRegStatus1], s.reg[kRegStatus2],
       s.reg[kRegStatus3], s.reg[kRegIntRtSts], f.any() ? "" : "no fault on piano's channels", f.shortCircuit ? "SHORT " : "",
       f.open ? "OPEN " : "", f.timeout ? "TIMER-EXPIRED " : "", f.thermal ? "OVER-TEMPERATURE " : "",
       f.current ? "CURRENT-LIMIT " : "", f.voltage ? "LOW-SUPPLY" : "");
}

// The probe of a no-light failure (piano-torch-probe=1): a full dump before the first window and after the last, and between
// them only the registers that changed (a diff against the previous dump), so the log stays readable. The probe runs once on
// this driver's own thread; gProbeLast is its private state.
Survey gProbeLast;
Survey probeSnapshot(bool active)
{
  // Use a private observer port so this read-only snapshot can coexist with a synchronous power-off.
  SpmiPort port;
  port.io = &gIo;
  port.apid = gApid.apid;
  Bus bus = portBus(port);
  if (active) bus.read = [](void *ctx, uint8_t reg, uint8_t *value, unsigned bytes) {
    const SpmiPort &p = *static_cast<SpmiPort *>(ctx);
    return pianopower::spmiReadOnce(*p.io, p.apid, reg, value, bytes) == pianopower::ReadResult::ok;
  };
  return survey(bus, active);
}
void probeSnapFull(const char *when)
{
  gProbeLast = probeSnapshot(false);
  note("probe dump %s (full, %u failed reads):", when, gProbeLast.failures);
  reportSurvey(gProbeLast);
}
void probeSnapDiff(const char *label, const Survey &now)
{
  unsigned changed = 0, unreadable = 0;
  for (unsigned row = 0; row < 256; row += 16) {
    char line[16 * 10 + 1] = {};
    size_t at = 0;
    for (unsigned i = row; i < row + 16; ++i) {
      if (!now.valid[i] || !gProbeLast.valid[i]) {
        ++unreadable;
        continue;
      }
      if (now.reg[i] == gProbeLast.reg[i]) continue;
      ++changed;
      const int wrote = snprintf(line + at, sizeof line - at, " %02x:%02x->%02x", i, gProbeLast.reg[i], now.reg[i]);
      if (wrote > 0) at += size_t(wrote);
    }
    if (at) note("probe diff %s:%s", label, line);
  }
  note("probe diff %s: %u changed, %u bytes unavailable for comparison", label, changed, unreadable);
  gProbeLast = now;
}

void formatChannels(uint8_t status1, uint8_t status3, uint8_t mask, char *out, unsigned size)
{
  unsigned at = 0;
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    if (!(mask & (1u << ch))) continue;
    const bool shorted = status1 & (1u << (2 * ch));
    const bool open = status1 & (1u << (2 * ch + 1));
    if (at) at += unsigned(snprintf(out + at, at < size ? size - at : 0, " "));
    const bool expired = status3 & (1u << (2 * ch));
    at += unsigned(snprintf(out + at, at < size ? size - at : 0, "ch%u[short=%u open=%u timer_expired=%u]",
                            ch + 1, shorted, open, expired));
    if (at >= size) {
      out[size - 1] = 0;
      return;
    }
  }
  if (!at) snprintf(out, size, "none");
}

void summary(const Survey &s, const char *refusal, char *out, unsigned size)
{
  if (!s.flash) {
    snprintf(out, size, "no flash module at 0x1ee (APID %d, type %02x)", gApid.apid, s.type);
    return;
  }
  snprintf(out, size, "%s rev %02x %s; APID %d owner EE %u; ch0/ch1 itarget %u/%u ires %02x clamp %u; status %02x %02x %02x; %s",
           s.fourChannel ? "4ch" : "other", s.revision, s.lit() ? "LIT" : s.moduleEn() ? "module-on" : "off", gApid.apid, gApid.owner,
           s.reg[kRegItarget0] & kItargetMask, s.reg[kRegItarget0 + 1] & kItargetMask, s.reg[kRegIres] & 0x0f,
           s.reg[kRegTorchClamp] & kClampMask, s.reg[kRegStatus1], s.reg[kRegStatus2], s.reg[kRegStatus3],
           refusal ? "not writable" : "writable");
}

void logOutcome(const char *what, const Torch::Outcome &o, unsigned askedMa)
{
  const StepResult &r = o.steps;
  if (o.reason != Reason::none && o.reason != Reason::user && r.ok && !r.writes && !o.cleanedUp) {
    note("refused (%s): %s; nothing written%s", what, reasonName(o.reason), o.lit ? ", the torch stays as it was" : "");
    return;
  }
  if (o.lit) {
    note("on: %u mA (2 x %u mA, itarget %u, clamp %u) for %s; %u writes, %u read back; status %02x %02x %02x rt %02x, enables %02x %02x",
         o.ma, o.ma / 2, itargetForMa(o.ma), clampForMa(gArgs.maxMa), what, r.writes, r.readbacks, o.live.status1,
         o.live.status2, o.live.status3, o.live.rt, o.live.moduleEn, o.live.chanEn);
    if (askedMa && askedMa != o.ma) note("on: %u mA asked, %u mA set (the limit is %u mA, 5 mA steps a channel)", askedMa, o.ma, gArgs.maxMa);
    return;
  }
  note("off (%s): %s; %u writes, %u read back%s", what, reasonName(o.reason), r.writes, r.readbacks, r.ok ? "" : "; A STEP FAILED");
  if (!r.ok && r.why)
    note("failed step: register %#04x wanted %02x read %02x: %s (refused %u)", r.failedReg, r.failedWant, r.failedGot, r.why, r.refused);
  if (o.cleanedUp) {
    const StepResult &off = o.cleanup;
    note("explicit off after failed activation: %u writes, %u read back%s", off.writes, off.readbacks,
         off.ok ? "" : "; OFF UNCONFIRMED");
    if (!off.ok)
      note("failed off step: register %#04x wanted %02x read %02x: %s", off.failedReg, off.failedWant, off.failedGot, off.why);
  }
}

void publishState(IOService *self)
{
  IOLockLock(gLock);
  self->setProperty(kPropertyState, gState);
  self->setProperty(kPropertyLit, gTorch.lit());
  self->setProperty(kPropertyMa, gTorch.ma(), 32);
  IOLockUnlock(gLock);
}

void queueLocked(unsigned ma, unsigned seconds, const char *source)
{
  gReq.pending = true;
  gReq.ma = ma;
  gReq.seconds = seconds;
  gReq.source = source ? source : "?";
  IOLockWakeup(gLock, &gReq, false);
}

}  // namespace

enum class ProbeResult { complete, alternate, stopped };

// ------------------------------------------------------------------------------------------------------------ the service
class PianoTorch : public IOService {
  OSDeclareDefaultStructors(PianoTorch)
  void run();
  void serve();
  void ladder();
  void probe();
  ProbeResult probeWindow(uint8_t mask, const char *label, bool flash = false);
  void probeIdleSamples(uint8_t mask, const char *label, const char *phase);
  void offNow(Reason why, const char *what);
  static void thread(void *self, wait_result_t);

 public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
};

OSDefineMetaClassAndStructors(PianoTorch, IOService)

IOService *PianoTorch::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoTorch::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-torch=%u ma=%u max=%u timeout=%u s test=%u s soc-max=%u C batt-max=%u C delay=%u probe=%u spare=%u flash=%u ladder=%u (PM8550 SID %u flash %#x, PPID %#x)",
       gArgs.level, gArgs.ma, gArgs.maxMa, gArgs.timeout, gArgs.test, gArgs.socMax, gArgs.battMax, gArgs.delay, gArgs.probe,
       gArgs.probeSpare, gArgs.flash, gArgs.ladder, kSid, kBase, kPpid);
  if (gArgs.level > 2) {
    note("piano-torch=%u: only levels 1 and 2 exist (docs/drivers/torch.md section 8); running 2", gArgs.level);
    gArgs.level = 2;
  }
  if (gArgs.level == 1) note("level 1: loads only, nothing written (the SPMI observer's read command is the only store)");
  setProperty("piano-torch", gArgs.level, 32);
  gLock = IOLockAlloc();
  if (!gLock) {
    stage(-1, "no lock; nothing done");
    return true;
  }
  if (gArgs.level >= 2) gPowerJoined = pianopower::join(this) == kIOReturnSuccess;
  thread_t t = nullptr;
  if (kernel_thread_start(&PianoTorch::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread; nothing done");
    return true;
  }
  thread_deallocate(t);
  registerService();                               // tooling/native/piano-torch and other drivers find it by class
  return true;
}

void PianoTorch::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-torch");
  static_cast<PianoTorch *>(self)->run();
}

void PianoTorch::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  stage(1, "SPMI: the arbiter and the flash module's APID (read only)");
  using namespace pianopower;
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    stage(-1, "the SPMI arbiter windows do not map");
    return;
  }
  gArbiter = spmiArbiter(gIo);
  if (!gArbiter.v7) {
    note("spmi: arbiter version %#x, %u APIDs: not the v7 layout", gArbiter.version, gArbiter.count);
    stage(-1, "not the SPMI arbiter v7; nothing read");
    return;
  }
  gApid = spmiFindApid(gIo, gArbiter, kPpid);
  note("spmi: arbiter %#x, %u APIDs; flash PPID %#x: APID %d, owner EE %u%s", gArbiter.version, gArbiter.count, kPpid, gApid.apid,
       gApid.owner, gApid.apid < 0 ? " (NOT in the map)" : gApid.owner == 0 ? " (this processor: writes possible)" : " (another EE: our writes would be refused)");
  if (gApid.apid < 0) {
    stage(-1, "the flash module is not in the APID map");
    return;
  }
  gPort.io = &gIo;
  gPort.apid = gApid.apid;

  stage(2, "the flash module's registers over the observer channel (read only)");
  gSurvey = survey(portBus(gPort));
  reportSurvey(gSurvey);
  const char *refusal = writeRefusal(gSurvey, gApid.apid, gApid.owner);
  if (!refusal && gArgs.level >= 2 && !gPowerJoined) refusal = "could not register for synchronous power-off";
  char line[200];
  summary(gSurvey, refusal, line, sizeof line);
  setProperty(kPropertySurvey, line);
  note("summary: %s", line);
  {   // the SPMI lock every kext shares (PianoSpmi.h): this kext's transactions and waits, and every kext's
    char lockLine[360];
    pianospmi::format(lockLine, sizeof lockLine);
    note("%s", lockLine);
  }

  if (gArgs.level < 2) {
    stage(3, "done");
    note("done: %u observer reads (%u failed), 0 PMIC writes", gPort.reads, gPort.readFailures);
    IOLockLock(gLock);
    snprintf(gState, sizeof gState, "unavailable: read-only survey (piano-torch=1)");
    IOLockUnlock(gLock);
    publishState(this);
    unmapRegion(gCore);
    unmapRegion(gCnfg);
    unmapRegion(gObsv);
    return;
  }

  stage(3, "level 2: torch control (writes to the flash module only, each read back)");
  if (refusal || !mapRegion(gChnl, kSpmiChnls + uint64_t(kChnlStride) * uint32_t(gApid.apid), 0x20)) {
    note("level 2 refused: %s; the survey above is all this boot does", refusal ? refusal : "the APID's write channel does not map");
    IOLockLock(gLock);
    snprintf(gState, sizeof gState, "unavailable: %s", refusal ? refusal : "no write channel");
    IOLockUnlock(gLock);
    publishState(this);
    stage(-3, "no control");
    return;
  }
  gPort.chnl = &gChnlIo;
  GuardConfig g;
  g.defaultS = gArgs.timeout;
  g.socMaxDeci = int32_t(gArgs.socMax) * 10;
  g.batteryMaxCenti = int32_t(gArgs.battMax) * 100;
  const bool t4 = !gArgs.ladder && gArgs.probe && gArgs.test;
  IOLockLock(gLock);
  gTorch.configure(portBus(gPort), g, gArgs.maxMa, t4 && gArgs.probeSpare);
  gReady = true;
  gBootDiagnostic = gArgs.ladder || t4;
  const Torch::Outcome o = gTorch.checkBootState(nowMs(), gSurvey);
  gTorch.describe(nowMs(), gState, sizeof gState);
  IOLockUnlock(gLock);
  note("level 2: torch control on channels 1+2 at %u mA (at most %u), auto-off after %u s, off at SoC >= %u C or battery >= %u C; "
       "writes only to PPID %#x, %u allowlisted registers",
       gArgs.ma, gArgs.maxMa, gArgs.timeout, gArgs.socMax, gArgs.battMax, kPpid, unsigned(sizeof kWritable / sizeof kWritable[0]));
  if (o.reason == Reason::bootFoundOn) logOutcome("it was lit when the driver started", o, 0);
  publishState(this);
  if (gArgs.ladder) {
    ladder();
  } else if (t4) {
    probe();   // replaces the single boot test with the T4 windows, dumps and polls
  } else if (gArgs.test) {
    note("test: on at %u mA for %u s (piano-torch-test)", gArgs.ma, gArgs.test);
    IOLockLock(gLock);
    if (!gReq.pending && gTorch.diagnosticAbort() == Reason::none)
      queueLocked(gArgs.ma, gArgs.test, "the boot test");
    IOLockUnlock(gLock);
  }
  IOLockLock(gLock);
  gBootDiagnostic = false;
  IOLockUnlock(gLock);
  serve();
}

// T4 uses the ordinary controlled torch path on each selected pair. Keep the samples and read-back labels tied to that pair.
static void probeSample(uint8_t mask, const char *label, const char *phase, uint64_t elapsed,
                        const Controller::Live &l, const Temperatures &t)
{
  char status[128] = "STATUS1=-- STATUS2=-- STATUS3=--", channels[128] = "";
  char rt[96] = "--", module[4] = "--", en[4] = "--";
  if (l.statusOk) {
    snprintf(status, sizeof status, "STATUS1=%02x STATUS2=%02x STATUS3=%02x VPH_LOW=%u BCL=%u BOB_ILIM=%u/%u OTST=%u/%u BOB_THERM=%u",
             l.status1, l.status2, l.status3, !!(l.status2 & kSts2VphLow), !!(l.status2 & kSts2BclIbat),
             !!(l.status2 & kSts2BobIlimS1), !!(l.status2 & kSts2BobIlimS2), !!(l.status2 & kSts2Otst1),
             !!(l.status2 & kSts2Otst2), !!(l.status2 & kSts2BobThermOverload));
    formatChannels(l.status1, l.status3, mask, channels, sizeof channels);
  }
  if (l.rtOk)
    snprintf(rt, sizeof rt, "%02x fault=%u ramp_up=%u ramp_down=%u", l.rt, !!(l.rt & kRtFault), !!(l.rt & kRtRampUpDone),
             !!(l.rt & kRtRampDownDone));
  if (l.moduleOk) snprintf(module, sizeof module, "%02x", l.moduleEn);
  if (l.channelsOk) snprintf(en, sizeof en, "%02x", l.chanEn);
  char flash[24], soc[24], battery[24];
  temperatureText(t, flash, soc, battery, sizeof flash);
  note("probe %s %s t+%llu ms: %s %s INT_RT_STS(0x10)=%s MODULE_EN=%s CHAN_EN=%s temp flash=%s SoC=%s battery=%s",
       label, phase, (unsigned long long)elapsed, status, channels, rt, module, en, flash, soc, battery);
}

static Survey probeSettings()
{
  Survey s;
  s.valid[kRegTimer0] = portRead(&gPort, kRegTimer0, &s.reg[kRegTimer0], 8);
  s.valid[kRegModuleEn] = portRead(&gPort, kRegModuleEn, &s.reg[kRegModuleEn], 8);
  s.valid[kRegTorchClamp] = portRead(&gPort, kRegTorchClamp, &s.reg[kRegTorchClamp], 1);
  return s;
}

static void reportProbeSettings(uint8_t mask, const char *label, const Survey &s)
{
  const bool channels = s.valid[kRegTimer0], strobe = s.valid[kRegModuleEn], clamp = s.valid[kRegTorchClamp];
  char ires[4] = "--", cl[4] = "--";
  if (strobe) snprintf(ires, sizeof ires, "%02x", s.reg[kRegIres]);
  if (clamp) snprintf(cl, sizeof cl, "%02x", s.reg[kRegTorchClamp]);
  note("probe %s activation settings: IRESOLUTION=%s TORCH_CLAMP=%s", label, ires, cl);
  for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
    if (!(mask & (1u << ch))) continue;
    char timer[4] = "--", itarget[4] = "--", word[4] = "--";
    if (channels) {
      snprintf(timer, sizeof timer, "%02x", s.reg[kRegTimer0 + ch]);
      snprintf(itarget, sizeof itarget, "%02x", s.reg[kRegItarget0 + ch]);
    }
    if (strobe) snprintf(word, sizeof word, "%02x", s.reg[kRegStrobe0 + ch]);
    note("probe %s ch%u activation settings: TIMER=%s ITARGET=%s STROBE=%s (%s)", label, ch + 1, timer, itarget, word,
         strobe ? strobeName(s.reg[kRegStrobe0 + ch]) : "unreadable");
  }
}

void PianoTorch::probe()
{
  note("probe: T4 starts at %u mA (cap %u mA), %u s per torch window, spare=%u flash=%u",
       maForItarget(itargetForMa(gArgs.ma)), gArgs.maxMa, gArgs.test, gArgs.probeSpare, gArgs.flash);
  probeSnapFull("before all windows");
  const ProbeResult a = probeWindow(kPianoChannelMask, "A ch1+2");
  ProbeResult b = ProbeResult::complete;
  if (a != ProbeResult::stopped && gArgs.probeSpare) b = probeWindow(kOtherChannelMask, "B ch3+4");
  else if (gArgs.probeSpare) note("probe B ch3+4: skipped after window A stopped");
  if (gArgs.flash) {
    if (gArgs.maxMa < kProbeFlashMa)
      note("probe FLASH ch1+2: skipped, %u mA exceeds the configured cap of %u mA", kProbeFlashMa, gArgs.maxMa);
    else if (a == ProbeResult::complete && b == ProbeResult::complete)
      probeWindow(kPianoChannelMask, "FLASH ch1+2", true);
    else
      note("probe FLASH ch1+2: skipped after a prior window stopped");
  }
  probeSnapFull("after all windows");
  note("probe: T4 finished");
}

void PianoTorch::probeIdleSamples(uint8_t mask, const char *label, const char *phase)
{
  const uint64_t start = nowMs();
  for (unsigned i = 0; i < 3; ++i) {
    if (i) IOSleep(kProbePollMs);
    const uint64_t now = nowMs();
    const Temperatures t = readTemperatures(now);
    IOLockLock(gLock);
    const bool operational = gTorch.operational();
    const Controller::Live live = operational ? gTorch.controller().live() : Controller::Live();
    IOLockUnlock(gLock);
    if (!operational) return;
    probeSample(mask, label, phase, now - start, live, t);
  }
}

ProbeResult PianoTorch::probeWindow(uint8_t mask, const char *label, bool flash)
{
  const unsigned duration = flash ? kProbeFlashMs : gArgs.test * 1000;
  const unsigned interval = flash ? kPulsePollMs : kProbePollMs;
  const unsigned ma = flash ? kProbeFlashMa : maForItarget(itargetForMa(gArgs.ma));
  note("probe %s: next window %u mA total for %u ms", label, ma, duration);
  probeIdleSamples(mask, label, "before");

  const Temperatures temps = readTemperatures(nowMs());
  IOLockLock(gLock);
  const uint64_t start = nowMs();
  const Torch::Outcome activation = gTorch.requestProbe(start, ma, duration, temps, mask, flash);
  const uint64_t firstSample = nowMs();
  // Copy settings during activation, before the short FLASH timer can expire.
  Survey settings;
  if (activation.steps.ok && activation.steps.writes) settings = probeSettings();
  gTorch.describe(firstSample, gState, sizeof gState);
  IOLockUnlock(gLock);
  const auto reportActivation = [&] {
    const StepResult &steps = activation.steps;
    note("probe %s: activation %s; %u writes, %u read back%s", label,
         activation.lit ? "on" : reasonName(activation.reason), steps.writes, steps.readbacks, steps.ok ? "" : "; A STEP FAILED");
    if (steps.why) note("probe %s: activation failed reg %02x wanted %02x read %02x: %s", label,
                        steps.failedReg, steps.failedWant, steps.failedGot, steps.why);
    if (activation.sampled)
      probeSample(mask, label, steps.writes ? "activation" : "preflight", firstSample - start, activation.live, temps);
    if (steps.ok && steps.writes) reportProbeSettings(mask, label, settings);
  };
  Survey active;
  if (!flash) {
    if (activation.lit) active = probeSnapshot(true);
    reportActivation();
    publishState(this);
  }

  // Keep console writes out of the timed pulse. Four ticks fit between activation and explicit off, including the timer sample.
  struct Reading { uint64_t elapsed; Controller::Live live; Temperatures temperatures; };
  Reading pulse[kProbeFlashMs / kPulsePollMs];
  unsigned pulseReadings = 0;
  StepResult off = activation.cleanup;
  Reason ended = activation.reason;
  bool lit = activation.lit;
  while (lit) {
    const uint64_t now = nowMs();
    const uint64_t next = start + (now - start) / interval * interval + interval;
    const uint64_t wake = next < start + duration ? next : start + duration;
    if (wake > now) IOSleep(unsigned(wake - now));
    const uint64_t sample = nowMs();
    const Temperatures t = readTemperatures(sample);
    IOLockLock(gLock);
    const Reason aborted = gTorch.diagnosticAbort();
    const Torch::Outcome outcome = gTorch.poll(nowMs(), t);
    gTorch.describe(nowMs(), gState, sizeof gState);
    IOLockUnlock(gLock);
    if (aborted != Reason::none) {
      // The callback already ran the synchronous OFF. Do not resume this window on wake.
      ended = aborted;
      break;
    }
    if (outcome.sampled) {
      if (!flash) probeSample(mask, label, "during", sample - start, outcome.live, t);
      else if (pulseReadings < sizeof pulse / sizeof pulse[0]) pulse[pulseReadings++] = {sample - start, outcome.live, t};
    }
    off = outcome.steps;
    ended = outcome.reason;
    lit = outcome.lit;
  }
  if (flash) {
    reportActivation();
    for (unsigned i = 0; i < pulseReadings; ++i)
      probeSample(mask, label, "during", pulse[i].elapsed, pulse[i].live, pulse[i].temperatures);
  }

  IOLockLock(gLock);
  const Reason aborted = gTorch.diagnosticAbort();
  const bool offUnconfirmed = gTorch.needsOff();
  IOLockUnlock(gLock);
  if (aborted != Reason::none) ended = aborted;
  if ((activation.lit || activation.cleanedUp) && aborted == Reason::none) {
    note("probe %s: explicit off (%s); %u writes, %u read back%s", label, reasonName(ended), off.writes, off.readbacks,
         off.ok ? "" : "; OFF UNCONFIRMED");
    if (off.why) note("probe %s: off failed reg %02x wanted %02x read %02x: %s", label,
                      off.failedReg, off.failedWant, off.failedGot, off.why);
  }
  if (active.identity) probeSnapDiff(label, active);
  if (!offUnconfirmed) probeIdleSamples(mask, label, "after");
  char diff[40];
  snprintf(diff, sizeof diff, "after %s", label);
  if (!offUnconfirmed) probeSnapDiff(diff, probeSnapshot(false));
  publishState(this);
  const bool alternate = mask == kPianoChannelMask && !flash && ended == Reason::channelOpen && gArgs.probeSpare;
  if (alternate && !offUnconfirmed) note("probe %s: open on factory pair, proceeding to the alternate-pair check", label);
  if (offUnconfirmed) {
    note("probe %s: stopped with OFF UNCONFIRMED", label);
    return ProbeResult::stopped;
  }
  if (ended != Reason::test && !alternate) note("probe %s: stopped: %s", label, reasonName(ended));
  if (alternate) return ProbeResult::alternate;
  return ended == Reason::test ? ProbeResult::complete : ProbeResult::stopped;
}

static void logLadderSample(unsigned step, unsigned ma, const char *phase, unsigned elapsed,
                            const Controller::Live &live, const Temperatures &t)
{
  char faults[80], channels[128];
  char flashTemp[24], socTemp[24], batteryTemp[24];
  formatFaults(decodeFaults(live.status1, live.status2, live.status3, kPianoChannelMask), faults, sizeof faults);
  formatChannels(live.status1, live.status3, kPianoChannelMask, channels, sizeof channels);
  temperatureText(t, flashTemp, socTemp, batteryTemp, sizeof flashTemp);
  note("ladder poll: step=%u ma=%u phase=%s t+%u ms STATUS1=%02x %s INT_RT_STS(0x10)=%02x status2=%02x status3=%02x enables=%02x/%02x faults=%s temp flash=%s SoC=%s battery=%s%s",
       step, ma, phase, elapsed, live.status1, channels, live.rt, live.status2, live.status3, live.moduleEn, live.chanEn, faults,
       flashTemp, socTemp, batteryTemp, live.ok ? "" : " status unreadable");
}

// Each step runs for ten seconds lit and ten seconds dark. Either phase stops on the first fault or unreadable status.
void PianoTorch::ladder()
{
  note("ladder: 10 s lit + 10 s dark per step, status/fault and temperatures every 500 ms; cap %u mA", gArgs.maxMa);
  unsigned step = 0;
  for (const unsigned ma : kLadderLevelsMa) {
    if (ma > gArgs.maxMa) {
      note("ladder: %u mA and higher steps skipped under the %u mA cap", ma, gArgs.maxMa);
      return;
    }
    ++step;
    const Temperatures t0 = readTemperatures(nowMs());
    IOLockLock(gLock);
    const uint64_t start = nowMs();
    Torch::Outcome on;
    if (gTorch.diagnosticAbort() == Reason::none) on = gTorch.request(start, ma, kLadderWindowSeconds, t0);
    else on.reason = gTorch.diagnosticAbort();
    if (on.lit) gTorch.setTestMode(true);
    gTorch.describe(start, gState, sizeof gState);
    IOLockUnlock(gLock);
    note("ladder step %u: request %u mA (2 x %u mA)", step, ma, ma / 2);
    logOutcome("ladder lit", on, ma);
    publishState(this);
    if (!on.lit) {
      note("ladder stopped at %u mA: %s", ma, reasonName(on.reason));
      return;
    }

    uint64_t darkStart = 0;
    for (unsigned elapsed = 0;; elapsed += kProbePollMs) {
      const uint64_t now = nowMs(), due = start + elapsed;
      if (now < due) IOSleep(unsigned(due - now));
      const Temperatures t = readTemperatures(nowMs());
      IOLockLock(gLock);
      if (gTorch.diagnosticAbort() != Reason::none) {
        IOLockUnlock(gLock);
        return;
      }
      const uint64_t sample = nowMs();
      const Torch::Outcome poll = gTorch.poll(sample, t, /*ladder=*/true);
      darkStart = nowMs();
      gTorch.describe(darkStart, gState, sizeof gState);
      IOLockUnlock(gLock);
      if (poll.sampled) logLadderSample(step, ma, "lit", unsigned(sample - start), poll.live, t);
      if (poll.lit) continue;
      logOutcome("ladder lit window", poll, 0);
      publishState(this);
      if (poll.reason != Reason::test || !poll.steps.ok) return;
      break;
    }

    for (unsigned elapsed = 0; elapsed <= kLadderWindowMs; elapsed += kProbePollMs) {
      const uint64_t now = nowMs(), due = darkStart + elapsed;
      if (now < due) IOSleep(unsigned(due - now));
      const Temperatures t = readTemperatures(nowMs());
      IOLockLock(gLock);
      if (gTorch.diagnosticAbort() != Reason::none) {
        IOLockUnlock(gLock);
        return;
      }
      const uint64_t sample = nowMs();
      const Controller::Live live = gTorch.controller().live();
      const Reason fault = ladderDarkFault(live, t, gTorch.config());
      Torch::Outcome off;
      if (fault != Reason::none) {
        off = gTorch.turnOff(sample, fault);
        gTorch.describe(sample, gState, sizeof gState);
      }
      IOLockUnlock(gLock);
      logLadderSample(step, ma, "dark", unsigned(sample - darkStart), live, t);
      if (fault != Reason::none) {
        logOutcome("ladder dark window", off, 0);
        publishState(this);
        return;
      }
    }
    note("ladder step %u complete: %u mA lit 10 s, dark 10 s", step, ma);
  }
  note("ladder complete: all levels passed without a fault or guard");
}

// The control loop: requests as they come; while lit, a poll every second (the time limit, the guard).
void PianoTorch::serve()
{
  uint64_t lastLogMs = 0;
  uint8_t lastStatus[3] = {0, 0, 0};
  for (;;) {
    IOLockLock(gLock);
    if (!gReq.pending) {
      uint64_t deadline = 0;
      const bool active = gTorch.operational() && (gTorch.lit() || gTorch.needsOff());
      const uint64_t now = nowMs();
      const uint64_t due = gTorch.deadlineMs();
      const unsigned wait = gTorch.lit() && due < now + 1000 ? (due > now ? unsigned(due - now) : 1) : 1000;
      clock_interval_to_deadline(active ? wait : 60000, kMillisecondScale, &deadline);
      IOLockSleepDeadline(gLock, &gReq, deadline, THREAD_UNINT);
    }
    const bool work = gTorch.operational() && (gReq.pending || gTorch.lit() || gTorch.needsOff());
    IOLockUnlock(gLock);
    if (!work) continue;

    const Temperatures t = readTemperatures(nowMs());
    IOLockLock(gLock);
    const uint64_t now = nowMs();
    const Request r = gReq;
    gReq.pending = false;
    Torch::Outcome o;
    bool acted = false, polled = false;
    if (r.pending && gTorch.operational()) {
      const bool test = r.source && !strcmp(r.source, "the boot test");
      o = gTorch.request(now, r.ma, r.seconds, t);
      if (o.lit) gTorch.setTestMode(test);   // the boot test's end is logged as such; any later request is the user's
      acted = true;
    } else if (gTorch.operational() && (gTorch.lit() || gTorch.needsOff())) {
      o = gTorch.poll(now, t);
      polled = true;
    }
    gTorch.describe(now, gState, sizeof gState);
    char state[sizeof gState];
    strlcpy(state, gState, sizeof state);
    IOLockUnlock(gLock);

    if (acted) {
      char what[96];
      snprintf(what, sizeof what, "%s, %u mA %u s", r.source, r.ma, r.seconds);
      logOutcome(what, o, r.ma);
      if (t.socValid || t.batteryValid)
        note("temperatures: SoC %d.%d C%s, battery %d.%02d C%s", t.socDeci / 10, t.socDeci % 10, t.socValid ? "" : " (none)",
             t.batteryCenti / 100, t.batteryCenti % 100, t.batteryValid ? "" : " (none)");
      lastLogMs = now;
      lastStatus[0] = o.live.status1;
      lastStatus[1] = o.live.status2;
      lastStatus[2] = o.live.status3;
    } else if (polled) {
      if (!o.lit) {
        logOutcome("guard", o, 0);
      } else if (now - lastLogMs >= 30000 || o.live.status1 != lastStatus[0] || o.live.status2 != lastStatus[1] ||
                 o.live.status3 != lastStatus[2]) {
        note("lit: %s; status %02x %02x %02x rt %02x enables %02x %02x; SoC %d.%d C battery %d.%02d C", state, o.live.status1,
             o.live.status2, o.live.status3, o.live.rt, o.live.moduleEn, o.live.chanEn, t.socValid ? t.socDeci / 10 : 0,
             t.socValid ? t.socDeci % 10 : 0, t.batteryValid ? t.batteryCenti / 100 : 0, t.batteryValid ? t.batteryCenti % 100 : 0);
        lastLogMs = now;
        lastStatus[0] = o.live.status1;
        lastStatus[1] = o.live.status2;
        lastStatus[2] = o.live.status3;
      }
    }
    publishState(this);
  }
}

// Verify OFF synchronously before acknowledging the power transition.
void PianoTorch::offNow(Reason why, const char *what)
{
  if (!gLock) return;
  IOLockLock(gLock);
  gReq.pending = false;
  const bool act = gReady && (gTorch.lit() || gTorch.needsOff());
  const Torch::Outcome o = gTorch.quiesce(nowMs(), why);
  if (act) gTorch.describe(nowMs(), gState, sizeof gState);
  IOLockUnlock(gLock);
  if (act) {
    if (why == Reason::sleep && !o.steps.ok)
      pianopower::cancelSystemSleep(kServiceClass, "torch OFF unconfirmed");
    logOutcome(what, o, 0);
    publishState(this);
  }
}

IOReturn PianoTorch::setPowerState(unsigned long state, IOService *)
{
  if (state == pianopower::kOff) offNow(Reason::sleep, "system sleep");
  else if (state == pianopower::kOn && gLock) {
    IOLockLock(gLock);
    gTorch.resume();
    IOLockWakeup(gLock, &gReq, false);
    IOLockUnlock(gLock);
  }
  return pianopower::kAckNow;
}

void PianoTorch::systemWillShutdown(IOOptionBits specifier)
{
  offNow(Reason::shutdown, "shutdown or restart");
  IOService::systemWillShutdown(specifier);
}

// An administrator's request (IORegistryEntrySetCFProperties): "piano-torch-set" = mA (0 off) or a boolean, "piano-torch-seconds"
// optional. Queued for the thread; the result is in "piano-torch-state".
IOReturn PianoTorch::setProperties(OSObject *properties)
{
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d) return kIOReturnBadArgument;
  OSObject *set = d->getObject(kPropertySet);
  if (!set) return kIOReturnUnsupported;
  if (gArgs.level < 2 || !gLock) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
  unsigned ma;
  if (OSNumber *n = OSDynamicCast(OSNumber, set)) ma = boundedRequest(n->unsigned64BitValue(), kHardMaxMa);
  else if (OSBoolean *b = OSDynamicCast(OSBoolean, set)) ma = b->isTrue() ? gArgs.ma : 0;
  else return kIOReturnBadArgument;
  OSObject *duration = d->getObject(kPropertySeconds);
  OSNumber *seconds = OSDynamicCast(OSNumber, duration);
  if (duration && !seconds) return kIOReturnBadArgument;
  IOLockLock(gLock);
  const bool ready = gReady && gTorch.operational() && !gBootDiagnostic;
  if (ready) queueLocked(ma, seconds ? boundedRequest(seconds->unsigned64BitValue(), gTorch.config().maxS) : 0, "setProperties");
  IOLockUnlock(gLock);
  return ready ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn PianoTorch::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  const bool set = name && name->isEqualTo(kFunctionSet), toggle = name && name->isEqualTo(kFunctionToggle),
             status = name && name->isEqualTo(kFunctionStatus);
  if (!set && !toggle && !status) return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
  if (!gLock) return kIOReturnNotReady;
  if (status) {
    char *out = static_cast<char *>(p1);
    const size_t size = size_t(uintptr_t(p2));
    if (!out || !size) return kIOReturnBadArgument;
    IOLockLock(gLock);
    strlcpy(out, gState, size);
    IOLockUnlock(gLock);
    return kIOReturnSuccess;
  }
  IOLockLock(gLock);
  const bool ready = gReady && gTorch.operational() && !gBootDiagnostic;
  if (ready) {
    if (set)
      queueLocked(boundedRequest(uintptr_t(p1), kHardMaxMa), boundedRequest(uintptr_t(p2), gTorch.config().maxS),
                  p3 ? static_cast<const char *>(p3) : "a driver");
    else {
      const bool lit = gReq.pending ? gReq.ma != 0 : gTorch.lit();
      queueLocked(lit ? 0 : gArgs.ma, 0, p1 ? static_cast<const char *>(p1) : "a driver");
    }
  }
  IOLockUnlock(gLock);
  return ready ? kIOReturnSuccess : kIOReturnNotReady;
}
