// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoA830's IOKit side (k27 native kext, tooling/k27/kext/PianoA830.json):
//  - PianoA830, matched on the platform expert like PianoDwc3/PianoPower: probe() declines unless piano-gpu=N is given, so without
//    the boot argument nothing is mapped, read or published. N=1: the read-only survey (PianoA830Survey.cpp) around the driver's S0,
//    which only reads (pins, cmd-db, SMEM, the speed-bin fuse); N=2: the staged bring-up S1..S10 (Adreno830Start.cpp) in polled mode
//    (no interrupt is attached: the k27 KC has no armcpu_spi_enable/disable, the driver's weak defaults refuse the attach and every
//    stage takes its polled path, as the annibale profile is designed to), piano-gpu-stop=N stopping after S_N. Both run on a
//    kernel thread of their own, never in matching.
//  - AGXAccelerator: the macOS accelerator, an ordinary IOAccelerator subclass of IOGraphicsFamily. Its personality matches the
//    NativeAdrenoGpuPublished service the driver's S9 registers (only after S0..S8 passed) and carries MetalPluginName
//    NativeAdrenoMetal / MetalPluginClassName NativeAdrenoMetalDevice, the VM's plugin contract; opens go to the driver's
//    AGXDeviceUserClient (NativeAdrenoABI.h v2, open type 0x4147). Without a starter class or virtio-gpu paths (provenance:
//    ../UPSTREAM).
//  - The BootProgress hooks the driver calls (strong definitions; tooling/k27/coalesce-weak.py drops the driver's weak defaults):
//    status rows and stage results, the bring-up register trace (piano-gpu-trace) as "PianoA830: t ..." lines, all through a line
//    ring that a logger thread prints (the hooks run with interrupts off; see the ring below). piano-gpu-trace=3 prints the trace
//    synchronously instead, from the thread that makes the access, before it (see "the synchronous trace" below).
//  piano-gpu-delay=<s> (default 0): the survey or bring-up thread waits that long first (after the root is up, say), so a hang there
//  does not also stop the boot.
//  piano-gpu-hang-ms=<ms> (default 0: 2000): the time without progress the fence and recovery watchdogs call a hang (1000..60000).
//  The session's GPU events (NativeAdrenoPublish.cpp: failed calls, refused opens, recoveries and their snapshot, page faults, the
//  device lost, the clients) are printed as "PianoA830: gpu ..." through the same ring, whatever piano-gpu-trace says.
//  2026-11-11 (docs/drivers/gpu.md "System suspend", "The LLCC island", "Ceiling slots"):
//  - the LLCC ISLAND1 slice (PianoLlccIsland.h): read at the start of every GPU boot (read only), programmed with
//    piano-llcc-island=1 at piano-gpu=2, asked through "piano-llcc-island-ready" / piano_a830_llcc_island_ready();
//  - piano-gpu-suspend=1 (A830Suspend.h): in the power tree once the GPU runs; kOff powers it down (the full slumber), the platform
//    sleep action lowers its host votes, the wake action puts them back, kOn wakes it;
//  - "piano-gpu-ceiling" with one slot per caller (PianoGpuCeiling.h), the lowest wins; piano-gpu-ceiling-clients.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <PianoBoardTemps.h>
#include <PianoGpuAndroidPreset.h>
#include "PianoA830.h"
#include "PianoA830Rows.h"
#include "A830Idle.h"
#include "A830Dcvs.h"
#include "Adreno830Status.h"
#include "Adreno830Device.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoMemory.h"
#include "A830GpuTier.h"
#include <IOKit/IODeviceTreeSupport.h>
#include "NativeAdrenoPoolBypass.h"   // GPU-PAGED P0b: machdep.piano_gpu_pool_bypass, machdep.piano_gpu_paged
#include "NativeAdrenoGpuContext.h"   // GPU-PAGED P1: NativeAdrenoGpuContextVm
#include "NativeAdrenoPagedLine.h"    // GPU-PAGED P1: machdep.piano_gpu_pt
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#define PIANO_SCM_LOCK_KERNEL 1
#include <PianoScmLock.h>
#include <PianoSpmi.h>   // the SPMI lock's words (PianoSpmiWords), defined below
#include <PianoPowerContract.h>
#include "A830Suspend.h"
#include "A830PowerState.h"
#include "PianoLlccAudit.h"
#include "PianoLlccIsland.h"
#include "PianoGpuCeiling.h"
#include "PianoGpuIdle.h"
#include "PianoGpuQualificationLog.h"
#include "../../include/PianoGpuQualificationDisplay.h"

extern "C" int adreno830_qual607_control(int);
extern "C" uint64_t gPhysBase,gPhysSize;
using QualLog=pianogpu::QualificationLog<>;
static QualLog *gQualLog;
static IOSimpleLock *gQualLogLock;
extern "C" bool adreno830_qualification_log_ready() {return gQualLog && gQualLogLock;}
extern "C" void adreno830_qualification_note(const char *line) {
  if(adreno830_qualification_log_ready()) {
    const auto irq=IOSimpleLockLockDisableInterrupt(gQualLogLock);
    gQualLog->push(line);
    IOSimpleLockUnlockEnableInterrupt(gQualLogLock,irq);
  }
  IOLog("PianoA830: %s\n",line); // convenience only; qualification evidence comes from the read-only export
}
extern "C" void adreno830_qualification_unsafe(A830ExitOrigin origin,uint64_t protectionAt,bool safeLow,bool safeOff) {
  a830ExitTrace.terminal(origin,protectionAt,safeLow,safeOff);
  // Native 0070 uses the SMC conduit (k27 0069). A graceful halt can wait on the very power mutex/HFI
  // whose deadline expired. Only this unconfirmed-GX emergency bypasses filesystem and IOKit shutdown.
  register uint64_t function __asm__("x0")=0x84000008u; // PSCI SYSTEM_OFF
  __asm__ volatile("dsb sy; smc #0" : "+r"(function) : : "x1","x2","x3","memory");
  function=0x84000009u; // SYSTEM_RESET if SYSTEM_OFF unexpectedly returned
  __asm__ volatile("dsb sy; smc #0" : "+r"(function) : : "x1","x2","x3","memory");
  panic("PianoA830: emergency PSCI power-off and reset returned");
}
extern "C" bool adreno830_qualification_board(piano_gpu_temps *out) {
  IOService *thermal=IOService::copyMatchingService(IOService::serviceMatching("PianoThermal"));
  if(!thermal)return false;
  const IOReturn r=thermal->callPlatformFunction("piano-thermal-gpu-temps",false,out,nullptr,nullptr,nullptr);
  thermal->release();return r==kIOReturnSuccess;
}
// gpu/fullopp1: the virtual skin sensor and its inputs for the Android thermal table (PianoThermal "piano-thermal-skin-temps"; a
// PianoThermal without it answers unsupported: the guard then treats the skin as unknown and caps at 734 MHz).
extern "C" bool adreno830_qualification_skin(piano_skin_temps *out) {
  IOService *thermal=IOService::copyMatchingService(IOService::serviceMatching("PianoThermal"));
  if(!thermal)return false;
  const IOReturn r=thermal->callPlatformFunction("piano-thermal-skin-temps",false,out,nullptr,nullptr,nullptr);
  thermal->release();return r==kIOReturnSuccess;
}
SYSCTL_DECL(_machdep);
static struct sysctl_oid gQual607Oid;
static struct sysctl_oid gQualLogOid;
static struct sysctl_oid gExitTraceOid;
static int exitTraceSysctl(struct sysctl_oid *,void *,int,struct sysctl_req *req) {
  if(req->newptr || !kauth_cred_issuser(kauth_cred_get()))return EPERM;
  const uint64_t boot=a830ExitTrace.boot();
  if(!boot)return EAGAIN;
  if(!req->oldptr) {req->oldidx=(A830ExitTrace::Capacity+2)*224;return 0;}
  const uint32_t head=a830ExitTrace.head();
  const uint32_t end=head<A830ExitTrace::Capacity?head:A830ExitTrace::Capacity;
  A830ExitTrace::Record row,terminal;
  const uint32_t terminalSeq=a830ExitTrace.copyTerminal(terminal);
  const bool appendTerminal=terminalSeq>end && terminalSeq<=head;
  // Do not output a successful partial snapshot if a producer reserved a slot but has not committed it.
  for(uint32_t seq=1;seq<=end;++seq)
    if(!a830ExitTrace.copy(seq,row))return EAGAIN;
  char line[224];
  int n=snprintf(line,sizeof line,"R3DXLOG-v1 record-bytes=%u boot=%llu next=%u lost=%u\n",
                 unsigned(sizeof row),boot,head+1,head-end-unsigned(appendTerminal));
  int err=SYSCTL_OUT(req,line,n);
  const auto writeRecord=[&](uint32_t seq,const A830ExitTrace::Record &r) {
    const int bytes=snprintf(line,sizeof line,"R3DXREC seq=%u at=%llu stage=%u status=%u a=%llx b=%llx c=%llx d=%llx commit=%u\n",
                             seq,r.at,r.stage,r.status,r.a,r.b,r.c,r.d,seq);
    return SYSCTL_OUT(req,line,bytes);
  };
  for(uint32_t seq=1;!err && seq<=end;++seq) {
    if(!a830ExitTrace.copy(seq,row))return EAGAIN;
    err=writeRecord(seq,row);
  }
  if(!err && appendTerminal)err=writeRecord(terminalSeq,terminal);
  n=snprintf(line,sizeof line,"R3DXEND next=%u",head+1);
  if(!err)err=SYSCTL_OUT(req,line,n+1);
  return err;
}
static int qualLogSysctl(struct sysctl_oid *,void *,int,struct sysctl_req *req) {
  if(req->newptr)return EPERM;
  if(!adreno830_qualification_log_ready())return ENOMEM;
  // sysctl first asks the required size. Reserve the maximum so concurrent new records cannot outgrow it.
  if(!req->oldptr) {req->oldidx=QualLog::capacity*(QualLog::bytes+96)+256;return 0;}
  auto irq=IOSimpleLockLockDisableInterrupt(gQualLogLock);
  const auto bounds=gQualLog->bounds();
  IOSimpleLockUnlockEnableInterrupt(gQualLogLock,irq);
  char header[160];
  int n=snprintf(header,sizeof header,"C5LOG-v1 first=%llu next=%llu rejected=%llu\n",bounds.first,bounds.next,bounds.rejected);
  int err=SYSCTL_OUT(req,header,n);
  QualLog::Record record;
  for(uint64_t sequence=bounds.first;!err && sequence<bounds.next;++sequence) {
    irq=IOSimpleLockLockDisableInterrupt(gQualLogLock);
    const bool ok=gQualLog->copy(sequence,record);
    IOSimpleLockUnlockEnableInterrupt(gQualLogLock,irq);
    if(!ok)return EAGAIN; // snapshot overwritten while reading; never return a successful partial capture
    n=snprintf(header,sizeof header,"C5REC seq=%llu bytes=%u\n",sequence,unsigned(record.length));
    err=SYSCTL_OUT(req,header,n);
    if(!err)err=SYSCTL_OUT(req,record.text,record.length);
    if(!err)err=SYSCTL_OUT(req,"\n",1);
  }
  if(!err)err=SYSCTL_OUT(req,"C5END\n",7); // includes final NUL for CTLTYPE_STRING
  return err;
}
static int qual607Sysctl(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req) {
  int value=adreno830_qual607_control(0);
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value<1 || value>4)return EINVAL;
  return adreno830_qual607_control(value)<0?EAGAIN:0;
}
static void registerQual607(bool record) {
  bzero(&gQual607Oid,sizeof gQual607Oid);
  gQual607Oid.oid_parent=&sysctl__machdep_children;gQual607Oid.oid_number=OID_AUTO;
  gQual607Oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  gQual607Oid.oid_name="piano_gpu_qual607";gQual607Oid.oid_handler=qual607Sysctl;gQual607Oid.oid_fmt="I";
  gQual607Oid.oid_descr="GPU qualification flags; commands 3 and 4 require the R3b exit test";
  gQual607Oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&gQual607Oid);
  if(record) {
    gQualLog=static_cast<QualLog *>(IOMallocZero(sizeof(QualLog)));gQualLogLock=IOSimpleLockAlloc();
  }
  bzero(&gQualLogOid,sizeof gQualLogOid);
  gQualLogOid.oid_parent=&sysctl__machdep_children;gQualLogOid.oid_number=OID_AUTO;
  gQualLogOid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  gQualLogOid.oid_name="piano_gpu_qual607_log";gQualLogOid.oid_handler=qualLogSysctl;gQualLogOid.oid_fmt="A";
  gQualLogOid.oid_descr="C5LOG-v1 bounded read-only qualification records";
  gQualLogOid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&gQualLogOid);
  bzero(&gExitTraceOid,sizeof gExitTraceOid);
  gExitTraceOid.oid_parent=&sysctl__machdep_children;gExitTraceOid.oid_number=OID_AUTO;
  gExitTraceOid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  gExitTraceOid.oid_name="piano_gpu_r3b_exit";
  gExitTraceOid.oid_handler=exitTraceSysctl;gExitTraceOid.oid_fmt="A";
  gExitTraceOid.oid_descr="R3DXLOG-v1 bounded R3b exit stages";
  gExitTraceOid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&gExitTraceOid);
}
#include <PianoDisplayFollow.h>

SYSCTL_DECL(_machdep);

extern "C" const Adreno830Board *piano_a830_board(void);

extern "C" void proc_selfname(char *, int);
extern "C" IOReturn native_adreno_new_user_client(IOService *, task_t, void *, uint32_t, OSDictionary *, IOUserClient **, unsigned);
extern "C" void native_adreno_note_property_read(const char *);
extern "C" int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
extern "C" int get_preemption_level(void);

namespace {
uint32_t gLevel;    // piano-gpu
uint32_t gTrace;    // the trace hook writes lines (piano-gpu=2 and piano-gpu-trace != 0)
uint32_t gTraceSync;   // piano-gpu-trace=3: the trace printed synchronously (below)
thread_t gRunThread;   // the starter thread (run below); piano-gpu-trace=3's heartbeat reports its IOLog in progress
uint64_t gRunNoteSince;   // mach_absolute_time at which the starter entered piano_a830_note's IOLog (0: not inside one)
char gRows[Adreno830StatusRows][Adreno830StatusRowBytes];

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t value = fallback;
  if (!PE_parse_boot_argn(name, &value, sizeof value)) value = fallback;
  return value;
}
bool bootArgGetter(void *, const char *name, uint32_t *value) { return PE_parse_boot_argn(name, value, sizeof *value); }

const char *stageName(unsigned stage)
{
  static const char *const names[] = {"S0 inputs", "S1 power", "S2 secure CX", "S3 SMMU", "S4 firmware memory", "S5 GMU/HFI",
                                      "S6 CP", "S7 ZAP", "S8 fence", "S9 Metal", "S10 DCVS"};
  return stage < sizeof names / sizeof names[0] ? names[stage] : stage == A830SRunning ? "running" : "?";
}
}  // namespace

extern "C" void piano_a830_note(const char *format, ...)
{
  char line[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  const bool starter = gTraceSync && gRunThread && current_thread() == gRunThread;
  if (starter) __atomic_store_n(&gRunNoteSince, mach_absolute_time(), __ATOMIC_RELAXED);
  IOLog("PianoA830: %s\n", line);
  if (starter) __atomic_store_n(&gRunNoteSince, 0ull, __ATOMIC_RELAXED);
}

// ---- the line ring ----------------------------------------------------------------------------------------------------------------
// The driver calls its BootProgress hooks in contexts where nothing may be printed: the register trace inside its tracer lock with
// this CPU's interrupts off (A830TraceLock), the status rows under the status block's spin lock. Kernel printf/IOLog end in
// os_log_with_args, which must not run with interrupts off: piano-gpu=2's first run printed the trace's first line and hung the
// system right after it (2026-11-11 07:20). So the hooks only copy their line into this ring (a short spin lock with interrupts off,
// a bounded memcpy, nothing else), and the logger thread prints it with IOLog from plain thread context. Since 2026-11-12 a push
// wakes the logger (thread_wakeup after the ring's unlock, only while it waits: callable with interrupts off and with spin locks
// held, it never blocks or prints), so the logger no longer polls every millisecond (1000 wakeups a second whatever the GPU did,
// docs/power.md "Idle wakeups by driver"); it still looks once a second (the canary, the held rows). piano-gpu-logger-ms=N polls
// every N ms as before (1: the old behaviour); piano-gpu-trace=3 polls every millisecond as before (its heartbeat).
// A full ring drops lines and counts them; the logger says how many. The trace line of an access is in the ring before the access
// is made, but the console shows it only once the logger has printed every line before it: with -v each console line is drawn
// (tens of ms), so the logger can be many seconds and the whole bring-up behind the driver (2026-11-11 "power1": S1's lines 5 s
// after adreno830_start had returned, docs/forensics-2026-11-11.md). The last printed line is then where the logger was, not the
// access in flight; piano-gpu-trace=3 (below) prints each line before its access instead.
namespace {
enum : unsigned { RingLines = 2048, RingBytes = 160 };
struct LineRing {
  char (*text)[RingBytes];
  uint64_t *stamp;   // piano-gpu-trace=3: mach_absolute_time of each line's push (else null)
  uint32_t head, tail, drops, lock;
};
LineRing gRing;
uint32_t gLoggerPollMs;            // piano-gpu-logger-ms: 0 woken by the pushes (default), N poll every N ms (1: before 2026-11-12)
volatile uint32_t gLoggerWaiting;  // the logger is (about to be) blocked on it: a push wakes it

uintptr_t ringLock()
{
  const uintptr_t irq = uintptr_t(ml_set_interrupts_enabled(FALSE));
  while (__atomic_exchange_n(&gRing.lock, 1u, __ATOMIC_ACQUIRE)) {
  }
  return irq;
}
void ringUnlock(uintptr_t irq)
{
  __atomic_store_n(&gRing.lock, 0u, __ATOMIC_RELEASE);
  ml_set_interrupts_enabled(boolean_t(irq));
}

// Any context but a primary interrupt handler; never blocks, never prints.
void ringPush(const char *prefix, const char *text, size_t length)
{
  if (!gRing.text) return;
  const uintptr_t irq = ringLock();
  if (gRing.head - gRing.tail >= RingLines) {
    gRing.drops++;
  } else {
    char *line = gRing.text[gRing.head % RingLines];
    size_t n = 0;
    while (prefix[n] && n < RingBytes - 1) {
      line[n] = prefix[n];
      n++;
    }
    for (size_t i = 0; i < length && text[i] && n < RingBytes - 1; ++i) line[n++] = text[i];
    line[n] = 0;
    if (gRing.stamp) gRing.stamp[gRing.head % RingLines] = mach_absolute_time();
    gRing.head++;
  }
  ringUnlock(irq);
  // The line is in (head stored) before the logger's state is read; the logger stores its state before it reads head: one of the
  // two sees the other (seq_cst on both sides), so no line waits for the logger's next look.
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (__atomic_load_n(&gLoggerWaiting, __ATOMIC_RELAXED)) thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gLoggerWaiting)));
}

bool ringPending()
{
  const uintptr_t irq = ringLock();
  const bool any = gRing.tail != gRing.head;
  ringUnlock(irq);
  return any;
}

bool ringPop(char *out, uint32_t *drops, uint64_t *stamp = nullptr)
{
  const uintptr_t irq = ringLock();
  const bool any = gRing.tail != gRing.head;
  if (any) {
    memcpy(out, gRing.text[gRing.tail % RingLines], RingBytes);
    if (stamp) *stamp = gRing.stamp ? gRing.stamp[gRing.tail % RingLines] : 0;
    gRing.tail++;
  }
  *drops = gRing.drops;
  gRing.drops = 0;
  ringUnlock(irq);
  return any;
}

// The console mailbox canary (2026-11-11): k27 0055/0058 mirror the console into pstore-console=<addr> (cvp@9cb00000, outside XNU's
// DRAM). Across a reset its DRAM decays (1 -> 0, worse when hot), which a dump shows as a damaged signature; during the run nothing
// decays, so a signature word that changes while XNU runs is a real overwrite (by a device, a GPU stream reaching memory untranslated
// above all: piano_a830_smmu_routes). The logger reads the word once a second through its own read-only uncached mapping.
struct Canary {
  IOMemoryMap *map;
  volatile const uint32_t *word;
  uint64_t phys;
  uint32_t expected, changes;
};
Canary gCanary;

void canaryStart()
{
  uint64_t base = 0;
  if (!PE_parse_boot_argn("pstore-console", &base, sizeof base) || !base || (base & 0xfff)) return;
  IODeviceMemory *memory = IODeviceMemory::withRange(base, 0x1000);
  gCanary.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
  if (memory) memory->release();
  if (!gCanary.map) return;
  gCanary.word = reinterpret_cast<volatile const uint32_t *>(gCanary.map->getVirtualAddress());
  gCanary.phys = base;
  gCanary.expected = *gCanary.word;
  IOLog("PianoA830: console mailbox canary at %#llx: signature %#x (watched once a second; a change while XNU runs is an overwrite)\n",
        base, gCanary.expected);
}

void canaryCheck()
{
  if (!gCanary.word) return;
  const uint32_t now = *gCanary.word;
  if (now == gCanary.expected) return;
  gCanary.changes++;
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  IOLog("PianoA830: CONSOLE MAILBOX OVERWRITTEN at %#llx while running (t=%llu s, change %u): signature %#x -> %#x; GPU %s\n", gCanary.phys,
        ns / 1000000000ull, gCanary.changes, gCanary.expected, now, adreno830_running() ? "running" : "not running");
  gCanary.expected = now;
}

void flushHeldRows();   // the status rows held back (below)

// ---- the synchronous trace (piano-gpu-trace=3, 2026-11-11) ------------------------------------------------------------------------
// Every trace line is printed by the thread that pushed it, after the tracer's lock is given back and before the access the line
// names: Adreno830Start.cpp's traceAccess calls boot_progress_trace_sync once its line is in the ring, and that drains the ring
// (this line and every line before it) with IOLog. So the console's last "t" line is the access in flight or the one just made,
// whatever the logger was doing. Each line carries the uptime of its push: "PianoA830: [27.612] t g1.7 GPUCC w 3d99010 8000001".
// Printing only happens in thread context with interrupts on and preemption enabled (never with interrupts off: the 07:20 hang of
// 2026-11-11); a line pushed anywhere else (a caller that masked interrupts) stays in the ring for the logger, as with 1, and the
// heartbeat counts it. The driver itself gets piano-gpu-trace=1 (PianoA830Board.cpp): runs are counted, so a poll costs a few lines.
// The cost is the console's: with -v every line is drawn into the boot framebuffer, so the bring-up takes tens of seconds instead of
// one, and the time between two accesses grows to milliseconds (a sequence that only fails when it is slow fails here, see the
// forensics page). The driver's polls are all bounded already (iteration or time budgets); what cannot be bounded is one access
// or one SMC that never returns, and the heartbeat names that one instead of a guard.
// The heartbeat (the logger, once a second; every 30 s from 120 s after the bring-up returned): the uptime, the lines pushed and
// printed, what the bring-up thread is doing (printing a line, or past it: its access) and the starter thread's phase and whether it
// is inside an IOLog, each with how long. A stall then shows whether the rest of the system still ran, and where the thread stopped.
IOLock *gPrintLock;            // one printer at a time (the bring-up thread or the logger), so the lines keep their order
uint32_t gPrinted, gPrintedSync, gSyncSkipped;   // (lines pushed: gRing.head)
uint32_t gSyncState;           // 0 none yet, 1 printing a line, 2 past its print (the access), 3 adreno830_start returned
uint64_t gSyncSince;           // mach_absolute_time of the state's start
uint32_t gSyncLineNo;          // the number of the line the bring-up thread printed last
char gSyncLast[RingBytes];     // and its text (copied under gSyncSeq: even = stable)
uint32_t gSyncSeq;
uint32_t gRunPhase;            // RunPhase
uint64_t gRunSince;            // the phase's start
enum RunPhase : uint32_t { RunInit, RunDelay, RunSurvey, RunBringUp, RunReport, RunRoutes, RunWait, RunDone };
const char *const kRunPhases[] = {"starting", "piano-gpu-delay", "survey", "bring-up", "report", "SMMU routes", "waiting 60 s", "done"};

uint64_t msOf(uint64_t abs)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(abs, &ns);
  return ns / 1000000ull;
}

void runPhase(uint32_t phase)
{
  __atomic_store_n(&gRunSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gRunPhase, phase, __ATOMIC_RELEASE);
}

void printLine(const char *line, uint64_t stamp)
{
  if (gTraceSync && stamp) {
    const uint64_t ms = msOf(stamp);
    IOLog("PianoA830: [%llu.%03llu] %s\n", ms / 1000, ms % 1000, line);
  } else {
    IOLog("PianoA830: %s\n", line);
  }
}

// Drains the ring with IOLog; the caller holds gPrintLock. bringUp: the bring-up thread's drain (its state and last line kept).
void drainRing(bool bringUp)
{
  char line[RingBytes];
  uint32_t drops = 0;
  uint64_t stamp = 0;
  while (ringPop(line, &drops, &stamp)) {
    if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    const uint32_t number = __atomic_add_fetch(&gPrinted, 1u, __ATOMIC_RELAXED);
    if (bringUp) {
      __atomic_add_fetch(&gSyncSeq, 1u, __ATOMIC_ACQ_REL);   // odd: being written
      memcpy(gSyncLast, line, RingBytes);
      gSyncLineNo = number;
      __atomic_add_fetch(&gSyncSeq, 1u, __ATOMIC_ACQ_REL);
      __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
      __atomic_store_n(&gSyncState, 1u, __ATOMIC_RELEASE);
      __atomic_add_fetch(&gPrintedSync, 1u, __ATOMIC_RELAXED);
    }
    printLine(line, stamp);
  }
  if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
}

void heartbeat()
{
  const uint64_t now = mach_absolute_time();
  char last[RingBytes] = "-";
  uint32_t lineNo = 0;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {   // the bring-up thread may be copying it right now
    const uint32_t before = __atomic_load_n(&gSyncSeq, __ATOMIC_ACQUIRE);
    if (before & 1) continue;
    memcpy(last, gSyncLast, RingBytes);
    lineNo = gSyncLineNo;
    if (__atomic_load_n(&gSyncSeq, __ATOMIC_ACQUIRE) == before) break;
    strlcpy(last, "(changing)", sizeof last);
  }
  last[RingBytes - 1] = 0;
  const uint32_t state = __atomic_load_n(&gSyncState, __ATOMIC_ACQUIRE);
  const uint64_t stateMs = msOf(now - __atomic_load_n(&gSyncSince, __ATOMIC_RELAXED));
  const uint32_t phase = __atomic_load_n(&gRunPhase, __ATOMIC_ACQUIRE);
  const uint64_t noteSince = __atomic_load_n(&gRunNoteSince, __ATOMIC_RELAXED);
  const uint64_t ms = msOf(now);
  const uint32_t head = __atomic_load_n(&gRing.head, __ATOMIC_RELAXED), tail = __atomic_load_n(&gRing.tail, __ATOMIC_RELAXED);
  char what[RingBytes + 64], note[48] = "";
  if (state == 1) snprintf(what, sizeof what, "printing line %u for %llu ms (%s)", lineNo, stateMs, last);
  else if (state == 2) snprintf(what, sizeof what, "past line %u for %llu ms: its access (%s)", lineNo, stateMs, last);
  else if (state == 3) snprintf(what, sizeof what, "adreno830_start returned %llu ms ago", stateMs);
  else snprintf(what, sizeof what, "no line printed yet");
  if (noteSince) snprintf(note, sizeof note, ", inside IOLog for %llu ms", msOf(now - noteSince));
  IOLog("PianoA830: hb %llu.%03llu s: bring-up thread %s; starter: %s for %llu ms%s; lines pushed %u, printed %u (%u by the bring-up "
        "thread), sync prints skipped %u, backlog %u\n", ms / 1000, ms % 1000, what,
        phase < sizeof kRunPhases / sizeof kRunPhases[0] ? kRunPhases[phase] : "?",
        msOf(now - __atomic_load_n(&gRunSince, __ATOMIC_RELAXED)), note, head, __atomic_load_n(&gPrinted, __ATOMIC_RELAXED),
        __atomic_load_n(&gPrintedSync, __ATOMIC_RELAXED), __atomic_load_n(&gSyncSkipped, __ATOMIC_RELAXED), head - tail);
}

// The logger's wait between passes: woken by a push (ringPush) or at the next once-a-second look (deadline).
void loggerWait(uint64_t deadline)
{
  const wait_result_t w = assert_wait_deadline(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gLoggerWaiting)), THREAD_UNINT, deadline);
  __atomic_store_n(&gLoggerWaiting, 1u, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (ringPending()) {
    if (w == THREAD_WAITING) clear_wait(current_thread(), THREAD_AWAKENED);
  } else if (w == THREAD_WAITING) {
    thread_block(THREAD_CONTINUE_NULL);
  }
  __atomic_store_n(&gLoggerWaiting, 0u, __ATOMIC_RELAXED);
}

void logger(void *, wait_result_t)
{
  char line[RingBytes];
  canaryStart();
  uint64_t second = 0, lastBeat = 0, lastLook = mach_absolute_time();
  nanoseconds_to_absolutetime(1000000000ull, &second);
  for (;;) {
    if (gTraceSync && gPrintLock) {
      // Never wait for the bring-up thread's print: if it is stuck inside IOLog, the heartbeat must still come.
      if (IOLockTryLock(gPrintLock)) {
        drainRing(false);
        IOLockUnlock(gPrintLock);
      }
      const uint64_t now = mach_absolute_time();
      const bool late = __atomic_load_n(&gSyncState, __ATOMIC_ACQUIRE) == 3 && now - gSyncSince > 120 * second;
      if (now - lastBeat >= (late ? 30 * second : second)) {
        lastBeat = now;
        heartbeat();
      }
    } else {
      uint32_t drops = 0;
      while (ringPop(line, &drops)) {
        if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
        IOLog("PianoA830: %s\n", line);
      }
      if (drops) IOLog("PianoA830: (%u lines dropped: the ring was full)\n", drops);
    }
    // Once a second (before 2026-11-12: every 1000th 1 ms pass): the canary and the rows held back.
    const uint64_t now = mach_absolute_time();
    if (now - lastLook >= second) {
      lastLook = now;
      canaryCheck();
      flushHeldRows();
    }
    if (gTraceSync || gLoggerPollMs) {
      IOSleep(gTraceSync ? 1 : gLoggerPollMs);
      continue;
    }
    loggerWait(lastLook + second);
  }
}
}  // namespace

// piano-gpu-trace=3: called by traceAccess (Adreno830Start.cpp) after the tracer's lock, before the access (see above).
extern "C" void boot_progress_trace_sync(void)
{
  if (!gTraceSync || !gPrintLock || !gRing.text) return;
  if (!ml_get_interrupts_enabled() || ml_at_interrupt_context() || get_preemption_level() != 0) {
    __atomic_add_fetch(&gSyncSkipped, 1u, __ATOMIC_RELAXED);   // the logger prints it
    return;
  }
  IOLockLock(gPrintLock);
  drainRing(true);
  __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gSyncState, 2u, __ATOMIC_RELEASE);
  IOLockUnlock(gPrintLock);
}

// ---- BootProgress hooks (Adreno830Status.h): into the ring only ------------------------------------------------------------------
// The overlay rows: each row whose text changed (row 0 the summary, rows 1-3 the diagnostic page of piano-gpu-status=2). Once the GPU
// runs ("GPU RUN ..."), the rows change every second (submissions, clock, temperature, the queue figures): the login run printed two
// of them a second for the whole session. From then on a row is printed when it changed and 10 s have passed since it was last, or at
// once when row 0's state (its second word: RUN, LOST, ...) or its fault or hang count changed; a row held back is printed by the
// logger thread when its 10 s are up, so the last state of an idle GPU still shows. Before the GPU runs (the bring-up) every change
// is printed at once, as before. Under the ring's lock (the hook runs under the status block's spin lock, the logger beside it).
namespace {
enum : unsigned { RowSlots = Adreno830StatusRows < 4 ? Adreno830StatusRows : 4 };
const char *const kRowPrefixes[] = {"row0 ", "row1 ", "row2 ", "row3 "};
char gRowHeld[RowSlots][Adreno830StatusRowBytes];
bool gRowPending[RowSlots];
uint64_t gRowShownAt[RowSlots];
uint64_t gRowEvery;   // 10 s in mach_absolute_time units (set by start())

}  // namespace

extern "C" void boot_progress_gpu_rows(const char (*rows)[Adreno830StatusRowBytes], unsigned count)
{
  char out[RowSlots][Adreno830StatusRowBytes];
  bool show[RowSlots] = {};
  const bool running = count && !strncmp(rows[0], "GPU RUN ", 8);
  const uint64_t now = mach_absolute_time();
  const uintptr_t irq = ringLock();
  for (unsigned i = 0; i < count && i < RowSlots; ++i) {
    if (!memcmp(gRows[i], rows[i], Adreno830StatusRowBytes)) {
      gRowPending[i] = false;   // back to what the console shows
      continue;
    }
    bool due = !running || !gRowEvery || now - gRowShownAt[i] >= gRowEvery;
    if (!due && i == 0) due = PianoA830RowZeroStateChanged(gRows[0], rows[0], Adreno830StatusRowBytes);
    if (due) {
      memcpy(gRows[i], rows[i], Adreno830StatusRowBytes);
      gRows[i][Adreno830StatusRowBytes - 1] = 0;
      memcpy(out[i], gRows[i], Adreno830StatusRowBytes);
      gRowShownAt[i] = now;
      gRowPending[i] = false;
      show[i] = true;
    } else {
      memcpy(gRowHeld[i], rows[i], Adreno830StatusRowBytes);
      gRowHeld[i][Adreno830StatusRowBytes - 1] = 0;
      gRowPending[i] = true;
    }
  }
  ringUnlock(irq);
  for (unsigned i = 0; i < RowSlots; ++i)
    if (show[i]) ringPush(kRowPrefixes[i], out[i], Adreno830StatusRowBytes);
}

namespace {
// The logger's once-a-second look: a row held back for 10 s is printed now.
void flushHeldRows()
{
  char out[RowSlots][Adreno830StatusRowBytes];
  bool show[RowSlots] = {};
  const uint64_t now = mach_absolute_time();
  const uintptr_t irq = ringLock();
  for (unsigned i = 0; i < RowSlots; ++i) {
    if (!gRowPending[i] || (gRowEvery && now - gRowShownAt[i] < gRowEvery)) continue;
    memcpy(gRows[i], gRowHeld[i], Adreno830StatusRowBytes);
    memcpy(out[i], gRowHeld[i], Adreno830StatusRowBytes);
    gRowShownAt[i] = now;
    gRowPending[i] = false;
    show[i] = true;
  }
  ringUnlock(irq);
  for (unsigned i = 0; i < RowSlots; ++i)
    if (show[i]) ringPush(kRowPrefixes[i], out[i], Adreno830StatusRowBytes);
}
}  // namespace

// UniXNU-piano's event lines (NativeAdrenoPublish.cpp, Adreno830Status.h): whatever piano-gpu-trace and piano-gpu-status say.
extern "C" void boot_progress_gpu_event(const char *text)
{
  if (text) ringPush("gpu ", text, RingBytes);
}
// piano-gpu-hang-ms=<ms>: the time without progress (fence, ring read pointer, BR's IB1) the fence and recovery watchdogs call a hang;
// absent or 0: their 2000 ms. Kept in 1000..60000.
extern "C" uint32_t adreno830_board_hang_ms(void)
{
  const uint32_t ms = bootArg("piano-gpu-hang-ms", 0);
  if (!ms) return 0;
  return ms < 1000 ? 1000 : ms > 60000 ? 60000 : ms;
}

// The kernel-wide secure-call lock (package/include/PianoScmLock.h, docs/scm-lock.md). PianoA830 owns the word, as it owns the DRV-2
// word: it links before every other user, which import this single copy (their descriptors name com.unixnu.driver.PianoA830 as a
// library). Its own calls take it through adreno830_board_scm_hold: around every yielding request of the driver's Sm8750Scm (its
// ops->lock/unlock, after/before its SCM mutex) and once around the ZAP's PAS 13 sequence (init_image .. auth_and_reset).
extern "C" volatile uint32_t sm8750_scm_owner;
volatile uint32_t sm8750_scm_owner = 0;
namespace {
PIANO_SCM_KERNEL_LOCK(gScmLock, PianoScmTagA830, "PianoA830");
}  // namespace
extern "C" void adreno830_board_scm_hold(int take)
{
  if (take) piano_scm_kernel_take(&gScmLock);
  else piano_scm_kernel_give(&gScmLock);
}

// The WCN7860 PMU's shared rails (package/include/PianoWcnRails.h, docs/drivers/bluetooth.md §7.1): the state and lock words of the
// two radios' drivers (PianoWifi, PianoBluetooth), kept here for the same reason as the words above: PianoA830 links before both,
// which import this single copy. PianoA830 itself never touches them.
extern "C" volatile uint32_t piano_wcn_rails_state;
volatile uint32_t piano_wcn_rails_state = 0;
extern "C" volatile uint32_t piano_wcn_pmu_owner;
volatile uint32_t piano_wcn_pmu_owner = 0;

// The shared multimedia votes (package/include/PianoMmVotes.h, docs/drivers/camera.md §4.3.1): MXC, MMCX and BCM MM1 on apps DRV-2,
// reference-counted between PianoVideo and PianoCamera (the display's floor in its own word): the lock and the state words, kept
// here for the same reason as the words above. PianoA830 itself never touches them (it votes none of the three).
extern "C" volatile uint32_t piano_mm_votes_owner;
volatile uint32_t piano_mm_votes_owner = 0;
extern "C" volatile uint32_t piano_mm_votes[32];
volatile uint32_t piano_mm_votes[32] = {};

// The SPMI transaction lock (package/include/PianoSpmi.h, docs/spmi-lock.md): one transaction at a time on the PMIC arbiter across
// every kext that drives it (PianoPower, PianoThermal, PianoWifi, PianoBluetooth, PianoCamera, PianoDwc3, PianoTorch, PianoInput,
// PianoUsbHost): the owner word, the holder, the nesting and every kext's counts, kept here for the same reason as the words above.
// PianoA830 itself never touches the arbiter.
extern "C" PianoSpmiWords piano_spmi_words;
PianoSpmiWords piano_spmi_words = {};

namespace {
__attribute__((format(printf, 1, 2))) void gpuLine(const char *format, ...)
{
  char line[RingBytes];
  va_list ap;
  va_start(ap, format);
  vsnprintf(line, sizeof line, format, ap);
  va_end(ap);
  boot_progress_gpu_event(line);
}
}  // namespace

// The display's DDR floor level as the idle policy computed it (5 on piano): the suspend keeps it while the panel still scans.
namespace {
uint8_t gDisplayFloorLevel = 5;
}  // namespace

// The GPU's idle policy (adreno830/A830Idle.h):
//   piano-gpu-idle=0|1|2 (default 1): 0 as before; 1 the DCVS floor (no wake boost, the idle window); 2 + the idle votes and slumber;
//   piano-gpu-idle-ms=<ms> (1000, 100..60000): quiet this long -> the idle votes and slumber;
//   piano-gpu-idle-slumber=0|1 (1): with 2, also P1's full slumber (0: the votes alone);
//   piano-gpu-idle-ddr=<level> (the scanout's: twice the panel's read rate at 120 Hz, from the boot framebuffer): the host DDR bus level
//       while idle, never above the level held;
//   piano-gpu-idle-rails=<vlvl> (0x140, RPMh NOM_L1: the lit panel's CX through MMCX; 0 keeps them): the host CX/MX level while
//       idle, never above the level held.
extern "C" void adreno830_board_idle(A830IdleConfig *config)
{
  if (!config) return;
  config->mode = uint8_t(bootArg("piano-gpu-idle", 1) > 2 ? 2 : bootArg("piano-gpu-idle", 1));
  const uint32_t ms = bootArg("piano-gpu-idle-ms", 1000);
  config->idleMs = ms < 100 ? 100 : ms > 60000 ? 60000 : ms;
  config->slumber = bootArg("piano-gpu-idle-slumber", 1) != 0;
  config->railVlvl = bootArg("piano-gpu-idle-rails", 0x140);
  // The scanout: the GOP framebuffer the panel reads from DDR at ABL's 120 Hz (NativeGopDisplay copies frames into it).
  const Adreno830Board *board = piano_a830_board();
  PE_Video video = {};
  uint32_t width = 3200, height = 2136, scanout = 0;
  if (IOService::getPlatform() && IOService::getPlatform()->getConsoleInfo(&video) == kIOReturnSuccess && video.v_width && video.v_height) {
    width = uint32_t(video.v_width);
    height = uint32_t(video.v_height);
  }
  uint8_t floor = board && board->ddr_kbps ? A830IdleScanoutLevel(board->ddr_kbps, board->ddr_levels, width, height, 4, 120, 2000, &scanout) : 5;
  const uint32_t asked = bootArg("piano-gpu-idle-ddr", 0xff);
  if (asked != 0xff && board && asked < board->ddr_levels) floor = uint8_t(asked);
  config->ddrLevel = floor;
  gDisplayFloorLevel = floor;   // the system suspend's display floor too (A830Suspend.h)
  gpuLine("idle: piano-gpu-idle=%u (slumber %u, after %u ms); idle DDR level %u (%u MB/s; %ux%u scanout %u MB/s at 120 Hz), "
          "rails vlvl %#x", config->mode, config->mode >= 2 && config->slumber ? 1u : 0u, config->idleMs, floor,
          board && board->ddr_kbps && floor < board->ddr_levels ? board->ddr_kbps[floor] / 1000 : 0, width, height, scanout / 1000,
          config->railVlvl);
}

extern "C" void adreno830_board_idle_event(const A830IdleEvent *e)
{
  if (!e) return;
  switch (e->kind) {
  case A830IdleEventDown:
    gpuLine("idle: votes down after %llu ms quiet (#%u): DDR level %u (%u MB/s), CX %#x MX %#x", (unsigned long long)e->ms, e->count,
            e->ddrLevel, e->ddrKbps / 1000, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventUp:
    gpuLine("idle: votes up after %llu ms down (#%u): DDR level %u (%u MB/s), CX %#x MX %#x", (unsigned long long)e->ms, e->count,
            e->ddrLevel, e->ddrKbps / 1000, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventSlumber:
    gpuLine("idle -> slumber after %llu ms quiet (#%u, entry %u us); votes CX %#x MX %#x DDR level %u (%u MB/s)",
            (unsigned long long)e->ms, e->count, e->us, e->cxVlvl, e->mxVlvl, e->ddrLevel, e->ddrKbps / 1000);
    break;
  case A830IdleEventWake:
    gpuLine("wake after %llu ms asleep (#%u): the wake took %u.%03u ms (what the first submission waited)", (unsigned long long)e->ms,
            e->count, e->us / 1000, e->us % 1000);
    break;
  case A830IdleEventFailed:
    gpuLine("idle: a host vote failed (rpmh %d, #%u): no idle votes for this boot; DDR level %u CX %#x MX %#x", e->result, e->count,
            e->ddrLevel, e->cxVlvl, e->mxVlvl);
    break;
  case A830IdleEventSlumberOff:
    gpuLine("idle: a slumber or wake failed (step %#x, %#x, %u failures): no slumber for this boot", e->us, unsigned(e->result), e->count);
    break;
  case A830IdleEventFloor:
    if (!e->result)
      gpuLine("idle: the boot DDR vote went to the display's floor, level %u (%u MB/s); the GMU votes the GPU's own", e->ddrLevel,
              e->ddrKbps / 1000);
    else
      gpuLine("idle: the boot DDR vote could not drop (rpmh %d): level %u (%u MB/s) stays", e->result, e->ddrLevel, e->ddrKbps / 1000);
    break;
  case A830IdleEventSummary:
    gpuLine("idle last %llu s: votes down %u, up %u, down %u.%u %% of the time; slumbers %u, wakes %u, longest wake %u.%03u ms",
            (unsigned long long)(e->ms / 1000), e->downs, e->ups, e->downPermille / 10, e->downPermille % 10, e->slumbers, e->wakes,
            e->maxWakeUs / 1000, e->maxWakeUs % 1000);
    break;
  default:
    break;
  }
}
extern "C" void boot_progress_display_status(const char *detail, uint64_t value)
{
  char text[96];
  snprintf(text, sizeof text, "%s %#llx", detail ? detail : "-", value);
  ringPush("status ", text, sizeof text);
}
// The register trace: called before each traced access, inside the tracer's lock with interrupts off.
extern "C" void boot_progress_trace(const char *text)
{
  if (gTrace && text) ringPush("t ", text, RingBytes);
}
extern "C" int boot_progress_trace_enabled(void) { return gTrace ? 1 : 0; }

extern "C" void piano_a830_report(IOReturn result)
{
  const Adreno830Status &s = adreno830_status;
  piano_a830_note("adreno830_start: %#x; stage %u (%s) state %u step %#x reason %u ioreturn %#x reg %#llx value %#llx expect %#llx",
                  result, s.stage, stageName(s.stage), s.state, s.step, s.reason, s.ioreturn, s.reg, s.value, s.expect);
  const Adreno830Failure *f = adreno830_last_failure();
  if (f && (f->stage || f->step || f->reason || f->ior))
    piano_a830_note("last failure: S%u step %#x reason %u ior %#x reg %#llx value %#llx expect %#llx", f->stage, f->step, f->reason,
                    f->ior, f->reg, f->value, f->expect);
  piano_a830_note("S0 facts: soc %u feature %#x speed-bin %#x profile %u (set %u) boot %u ms; aux0 %#llx aux1 %#llx", s.soc_id,
                  s.feature_code, s.speed_bin, s.profile, s.profile_set, s.boot_ms, s.aux[0], s.aux[1]);
  for (unsigned i = 0; i < A830StageCount; ++i)
    if (s.stage_us[i]) piano_a830_note("stage %s: %u us", stageName(i), s.stage_us[i]);
  for (unsigned i = 0; i < Adreno830StatusRows; ++i)
    if (s.rows[i][0]) piano_a830_note("row%u %.*s", i, int(Adreno830StatusRowBytes), s.rows[i]);
}

// ---- the LLCC island slice (PianoLlccIsland.h) -------------------------------------------------------------------------------------
// Read at the start of every GPU boot (read only), programmed with piano-llcc-island=1 at piano-gpu=2, asked by others through
// "piano-llcc-island-ready" (PianoPowerContract.h pianopower::llccIslandReady) and piano_a830_llcc_island_ready(). One uncached page per
// register group, mapped by the starter thread only; the ready query maps nothing (it reads through the ATTR page or answers false).
namespace {
struct LlccPage {
  uint64_t phys;
  IOMemoryMap *map;
  uintptr_t va;
};
LlccPage gLlccPages[6];
uint32_t gLlccArmed;       // the ATTR page is mapped and its first reading done (release/acquire)
uint8_t gIslandState;      // pianollcc::State of the last reading
bool gIslandProgrammed;

uintptr_t llccVa(uint64_t phys, bool mayMap)
{
  const uint64_t page = phys & ~0xfffull;
  for (const LlccPage &p : gLlccPages)
    if (p.va && p.phys == page) return p.va + uintptr_t(phys & 0xfff);
  if (!mayMap) return 0;
  for (LlccPage &p : gLlccPages)
    if (!p.va) {
      IODeviceMemory *memory = IODeviceMemory::withRange(page, 0x1000);
      IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
      if (memory) memory->release();
      if (!map) return 0;
      p = {page, map, uintptr_t(map->getVirtualAddress())};
      return p.va + uintptr_t(phys & 0xfff);
    }
  return 0;
}
// context: non-null = the starter thread (may map); null = any context (mapped pages only).
bool llccRead(void *context, pianollcc::Block block, uint32_t offset, uint32_t *value)
{
  const uintptr_t va = llccVa(pianollcc::kBlockPhys[block] + offset, context != nullptr);
  if (!va) return false;
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(va) : "memory");
  *value = v;
  return true;
}
bool llccWrite(void *context, pianollcc::Block block, uint32_t offset, uint32_t value)
{
  const uintptr_t va = llccVa(pianollcc::kBlockPhys[block] + offset, context != nullptr);
  if (!va) return false;
  __asm__ volatile("str %w0, [%1]\n\tdsb sy" : : "r"(value), "r"(va) : "memory");
  return true;
}
int gMayMap;
const pianollcc::Io kLlccThread = {&gMayMap, llccRead, llccWrite};
const pianollcc::Io kLlccAny = {nullptr, llccRead, llccWrite};

IOLock *gLlccAuditLock;
int gLlccAuditGeneration;
unsigned gLlccAuditIndex;
bool gLlccAuditPrepared;
enum class LlccAuditState { waiting, complete, stopped };
LlccAuditState gLlccAuditState;
bool gLlccIsland3;
struct LlccIslandReadback {
  uint32_t before[5], after[5];  // ATTR0..3, ALLOC3
  unsigned beforeSeen, afterSeen;
};
LlccIslandReadback gLlccIsland3Readback;
const char *gLlccIsland3Result;

void finishLlccAudit(LlccAuditState state)
{
  gLlccAuditState = state;
  IOLockWakeup(gLlccAuditLock, &gLlccAuditState, true);
}

int llccAuditCommand(int command, int token)
{
  if (gLlccAuditState != LlccAuditState::waiting) return ECANCELED;
  if (gLlccAuditIndex == pianollcc::kAuditCount) {
    if (command != token) return EINVAL;
    IOLog("PianoA830: sens-trace29 RELEASE token=%d reads=%u\n", token, gLlccAuditIndex);
    finishLlccAudit(LlccAuditState::complete);
    return 0;
  }

  const auto &read = pianollcc::kAuditReads[gLlccAuditIndex];
  const auto physical = static_cast<unsigned long long>(read.physical());
  if (command == token && !gLlccAuditPrepared) {
    IOLog("PianoA830: sens-trace29 PRE token=%d seq=%u op=load pa=0x%08llx\n", token, gLlccAuditIndex + 1, physical);
    gLlccAuditPrepared = true;
    return 0;
  }
  if (command != -token || !gLlccAuditPrepared) return EINVAL;
  gLlccAuditPrepared = false;
  uint32_t value = 0;
  const bool answered = llccRead(&gMayMap, read.view, read.offset, &value);
  const bool ok = answered && read.accepts(value);
  ++gLlccAuditIndex;
  IOLog("PianoA830: sens-trace29 POST token=%d seq=%u op=load pa=0x%08llx value=0x%08x bit12=%u status=%s next=%s\n",
        token, gLlccAuditIndex, physical, value, (value >> pianollcc::kScid) & 1u,
        !answered ? "access-failed" : !ok ? "unexpected" : "ok",
        !ok ? "stop" : gLlccAuditIndex == pianollcc::kAuditCount ? "release" : "step");
  if (!ok) {
    finishLlccAudit(LlccAuditState::stopped);
    return EIO;
  }
  return 0;
}

bool llccIslandSkip(unsigned index, const LlccIslandReadback &readback)
{
  using namespace pianollcc;
  return kIsland3Accesses[index].seq == 20 && !(readback.before[4] & kIsland3Bit);
}

enum class LlccIslandResult { ok, accessFailed, unexpected, readbackFailed };

// Both modes use the same single-access path and stop at the same failing row.
LlccIslandResult llccIslandAccess(unsigned index, LlccIslandReadback &readback, uint32_t &value)
{
  using namespace pianollcc;
  const auto &access = kIsland3Accesses[index];
  const bool alloc3 = access.offset == kIsland3Alloc3;
  const uint32_t expected = alloc3 ? readback.before[4] & ~kIsland3Bit : access.expected;
  value = access.store ? expected : 0;
  const bool answered = access.store ? llccWrite(&gMayMap, access.view, access.offset, value)
                                     : llccRead(&gMayMap, access.view, access.offset, &value);
  if (!answered) return LlccIslandResult::accessFailed;

  if (access.seq == 4 || access.seq == 19) {
    const unsigned slot = alloc3 ? 4 : index - 3;
    readback.before[slot] = value;
    readback.beforeSeen |= 1u << slot;
  } else if (access.seq == 21) {
    const unsigned slot = index - (kIsland3Count - 5);
    readback.after[slot] = value;
    readback.afterSeen |= 1u << slot;
  }
  if (access.store || (value & access.mask) == (expected & access.mask)) return LlccIslandResult::ok;
  return access.seq == 21 ? LlccIslandResult::readbackFailed : LlccIslandResult::unexpected;
}

const char *llccIslandResultName(LlccIslandResult result)
{
  switch (result) {
  case LlccIslandResult::ok: return "ok";
  case LlccIslandResult::accessFailed: return "access-failed";
  case LlccIslandResult::readbackFailed: return "write not accepted";
  case LlccIslandResult::unexpected: return "unexpected";
  }
  return "unexpected";
}

int llccIsland3Command(int command, int token)
{
  using namespace pianollcc;
  if (gLlccAuditState != LlccAuditState::waiting) return ECANCELED;
  if (gLlccAuditIndex == kIsland3Count) {
    if (command != token) return EINVAL;
    gLlccIsland3Result = "ok";
    finishLlccAudit(LlccAuditState::complete);
    return 0;
  }

  const auto &access = kIsland3Accesses[gLlccAuditIndex];
  const auto physical = static_cast<unsigned long long>(access.physical());
  const char *op = access.store ? "store" : "load";
  if (command == token && !gLlccAuditPrepared) {
    if (llccIslandSkip(gLlccAuditIndex, gLlccIsland3Readback)) {
      IOLog("PianoA830: sens-island3 SKIP token=%d seq=20 pa=0x%08llx bit12=0\n", token, physical);
      ++gLlccAuditIndex;
      return 0;
    }
    IOLog("PianoA830: sens-island3 PRE token=%d seq=%u op=%s pa=0x%08llx\n", token, access.seq, op, physical);
    gLlccAuditPrepared = true;
    return 0;
  }
  if (command != -token || !gLlccAuditPrepared) return EINVAL;
  gLlccAuditPrepared = false;

  uint32_t value;
  const auto result = llccIslandAccess(gLlccAuditIndex, gLlccIsland3Readback, value);
  const bool ok = result == LlccIslandResult::ok;
  ++gLlccAuditIndex;
  IOLog("PianoA830: sens-island3 POST token=%d seq=%u op=%s pa=0x%08llx value=0x%08x status=%s next=%s\n",
        token, access.seq, op, physical, value,
        llccIslandResultName(result == LlccIslandResult::readbackFailed ? LlccIslandResult::unexpected : result),
        !ok ? "stop" : gLlccAuditIndex == kIsland3Count ? "release" : "step");
  if (!ok) {
    gLlccIsland3Result = llccIslandResultName(result);
    finishLlccAudit(LlccAuditState::stopped);
    return EIO;
  }
  return 0;
}

void llccIslandSummary(const char *mode, const char *result, const LlccIslandReadback &readback)
{
  const auto &b = readback.before;
  const auto &a = readback.after;
  IOLog("PianoA830: %s SUMMARY result=%s ATTR before=%08x/%08x/%08x/%08x after=%08x/%08x/%08x/%08x "
        "ALLOC3 before=%08x after=%08x valid=%02x/%02x\n",
        mode, result, b[0], b[1], b[2], b[3], a[0], a[1], a[2], a[3], b[4], a[4],
        readback.beforeSeen, readback.afterSeen);
}

int sysctlLlccAudit(struct sysctl_oid *oid, void *, int, struct sysctl_req *req)
{
  if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
  IOLockLock(gLlccAuditLock);
  const int token = gLlccAuditGeneration + int(gLlccAuditIndex) + 1;
  int command = gLlccAuditState == LlccAuditState::waiting ? token : gLlccAuditState == LlccAuditState::complete ? 0 : -1;
  int error = sysctl_handle_int(oid, &command, 0, req);
  if (!error && req->newptr) error = gLlccIsland3 ? llccIsland3Command(command, token) : llccAuditCommand(command, token);
  IOLockUnlock(gLlccAuditLock);
  return error;
}

void islandLine(const char *what, const pianollcc::Reading &r, pianollcc::State state)
{
  piano_a830_note("llcc: ISLAND1 (SCID 12) %s: ATTR0 %#x (0x26841300, expect 0x7fffffff) ATTR1 %#x (0x26841308, expect 0) ATTR2 %#x "
                  "ATTR3 %#x; TRP_STATUS %#x (%s); hw-info %#x: %s", what, r.attr[0], r.attr[1], r.attr[2], r.attr[3], r.status,
                  r.status & pianollcc::kActive ? "active" : "not active", r.hwInfo, pianollcc::stateName(state));
}

void islandPublish(IOService *service)
{
  if (!service) return;
  const pianollcc::State s = pianollcc::State(gIslandState);
  char text[64];
  snprintf(text, sizeof text, "%s%s", pianollcc::stateName(s), gIslandProgrammed ? " (programmed by PianoA830)" : "");
  service->setProperty("piano-llcc-island", text);
}

// The starter thread, after piano-gpu-delay and before the survey or the bring-up: the read-only line, then with
// piano-llcc-island=1 (piano-gpu=2) the programming.
void islandStart(IOService *service, unsigned level)
{
  const uint32_t want = bootArg("piano-llcc-island", 0xffffffffu);
  if (bootArg("piano-llcc-audit", 0) == 1 || want == 3 || want == 4) return;
  // Opt-in until a tablet boot has shown the reads are safe under Gunyah: without piano-llcc-island (0 = read only,
  // 1 = read and program) the GPU boot does not touch the LLCC at all and the island stays "unknown".
  if (want == 0xffffffffu) {
    piano_a830_note("llcc: ISLAND1 not read (piano-llcc-island=0 reads it, =1 programs it)");
    return;
  }
  piano_a830_note("llcc: reading the ISLAND1 slice, read only (0x26864000 hw-info, 0x26841300..0x26841314 ATTR0..3, 0x2680c004 status)");
  const pianollcc::Reading r = pianollcc::read(kLlccThread);
  pianollcc::State state = pianollcc::judge(r);
  islandLine("as found", r, state);
  gIslandState = uint8_t(state);
  __atomic_store_n(&gLlccArmed, 1u, __ATOMIC_RELEASE);
  if (want == 1 && level < 2) piano_a830_note("llcc: piano-llcc-island=1 needs piano-gpu=2 (piano-gpu=1 writes nothing): not programmed");
  if (want == 1 && level >= 2) {
    piano_a830_note("llcc: piano-llcc-island=1: reserving ISLAND1's ways (ATTR0 0x7fffffff, ATTR1 0; ATTR2/ATTR3 only where 0)");
    const pianollcc::Program p = pianollcc::program(kLlccThread);
    state = pianollcc::judge(p.after);
    if (p.outcome == pianollcc::Outcome::programmed) gIslandProgrammed = true;
    piano_a830_note("llcc: piano-llcc-island=1: %s; ATTR0 %#x (AND view %#x) ATTR1 %#x ATTR2 %#x%s ATTR3 %#x%s; %u writes, %u banks%s",
                    pianollcc::outcomeName(p.outcome), p.after.attr[0], p.andAttr0, p.after.attr[1], p.after.attr[2],
                    p.wroteAttr2 ? " (written)" : "", p.after.attr[3], p.wroteAttr3 ? " (written)" : "", p.writes, p.banks,
                    p.outcome == pianollcc::Outcome::readbackFailed ? (p.restored ? "; restored as found" : "; NOT restored") : "");
    if (p.outcome == pianollcc::Outcome::readbackFailed)
      piano_a830_note("llcc: the first mismatch: +%#x read %#x, expected %#x (LLCC writes do not reach the slice here)", p.badOffset,
                      p.badValue, p.badExpect);
    gIslandState = uint8_t(state);
  } else if (want > 1) {
    piano_a830_note("llcc: piano-llcc-island=%u: unknown (1 programs); nothing written", want);
  }
  piano_a830_note("llcc: island %s", state == pianollcc::State::reserved ? "ready (reserved ways 0x7fffffff, no bonus ways)"
                                                                         : "NOT ready: CX-off sleep must stay off (docs/power.md §3.6)");
  islandPublish(service);
}
}  // namespace

// The shared ADSP starter calls this once before firmware/PAS. Keep the legacy
// GPU readiness path unarmed so it cannot issue extra LLCC reads.
extern "C" void piano_a830_llcc_island4_run(bool ownsStart)
{
  LlccIslandReadback readback{};
  auto result = LlccIslandResult::ok;
  for (unsigned index = 0; ownsStart && result == LlccIslandResult::ok && index < pianollcc::kIsland3Count; ++index) {
    if (llccIslandSkip(index, readback)) continue;
    uint32_t value;
    result = llccIslandAccess(index, readback, value);
  }
  llccIslandSummary("sens-island4", ownsStart ? llccIslandResultName(result) : "ownership-unproven", readback);
}

// Called once by the normal shared ADSP owner before firmware/PAS. Only the
// root sysctl's acknowledged access command can reach the LLCC while it waits.
extern "C" void piano_a830_llcc_audit_wait(bool island3)
{
  gLlccIsland3 = island3;
  gLlccAuditLock = IOLockAlloc();
  if (!gLlccAuditLock) {
    if (island3) llccIslandSummary("sens-island3", "lock-failed", gLlccIsland3Readback);
    else IOLog("PianoA830: sens-trace29 STOP lock allocation failed\n");
    return;
  }
  IOLockLock(gLlccAuditLock);
  gLlccAuditGeneration = int((mach_absolute_time() & 0x7fffffull) << 8);
  static struct sysctl_oid oid;
  oid.oid_parent = &sysctl__machdep_children;
  oid.oid_number = OID_AUTO;
  oid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  oid.oid_name = "piano_llcc_diag";
  oid.oid_handler = sysctlLlccAudit;
  oid.oid_fmt = "I";
  oid.oid_descr = island3 ? "Root-only, host-acknowledged LLCC SCID12 access" : "Root-only, host-acknowledged LLCC audit read";
  oid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&oid);
  if (island3)
    IOLog("PianoA830: sens-island3 READY token=%d steps=%u awaiting host\n", gLlccAuditGeneration + 1, pianollcc::kIsland3Count);
  else
    IOLog("PianoA830: sens-trace29 READY token=%d steps=%u awaiting host\n", gLlccAuditGeneration + 1, pianollcc::kAuditCount);
  uint64_t deadline;
  clock_interval_to_deadline(330, kSecondScale, &deadline);
  while (gLlccAuditState == LlccAuditState::waiting) {
    if (IOLockSleepDeadline(gLlccAuditLock, &gLlccAuditState, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
        gLlccAuditState == LlccAuditState::waiting) {
      gLlccAuditState = LlccAuditState::stopped;
      if (island3) gLlccIsland3Result = "host-timeout";
      else IOLog("PianoA830: sens-trace29 STOP host handshake timed out\n");
    }
  }
  if (island3) {
    llccIslandSummary("sens-island3", gLlccIsland3Result, gLlccIsland3Readback);
    IOLog("PianoA830: sens-island3 RELEASE seq=22\n");
  }
  IOLockUnlock(gLlccAuditLock);
}

// For kexts linked after PianoA830 (their descriptors name it as a library) and the platform function: the two words read again.
extern "C" bool piano_a830_llcc_island_ready(void)
{
  if (!__atomic_load_n(&gLlccArmed, __ATOMIC_ACQUIRE)) return false;
  return pianollcc::ready(kLlccAny);
}

// Read-only first capacity-refusal snapshots survive process exit and bypass IOLog format-address decoding.
namespace {
int sysctlGpuRefusals(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  bool any=false;
  for(unsigned entry=0;entry<NativeAdrenoFailureEntries;++entry) {
    for(unsigned row=0;row<NativeAdrenoFailureRows;++row) {
      char line[160];
      const int count=native_adreno_failure_line(entry,row,line,sizeof(line)-1);
      if(count<=0)break;
      const size_t bytes=size_t(count)<sizeof(line)-1?size_t(count):sizeof(line)-2;
      line[bytes]='\n';
      const int error=SYSCTL_OUT(req,line,bytes+1);
      if(error)return error;
      any=true;
    }
  }
  for(unsigned slot=0;slot<NativeAdrenoMaxClients;++slot) {
    for(unsigned closed=0;closed<2;++closed) {
      char line[NativeAdrenoResidencyLineBytes];
      const int count=native_adreno_residency_line(slot,closed!=0,line,sizeof(line)-1);
      if(count<=0)continue;
      const size_t bytes=size_t(count)<sizeof(line)-1?size_t(count):sizeof(line)-2;
      line[bytes]='\n';
      const int error=SYSCTL_OUT(req,line,bytes+1);
      if(error)return error;
      any=true;
    }
  }
  return any?SYSCTL_OUT(req,"",1):SYSCTL_OUT(req,"none\n",6);
}
// machdep.piano_gpu_input_floor (root): the DCVS input floor in MHz (0 off; 160..1100), A830Dcvs::setInputFloor. Read: the floor in force.
int sysctlGpuInputFloor(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(adreno830_dcvs_input_floor());
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value<0)return EINVAL;
  const int r=adreno830_dcvs_set_input_floor(uint32_t(value));
  if(r==-2)return EINVAL;
  if(r)return EAGAIN;
  gpuLine("input floor %d MHz (machdep.piano_gpu_input_floor; hold 80 ms after each submission)",value);
  return 0;
}
void registerGpuInputFloor()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_input_floor";oid.oid_handler=sysctlGpuInputFloor;oid.oid_fmt="I";
  oid.oid_descr="GPU DCVS input floor (MHz) held 80 ms after each submission; 0 off";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
// ---- the boost floor (piano-gpu-boost, machdep.piano_gpu_boost) ----------------------------------------------------------------
volatile uint32_t gBoostEnabled;   // piano-gpu-boost=1 at start; machdep.piano_gpu_boost 0/1 at run time
uint32_t gBoostArg;                // piano-gpu-boost=1 on the command line (never changes)
IOReturn gpuBoost(uint32_t mhzOrLevel, uint32_t holdMs, uint32_t capMhz = 0)
{
  // Unsupported only without the boot argument (senders stop asking for the boot); switched off at run time: Offline, so the senders
  // keep asking and the next switch-on works at once (gpu-boost342a: the A/B's first "off" ended the senders for the rest of the boot).
  if (!gBoostArg) return kIOReturnUnsupported;
  if (!__atomic_load_n(&gBoostEnabled, __ATOMIC_ACQUIRE)) return kIOReturnOffline;
  // gpu/fullopp1: param4 a cap for the hold (PianoThermal's launch: Android's 967 MHz); 0 none
  if (capMhz ? adreno830_dcvs_boost_capped(mhzOrLevel, holdMs, capMhz) : adreno830_dcvs_boost(mhzOrLevel, holdMs)) return kIOReturnNotReady;
  const uint32_t n = adreno830_dcvs_boost_requests();
  if (n <= 3 || n % 1000 == 0)
    gpuLine("boost #%u: %u%s for %u ms (piano-gpu-boost; every ceiling still wins)", n, mhzOrLevel, mhzOrLevel == 0 ? " (input)" :
            mhzOrLevel <= 16 ? " (GMU level)" : " MHz", holdMs);
  return kIOReturnSuccess;
}
int sysctlGpuBoost(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
  int value = int(__atomic_load_n(&gBoostEnabled, __ATOMIC_ACQUIRE));
  const int err = sysctl_handle_int(oidp, &value, 0, req);
  if (err || !req->newptr) return err;
  if (value != 0 && value != 1) return EINVAL;
  if (value && !gBoostArg) return EPERM;   // the boot argument decides whether senders exist at all
  __atomic_store_n(&gBoostEnabled, uint32_t(value), __ATOMIC_RELEASE);
  gpuLine("boost %s (machdep.piano_gpu_boost; %u requests so far)", value ? "on" : "off", adreno830_dcvs_boost_requests());
  return 0;
}
void registerGpuBoost()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_boost";oid.oid_handler=sysctlGpuBoost;oid.oid_fmt="I";
  oid.oid_descr="GPU boost floor requests (piano-gpu-boost) taken: 1, refused: 0";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
// machdep.piano_gpu_busy (read-only, gpu-boost342c): the frame-weighted counters an A/B diffs between its marks (A830DcvsStats).
int sysctlGpuBusy(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  uint64_t v[6];
  if(adreno830_dcvs_busy(v))return SYSCTL_OUT(req,"none\n",6);
  char line[192];
  const int n=snprintf(line,sizeof line,"busy_us %llu low_busy_us %llu submits %llu bursts %llu low_submits %llu tops %llu\n",
                       (unsigned long long)(v[0]/1000),(unsigned long long)(v[1]/1000),(unsigned long long)v[2],(unsigned long long)v[3],
                       (unsigned long long)v[4],(unsigned long long)v[5]);
  if(n<=0)return EIO;
  return SYSCTL_OUT(req,line,size_t(n)<sizeof line?size_t(n)+1:sizeof line);
}
void registerGpuBusy()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_busy";oid.oid_handler=sysctlGpuBusy;oid.oid_fmt="A";
  oid.oid_descr="GPU busy time (all, at the lowest level), submissions, bursts, burst submissions at the lowest level, bursts to the top";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
void registerGpuRefusals()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_refusals";oid.oid_handler=sysctlGpuRefusals;oid.oid_fmt="A";
  oid.oid_descr="First GPU capacity refusals and per-connection residency high water";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
// ---- test-only GPU fault injection (piano-gpu-fault-test=1, machdep.piano_gpu_fault_test; gpu-daily3, from gpu/daily3c 6b890883) ------
// Inert without the boot argument: the sysctl is then not registered. Root writes 1 (a software-watchdog hang, the class of the
// gpu-daily1 iconservicesagent hang) or 2 (a CP hang) and the fault is reported to A830Recovery exactly as the fence watchdog or the
// hang interrupt would report it; the recovery that follows is the real one (on the daily line: the daily recovery, no strict). Nothing
// stops the GPU: whoever's job happens to be in flight is the "culprit" (machdep.piano_gpu_fault_hold below gives a real one). At most one per 70 s, so the recovery's "third in
// 60 s is final" rule stays out of reach. Read: injections so far.
uint32_t gFaultTests;
uint64_t gFaultTestLastNs;
int sysctlGpuFaultTest(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(__atomic_load_n(&gFaultTests,__ATOMIC_ACQUIRE));
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value!=1 && value!=2)return EINVAL;
  uint64_t now=0;
  absolutetime_to_nanoseconds(mach_absolute_time(),&now);
  const uint64_t last=__atomic_load_n(&gFaultTestLastNs,__ATOMIC_ACQUIRE);
  if(last && now-last<70ull*1000000000ull)return EBUSY;
  __atomic_store_n(&gFaultTestLastNs,now,__ATOMIC_RELEASE);
  const uint32_t n=__atomic_add_fetch(&gFaultTests,1u,__ATOMIC_ACQ_REL);
  gpuLine("fault test #%u: %s reported to the recovery (machdep.piano_gpu_fault_test, piano-gpu-fault-test=1)",n,
          value==1?"a watchdog hang":"a CP hang");
  adreno830_recovery_report(value==1?uint8_t(A830FaultWatchdog):uint8_t(A830FaultHang),0x7E57000000000000ull|n);
  return 0;
}
void registerGpuFaultTest()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_fault_test";oid.oid_handler=sysctlGpuFaultTest;oid.oid_fmt="I";
  oid.oid_descr="Test only (piano-gpu-fault-test=1): 1 reports a watchdog hang, 2 a CP hang to the GPU recovery; at most one per 70 s";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
// ---- test-only real GPU hang with a known culprit (piano-gpu-fault-test=1, machdep.piano_gpu_fault_hold; gpu-recover2) --------------
// Root writes a pid: the next submission of that process's GPU connections starts with a wait nothing ever satisfies
// (NativeAdrenoSubmit::armHold), so the CP stops inside that process's job and every later job (WindowServer's frames included) queues
// behind it, in flight. The fence watchdog finds the hang after piano-gpu-hang-ms (2 s) and the recovery runs: with the replay
// (machdep.piano_gpu_recovery_replay=1) only that process is faulted, the others' jobs are replayed. At most one per 35 s (two
// recoveries in 60 s are allowed, the third is final). ESRCH: no live GPU connection of that pid. Read: holds armed so far.
uint32_t gFaultHolds;
uint64_t gFaultHoldLastNs;
int sysctlGpuFaultHold(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(__atomic_load_n(&gFaultHolds,__ATOMIC_ACQUIRE));
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value<=1)return EINVAL;
  uint64_t now=0;
  absolutetime_to_nanoseconds(mach_absolute_time(),&now);
  const uint64_t last=__atomic_load_n(&gFaultHoldLastNs,__ATOMIC_ACQUIRE);
  if(last && now-last<35ull*1000000000ull)return EBUSY;
  char name[20]={};
  const unsigned connections=native_adreno_fault_hold(value,name);
  if(!connections)return ESRCH;
  __atomic_store_n(&gFaultHoldLastNs,now,__ATOMIC_RELEASE);
  const uint32_t n=__atomic_add_fetch(&gFaultHolds,1u,__ATOMIC_ACQ_REL);
  gpuLine("fault hold #%u: pid %d %s (%u connection%s): its next submission stops the CP (a real hang, culprit known; replay %s)",n,
          value,name,connections,connections==1?"":"s",adreno830_recovery_replay_mode()?"on":"off");
  return 0;
}
void registerGpuFaultHold()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_fault_hold";oid.oid_handler=sysctlGpuFaultHold;oid.oid_fmt="I";
  oid.oid_descr="Test only (piano-gpu-fault-test=1): a pid whose next GPU submission stops the CP (a real hang); at most one per 35 s";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
// ---- the recovery's replay (2026-11-22, private 20261122-render/ANALYSIS.md): an A/B switch and its counters --------------------------
// piano-gpu-recovery-replay=0 (default 1) and machdep.piano_gpu_recovery_replay (root, 0/1, read at each recovery's start): 1 replays
// the jobs of innocent clients and faults the culprit alone (Linux msm recover_worker's behaviour), 0 fails every job in flight and
// faults every connection that had one (the behaviour before: an innocent WindowServer lost its device and the session ended).
int sysctlGpuRecoveryReplay(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(adreno830_recovery_replay_mode());
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value!=0 && value!=1)return EINVAL;
  if(uint32_t(value)!=adreno830_recovery_replay_mode())
    gpuLine("recovery replay %s (machdep.piano_gpu_recovery_replay): %s",value?"on":"off",
            value?"innocent clients' jobs are replayed, the culprit alone is faulted":"every job in flight fails (the old behaviour)");
  adreno830_recovery_set_replay(uint32_t(value));
  return 0;
}
int sysctlGpuRecovery(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  A830RecoveryStats s{};
  if(!adreno830_recovery_stats(&s))return SYSCTL_OUT(req,"none\n",6);
  char line[256];
  const int n=snprintf(line,sizeof line,"replay %u recoveries %u replay_recoveries %u jobs_replayed %u jobs_failed %u failures %u "
                       "reports %u coalesced %u watchdog %u last_us %u max_us %u last_cause %u lost_cause %u holds %u\n",
                       adreno830_recovery_replay_mode(),s.recoveries,s.replayRecoveries,s.jobsReplayed,s.jobsFailed,s.failures,s.reports,
                       s.coalesced,s.watchdogHangs,s.lastUs,s.maxUs,unsigned(s.lastCause),unsigned(s.lostCause),
                       __atomic_load_n(&gFaultHolds,__ATOMIC_ACQUIRE));
  if(n<=0)return EIO;
  return SYSCTL_OUT(req,line,size_t(n)<sizeof line?size_t(n)+1:sizeof line);
}
void registerGpuRecovery()
{
  static struct sysctl_oid replay,stats;
  replay.oid_parent=&sysctl__machdep_children;replay.oid_number=OID_AUTO;
  replay.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  replay.oid_name="piano_gpu_recovery_replay";replay.oid_handler=sysctlGpuRecoveryReplay;replay.oid_fmt="I";
  replay.oid_descr="GPU recovery: 1 replays innocent clients' jobs and faults the culprit alone, 0 fails every job in flight";
  replay.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&replay);
  stats.oid_parent=&sysctl__machdep_children;stats.oid_number=OID_AUTO;
  stats.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  stats.oid_name="piano_gpu_recovery";stats.oid_handler=sysctlGpuRecovery;stats.oid_fmt="A";
  stats.oid_descr="GPU recovery counters: mode, recoveries, replays, jobs replayed and failed, last and longest gate-to-open time (us)";
  stats.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&stats);
}
// ---- GPU-PAGED P0b (adreno830/NativeAdrenoPoolBypass.h; private analysis DESIGN.md section 4 and its gate) -------------------------
// machdep.piano_gpu_pool_bypass (root, 0/1; piano-gpu-pool-bypass=1 at start): 1 sends every NEW BoAlloc past the BO pool to the path
// outside it (with piano-gpu-user-vm=1 the pageable named entry, 4 KiB pages wired by prepare()); BOs already allocated keep their
// backing. 0, the default, is today's behaviour. One boot can then run the same workload pooled and scattered.
int sysctlGpuPoolBypass(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(__atomic_load_n(&native_adreno_pool_bypass,__ATOMIC_ACQUIRE));
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value!=0 && value!=1)return EINVAL;
  const uint32_t before=__atomic_exchange_n(&native_adreno_pool_bypass,uint32_t(value),__ATOMIC_ACQ_REL);
  if(before!=uint32_t(value))__atomic_fetch_add(&native_adreno_pool_bypass_flips,1u,__ATOMIC_RELAXED);
  uint32_t poolMib=0,usedMib=0;
  NativeAdrenoMemory::usage(&poolMib,&usedMib,nullptr);
  gpuLine("pool bypass %s (machdep.piano_gpu_pool_bypass, GPU-PAGED P0b): new BOs %s; pool %u/%u MiB in use, flip #%u",
          value?"on":"off",value?"skip the BO pool":"use the BO pool as before",usedMib,poolMib,
          unsigned(__atomic_load_n(&native_adreno_pool_bypass_flips,__ATOMIC_RELAXED)));
  return 0;
}
// machdep.piano_gpu_paged (read-only): the P0b figures, one row per line (NativeAdrenoPoolBypassLine): BoAllocs by path with bytes,
// failures and a latency histogram, the wiring and physical lookups of allocateBacking, GPU map time by backing, TLB invalidations,
// the per-submission maintenance walk. Cumulative since boot: an A/B diffs two reads.
int sysctlGpuPaged(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  for(unsigned row=0;;++row) {
    char line[NativeAdrenoP0bLineBytes];
    const int count=NativeAdrenoPoolBypassLine(row,line,sizeof(line)-1);
    if(count<=0)break;
    const size_t bytes=size_t(count)<sizeof(line)-1?size_t(count):sizeof(line)-2;
    line[bytes]='\n';
    const int error=SYSCTL_OUT(req,line,bytes+1);
    if(error)return error;
  }
  return SYSCTL_OUT(req,"",1);
}
// ---- GPU-PAGED P1 (adreno830/NativeAdrenoVm.h paged mode; private P1-GATE.md) ----------------------------------------------------
// piano-gpu-mem=paged (read at probe, before S3): no BO pool, tables on demand, one record per mapping; piano-gpu-mem-cap-mib and
// piano-gpu-mem-ws-reserve-mib (0: 7168 / 1536); piano-gpu-pt-audit=N (the tick audits every N s; 0: only on request).
// machdep.piano_gpu_pt (read-only): the mode, then the paged VM's figures (NativeAdrenoPagedLine rows 1-3).
// machdep.piano_gpu_pt_audit (root, write 1): the whole table tree audited now against the records (EIO when it finds a fault).
bool gpuMemPagedArg()
{
  char value[16] = {};
  if (!PE_parse_boot_argn("piano-gpu-mem", value, sizeof value - 1)) return false;
  return !strncmp(value, "paged", sizeof value);
}
int sysctlGpuPt(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  NativeAdrenoVmPagedStats s{};
  if(NativeAdrenoVm *vm=NativeAdrenoGpuContextVm())vm->pagedStats(&s);
  for(unsigned row=0;row<4;++row) {
    char line[200];
    const int count=NativeAdrenoPagedLine(row,s,line,sizeof(line)-1);
    if(count<=0)break;
    const size_t bytes=size_t(count)<sizeof(line)-1?size_t(count):sizeof(line)-2;
    line[bytes]='\n';
    const int error=SYSCTL_OUT(req,line,bytes+1);
    if(error)return error;
  }
  return SYSCTL_OUT(req,"",1);
}
int sysctlGpuPtAudit(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=0;
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value!=1)return EINVAL;
  NativeAdrenoVm *vm=NativeAdrenoGpuContextVm();
  if(!vm || !vm->paged())return ENODEV;
  ArmSmmu4KPagedAuditResult r{};
  const IOReturn status=vm->auditTables(&r);
  gpuLine("pt audit (machdep.piano_gpu_pt_audit): %s: why %u at %#llx, %llu tables, %llu leaves, %llu descriptors read",
          status==kIOReturnSuccess?"ok":"FAILED",unsigned(r.why),(unsigned long long)r.iova,(unsigned long long)r.tables,
          (unsigned long long)r.leaves,(unsigned long long)r.descriptor_reads);
  return status==kIOReturnSuccess?0:EIO;
}
// GPU-PAGED P1b: machdep.piano_gpu_wire_workers (root, 0..7; piano-gpu-wire-workers at start): kernel threads that wire the parts of a
// user BO of >= 1 MiB at the same time as the BoAlloc thread (0: one named entry wired by that thread, as before); machdep.piano_gpu_wire
// (read-only): BOs split, their parts, BOs wired whole for want of workers, failed splits, threads started, parts the workers wired.
int sysctlGpuWireWorkers(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(__atomic_load_n(&native_adreno_wire_workers,__ATOMIC_ACQUIRE));
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value<0 || value>int(NativeAdrenoWireMaxWorkers))return EINVAL;
  __atomic_store_n(&native_adreno_wire_workers,uint32_t(value),__ATOMIC_RELEASE);
  gpuLine("wire workers %d (machdep.piano_gpu_wire_workers, GPU-PAGED P1b): user BOs of >= 1 MiB %s",value,
          value?"are wired as parts at the same time":"are wired whole by the BoAlloc thread");
  return 0;
}
// GPU-PAGED P1d: machdep.piano_gpu_wire_priority (root; piano-gpu-wire-priority at start, default 47): the priority the workers wire
// at (0 or 81: kernel_thread_start's BASEPRI_KERNEL, gpu-p1b/p1c's, above WindowServer's compositing threads); applied before a worker's
// next part.
int sysctlGpuWirePriority(struct sysctl_oid *oidp,void *,int,struct sysctl_req *req)
{
  int value=int(__atomic_load_n(&native_adreno_wire_priority,__ATOMIC_ACQUIRE));
  const int err=sysctl_handle_int(oidp,&value,0,req);
  if(err || !req->newptr)return err;
  if(value<0 || value>int(NativeAdrenoWirePriorityKernel))return EINVAL;
  __atomic_store_n(&native_adreno_wire_priority,uint32_t(value),__ATOMIC_RELEASE);
  gpuLine("wire priority %d (machdep.piano_gpu_wire_priority, GPU-PAGED P1d)",value?value:int(NativeAdrenoWirePriorityKernel));
  return 0;
}
int sysctlGpuWire(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  char line[200];
  typedef unsigned long long u64;
  const int n=snprintf(line,sizeof line,"wire workers=%u split=%llu parts=%llu whole=%llu failed=%llu threads=%llu worker-parts=%llu priority=%u\n",
                       unsigned(__atomic_load_n(&native_adreno_wire_workers,__ATOMIC_ACQUIRE)),u64(native_adreno_wire_stats[0]),
                       u64(native_adreno_wire_stats[1]),u64(native_adreno_wire_stats[2]),u64(native_adreno_wire_stats[3]),
                       u64(native_adreno_wire_stats[4]),u64(native_adreno_wire_stats[5]),
                       unsigned(__atomic_load_n(&native_adreno_wire_priority,__ATOMIC_ACQUIRE)));
  if(n<=0)return EIO;
  const int error=SYSCTL_OUT(req,line,size_t(n)<sizeof line?size_t(n):sizeof line-1);
  return error?error:SYSCTL_OUT(req,"",1);
}
void registerGpuPt()
{
  static struct sysctl_oid pt,audit,workers,wire,wirePriority;
  wirePriority.oid_parent=&sysctl__machdep_children;wirePriority.oid_number=OID_AUTO;
  wirePriority.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  wirePriority.oid_name="piano_gpu_wire_priority";wirePriority.oid_handler=sysctlGpuWirePriority;wirePriority.oid_fmt="I";
  wirePriority.oid_descr="GPU-PAGED P1d: the priority the wiring workers run at (0/81: the kernel's, above the compositor; default 47)";
  wirePriority.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&wirePriority);
  workers.oid_parent=&sysctl__machdep_children;workers.oid_number=OID_AUTO;
  workers.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  workers.oid_name="piano_gpu_wire_workers";workers.oid_handler=sysctlGpuWireWorkers;workers.oid_fmt="I";
  workers.oid_descr="GPU-PAGED P1b: kernel threads wiring the parts of a large user BO at the same time (0: serial, as before)";
  workers.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&workers);
  wire.oid_parent=&sysctl__machdep_children;wire.oid_number=OID_AUTO;
  wire.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  wire.oid_name="piano_gpu_wire";wire.oid_handler=sysctlGpuWire;wire.oid_fmt="A";
  wire.oid_descr="GPU-PAGED P1b: user BOs split into parts, parts, BOs wired whole, failures, worker threads, parts the workers wired";
  wire.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&wire);
  pt.oid_parent=&sysctl__machdep_children;pt.oid_number=OID_AUTO;
  pt.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  pt.oid_name="piano_gpu_pt";pt.oid_handler=sysctlGpuPt;pt.oid_fmt="A";
  pt.oid_descr="GPU-PAGED P1: the GPU VM's mode and, paged, its on-demand page tables, records, cap, trims and refusals";
  pt.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&pt);
  audit.oid_parent=&sysctl__machdep_children;audit.oid_number=OID_AUTO;
  audit.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  audit.oid_name="piano_gpu_pt_audit";audit.oid_handler=sysctlGpuPtAudit;audit.oid_fmt="I";
  audit.oid_descr="GPU-PAGED P1: write 1 to audit the paged GPU page tables against the mappings now (EIO on a finding)";
  audit.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&audit);
}
int sysctlGpuTier(struct sysctl_oid *,void *,int,struct sysctl_req *req)
{
  if(req->newptr)return EPERM;
  const auto tier=NativeAdrenoMemory::tierStats();
  char line[256];
  const int n=snprintf(line,sizeof line,
      "mode=%u state=%s runs=%u checked=%u pass=%u fail=%u pass-mask=%#x free-mib=%llu used-mib=%llu refusals=%llu allocs=%llu",
      tier.mode,tier.state,tier.runs,tier.tested,tier.passed,tier.tested-tier.passed,tier.pass_mask,
      (unsigned long long)(tier.free_bytes>>20),(unsigned long long)(tier.used_bytes>>20),
      (unsigned long long)tier.refusals,(unsigned long long)tier.allocations);
  if(n<0 || size_t(n)>=sizeof line)return EIO;
  return SYSCTL_OUT(req,line,size_t(n)+1);
}
void registerGpuTier()
{
  static struct sysctl_oid tier;
  tier.oid_parent=&sysctl__machdep_children;tier.oid_number=OID_AUTO;
  tier.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  tier.oid_name="piano_gpu_tier";tier.oid_handler=sysctlGpuTier;tier.oid_fmt="A";
  tier.oid_descr="RAM16G R1: region1 GPU tier state, per-run qualification mask, capacity and allocation counters";
  tier.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&tier);
}
void configureGpuTier()
{
  const unsigned mode=a830tier::bootArgMode(PE_boot_args());
  if(!mode)return;
  IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
  OSData *property=chosen?OSDynamicCast(OSData,chosen->getProperty("piano-extra-ram")):nullptr;
  NativeAdrenoMemory::configureTier(mode,property?static_cast<const uint8_t *>(property->getBytesNoCopy()):nullptr,
                                 property?property->getLength():0,gPhysBase,gPhysSize);
  if(chosen)chosen->release();
  const auto tier=NativeAdrenoMemory::tierStats();
  piano_a830_note("gpu tier: mode=%u state=%s runs=%u",mode,tier.mode?tier.state:"no-lock",tier.runs);
}

void registerGpuPaged()
{
  static struct sysctl_oid bypass,paged;
  bypass.oid_parent=&sysctl__machdep_children;bypass.oid_number=OID_AUTO;
  bypass.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  bypass.oid_name="piano_gpu_pool_bypass";bypass.oid_handler=sysctlGpuPoolBypass;bypass.oid_fmt="I";
  bypass.oid_descr="GPU-PAGED P0b: 1 = new GPU BOs skip the BO pool (the scattered per-page path), 0 = the pool as before";
  bypass.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&bypass);
  paged.oid_parent=&sysctl__machdep_children;paged.oid_number=OID_AUTO;
  paged.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
  paged.oid_name="piano_gpu_paged";paged.oid_handler=sysctlGpuPaged;paged.oid_fmt="A";
  paged.oid_descr="GPU-PAGED P0b figures: BoAlloc by path, wiring, physical lookups, map time by backing, TLB invalidations, submissions";
  paged.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&paged);
}
} // namespace

// ---- the GPU's ceiling slots (PianoGpuCeiling.h) ---------------------------------------------------------------------------------
extern "C" void adreno830_note_ceiling_holder(uint32_t client);
extern "C" void adreno830_set_skin_table(uint32_t table);
extern "C" uint32_t adreno830_skin_table(void);
extern "C" size_t adreno830_gpu_state(char *out, size_t size);
namespace {
IOLock *gCeilingLock;
pianogpu::CeilingSlots gCeiling;
uint32_t gCeilingChanges;

IOReturn ceilingSet(unsigned client, uint32_t khz)
{
  if (client >= pianogpu::kCeilingClients) return kIOReturnBadArgument;
  if (gCeilingLock) IOLockLock(gCeilingLock);
  bool changed = false;
  gCeiling.set(client, khz, &changed);
  const pianogpu::CeilingSlots now = gCeiling;
  if (changed) adreno830_dcvs_set_ceiling(now.effective);
  adreno830_note_ceiling_holder(now.holder());   // gpu/throttle1: the LV line's and machdep.piano_gpu_state's "lpm" / "thermal"
  const uint32_t n = changed ? ++gCeilingChanges : 0;
  if (gCeilingLock) IOLockUnlock(gCeilingLock);
  if (changed && A830IdleLogged(n)) {
    if (now.effective)
      gpuLine("ceiling %u MHz (%s's; slots thermal %u, power profile %u, %u, %u MHz; the lowest wins; #%u)", now.effective / 1000,
              pianogpu::ceilingClientName(now.holder()), now.khz[0] / 1000, now.khz[1] / 1000, now.khz[2] / 1000, now.khz[3] / 1000, n);
    else
      gpuLine("ceiling cleared (no slot holds one; #%u)", n);
  }
  return kIOReturnSuccess;
}
}  // namespace

// ---- gpu/fullopp1: the sweep's ceiling (test only, piano-gpu-fullopp=1) ---------------------------------------------------------------
// machdep.piano_gpu_sweep_ceiling (root): MHz in ceiling slot 2 (the lowest slot still wins, so Low Power Mode and the thermal trip keep
// theirs), 0 clears it. The per-OPP holds of spec gpu-fullopp1 set it to each level under the bench (the governor climbs to it).
namespace {
uint32_t gSweepMhz;
int sysctlGpuSweepCeiling(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
  int value = int(__atomic_load_n(&gSweepMhz, __ATOMIC_ACQUIRE));
  const int err = sysctl_handle_int(oidp, &value, 0, req);
  if (err || !req->newptr) return err;
  if (value < 0 || (value && (value < 160 || value > 1100))) return EINVAL;
  if (ceilingSet(2, uint32_t(value) * 1000u) != kIOReturnSuccess) return EIO;
  __atomic_store_n(&gSweepMhz, uint32_t(value), __ATOMIC_RELEASE);
  gpuLine("sweep ceiling %d MHz (machdep.piano_gpu_sweep_ceiling, slot 2)", value);
  return 0;
}
void registerGpuSweepCeiling()
{
  static struct sysctl_oid oid;
  oid.oid_parent=&sysctl__machdep_children;oid.oid_number=OID_AUTO;
  oid.oid_kind=int(CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_OID2);
  oid.oid_name="piano_gpu_sweep_ceiling";oid.oid_handler=sysctlGpuSweepCeiling;oid.oid_fmt="I";
  oid.oid_descr="Test only (piano-gpu-fullopp=1): GPU ceiling slot 2 in MHz for the OPP sweep; 0 clears it";
  oid.oid_version=SYSCTL_OID_VERSION;sysctl_register_oid(&oid);
}
}  // namespace

// ---- gpu/throttle1 (private/host-checks/20261124-gpu-throttle/TABLE.md): the skin table and the GPU state ----------------------
// machdep.piano_gpu_skin_table (root, 0/1; boot default piano-gpu-skin-table, 1): 1 the docked table (one GMU level per 2 C band of
// the virtual skin, 1050 from 35 C down to 525 from 55 C), 0 Android's normal scene (1050 from 25 C, 734 from 45 C). Only the
// fullopp guard uses it; the die ladder, the XO rules, the guard exit and the unsafe exit are the same in both.
// machdep.piano_gpu_state (read only): the voted level and MHz, the cap and MHz, why (demand = the governor votes below the cap, else
// the binding limit: lpm, thermal, top, ladder, skin25, skin45, skinband, xo, skin-unknown, hard, startup, permit, knob, boostcap), the
// governor's level, the table, its band, the guard's step, the external slot, skin / GPU raw / filtered / XO in deci-C.
namespace {
int sysctlGpuSkinTable(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
  int value = int(adreno830_skin_table());
  const int err = sysctl_handle_int(oidp, &value, 0, req);
  if (err || !req->newptr) return err;
  if (value != 0 && value != 1) return EINVAL;
  adreno830_set_skin_table(uint32_t(value));
  gpuLine("skin table %s (machdep.piano_gpu_skin_table %d)", value ? "docked" : "android normal", value);
  return 0;
}
int sysctlGpuState(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  if (req->newptr) return EPERM;
  char line[320];
  const size_t n = adreno830_gpu_state(line, sizeof line);
  return SYSCTL_OUT(req, line, n + 1 <= sizeof line ? n + 1 : sizeof line);
}
void registerGpuThrottle()
{
  static struct sysctl_oid table, state;
  table.oid_parent = &sysctl__machdep_children; table.oid_number = OID_AUTO;
  table.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  table.oid_name = "piano_gpu_skin_table"; table.oid_handler = sysctlGpuSkinTable; table.oid_fmt = "I";
  table.oid_descr = "GPU skin table of the fullopp guard: 1 docked (2 C bands), 0 Android's normal scene";
  table.oid_version = SYSCTL_OID_VERSION; sysctl_register_oid(&table);
  state.oid_parent = &sysctl__machdep_children; state.oid_number = OID_AUTO;
  state.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
  state.oid_name = "piano_gpu_state"; state.oid_handler = sysctlGpuState; state.oid_fmt = "A";
  state.oid_descr = "GPU level, cap, binding limit, skin table and temperatures (the fullopp guard, once a second)";
  state.oid_version = SYSCTL_OID_VERSION; sysctl_register_oid(&state);
}
}  // namespace

// ---- the system suspend (A830Suspend.h, piano-gpu-suspend=1) --------------------------------------------------------------------
// kOff and kOn on a thread call (acknowledgeSetPowerState when done: the wait for quiet and a recovery can take seconds); the host
// votes in the platform sleep and wake actions (IOPlatformSleepAction / IOPlatformWakeAction, installed from the service's properties:
// IOCPUSleepKernel runs them after every driver's kOff and before any kOn). Lines through piano_a830_note: all of it is thread context
// and the logger thread may not run again before the CPUs park.
namespace {
A830SuspendConfig gSuspendConfig;
A830SuspendMachine gSuspend;
IOService *gService;            // the PianoA830 instance
IOService *gDpu;                // PianoDpu, looked up at kOff (retained until kOn)
thread_call_t gPmCall;
unsigned long gPmTarget;
bool gJoined;
uint32_t gStartDone;
constexpr uint32_t kActionPriority = 100000;   // late in PreSleep, early in PostResume (the wake queue runs in reverse)
constexpr uint32_t kPmBudgetUs = 10 * 1000 * 1000;

const char *scanoutName(pianopower::Scanout s)
{
  return s == pianopower::Scanout::stopped ? "stopped" : s == pianopower::Scanout::running ? "running" : "unknown (running)";
}

void suspendPublish()
{
  if (!gService) return;
  char text[160];
  snprintf(text, sizeof text, "suspends %u, refused %u%s, votes down %u, vote failures %u, wakes %u (failed %u, over %u ms: %u), longest %u us",
           gSuspend.suspends, gSuspend.refusals, gSuspend.blocked ? " (off for this boot)" : "", gSuspend.voteDowns,
           gSuspend.voteFailures, gSuspend.wakes, gSuspend.wakeFailures, gSuspendConfig.wakeBoundMs, gSuspend.overBound,
           gSuspend.maxWakeUs);
  gService->setProperty("piano-gpu-suspend", text);
}

void suspendGpu()
{
  const unsigned n = gSuspend.suspends + gSuspend.refusals + 1;
  if (!gSuspend.sleepWanted()) {
    piano_a830_note("gpu suspend #%u: not taking part (%s): the GPU and its votes stay as they are", n,
                    gSuspend.blocked ? "off for this boot after a failed entry" : "its votes are still down");
    return;
  }
  if (gDpu) gDpu->release();
  gDpu = IOService::copyMatchingService(IOService::serviceMatching("PianoDpu"));
  A830SuspendReport r{};
  // piano-gpu-suspend-fail-test=N (a test's switch, default 0): suspend N counts as a failed entry without touching the GPU (no gate,
  // no wait, no slumber; no GPU suspend for the rest of the boot, as after a real failed entry), for the opt-in cancel's proof.
  const bool failTest = gSuspendConfig.failTest && n == gSuspendConfig.failTest;
  const IOReturn result = failTest ? kIOReturnIOError : adreno830_suspend_gpu(gSuspendConfig.idleWaitMs, &r);
  uint8_t outcome = A830SuspendOk;
  if (result == kIOReturnSuccess) outcome = A830SuspendOk;
  else if (result == kIOReturnNotReady) outcome = A830SuspendOff;
  else if (result == kIOReturnBusy) outcome = A830SuspendBusy;
  else if (result == kIOReturnIOError) outcome = A830SuspendFailed;
  else outcome = r.powerPhase == A830PowerLost ? A830SuspendLost : A830SuspendBusy;   // still being recovered after the wait
  gSuspend.sleepDone(outcome);
  // piano-gpu-suspend-cancel=1 (default 0; docs/power.md 3.7): a failed entry cancels the whole sleep (k27 0064, piano-sleep-cancel=1),
  // asked after the recovery wait below, still inside kOff (the thread call acknowledges after it).
  const bool cancel = gSuspendConfig.cancel && A830SuspendMachine::cancelWanted(outcome);
  switch (outcome) {
  case A830SuspendOk:
    piano_a830_note("gpu suspend #%u: GPU down in %u.%03u ms (quiet after %u us; %s; %s): submissions wait for the resume", n,
                    r.waitedUs / 1000, r.waitedUs % 1000, r.waitedUs - r.entryUs,
                    r.alreadyAsleep ? "already in its full slumber" : "full slumber entered",
                    r.cxCollapsed || r.alreadyAsleep ? "CX GDSC collapsed" : "CX STILL ON: another voter keeps the GDSC");
    break;
  case A830SuspendOff:
    piano_a830_note("gpu suspend #%u: not taking part (the GPU is not running): its votes stay as they are", n);
    break;
  case A830SuspendBusy:
    piano_a830_note("gpu suspend #%u: REFUSED: still busy after %u ms (work in flight, a holder or a recovery): the GPU stays up as it "
                    "was, its votes too; the suspend goes on without the GPU's share", n, gSuspendConfig.idleWaitMs);
    break;
  case A830SuspendLost:
    piano_a830_note("gpu suspend #%u: REFUSED: the GPU is lost: nothing of it is touched, its votes stay", n);
    break;
  default: {
    if (failTest) {
      piano_a830_note("gpu suspend #%u: REFUSED (piano-gpu-suspend-fail-test=%u: a failed entry, the GPU not touched); no GPU suspend for "
                      "the rest of the boot; %s", n, gSuspendConfig.failTest, gSuspendConfig.cancel ?
                      "the kernel is asked to cancel this sleep (piano-gpu-suspend-cancel=1)" : "the suspend goes on without the GPU's share");
      break;
    }
    // The recovery has the GPU: wait for it here, so the suspend never freezes a GPU half down.
    uint32_t waited = 0;
    A830PowerState *power = adreno830_power_state();
    while (power && waited < 3000 && power->phase() != A830PowerActive && power->phase() != A830PowerLost) {
      IOSleep(10);
      waited += 10;
    }
    const A830PowerPhase phase = power ? power->phase() : A830PowerLost;
    piano_a830_note("gpu suspend #%u: REFUSED: the slumber entry failed; the recovery %s after %u ms; no GPU suspend for the rest of the "
                    "boot; %s", n, phase == A830PowerActive ? "brought it back" : phase == A830PowerLost ? "lost it" : "is STILL RUNNING",
                    waited, gSuspendConfig.cancel ? "the kernel is asked to cancel this sleep (piano-gpu-suspend-cancel=1)"
                                                  : "the suspend goes on without the GPU's share");
    break;
  }
  }
  if (cancel) {
    const IOReturn c = pianopower::cancelSystemSleep("PianoA830", "the GPU's slumber entry failed (the recovery had it)");
    piano_a830_note("gpu suspend #%u: cancel of the sleep: %s", n, c == kIOReturnSuccess ? "taken, nothing is suspended (a normal wake follows)"
                    : c == kIOReturnUnsupported ? "refused (piano-sleep-cancel=0, no piano-sleep, or a kernel before k27 0064): the sleep goes on"
                    : c == kIOReturnNotReady ? "no system sleep in progress" : "too late");
  }
  suspendPublish();
}

void votesDown()
{
  if (!gSuspend.votesDownWanted()) return;
  pianopower::Scanout scanout = pianopower::displayScanout(gDpu);
  // PianoDpu's panel sleep runs on its own thread after the display went off: give it displayWaitMs to finish.
  uint32_t waited = 0;
  while (gDpu && scanout == pianopower::Scanout::running && waited < gSuspendConfig.displayWaitMs) {
    IOSleep(10);
    waited += 10;
    scanout = pianopower::displayScanout(gDpu);
  }
  A830SuspendReport r{};
  const IOReturn result = adreno830_suspend_votes(uint8_t(scanout), &gSuspendConfig, &r);
  gSuspend.votesDownDone(result == kIOReturnSuccess);
  if (result == kIOReturnSuccess)
    piano_a830_note("gpu suspend: votes down in %u us (scanout %s%s): DDR %u -> %u MB/s%s, CX %#x -> %#x, MX %#x -> %#x%s", r.votesUs,
                    scanoutName(scanout), waited ? " after waiting for the panel" : "", r.ddrBefore / 1000, r.ddrAfter / 1000,
                    r.ddrAfter ? " (the display's floor)" : " (keepalive minimum)", r.cxBefore, r.cxAfter, r.mxBefore, r.mxAfter,
                    r.cxAfter ? "" : " (the rails' votes gone)");
  else
    piano_a830_note("gpu suspend: votes NOT lowered (rpmh %d, %#x): put back as they were; the GPU stays down", r.rpmh, unsigned(result));
  suspendPublish();
}

void votesUp(const char *where)
{
  if (!gSuspend.votesUpWanted()) return;
  A830SuspendReport r{};
  const IOReturn result = adreno830_resume_votes(&r);
  gSuspend.votesUpDone(result == kIOReturnSuccess);
  if (result == kIOReturnSuccess)
    piano_a830_note("gpu resume: votes back in %u us (%s): CX %#x MX %#x, DDR %u MB/s", r.votesUs, where, r.cxAfter, r.mxAfter,
                    r.ddrAfter / 1000);
  else
    piano_a830_note("gpu resume: votes NOT back (%s, rpmh %d): %s", where, r.rpmh,
                    strcmp(where, "kOn") ? "kOn tries again" : "the GPU stays asleep (its gate shut) until a later kOn");
  suspendPublish();
}

void resumeGpu()
{
  votesUp("kOn");
  if (gSuspend.votesUpWanted() || !gSuspend.wakeWanted()) {
    suspendPublish();
    return;
  }
  A830SuspendReport r{};
  const IOReturn result = adreno830_resume_gpu(gSuspendConfig.eagerWake, &r);
  gSuspend.wakeDone(result == kIOReturnSuccess, r.wakeUs, gSuspendConfig.eagerWake ? gSuspendConfig.wakeBoundMs : 0);
  if (result != kIOReturnSuccess)
    piano_a830_note("gpu resume #%u: the WAKE FAILED (%#x): the recovery has the GPU", gSuspend.wakes + gSuspend.wakeFailures, unsigned(result));
  else if (gSuspendConfig.eagerWake)
    piano_a830_note("gpu resume #%u: GPU awake in %u.%03u ms (%s the %u ms bound): the first frame waits for nothing", gSuspend.wakes,
                    r.wakeUs / 1000, r.wakeUs % 1000, r.wakeUs > gSuspendConfig.wakeBoundMs * 1000 ? "OVER" : "within",
                    gSuspendConfig.wakeBoundMs);
  else
    piano_a830_note("gpu resume #%u: the gate open, the GPU left asleep (piano-gpu-suspend-wake=0): the first submission wakes it",
                    gSuspend.wakes);
  if (gDpu) {
    gDpu->release();
    gDpu = nullptr;
  }
  suspendPublish();
}

void pmWork(thread_call_param_t, thread_call_param_t)
{
  if (gPmTarget == pianopower::kOff) suspendGpu();
  else resumeGpu();
  if (gService) gService->acknowledgeSetPowerState();
}

void suspendConfigure(unsigned level)
{
  A830SuspendConfig &c = gSuspendConfig;
  c = A830SuspendConfig{};
  c.enabled = level >= 2 && bootArg("piano-gpu-suspend", 0) == 1;
  const uint32_t idle = bootArg("piano-gpu-suspend-idle-ms", 500);
  c.idleWaitMs = idle < 50 ? 50 : idle > 2000 ? 2000 : idle;
  c.railVlvl = bootArg("piano-gpu-suspend-rails", 0x140);
  c.eagerWake = bootArg("piano-gpu-suspend-wake", 1) != 0;
  const uint32_t bound = bootArg("piano-gpu-suspend-wake-ms", 100);
  c.wakeBoundMs = bound < 1 ? 1 : bound > 10000 ? 10000 : bound;
  const uint32_t display = bootArg("piano-gpu-suspend-display-ms", 300);
  c.displayWaitMs = display > 2000 ? 2000 : display;
  c.cancel = c.enabled && bootArg("piano-gpu-suspend-cancel", 0) == 1;
  c.failTest = c.enabled ? bootArg("piano-gpu-suspend-fail-test", 0) : 0;
}

// The starter thread, once the GPU runs: into the power tree (the contract's two states), so IOPMrootDomain takes the GPU down
// before a system sleep and up after it.
void suspendJoin()
{
  if (!gSuspendConfig.enabled || !gService || gJoined) return;
  for (unsigned i = 0; i < 500 && !__atomic_load_n(&gStartDone, __ATOMIC_ACQUIRE); ++i) IOSleep(10);   // start() returned
  gSuspendConfig.ddrFloorLevel = gDisplayFloorLevel;
  gPmCall = thread_call_allocate_with_options(pmWork, nullptr, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0);
  gJoined = true;
  const IOReturn r = pianopower::join(gService);
  piano_a830_note("gpu suspend: piano-gpu-suspend=1: %s the power tree (%#x): kOff waits up to %u ms for quiet, then the full slumber; "
                  "PreSleep: DDR to the keepalive minimum (the display's floor, level %u, while it scans), CX/MX to %#x%s; PostResume "
                  "puts them back; kOn %s (bound %u ms)", r == kIOReturnSuccess ? "joined" : "COULD NOT JOIN", unsigned(r),
                  gSuspendConfig.idleWaitMs, gSuspendConfig.ddrFloorLevel, gSuspendConfig.railVlvl,
                  gSuspendConfig.railVlvl ? " (at least NOM_L1 while the panel scans)" : " (the votes gone; NOM_L1 while the panel scans)",
                  gSuspendConfig.eagerWake ? "wakes the GPU" : "opens the gate (the first submission wakes it)", gSuspendConfig.wakeBoundMs);
  if (r != kIOReturnSuccess) gJoined = false;
  if (gJoined && gSuspendConfig.cancel)
    piano_a830_note("gpu suspend: piano-gpu-suspend-cancel=1: a failed slumber entry asks the kernel to cancel that sleep (k27 0064 "
                    "with piano-sleep-cancel=1; else the sleep goes on without the GPU's share)");
  suspendPublish();
}
}  // namespace

// ---- the starter ------------------------------------------------------------------------------------------------------------------
namespace {
void run(void *, wait_result_t)
{
  gRunThread = current_thread();
  runPhase(RunInit);
  const uint32_t delay = bootArg("piano-gpu-delay", 0);
  if (delay) {
    runPhase(RunDelay);
    piano_a830_note("piano-gpu-delay=%u: waiting %u s before the %s", delay, delay, gLevel == 1 ? "survey" : "bring-up");
    IOSleep(delay * 1000);
  }
  // The LLCC island first (read only at any GPU boot; piano-llcc-island=1 programs it at piano-gpu=2), after the delay like
  // everything of the GPU's, so a fault there does not stop the boot before its root.
  islandStart(gService, gLevel);
  const uint32_t defaultMask = gLevel == 1 ? uint32_t(PianoA830SurveyDefault) : 0u;
  const uint32_t mask = bootArg("piano-gpu-survey", defaultMask);
  if (gLevel == 1) {
    runPhase(RunSurvey);
    piano_a830_note("piano-gpu=1: read-only survey (mask %#x) around the driver's S0; nothing is written, nothing is published", mask);
    piano_a830_survey(mask & PianoA830SurveyInputs);
    piano_a830_note("S0 (board inputs, read only): adreno830_start with piano-gpu-stop=0");
    piano_a830_report(piano_a830_board_start(1));
    piano_a830_survey(mask & ~uint32_t(PianoA830SurveyInputs));
    piano_a830_note("survey done (piano-gpu=1)");
    runPhase(RunDone);
    return;
  }
  piano_a830_note("piano-gpu=2: staged bring-up S1..S10, polled (piano-gpu-stop=%d, trace %u%s)", int(bootArg("piano-gpu-stop", ~0u)),
                  gTrace, gTraceSync ? ", synchronous: piano-gpu-trace=3, a heartbeat every second" : "");
  if (mask) {
    runPhase(RunSurvey);
    piano_a830_survey(mask);   // piano-gpu-survey=<mask>: the read-only survey first
  }
  runPhase(RunBringUp);
  const IOReturn result = piano_a830_board_start(2);
  __atomic_store_n(&gSyncSince, mach_absolute_time(), __ATOMIC_RELAXED);
  __atomic_store_n(&gSyncState, 3u, __ATOMIC_RELEASE);
  runPhase(RunReport);
  piano_a830_report(result);
  piano_a830_note("bring-up %s", adreno830_running() ? "done: the GPU runs" : result == kIOReturnSuccess ? "stopped (piano-gpu-stop)" : "failed");
  if (adreno830_running()) suspendJoin();   // piano-gpu-suspend=1: the GPU's part of a system sleep (A830Suspend.h)
  else if (gSuspendConfig.enabled) piano_a830_note("gpu suspend: piano-gpu-suspend=1 but the GPU does not run: not in the power tree");
  // Where the GPU-side streams go, once S3 programmed the SMMU, and again once the session's Metal clients are up. Only while the GPU
  // stays on: piano-gpu-slumber would collapse CX, and an unclocked SMMU read can hang the bus.
  const Adreno830Board *board = piano_a830_board();
  if (result == kIOReturnSuccess && adreno830_status.stage >= A830SSmmu && board && !board->knobs.slumber) {
    runPhase(RunRoutes);
    piano_a830_smmu_routes("after the bring-up");
    if (adreno830_running()) {
      runPhase(RunWait);
      IOSleep(60000);
      runPhase(RunRoutes);
      // By now the GPU may sleep (piano-gpu-idle=2's slumber, a system suspend's full slumber): read only while it is up, holding it
      // so (a slumber entry waits for the reader).
      A830PowerState *power = adreno830_power_state();
      if (!power || power->accessBegin(false)) {
        piano_a830_smmu_routes("60 s later");
        if (power) power->accessEnd(false);
      } else {
        piano_a830_note("SMMU routes (60 s later): skipped, the GPU sleeps (CX may be off: an unclocked SMMU read can hang the bus)");
      }
    }
  }
  runPhase(RunDone);
}
}  // namespace

// The display's fence rule without a link dependency either way (the KC resolves a native kext's imports only from the natives linked
// before it, and NativeGopDisplay comes first): NativeGopDisplay finds this registered service by class (serviceMatching("PianoA830"))
// and calls callPlatformFunction("native-adreno-fences", false, uint32_t *submitted, uint32_t *retired), which answers with
// native_adreno_fences() (NativeAdrenoPublish.cpp): kIOReturnSuccess with both, kIOReturnNotReady before S9 or after a device loss.
// Lock-free, any thread context.
extern "C" bool native_adreno_fences(uint32_t *submitted, uint32_t *retired);

class PianoA830 : public IOService {
  OSDeclareDefaultStructors(PianoA830)
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
  using IOService::callPlatformFunction;
};

OSDefineMetaClassAndStructors(PianoA830, IOService)

IOService *PianoA830::probe(IOService *provider, SInt32 *score)
{
  gLevel = bootArg("piano-gpu", 0);
  NativeAdrenoMemory::setUnmappedUser(bootArg("piano-gpu-user-vm", 0) == 1);
  // GPU-PAGED P1: the VM's mode and its limits, before the bring-up thread reaches S3
  __atomic_store_n(&native_adreno_gpu_mem_paged, gpuMemPagedArg() ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&native_adreno_gpu_mem_cap_mib, bootArg("piano-gpu-mem-cap-mib", 0), __ATOMIC_RELEASE);
  __atomic_store_n(&native_adreno_gpu_mem_reserve_mib, bootArg("piano-gpu-mem-ws-reserve-mib", 0), __ATOMIC_RELEASE);
  __atomic_store_n(&native_adreno_gpu_pt_audit_s, bootArg("piano-gpu-pt-audit", 0), __ATOMIC_RELEASE);
  {  // GPU-PAGED P1b: the parallel wiring of large user BOs
    uint32_t workers = bootArg("piano-gpu-wire-workers", 0);
    if (workers > NativeAdrenoWireMaxWorkers) workers = NativeAdrenoWireMaxWorkers;
    __atomic_store_n(&native_adreno_wire_workers, workers, __ATOMIC_RELEASE);
    uint32_t priority = bootArg("piano-gpu-wire-priority", NativeAdrenoWirePriorityDefault);
    if (priority > NativeAdrenoWirePriorityKernel) priority = NativeAdrenoWirePriorityKernel;
    __atomic_store_n(&native_adreno_wire_priority, priority, __ATOMIC_RELEASE);
  }
  if (!gLevel || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

// The panel's display power (NativeGopDisplay's piano-display-power, followed through its messages: package/include/PianoDisplayFollow.h),
// 2026-11-12: the mailbox server's display-off wait (board/PianoGpuIdle.h) and the DCVS screen rule's word (pagani_screen_state: read
// only with piano-screenoff bit 1, so nothing else changes without it). The callback runs in NativeGopDisplay's gated power change:
// it records, and at most wakes the mailbox server.
namespace {
pianodisplay::Follow gDisplayFollow;
void displayChanged(void *, int state)
{
  const bool off = state == pianodisplay::kOff;
  native_adreno_mailbox_display(off);
  const uint32_t before = __atomic_load_n(&pagani_screen_state, __ATOMIC_ACQUIRE);
  __atomic_store_n(&pagani_screen_state, pianogpu::screenWord(before, off), __ATOMIC_RELEASE);
}
}  // namespace

bool PianoA830::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  static bool started;
  if (started) return true;
  started = true;
  // Board parsing happens later on the bring-up thread; allocate before its first qualification record, under S10's own rule
  // (gpu/fullopp1b: the 7/607-only test here made every other top fail S10 with reason 12; A830QualificationTop.h).
  registerQual607(piano_a830_qualification_top() != 0);
  if (gLevel > 2) {
    piano_a830_note("piano-gpu=%u: unknown level (1 survey, 2 bring-up); nothing done", gLevel);
    return true;
  }
  const uint32_t traceArg = bootArg("piano-gpu-trace", 1);
  gTrace = gLevel >= 2 && traceArg != 0;
  gTraceSync = gLevel >= 2 && traceArg == 3;   // the driver gets 1 (PianoA830Board.cpp)
  gLoggerPollMs = bootArg("piano-gpu-logger-ms", 0);
  if (gLoggerPollMs > 1000) gLoggerPollMs = 1000;
  const bool mailboxPoll = bootArg("piano-gpu-mailbox-poll", 0) == 1;
  const uint32_t mailboxDarkMs = pianogpu::clampDarkMs(bootArg("piano-gpu-mailbox-dark-ms", pianogpu::kMailboxDarkMsDefault));
  native_adreno_mailbox_pace(mailboxPoll, mailboxDarkMs);
  nanoseconds_to_absolutetime(10ull * 1000000000ull, &gRowEvery);
  setProperty("piano-gpu", gLevel, 32);
  gService = this;
  registerGpuRefusals();
  registerGpuInputFloor();
  registerGpuBoost();
  registerGpuBusy();
  // GPU-PAGED P0b: the pool bypass switch, off unless piano-gpu-pool-bypass=1 (machdep.piano_gpu_pool_bypass switches it at run time)
  __atomic_store_n(&native_adreno_pool_bypass, bootArg("piano-gpu-pool-bypass", 0) == 1 ? 1u : 0u, __ATOMIC_RELEASE);
  registerGpuPaged();
  registerGpuPt();   // GPU-PAGED P1
  configureGpuTier();
  registerGpuTier();
  gBoostArg = bootArg("piano-gpu-boost", 0) == 1 ? 1u : 0u;
  __atomic_store_n(&gBoostEnabled, gBoostArg, __ATOMIC_RELEASE);
  // piano_a830_note: start() runs before the line ring exists (gpu-boost342a lost these two through gpuLine).
  if (gBoostArg) piano_a830_note("boost: piano-gpu-boost=1: the input kexts and callPlatformFunction(\"piano-gpu-boost\") raise a floor "
                                 "under every ceiling (machdep.piano_gpu_boost)");
  if (pianogpu::presetArg(bootArgGetter, nullptr, gLevel, "piano-gpu-fullopp", 0) == 1) {
    registerGpuSweepCeiling();
    piano_a830_note("fullopp: piano-gpu-fullopp=1: Android's bin-2 curve on the R3b line (ANALYSIS gpu-fullopp); "
                    "machdep.piano_gpu_sweep_ceiling (root, test only) sets ceiling slot 2");
    // gpu/throttle1: the docked skin table by default (owner 2026-11-24 05:45), piano-gpu-skin-table=0 keeps Android's normal scene
    adreno830_set_skin_table(bootArg("piano-gpu-skin-table", 1) == 1 ? 1u : 0u);
    registerGpuThrottle();
    piano_a830_note("throttle: skin table %s (piano-gpu-skin-table, machdep.piano_gpu_skin_table); machdep.piano_gpu_state; "
                    "android-curve preset %s (piano-gpu-android-curve)", adreno830_skin_table() ? "docked" : "android normal",
                    pianogpu::presetActive(bootArgGetter, nullptr, gLevel) ? "on (no GPU qualification argument given)" :
                    "off (the boot arguments give the GPU knobs)");
  }
  if (const uint32_t floorMhz = bootArg("piano-gpu-input-floor", 0))
    piano_a830_note("input floor: piano-gpu-input-floor=%u MHz, held 80 ms after each submission, a 50 ms busy burst to the top (machdep.piano_gpu_input_floor)", floorMhz);
  // 2026-11-22 (private 20261122-render/ANALYSIS.md): the recovery replays innocent clients' jobs unless piano-gpu-recovery-replay=0.
  adreno830_recovery_set_replay(bootArg("piano-gpu-recovery-replay", 1) != 0 ? 1u : 0u);
  registerGpuRecovery();
  piano_a830_note("recovery: piano-gpu-recovery-replay=%u: %s (machdep.piano_gpu_recovery_replay, machdep.piano_gpu_recovery)",
                  adreno830_recovery_replay_mode(), adreno830_recovery_replay_mode() ?
                  "a GPU hang faults the culprit alone, the other clients' jobs in flight are replayed" :
                  "a GPU hang fails every job in flight (the old behaviour)");
  if (bootArg("piano-gpu-fault-test", 0) == 1) {
    registerGpuFaultTest();
    registerGpuFaultHold();
    piano_a830_note("fault test: piano-gpu-fault-test=1: machdep.piano_gpu_fault_test (root, test only) reports a hang to the recovery; "
                    "machdep.piano_gpu_fault_hold (root, a pid) stops the CP in that process's next job");
  }
  gCeilingLock = IOLockAlloc();
  setProperty("piano-gpu-ceiling-clients", pianogpu::kCeilingClients, 32);   // docs/policy.md §5.3: one slot per caller
  suspendConfigure(gLevel);
  if (gSuspendConfig.enabled) {
    // The platform sleep and wake actions (the host votes): installed from these properties when start() returns.
    setProperty("IOPlatformSleepAction", kActionPriority, 32);
    setProperty("IOPlatformWakeAction", kActionPriority, 32);
  }
  // The async line ring is not allocated yet: use the direct startup IOLog channel.
  piano_a830_note("gpu bo backing: user fallback=%s; kernel objects=mapped; pool=unchanged; gpuoom-vm1",
          NativeAdrenoMemory::unmappedUser() ? "unmapped-vm" : "mapped-control");
  piano_a830_note("pool bypass: piano-gpu-pool-bypass=%u at start (GPU-PAGED P0b; machdep.piano_gpu_pool_bypass switches it, "
          "machdep.piano_gpu_paged has the per-path figures)", unsigned(__atomic_load_n(&native_adreno_pool_bypass, __ATOMIC_ACQUIRE)));
  piano_a830_note("gpu mem: piano-gpu-mem=%s (GPU-PAGED P1): %s; cap %u MiB, render-server reserve %u MiB, audit every %u s "
          "(machdep.piano_gpu_pt, machdep.piano_gpu_pt_audit)",
          __atomic_load_n(&native_adreno_gpu_mem_paged, __ATOMIC_ACQUIRE) ? "paged" : "pool",
          __atomic_load_n(&native_adreno_gpu_mem_paged, __ATOMIC_ACQUIRE) ?
            "no BO pool, page tables on demand, one record per mapping" : "the BO pool and the preallocated tables as before",
          unsigned(native_adreno_gpu_mem_cap_mib ? native_adreno_gpu_mem_cap_mib : uint32_t(NativeAdrenoVmPagedCapDefault >> 20)),
          unsigned(native_adreno_gpu_mem_reserve_mib ? native_adreno_gpu_mem_reserve_mib : uint32_t(NativeAdrenoVmPagedReserveDefault >> 20)),
          unsigned(native_adreno_gpu_pt_audit_s));
  piano_a830_note("wire workers: piano-gpu-wire-workers=%u (GPU-PAGED P1b): user BOs of >= 1 MiB %s (machdep.piano_gpu_wire_workers, "
          "machdep.piano_gpu_wire)", unsigned(__atomic_load_n(&native_adreno_wire_workers, __ATOMIC_ACQUIRE)),
          __atomic_load_n(&native_adreno_wire_workers, __ATOMIC_ACQUIRE) ? "are wired as parts at the same time" : "are wired whole, as before");
  piano_a830_note("start: piano-gpu=%u (A830 chip %#x, k27 native kext, polled)", gLevel, 0x44050001u);
  if (gTraceSync) {
    gRing.stamp = static_cast<uint64_t *>(IOMallocZero(size_t(RingLines) * sizeof(uint64_t)));
    gPrintLock = IOLockAlloc();
    if (!gRing.stamp || !gPrintLock) {
      piano_a830_note("piano-gpu-trace=3: no memory for the stamps or the print lock; the trace stays asynchronous (as 1)");
      if (gRing.stamp) IOFree(gRing.stamp, size_t(RingLines) * sizeof(uint64_t));
      if (gPrintLock) IOLockFree(gPrintLock);
      gRing.stamp = nullptr;
      gPrintLock = nullptr;
      gTraceSync = 0;
    }
  }
  thread_t thread = nullptr;
  gRing.text = static_cast<char (*)[RingBytes]>(IOMallocZero(size_t(RingLines) * RingBytes));
  if (!gRing.text || kernel_thread_start(logger, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no line ring or logger thread (%u KiB); the trace stays off", RingLines * RingBytes / 1024);
    if (gRing.text) IOFree(gRing.text, size_t(RingLines) * RingBytes);
    gRing.text = nullptr;
    gTrace = 0;
    gTraceSync = 0;
  } else {
    thread_deallocate(thread);
    piano_a830_note("line ring %u x %u bytes, logger thread running (%s)%s", RingLines, RingBytes,
                    gTraceSync ? "polling every 1 ms" : gLoggerPollMs ? "polling: piano-gpu-logger-ms" : "woken by each line, a look once a second",
                    gTraceSync ? "; piano-gpu-trace=3: each trace line printed before its access by the thread making it" : "");
  }
  thread = nullptr;
  if (kernel_thread_start(run, nullptr, &thread) != KERN_SUCCESS) {
    piano_a830_note("FAILED: no kernel thread");
    return true;
  }
  thread_deallocate(thread);
  const bool following = gDisplayFollow.start(displayChanged, nullptr);
  piano_a830_note("display power: %s; display off: the mailbox looked at every %u ms (piano-gpu-mailbox-dark-ms, 0: as lit)",
                  following ? "following NativeGopDisplay's messages" : "NOT followed (no notification)", mailboxPoll ? 0u : mailboxDarkMs);
  piano_a830_note("mailbox: %s", mailboxPoll ? "polled as before (piano-gpu-mailbox-poll=1: 1 ms for a second after a request, else 16 ms)"
                                             : "a look every 100 ms while its doorbell rings for the requests, else polled as before");
  // For the display's fence lookup, the thermal and power-profile ceilings and the LLCC island query (callPlatformFunction below).
  // At piano-gpu=1 too since 2026-11-11 (the island query): the fences answer NotReady and a ceiling waits for a DCVS that never starts.
  registerService();
  __atomic_store_n(&gStartDone, 1u, __ATOMIC_RELEASE);
  return true;
}

// The contract's two states (PianoPowerContract.h), only once the GPU runs and piano-gpu-suspend=1 (suspendJoin): the work on a
// thread call, acknowledged when done.
IOReturn PianoA830::setPowerState(unsigned long state, IOService *)
{
  if (!gJoined) return pianopower::kAckNow;
  gPmTarget = state;
  if (gPmCall) {
    thread_call_enter(gPmCall);   // (one already queued takes the new target: IOPM never overlaps two changes of one driver)
    return kPmBudgetUs;
  }
  if (state == pianopower::kOff) suspendGpu();
  else resumeGpu();
  return pianopower::kAckNow;
}

IOReturn PianoA830::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                         void *param4)
{
  if (functionName && functionName->isEqualTo("piano-gpu-qualification-display")) {
    adreno830_qualification_display(static_cast<const PianoGpuQualificationDisplay *>(param1));
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("native-adreno-fences")) {
    if (!param1 || !param2) return kIOReturnBadArgument;
    return native_adreno_fences(static_cast<uint32_t *>(param1), static_cast<uint32_t *>(param2)) ? kIOReturnSuccess : kIOReturnNotReady;
  }
  // PianoThermal's passive trip (docs/drivers/thermal.md §10.5) and the power profile (PianoPolicy, docs/policy.md §5.3): a reversible
  // DCVS ceiling, param1 = kHz (0 clears), param2 = the caller's slot (null: 0, thermal; 1 the power profile; 2..3 free). The lowest
  // non-zero slot is the ceiling (PianoGpuCeiling.h, adreno830_dcvs_set_ceiling; one set before S10 applies when the DCVS starts). A line
  // per change of the ceiling in force, the first 8 then every 10th, 100th, ...
  if (functionName && functionName->isEqualTo("piano-gpu-ceiling"))
    return ceilingSet(unsigned(uintptr_t(param2)), uint32_t(uintptr_t(param1)));
  // A boost floor (PianoGpuBoost.h; boost342 / GPU-DAILY.md): param1 0 = the input boost, 1..16 a GMU level, else MHz; param2 the
  // hold in ms (0 = the touch hold). From the input kexts and the CPU agent's boost policy; every ceiling still wins. Only with
  // piano-gpu-boost=1 (machdep.piano_gpu_boost switches it at run time); otherwise unsupported, so senders stop asking.
  if (functionName && functionName->isEqualTo("piano-gpu-boost"))
    return gpuBoost(uint32_t(uintptr_t(param1)), uint32_t(uintptr_t(param2)), uint32_t(uintptr_t(param4)));
  // The LLCC island (PianoPowerContract.h pianopower::llccIslandReady): param1 = bool *, the two words read again. Any thread.
  if (functionName && functionName->isEqualTo("piano-llcc-island-ready")) {
    if (!param1) return kIOReturnBadArgument;
    if (!__atomic_load_n(&gLlccArmed, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
    *static_cast<bool *>(param1) = piano_a830_llcc_island_ready();
    return kIOReturnSuccess;
  }
  // The system suspend's host votes (piano-gpu-suspend=1): IOCPUSleepKernel's PreSleep and PostResume platform actions.
  if (functionName && functionName->isEqualTo("IOPlatformSleepAction")) {
    if (gJoined) votesDown();
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("IOPlatformWakeAction")) {
    if (gJoined) votesUp("PostResume");
    return kIOReturnSuccess;
  }
  return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

// ---- the macOS accelerator --------------------------------------------------------------------------------------------------------
// Metal's discovery (MTLIOAccelServiceGlobalContext) watches IOServiceMatching("IOAccelerator"); IOGPU's +[IOGPUMetalDevice
// registerAcceleratorService:] loads <MetalPluginName>.bundle from /Library/GPUBundles (trusted by gpu_bundle_find_trusted) and takes
// MetalPluginClassName if it subclasses IOGPUMetalDevice. Named after Apple's accelerator, as on iOS and in the VM, so sandbox profiles
// that name the class accept the plugin's open.
class AGXAccelerator : public IOAccelerator {
  OSDeclareDefaultStructors(AGXAccelerator)
public:
  bool start(IOService *provider) override;
  IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;
  OSObject *copyProperty(const OSSymbol *key) const override;
  OSObject *copyProperty(const OSString *key) const override;
  OSObject *copyProperty(const char *key) const override;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAccelerator)

bool AGXAccelerator::start(IOService *provider)
{
  if (!IOAccelerator::start(provider)) return false;
  // The processes an XNU sandbox-grant patch would let open the user client (inert without one; NativeAdrenoPublish.h, macOS list).
  setProperty("NativeSandboxGrantProcess", NativeAdrenoSandboxGrantList);
  registerService();
  piano_a830_note("Metal accelerator AGXAccelerator on %s (MetalPluginName NativeAdrenoMetal)", provider->getName());
  return true;
}

IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
  const unsigned limit = bootArg("piano-gpu-metal-opens", 0);   // connections ever granted to the plugin (0 = no limit)
  const IOReturn status = native_adreno_new_user_client(this, owningTask, securityID, type, properties, handler, limit);
  // Every Metal process opens twice (Turnip's enumeration, then its device) and a login session has a few dozen of them: the first 160
  // opens are logged (64 before 2026-11-11, which a session after login could use up); a refused GPU open also gets the publish
  // shell's "gpu open refused" line with its reason.
  static unsigned logged;
  if (logged < 160) {
    logged++;
    char name[20] = {};
    proc_selfname(name, sizeof name);
    uint64_t ns = 0;   // the uptime: the boot-time timeline's GUI milestones (docs/boot-time.md section 10), to the millisecond
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    piano_a830_note("%s opens the accelerator, type %#x: %#x (at %llu ms)", name, unsigned(type), status,
                    (unsigned long long)(ns / 1000000ull));
  }
  return status;
}

static void notePropertyRead(const char *key)
{
  native_adreno_note_property_read(key);
  static unsigned logged;
  if (key && !strcmp(key, "MetalPluginName") && logged < 96) {   // 32 before 2026-11-11 (a session after login reads more)
    logged++;
    char name[20] = {};
    proc_selfname(name, sizeof name);
    piano_a830_note("%s reads MetalPluginName", name);
  }
}

OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
  notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
  return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
  notePropertyRead(key ? key->getCStringNoCopy() : nullptr);
  return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const char *key) const
{
  notePropertyRead(key);
  return IORegistryEntry::copyProperty(key);
}
