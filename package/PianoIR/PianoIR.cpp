// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoIR: piano's IR transmitter (the MOSI line of QUP2 SE7 "SE15", GPIO29, active high) for macOS 27, native at EL1. Facts:
// IrHw.h; the waveform: IrWave.h; the design and the tablet checklist: docs/drivers/ir.md.
//   piano-ir=1  a read-only survey, logged as "PianoIR: ..." lines: the four pins as found, GCC's QUP2 vote word and the five
//               branches, the s7 clock (its DFS table), SE15's registers only when every branch runs, the QUP2 version, the SMMU
//               entry of stream 0x423. No store.
//   piano-ir=2  the survey, then the transmitter: our votes (only ever added, under PianoA830's GCC lock word), stream 0x423
//               through a bypass bank (SE DMA; on the matching thread), the GENI engine in SE DMA mode (or FIFO mode if its FIFO
//               is enabled), the clock (a DFS level and divider for ~1.92 MHz), an emission-free loopback (GPIO29 stays a GPIO
//               driven low: the engine's output never reaches the pad), GPIO29 parked low. Then sends from an administrator
//               (tooling/native/piano-ir, PianoIRClient.h): each one checked and encoded (IrWave.h), GPIO29 handed to the engine
//               for the transfer only and parked low again once the engine is idle.
// Without piano-ir the driver declines in probe() and maps nothing.
// Other arguments: piano-ir-test=N (0..20: N self-test bursts after the start, 500 ms of 38 kHz carrier then 1.5 s dark, and one
// NEC frame), piano-ir-sample=1 (sample GPIO29's pad while each send runs: edges and high share against the waveform's),
// piano-ir-smmu=0 (leave the SMMU alone), piano-ir-pins=4 (hand all four SE15 pins to the engine, as Linux does; default: MOSI
// only), piano-ir-delay=S (0..600, before the survey).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoPowerContract.h>
extern "C" {
#include "GeniSpi.h"
}
#include "IrEngine.h"
#include "SmmuStream.h"
#include "PianoIRClient.h"

using namespace pianoir;

extern "C" {
volatile int32_t piano_ir_stage = 0;   // post-mortem breadcrumb: the last stage reached, negative when it failed there
}
extern volatile uint32_t sm8750_gcc_vote_owner;   // PianoA830's GCC vote lock word (0 free, else the holder's tag)

namespace {

struct Args {
  uint32_t level, test, sample, bus, smmu, pins, delay;
};

uint64_t bootArg(const char *name, uint64_t fallback)
{
  uint64_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampArg(uint64_t v, uint32_t hi) { return v > hi ? hi : uint32_t(v); }
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoIR: %s\n", line);
}
void stage(int32_t value, const char *what)
{
  piano_ir_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}

struct Region {
  IOMemoryMap *map = nullptr;
  uintptr_t va = 0;
};
constexpr unsigned kOthers[3] = {kMiso, kClk, kCs};
constexpr unsigned kSpinPolls = 300;

struct PadStats {
  uint32_t samples, highs, edges;
  bool last;
};

// Appends to a fixed line; never past its end.
void append(char *line, size_t size, size_t &used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void append(char *line, size_t size, size_t &used, const char *fmt, ...)
{
  if (used >= size) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line + used, size - used, fmt, ap);
  va_end(ap);
  if (n > 0) used = used + size_t(n) < size ? used + size_t(n) : size - 1;
}
void dmaCache(void *bytes, size_t size, unsigned op)
{
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = 4ull << ((ctr >> 16) & 15), end = uintptr_t(bytes) + size;
  for (uintptr_t at = uintptr_t(bytes) & ~(line - 1); at < end; at += line) {
    if (op == 0) __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    else if (op == 1) __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    else __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t n) { dmaCache(p, n, 0); }
void dmaCleanInval(void *, void *p, size_t n) { dmaCache(p, n, 1); }
void dmaInval(void *, void *p, size_t n) { dmaCache(p, n, 2); }

}  // namespace

class PianoIR : public IOService {
  OSDeclareDefaultStructors(PianoIR);

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;

private:
  Args gArgs = {};
  Region gGcc, gSe, gQup, gSmmu, gPinPage[kPins];
  uint32_t gPinFound[kPins][2] = {};
  uint32_t gOtherCtl[kPins] = {};
  unsigned gLoads = 0, gStores = 0, gMapFailures = 0;
  unsigned gDelayCalls = 0;
  bool gSampling = false, gMosiMuxed = false;
  uint64_t gPollDeadline = 0;
  PadStats gPad = {};
  geni_spi gSpi = {};
  IOBufferMemoryDescriptor *gDma = nullptr;
  uint32_t gRate = 0;
  bool gSmmuReady = false, gDmaIdle = true;
  IOLock *gLock = nullptr;
  bool gReady = false, gAsleep = false, gStopping = false, gWorker = false, gPm = false;
  uint8_t *gWave = nullptr;
  uint32_t gSends = 0, gFailures = 0;
  char gLast[200] = "none", gState[200] = "starting";

  static void thread(void *self, wait_result_t);
  static uint32_t seRead(void *ctx, uint32_t reg);
  static void seWrite(void *ctx, uint32_t reg, uint32_t value);
  static void seDelay(void *ctx, unsigned us);
  static uint32_t qupRead(void *ctx, uint32_t reg);
  uint32_t rd32(uintptr_t address);
  void wr32(uintptr_t address, uint32_t value);
  bool mapRegion(Region &region, uint64_t pa, uint64_t bytes);
  uint32_t pinRd(unsigned pin, uint32_t reg);
  void pinWr(unsigned pin, uint32_t reg, uint32_t value);
  bool parkMosi();
  void mosiToEngine();
  void restoreOtherPins();
  void padSample();
  uint32_t gcc(uint32_t off) { return rd32(gGcc.va + off); }
  bool allBranchesOn(uint32_t *offValue = nullptr);
  VoteResult voteClocks();

  // The worker, transfers and power callbacks serialize hardware and buffer access with gLock.
  void run();
  void quiesce();
  bool waitUntil(uint64_t deadline, bool stopOnSleep);
  void survey();
  void surveySmmu(bool apply);
  bool geniUp();
  bool geniLoopback();
  bool allocateDma();
  void releaseDma();
  int finishTransfer(int rc, size_t bytes, bool loopback);
  IOReturn send(const uint32_t *list, unsigned count, uint32_t carrier, uint32_t duty, const char *who);
  IOReturn sendBurst(uint32_t ms, const char *who);
  int transmit(const ir_wave_plan &plan, char *detail, size_t size);
  void selfTest();
  void publish();
  void setState(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

OSDefineMetaClassAndStructors(PianoIR, IOService);

IOService *PianoIR::probe(IOService *provider, SInt32 *score)
{
  const uint64_t level = bootArg("piano-ir", 0);
  if (!piano_ir_enabled(level)) return nullptr;
  gArgs.level = uint32_t(level);
  return IOService::probe(provider, score);
}

bool PianoIR::start(IOService *provider)
{
  if (!piano_ir_enabled(gArgs.level) || !IOService::start(provider)) return false;
  gArgs.test = piano_ir_boot_test_count(bootArg("piano-ir-test", 0));
  gArgs.sample = bootArg("piano-ir-sample", 0) == 1;
  gArgs.bus = clampArg(bootArg("piano-ir-bus", 1), 2);
  gArgs.smmu = bootArg("piano-ir-smmu", 1) == 1;
  gArgs.pins = bootArg("piano-ir-pins", 1) == 4 ? 4 : 1;
  gArgs.delay = clampArg(bootArg("piano-ir-delay", 0), 600);

  gLock = IOLockAlloc();
  if (gArgs.level == 2) gWave = static_cast<uint8_t *>(IOMalloc(IR_MAX_WAVE_BYTES));
  if (!gLock || (gArgs.level == 2 && !gWave)) {
    stage(-1, "no lock or waveform buffer");
    return false;
  }
  note("start: piano-ir=%u, test %u, sample %u, smmu %u, pins %u, delay %u s",
       gArgs.level, gArgs.test, gArgs.sample, gArgs.smmu, gArgs.pins, gArgs.delay);
  setProperty(kPianoIRLevel, gArgs.level, 32);
  if (gArgs.level == 2) {
    if (pianopower::join(this) != kIOReturnSuccess) {
      PMstop();
      stage(-1, "could not join the power tree");
      return false;
    }
    gPm = true;
  }

  IOLockLock(gLock);
  thread_t t = nullptr;
  retain();
  if (kernel_thread_start(&PianoIR::thread, this, &t) != KERN_SUCCESS) {
    IOLockUnlock(gLock);
    release();
    if (gPm) {
      PMstop();
      gPm = false;
    }
    stage(-1, "could not start the survey thread");
    return false;
  }
  gWorker = true;
  thread_deallocate(t);
  // Stream-match accesses belong on the matching thread (NativeUfs and PianoTouch).
  if (!gStopping && !gAsleep) surveySmmu(gArgs.level == 2 && gArgs.bus <= 1 && gArgs.smmu);
  IOLockUnlock(gLock);
  registerService();
  return true;
}

void PianoIR::thread(void *ctx, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-ir");
  auto *self = static_cast<PianoIR *>(ctx);
  IOLockLock(self->gLock);
  self->run();
  self->gWorker = false;
  IOLockWakeup(self->gLock, &self->gWorker, false);
  IOLockUnlock(self->gLock);
  self->release();
  thread_terminate(current_thread());
}

void PianoIR::quiesce()
{
  if (!gLock) return;
  IOLockLock(gLock);
  gStopping = true;
  gReady = false;
  IOLockWakeup(gLock, &gAsleep, false);
  while (gWorker) IOLockSleep(gLock, &gWorker, THREAD_UNINT);
  releaseDma();
  IOLockUnlock(gLock);
}

void PianoIR::stop(IOService *provider)
{
  quiesce();
  if (gPm) {
    PMstop();
    gPm = false;
  }
  IOService::stop(provider);
}

void PianoIR::free()
{
  releaseDma();
  Region *regions[] = {&gGcc, &gSe, &gQup, &gSmmu, &gPinPage[kMiso], &gPinPage[kMosi], &gPinPage[kClk], &gPinPage[kCs]};
  for (Region *region : regions)
    if (region->map) region->map->release();
  if (gWave) IOFree(gWave, IR_MAX_WAVE_BYTES);
  if (gLock) IOLockFree(gLock);
  IOService::free();
}

void PianoIR::setState(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(gState, sizeof gState, fmt, ap);
  va_end(ap);
  publish();
}

void PianoIR::publish()
{
  setProperty(kPianoIRState, gState);
  setProperty(kPianoIRLast, gLast);
  setProperty(kPianoIRSends, gSends, 32);
}

// Explicit ldr/str (Qualcomm's trapped MMIO wants every store from a general register).
uint32_t PianoIR::rd32(uintptr_t address)
{
  uint32_t value;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
  ++gLoads;
  return value;
}

void PianoIR::wr32(uintptr_t address, uint32_t value)
{
  __asm__ volatile("str %w0, [%1]" ::"r"(value), "r"(address) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
  ++gStores;
}

bool PianoIR::mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (r.map && r.map->getVirtualAddress()) {
    r.va = uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base);
    return true;
  }
  if (r.map) r.map->release();
  r.map = nullptr;
  ++gMapFailures;
  return false;
}

uint32_t PianoIR::pinRd(unsigned p, uint32_t reg) { return rd32(gPinPage[p].va + reg); }
void PianoIR::pinWr(unsigned p, uint32_t reg, uint32_t v) { wr32(gPinPage[p].va + reg, v); }

bool PianoIR::parkMosi()
{
  gMosiMuxed = false;
  return parkEmitter(gPinFound[kMosi][0], [this](uint32_t reg) { return pinRd(kMosi, reg); },
                     [this](uint32_t reg, uint32_t value) { pinWr(kMosi, reg, value); });
}

void PianoIR::mosiToEngine()
{
  if (gArgs.pins == 4)
    for (unsigned p : kOthers) {
      gOtherCtl[p] = pinRd(p, kCtl);
      pinWr(p, kCtl, pinCtl(gOtherCtl[p], kFunctionQup, kDrive6mA, kNoPull, false));
    }
  gMosiMuxed = true;
  pinWr(kMosi, kCtl, pinCtl(gPinFound[kMosi][0], kFunctionQup, kDrive6mA, kNoPull, false));
}

void PianoIR::restoreOtherPins()
{
  if (gArgs.pins == 4)
    for (unsigned p : kOthers) pinWr(p, kCtl, gOtherCtl[p]);
}

bool PianoIR::allBranchesOn(uint32_t *offValue)
{
  for (const Branch &b : kBranches) {
    const uint32_t value = gcc(b.cbcr);
    if (!branchOn(value)) {
      if (offValue) *offValue = value;
      return false;
    }
  }
  return true;
}

VoteResult PianoIR::voteClocks()
{
  return addClockVotes([this](uint32_t reg) { return gcc(reg); },
                       [this](uint32_t reg, uint32_t value) { wr32(gGcc.va + reg, value); },
                       [] {
                         uint32_t expected = 0;
                         return __atomic_compare_exchange_n(&sm8750_gcc_vote_owner, &expected, kGccVoteTag, 0,
                                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
                       },
                       [] { __atomic_store_n(&sm8750_gcc_vote_owner, 0u, __ATOMIC_RELEASE); }, [] { IODelay(10); });
}

void PianoIR::padSample()
{
  const bool high = pinRd(kMosi, kIo) & 1;
  if (gPad.samples && high != gPad.last) ++gPad.edges;
  gPad.highs += high;
  gPad.last = high;
  ++gPad.samples;
}

uint32_t PianoIR::seRead(void *ctx, uint32_t reg)
{
  auto *self = static_cast<PianoIR *>(ctx);
  return self->rd32(self->gSe.va + reg);
}

void PianoIR::seWrite(void *ctx, uint32_t reg, uint32_t value)
{
  auto *self = static_cast<PianoIR *>(ctx);
  if (reg == SE_GENI_M_CMD_CTRL_REG && self->gMosiMuxed) self->parkMosi();
  self->wr32(self->gSe.va + reg, value);
}

void PianoIR::seDelay(void *ctx, unsigned us)
{
  auto *self = static_cast<PianoIR *>(ctx);
  if (nowUs() >= self->gPollDeadline) {
    if (self->gMosiMuxed) self->parkMosi();
    return;
  }
  if (self->gSampling) self->padSample();
  else if (!self->gSpi.use_dma || ++self->gDelayCalls <= kSpinPolls) IODelay(us);
  else IOSleep(1);
}

uint32_t PianoIR::qupRead(void *ctx, uint32_t reg)
{
  auto *self = static_cast<PianoIR *>(ctx);
  return self->rd32(self->gQup.va + reg);
}

// ------------------------------------------------------------------------------------------------------------- survey
void PianoIR::surveySmmu(bool apply)
{
  if (!mapRegion(gSmmu, kSmmu, kSmmuBytes)) {
    note("smmu: cannot map %#llx", (unsigned long long)kSmmu);
    return;
  }
  const auto read = [this](uint32_t off) { return rd32(gSmmu.va + off); };
  const pianousb::SmmuPlan p = pianousb::planSmmuStream(kQup2Stream, kSmmuBytes, read);
  note("smmu: stream %#x: groups %u banks %u pages %u; %s slot %u (smr %08x s2cr %08x; bank sctlr %08x cbar %08x cba2r %08x) -> %s bank %u%s%s",
       kQup2Stream, p.groups, p.banks, p.pages, p.reuse ? "existing entry" : "free", p.slot, p.smr, p.s2cr, p.sctlr, p.cbar, p.cba2r,
       p.keep ? "keep" : "bypass", p.bank, p.error ? ": " : "", p.error ? p.error : "");
  if (!p.error && p.keep) gSmmuReady = true;
  if (!apply || p.error || p.keep) {
    if (apply && p.error) note("smmu: no bypass for stream %#x: SE DMA unusable", kQup2Stream);
    return;
  }
  const uint64_t deadline = nowUs() + 100000;
  const char *failure = pianousb::applySmmuStream(p, kQup2Stream, read, [this](uint32_t off, uint32_t v) { wr32(gSmmu.va + off, v); },
                                                  [deadline] { return nowUs() >= deadline; });
  note("smmu: stream %#x: %s (slot %u: smr %08x s2cr %08x)", kQup2Stream, failure ? failure : "bypass set", p.slot,
       read(0x800 + 4 * p.slot), read(0xc00 + 4 * p.slot));
  gSmmuReady = failure == nullptr;
}

void PianoIR::survey()
{
  const unsigned storesBefore = gStores, loadsBefore = gLoads;
  char line[240] = {};
  size_t used = 0;
  for (unsigned p = 0; p < kPins; ++p) {
    if (!mapRegion(gPinPage[p], kTlmm + uint64_t(kGpio[p]) * kTlmmStride, 0x10)) {
      note("pins: gpio%u does not map", kGpio[p]);
      continue;
    }
    gPinFound[p][0] = pinRd(p, kCtl);
    gPinFound[p][1] = pinRd(p, kIo);
    append(line, sizeof line, used, "%sgpio%u ctl %08x io %08x (function %u, %u mA, %s)",
                     p ? "; " : "", kGpio[p], gPinFound[p][0], gPinFound[p][1], pinFunction(gPinFound[p][0]), pinDriveMa(gPinFound[p][0]),
                     pinOutput(gPinFound[p][0]) ? (gPinFound[p][1] & 2 ? "out high" : "out low") : "input");
  }
  note("pins as found: %s", line);
  note("pins: gpio29 (the emitter) %s", mosiDark(gPinFound[kMosi][0], gPinFound[kMosi][1]) ? "dark (GPIO, not driven high)" :
       pinFunction(gPinFound[kMosi][0]) == kFunctionQup ? "with the engine (qup2_se7)" : "DRIVEN HIGH or another function");
  if (!mapRegion(gGcc, kGcc, kGccBytes)) {
    note("gcc: does not map");
    return;
  }
  used = 0;
  for (const Branch &b : kBranches) {
    const uint32_t v = gcc(b.cbcr);
    append(line, sizeof line, used, "%s%s %08x (%s)", used ? ", " : "", b.name, v,
                     (v & kCbcrHwCtl) && (v & kCbcrClkOff) ? "hw-gated" : branchOn(v) ? "on" : "off");
  }
  const uint32_t vote = gcc(kGccQup2Vote);
  note("gcc: vote %#x = %08x (bits %08x %s); %s", kGccQup2Vote, vote, kVoteBits, (vote & kVoteBits) == kVoteBits ? "all set" : "not all set", line);
  const S7Clock c = s7Clock([this](uint32_t off) { return gcc(off); });
  used = 0;
  for (unsigned l = 0; l < c.levels; ++l)
    append(line, sizeof line, used, "%s%u:%u", l ? " " : "", l, c.hz[l]);
  note("clock: s7 rcg cmd %08x dfs %08x (%s): rates %s Hz", c.cmd, c.dfs, c.dfsOn ? "DFS on, 8 levels" : "DFS off, the current rate", line);
  if (allBranchesOn() && mapRegion(gSe, kSe15, kSe15Bytes) && mapRegion(gQup, kQup2, kQup2Bytes)) {
    geni_spi probe = {};
    probe.se = {this, seRead, nullptr, nullptr};
    probe.wrapper = {this, qupRead, nullptr, nullptr};
    geni_spi_read_info(&probe);
    const geni_spi_info &in = probe.info;
    note("se15: FW %08x (protocol %u%s), IF_DISABLE %08x (FIFO %s), STATUS %08x, HW_PARAM_0 %08x (TX FIFO %u words of %u bits), QUP %08x",
         in.fw_revision, in.protocol, in.protocol == GENI_PROTO_SPI ? ": SPI" : in.protocol == GENI_PROTO_NONE ? ": none" : "", in.if_disable,
         in.fifo_disabled ? "disabled: SE DMA" : "enabled", in.geni_status, in.hw_param0, in.tx_fifo_depth, in.fifo_width_bits, in.qup_hw_version);
    snprintf(line, sizeof line, "protocol %u, FIFO %s, rates %u..%u Hz (%s), gpio29 ctl %08x", in.protocol, in.fifo_disabled ? "disabled" : "enabled",
             c.hz[0], c.hz[c.levels - 1], c.dfsOn ? "DFS" : "fixed", gPinFound[kMosi][0]);
  } else {
    note("se15: not read (a QUP2/s7 branch is off: an unclocked access stalls the bus)");
    snprintf(line, sizeof line, "se15 not clocked, vote %08x, gpio29 ctl %08x", vote, gPinFound[kMosi][0]);
  }
  setProperty(kPianoIRSurvey, line);
  note("survey done: %u MMIO loads, %u stores; %u map failures", gLoads - loadsBefore, gStores - storesBefore, gMapFailures);
}

// ------------------------------------------------------------------------------------------------------------- bring-up
bool PianoIR::geniUp()
{
  const VoteResult v = voteClocks();
  bool on = false;
  uint32_t off = 0;
  for (unsigned n = 0; n < 200 && !(on = allBranchesOn(&off)); ++n) IODelay(10);
  note("gcc: votes %08x -> %08x (%s, %u lock waits); branches %s", v.before, v.after,
       v.locked ? "checked under lock" : "no write", v.waits, on ? "on" : "off");
  if (!on || (v.after & kVoteBits) != kVoteBits) {
    note("geni: a branch stays off (CBCR %08x): SE15 not touched", off);
    return false;
  }
  if (!mapRegion(gSe, kSe15, kSe15Bytes) || !mapRegion(gQup, kQup2, kQup2Bytes)) {
    note("geni: SE15 or QUP2 does not map");
    return false;
  }
  gSpi.se = {this, seRead, seWrite, seDelay};
  gSpi.wrapper = {this, qupRead, nullptr, nullptr};
  if (int rc = geni_spi_read_info(&gSpi)) {
    note("geni: read_info %d", rc);
    return false;
  }
  const geni_spi_info &in = gSpi.info;
  note("se15: FW %08x (protocol %u), IF_DISABLE %08x (FIFO %s), STATUS %08x, HW_PARAM_0 %08x, QUP %08x", in.fw_revision, in.protocol,
       in.if_disable, in.fifo_disabled ? "disabled: SE DMA" : "enabled", in.geni_status, in.hw_param0, in.qup_hw_version);
  if (in.protocol != GENI_PROTO_SPI) {
    note("geni: SE15 holds no SPI firmware (protocol %u)", in.protocol);
    return false;
  }
  if (in.geni_status & GENI_STATUS_M_ACTIVE) {
    note("geni: a command is running on SE15 (not ours): left alone");
    return false;
  }
  // The clock: the s7 rates (DFS levels) divided by the GENI divider; the waveform is encoded at the rate obtained.
  const S7Clock c = s7Clock([this](uint32_t off) { return gcc(off); });
  const uint32_t major = in.qup_hw_version >> 28, minor = (in.qup_hw_version >> 16) & 0xfff;
  const uint32_t os = (major == 1 && minor == 0) ? 2 : 1;
  uint32_t perOs[kRcgDfsLevels];
  for (unsigned l = 0; l < c.levels; ++l) perOs[l] = c.hz[l] / os;
  ir_clock_pick pick = {};
  if (!ir_pick_clock(perOs, c.levels, &pick)) {
    note("clock: no s7 rate gives 1..4 MHz with an integer divider (rates decoded: %u levels)", c.levels);
    return false;
  }
  if (in.fifo_disabled) {
    if (!gSmmuReady) {
      note("geni: FIFO disabled and stream %#x not bypassing: no SE DMA", kQup2Stream);
      return false;
    }
    gSpi.use_dma = 1;
    if (!allocateDma()) return false;
  }
  gSpi.poll_only = 1;   // completion polled; the engine's interrupt outputs off (nothing handles SPI 462)
  if (int rc = geni_spi_init(&gSpi)) {
    note("geni: init %d", rc);
    return false;
  }
  geni_spi_setup(&gSpi, 0, 0);   // mode 0, CS0, MSB first
  if (int rc = setSampleClock(gSpi, pick, c.hz[pick.level])) {
    note("clock: set_clock %d", rc);
    return false;
  }
  gRate = pick.rate;
  note("clock: level %u %u Hz%s, GENI divider %u -> %u Hz sample rate (%s 1.92 MHz)", pick.level, pick.source_hz * os,
       os == 2 ? " (oversampling 2)" : "", pick.divider, pick.rate, pick.exact ? "exactly" : "near");
  return geniLoopback();
}

bool PianoIR::allocateDma()
{
  if (!gSpi.use_dma || gDma) return gDmaIdle;

  constexpr uint32_t half = IR_MAX_WAVE_BYTES;
  gDma = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                                    IOOptionBits(kIOMemoryMapperNone), 2 * half, 4096);
  IOByteCount contiguous = 0;
  const uint64_t pa = gDma ? gDma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone) : 0;
  auto *va = gDma ? static_cast<uint8_t *>(gDma->getBytesNoCopy()) : nullptr;
  geni_spi::geni_dma dma = {nullptr, dmaClean, dmaCleanInval, dmaInval, nullptr, nullptr, 0, 0, 0};
  if (!setDmaBuffers(dma, va, pa, contiguous)) {
    note("geni: no aligned, contiguous DMA buffer");
    releaseDma();
    return false;
  }
  bzero(va, 2 * half);
  dmaCache(va, 2 * half, 1);
  gSpi.dma = dma;
  note("geni: SE DMA, %u-byte aligned-buffer halves", half);
  return true;
}

void PianoIR::releaseDma()
{
  if (!gDma || !gDmaIdle) return;
  gSpi.dma = {};
  gDma->release();
  gDma = nullptr;
}

// A completed transfer needs both the DMA completion reported by GeniSpi and an idle shifter.
int PianoIR::finishTransfer(int rc, size_t bytes, bool loopback)
{
  const TransferEnd end = pianoir::finishTransfer(
      gSpi, rc, bytes, loopback, gMosiMuxed, [this](uint32_t reg) { return seRead(this, reg); },
      [this](uint32_t reg, uint32_t v) { seWrite(this, reg, v); }, [] { IODelay(1); }, [this] { return parkMosi(); });
  gDmaIdle = end.reusable;
  // Keep DMA pages pinned if a wedged engine could still access them.
  if (!gDmaIdle) {
    gReady = false;
    setState("disabled: GENI did not quiesce");
  }
  if (!end.parked) {
    gReady = false;
    setState("disabled: GPIO29 park readback failed");
  }
  return end.status;
}

bool PianoIR::geniLoopback()
{
  if (!beginLoopback(gSpi, [this] { return parkMosi(); })) {
    note("loopback refused: could not park GPIO29 or configure GENI");
    return false;
  }
  uint8_t tx[64], rx[64] = {};
  for (unsigned i = 0; i < sizeof tx - 1; ++i) tx[i] = uint8_t(0xa5 ^ (i * 37));
  tx[sizeof tx - 1] = 0;
  gDelayCalls = 0;
  gSpi.snap_valid = 0;
  gSpi.poll_limit = kSpinPolls + 200;
  const uint64_t t0 = nowUs();
  gPollDeadline = t0 + 20000;
  gDmaIdle = !gSpi.use_dma;
  const int rc = finishTransfer(geni_spi_transfer(&gSpi, tx, rx, sizeof tx, 0), sizeof tx, true);
  if (gDmaIdle) geni_spi_setup(&gSpi, 0, 0);
  unsigned same = 0;
  if (!rc)
    for (unsigned i = 0; i < sizeof tx; ++i) same += rx[i] == tx[i];
  note("loopback %s (rc %d, %llu us): %u/64 bytes back, moved %u/%u (GPIO29 held as GPIO)",
       !rc && same == 64 ? "PASSED" : "FAILED", rc, (unsigned long long)(nowUs() - t0), same, gSpi.last_tx_in, gSpi.last_rx_in);
  if (rc && gSpi.snap_valid)
    note("loopback: M_IRQ %08x STATUS %08x DMA tx %08x rx %08x, moved %u/%u", gSpi.snap_irq, gSpi.snap_status, gSpi.snap_dma_tx,
         gSpi.snap_dma_rx, gSpi.snap_dma_txin, gSpi.snap_dma_rxin);
  return !rc && same == 64;
}

// gLock is reacquired by IOLockSleepDeadline. Startup waits through sleep; the
// boot emission test ends if the machine goes to sleep during its dark interval.
bool PianoIR::waitUntil(uint64_t deadline, bool stopOnSleep)
{
  while (!gStopping) {
    if (gAsleep) {
      if (stopOnSleep) return false;
      IOLockSleep(gLock, &gAsleep, THREAD_UNINT);
    } else if (mach_absolute_time() >= deadline) {
      return true;
    } else {
      IOLockSleepDeadline(gLock, &gAsleep, deadline, THREAD_UNINT);
    }
  }
  return false;
}

void PianoIR::run()
{
  uint64_t deadline;
  clock_interval_to_deadline(gArgs.delay, NSEC_PER_SEC, &deadline);
  if (!waitUntil(deadline, false)) return;
  stage(1, "survey (read only)");
  survey();
  if (gArgs.level < 2) {
    setState("level 1: survey only");
    return;
  }
  if (gArgs.bus > 1 || !gGcc.va || !gPinPage[kMosi].va) {
    stage(-2, "unsupported bus or a missing GCC/GPIO29 mapping");
    setState("not ready: GENI requires GCC and GPIO29 mappings");
    return;
  }
  if (gArgs.pins == 4)
    for (unsigned p : kOthers)
      if (!gPinPage[p].va) {
        stage(-2, "a requested SE15 pin did not map");
        setState("not ready: could not map all four pins");
        return;
      }
  stage(2, "transmitter: clocks, SE15, the loopback");
  if (!parkMosi() || !geniUp()) {
    releaseDma();
    stage(-2, "no usable GENI transmitter");
    setState("not ready: GENI initialization failed (see the console)");
    return;
  }
  stage(3, "ready");
  gReady = true;
  setState("ready: GENI SE15 at %u Hz", gRate);
  if (gArgs.test) selfTest();
}

// ------------------------------------------------------------------------------------------------------------- sends
// One encoded waveform on the wire (gLock held). detail gets the measurement.
int PianoIR::transmit(const ir_wave_plan &plan, char *detail, size_t size)
{
  const uint64_t wireUs = uint64_t(plan.bytes) * 8 * 1000000 / plan.rate;
  uint32_t off = 0;
  gPad = {};
  if (!allBranchesOn(&off)) {
    const VoteResult v = voteClocks();
    snprintf(detail, size, "a QUP2/s7 branch was off (CBCR %08x, vote %08x -> %08x): retry", off, v.before, v.after);
    return GENI_ERR_IO;
  }
  const uint32_t status = seRead(this, SE_GENI_STATUS);
  if (status & GENI_STATUS_M_ACTIVE) {
    snprintf(detail, size, "the engine is busy (STATUS %08x)", status);
    return GENI_ERR_IO;
  }
  if (!allocateDma()) {
    snprintf(detail, size, "DMA buffer unavailable");
    return GENI_ERR_IO;
  }
  if (!parkMosi()) {
    gReady = false;
    setState("disabled: GPIO29 park readback failed");
    snprintf(detail, size, "GPIO29 park readback failed");
    return GENI_ERR_IO;
  }

  gSampling = gArgs.sample;
  gDelayCalls = 0;
  gSpi.snap_valid = 0;
  gSpi.poll_limit = gSampling ? unsigned(4 * wireUs + 200000) : gSpi.use_dma ? unsigned(kSpinPolls + 50 + wireUs / 1000)
                                                                            : unsigned(50000 + wireUs);
  const uint64_t t0 = nowUs();
  gPollDeadline = t0 + wireUs + 20000;
  mosiToEngine();
  gDmaIdle = !gSpi.use_dma;
  const int rc = finishTransfer(geni_spi_transfer(&gSpi, gWave, nullptr, plan.bytes, 0), plan.bytes, false);
  restoreOtherPins();
  gSampling = false;
  const uint64_t elapsed = nowUs() - t0;
  if (rc)
    snprintf(detail, size, "GENI %d after %llu us, moved %u/%u, idle %s", rc, (unsigned long long)elapsed,
             gSpi.last_tx_in, plan.bytes, gDmaIdle ? "yes" : "unconfirmed");
  else
    snprintf(detail, size, "wire %llu us (expected %llu), moved %u of %u bytes", (unsigned long long)elapsed, (unsigned long long)wireUs,
             gSpi.use_dma ? gSpi.last_tx_in : plan.bytes, plan.bytes);
  return rc;
}

IOReturn PianoIR::send(const uint32_t *list, unsigned count, uint32_t carrier, uint32_t duty, const char *who)
{
  if (!gReady || gAsleep || gStopping) return kIOReturnNotReady;
  ir_wave_plan plan;
  const int e = ir_wave_encode(list, count, carrier, duty, gRate, gWave, IR_MAX_WAVE_BYTES, &plan);
  if (e < 0) {
    snprintf(gLast, sizeof gLast, "refused (%s): %s", who, ir_error_name(e));
    note("send (%s): refused: %s", who, ir_error_name(e));
    publish();
    return kIOReturnBadArgument;
  }
  char detail[200];
  const int rc = transmit(plan, detail, sizeof detail);
  ++gSends;
  if (rc) ++gFailures;
  char pad[120] = "";
  if (gArgs.sample)
    snprintf(pad, sizeof pad, "; pad: %u samples, %u edges, high %u%% (waveform %u%%)", gPad.samples, gPad.edges,
             gPad.samples ? gPad.highs * 100 / gPad.samples : 0, plan.samples ? plan.high_samples * 100 / plan.samples : 0);
  snprintf(gLast, sizeof gLast, "%s (%s): %u entries, %u us, %u Hz %u%%, %u bytes: %s%s", rc ? "FAILED" : "sent", who, count, plan.total_us,
           carrier, duty, plan.bytes, detail, pad);
  note("send #%u %s", gSends, gLast);
  note("pins after send: gpio29 ctl %08x io %08x", pinRd(kMosi, kCtl), pinRd(kMosi, kIo));
  publish();
  return rc ? kIOReturnIOError : kIOReturnSuccess;
}

// A continuous carrier burst of ms milliseconds as back-to-back single marks of at most 120 ms each (one transfer each; the gap
// between two is the engine's set-up, microseconds).
IOReturn PianoIR::sendBurst(uint32_t ms, const char *who)
{
  if (!piano_ir_burst_valid(ms)) return kIOReturnBadArgument;
  if (!gReady || gAsleep || gStopping) return kIOReturnNotReady;
  IOReturn rc = kIOReturnSuccess;
  for (uint32_t left = ms; left && rc == kIOReturnSuccess;) {
    const uint32_t part = ir_burst_part_ms(left, gRate);
    const uint32_t mark = part * 1000;
    rc = send(&mark, 1, IR_DEFAULT_CARRIER, IR_DEFAULT_DUTY, who);
    left -= part;
  }
  return rc;
}

void PianoIR::selfTest()
{
  if (gArgs.sample) {   // the known-negative: the idle line, 50 ms, no send
    gPad = {};
    for (unsigned i = 0; i < 50000; ++i) {
      padSample();
      IODelay(1);
    }
    note("pad (idle, no send, the known-negative): %u samples, %u edges, high %u", gPad.samples, gPad.edges, gPad.highs);
  }
  note("test window opens: %u bursts of 500 ms (38 kHz, 40 %%), each followed by 1.5 s dark, then one NEC frame (address 0x00, command 0x45)", gArgs.test);
  for (uint32_t i = 1; i <= gArgs.test; ++i) {
    note("test %u/%u: burst", i, gArgs.test);
    const int rc = sendBurst(500, "self-test");
    note("test %u/%u: %s", i, gArgs.test, rc == kIOReturnSuccess ? "sent" : "FAILED");
    if (rc != kIOReturnSuccess) return;
    uint64_t deadline;
    clock_interval_to_deadline(1500, NSEC_PER_MSEC, &deadline);
    if (!waitUntil(deadline, true)) return;
  }
  uint32_t nec[IR_NEC_ENTRIES];
  const int n = ir_nec_frame(0x00, 0x45, nec, IR_NEC_ENTRIES);
  if (n > 0) send(nec, unsigned(n), IR_DEFAULT_CARRIER, IR_DEFAULT_DUTY, "self-test NEC");
  note("test window closes: %u sends, %u failed", gSends, gFailures);
}

// An administrator's request (PianoIRClient.h), synchronous.
IOReturn PianoIR::setProperties(OSObject *properties)
{
  auto *d = OSDynamicCast(OSDictionary, properties);
  if (!d) return kIOReturnBadArgument;
  if (gArgs.level != 2 || !gLock) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;

  auto *sendObj = d->getObject(kPianoIRSend);
  auto *burstObj = d->getObject(kPianoIRBurst);
  auto *carrierObj = d->getObject(kPianoIRCarrier);
  auto *dutyObj = d->getObject(kPianoIRDuty);
  if (!sendObj && !burstObj) return kIOReturnUnsupported;
  if (burstObj) {
    auto *burst = OSDynamicCast(OSNumber, burstObj);
    if (!burst || sendObj || carrierObj || dutyObj || !piano_ir_burst_valid(burst->unsigned64BitValue()))
      return kIOReturnBadArgument;
    IOLockLock(gLock);
    const IOReturn rc = sendBurst(burst->unsigned32BitValue(), "burst");
    IOLockUnlock(gLock);
    return rc;
  }

  auto *list = OSDynamicCast(OSData, sendObj);
  auto *carrier = OSDynamicCast(OSNumber, carrierObj);
  auto *duty = OSDynamicCast(OSNumber, dutyObj);
  if (!list || (carrierObj && !carrier) || (dutyObj && !duty)) return kIOReturnBadArgument;
  const uint64_t hz = carrier ? carrier->unsigned64BitValue() : IR_DEFAULT_CARRIER;
  const uint64_t pct = duty ? duty->unsigned64BitValue() : IR_DEFAULT_DUTY;
  const unsigned bytes = list->getLength(), count = bytes / sizeof(uint32_t);
  if (bytes % sizeof(uint32_t) || !(count & 1) || count > IR_MAX_DURATIONS || !ir_carrier_valid(hz) || !ir_duty_valid(pct))
    return kIOReturnBadArgument;
  auto *copy = static_cast<uint32_t *>(IOMalloc(bytes));
  if (!copy) return kIOReturnNoMemory;
  memcpy(copy, list->getBytesNoCopy(), bytes);
  IOLockLock(gLock);
  const IOReturn rc = send(copy, count, uint32_t(hz), uint32_t(pct), "a client");
  IOLockUnlock(gLock);
  IOFree(copy, bytes);
  return rc;
}

IOReturn PianoIR::setPowerState(unsigned long state, IOService *)
{
  if (!gLock) return pianopower::kAckNow;
  IOLockLock(gLock);
  if (!gStopping) {
    gAsleep = state == pianopower::kOff;
    if (gAsleep) {
      releaseDma();
      if (!gDmaIdle) pianopower::cancelSystemSleep("PianoIR", "GENI did not quiesce");
    }
    IOLockWakeup(gLock, &gAsleep, false);
  }
  IOLockUnlock(gLock);
  return pianopower::kAckNow;
}

void PianoIR::systemWillShutdown(IOOptionBits specifier)
{
  quiesce();
  IOService::systemWillShutdown(specifier);
}
