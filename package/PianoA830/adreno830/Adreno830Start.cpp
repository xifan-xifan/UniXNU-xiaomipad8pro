/* SPDX-License-Identifier: MIT */
/* The A830 orchestrator (ARCHITECTURE 3.4, IMPLEMENTATION_PLAN M0-M13): S0..S10 on the device object g830, every failure
 * recorded as stage, step, reason, IOReturn and the register touched, no retry but the GMU cold boot of S5, nothing published
 * unless every stage before S9 succeeded. It also binds the operations the power state machine (A830PowerState) and the
 * recovery (A830Recovery) run later: the same stage bodies, re-entered without the once-per-boot parts. The bring-up
 * profile (A830Profile.h) is set in S0; what it changes here is listed in Adreno830Start.h. */
#include "Adreno830Start.h"
#include "Adreno830Status.h"
#include "A830Profile.h"
#include "A830Regs.h"
#include "A830CxGate.h"
#include "A830Votes.h"
#include "A830Smmu.h"
#include "A830GmuFirmware.h"
#include "A830Gmu.h"
#include "A830Hfi.h"
#include "A830Cp.h"
#include "A830Zap.h"
#include "A830Irq.h"
#include "A830Fence.h"
#include "A830SelfTest.h"
#include "A830PowerState.h"
#include "A830Recovery.h"
#include "A830Dcvs.h"
#include "A830QualificationExitTest.h"
#include "A830QualificationTop.h"
#include "A830Bind.h"
#include "../../include/PianoGpuQualificationDisplay.h"
#include <stdarg.h>
#ifdef ADRENO830_MACOS
#include "A830Idle.h"
#include "A830Suspend.h"
#endif
#include "A830Trace.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGmuMemory.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoStatus.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoTimer.h"
#include "Sm8750AossQmp.h"
#include "Sm8750Llcc.h"
#include "Sm8750Tsens.h"
#include <string.h>
#ifdef ADRENO830_HOST_TEST
#include <new>
#else
A830ExitTrace a830ExitTrace;
#endif

extern "C" __attribute__((weak)) void adreno830_qualification_note(const char *) {}
extern "C" bool adreno830_qualification_log_ready();
extern "C" __attribute__((weak)) bool adreno830_qualification_board(piano_gpu_temps *) {return false;}
/* gpu/fullopp1: the virtual skin sensor's inputs (PianoThermal "piano-thermal-skin-temps"); the board defines it. */
extern "C" __attribute__((weak)) bool adreno830_qualification_skin(piano_skin_temps *) {return false;}
extern "C" __attribute__((weak)) void adreno830_qualification_unsafe(A830ExitOrigin,uint64_t,bool,bool) {}

#ifndef ADRENO830_HOST_TEST
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#endif

#ifdef ADRENO830_MACOS
/* UniXNU-piano (macOS 27): the publish shell's definition (NativeAdrenoPublish.cpp) replaces this weak default, which the host tests
 * that link the orchestrator without the shell take: no snapshot line. */
extern "C" __attribute__((weak)) void native_adreno_note_hang_snapshot(const A830HangSnapshot *) {}
/* The same for a CB0 fault's detail lines (2026-11-21, NativeAdrenoStatus.h). */
extern "C" __attribute__((weak)) void native_adreno_note_fault_detail(const NativeAdrenoFaultDetail *) {}
/* The idle policy's board hooks (A830Idle.h): mode 0 and no line without the board (the host tests). */
extern "C" __attribute__((weak)) void adreno830_board_idle(A830IdleConfig *) {}
extern "C" __attribute__((weak)) void adreno830_board_idle_event(const A830IdleEvent *) {}
static A830IdleConfig gIdle;   /* piano-gpu-idle and its knobs (the board's), filled by bindPower */
/* The kernel-wide secure-call lock (UniXNU-piano package/include/PianoScmLock.h): the board takes (1) and gives (0) one hold; a
 * no-op without the board (the host tests). Held around every yielding SCM request and once around the ZAP's PAS sequence. */
extern "C" __attribute__((weak)) void adreno830_board_scm_hold(int) {}
#endif

/* An A830Irq window the bring-up trace forwards to (A830Trace.h): the window as bound, and its id for the lines. */
struct A830TraceMmio {A830Mmio raw;unsigned window;};

/* ===== The device object (ARCHITECTURE 2.3) ======================================================================== */
struct Adreno830Device {
    const Adreno830Board *board;
    Adreno830Mmio win[A830WinCount];
    Adreno830Failure failure;
    uint64_t started_us;
    bool started,running;
    /* S0 */
    Sm8750RpmhResources rpmh;
    Sm8750SocInfo soc;
    uint32_t speed_bin;                 /* A830VotesNoFuse when the fuse read failed */
    Sm8750GpuLevelTable levels;
    A830VoteTables votes;
    A830GmuBlocks gmu_blocks;
    /* S1/S2 */
    Sm8750RpmhIo cmd_db_io,drv2_io;
    Sm8750RpmhHost host;
    Sm8750GpuPowerIo power_io;
    Sm8750GpuPowerTrace power;
    Sm8750AossQmpIo qmp_msgram,qmp_ipcc;
    Sm8750AossQmp qmp;
    bool acd_ready,acd_running;         /* P3: the AOP was told ACD is on (QMP val 1) / the GMU acknowledged the ACD table */
    Sm8750ScmOps scm_ops;
    Sm8750Scm scm;
    void *scm_mutex;
    /* S3 */
    A830SmmuMmio smmu_mmio;
    ArmSmmu4KIo smmu_io;
    ArmSmmu4KIo smmu_raw;               /* pagani-gpu-trace: the io A830SmmuBindMmio bound, which smmu_io forwards to */
    A830Smmu smmu;
    void *smmu_fault_call;
    uint32_t fault_lines[2];            /* the bottom half's: A830SmmuFaultLine this boot, [1] the GPU's bank, [0] the others */
    bool smmu_claimed;
    NativeAdrenoGmuMemory gmu_memory;
    NativeAdrenoGmuContext gmu_context;
    NativeAdrenoGpuContext gpu_context;
    NativeAdrenoGpuBuffer buffers[GpuBufferCount];
    NativeAdrenoGmuContextReport gmu_report,gpu_report;
    /* S4/S5 */
    A830GmuMmio gmu_mmio;
    A830GmuIo gmu_io;
    A830GmuIo gmu_raw;                  /* pagani-gpu-trace: the io A830GmuBindMmio bound, which gmu_io forwards to */
    A830GmuOs gmu_os;
    A830GmuIrqOps gmu_irq_ops;
    A830GmuHooks gmu_hooks;
    A830GmuGxVoter gx_voter;
    A830CxMisc cx_timer;
    A830Gmu gmu;
    A830HfiIo hfi_io;
    A830Hfi hfi;
    void *hfi_mutex;
    bool gmu_bound,hfi_initialized,floor_done;
    uint8_t level,bus;                  /* the M9 vote of S5 */
    /* S6/S7 */
    NativeAdrenoRingIo ring_io;
    NativeAdrenoRingCache ring_cache;
    NativeAdrenoRingClock clock;
    NativeAdrenoRing ring;
    A830CpIo cp_io;
    A830CpSmmu cp_smmu;
    A830Cp cp;
    void *mmio_mutex;
    Sm8750LlccIo llcc_io;
    Sm8750Llcc llcc;
    uint32_t llcc_scid;
    A830Zap zap;
    /* S8 */
    A830Irq irq;
    A830TraceMmio irq_windows[3];       /* pagani-gpu-trace: the GPU, GMU and CX_MISC windows A830Irq gets, as bound */
    A830Fence fence;
    NativeAdrenoTimeIo time_io;
    NativeAdrenoTimer timer;
    NativeAdrenoMemory probe[3];
    A830SelfTestReport selftest;
    /* S9 */
    NativeAdrenoSubmit submit;
    NativeAdrenoAbiDeviceInfo info;
    Adreno830MetalReport metal;
    bool published;
    /* S10 */
    A830PowerIo power_win;
    A830PowerEnv power_env;
    A830PowerOps power_ops;
    A830RecoveryOps recovery_ops;
    A830PowerState power_state;
    A830Recovery recovery;
    A830Dcvs dcvs;
    Sm8750TsensIo tsens_io;
    void *power_mutex;
    char power_event;
    bool power_bound;
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: the host votes' owner after S10 (A830Suspend.h): the DCVS thread's idle votes and floor raises, and the system
     * suspend's PreSleep/PostResume votes, one at a time. */
    void *host_mutex;
#endif
};

extern "C" {__attribute__((weak)) volatile uint32_t sm8750_aoss_qmp_owner;}
namespace {
Adreno830Device g830;
/* UniXNU-piano (2026-11-23, GPU-PAGED P1, NativeAdrenoVm.h): the GPU VM's mode and limits, set by the board before S3. */
extern "C" {volatile uint32_t native_adreno_gpu_mem_paged,native_adreno_gpu_mem_cap_mib,native_adreno_gpu_mem_reserve_mib,
    native_adreno_gpu_pt_audit_s;}
A830Qualification gQualification;
bool gQualificationEnabled=false;
uint64_t gQualificationInjectAt=0;
bool gQualificationInjectionScheduled=false;
uint32_t gQualificationExitTest=0;
/* The qualified ceiling (gpu/daily4: 607 / 660 / 734; gpu/fullopp1: bin 2's 7..14) and bin 2's MHz: A830QualificationTop.h, the rule the
 * board also uses to allocate the qualification log S10 requires (gpu/fullopp1b). 0 when the knobs name none of them. */
constexpr const auto &FulloppMhz=Adreno830FulloppMhz;
uint8_t qualificationTop(const Adreno830Board *b) {return adreno830_qualification_top(b);}
bool qualificationMode(const Adreno830Board *b) {return qualificationTop(b)!=0;}
bool qualificationResidency(const Adreno830Board *b) {
    return b->qualification_r3b==1 && !b->qualification_exit_test && b->knobs.dcvs_min_mhz==607;
}
void qualLog(const char *format,...) __attribute__((format(printf,1,2)));
void qualLog(const char *format,...) {
    char line[960];va_list ap;va_start(ap,format);const int n=vsnprintf(line,sizeof line,format,ap);va_end(ap);
    if(n<0 || unsigned(n)>=sizeof line)line[0]=0; // the export counts a rejected record; never certify a truncated tail
    adreno830_qualification_note(line);
}
void qualificationUnsafe(void *,A830ExitOrigin origin) {
    adreno830_qualification_unsafe(origin,g830.recovery.protectionAt(),
                                    g830.recovery.qualificationSafeLow(),g830.recovery.safeOff());
} // no logger, allocation, power or HFI lock on the emergency path
/* The cmd-db snapshot: 128 KiB, static storage (never a stack object: XNU kernel stacks are 16 KiB, ARCHITECTURE 4.1.1). */
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
/* UniXNU-piano (k27 native kext): a one-time heap allocation instead (Sm8750Rpmh.h allows either). The k27 kernel collection places a
 * native kext's __DATA in the gaps of the collection's __DATA, and the largest one is about 166 KiB: the snapshot would not fit with the
 * rest of the driver. adreno830_start allocates it before S0; it is never freed. */
Sm8750CmdDb *cmdDbHeap;
#define cmdDbSnapshot (*cmdDbHeap)
#else
Sm8750CmdDb cmdDbSnapshot;
#endif

/* ===== Kernel services ============================================================================================== */
#ifndef ADRENO830_HOST_TEST
#ifndef ADRENO830_MACOS
extern "C" kmod_info_t g_kernel_kmod_info;
#endif
/* NativeAdrenoAccelerator.cpp's class initializer, moved out of __mod_init_func by scripts/build-adreno830.py: it may run only
 * after IOGPUFamily is loaded (ARCHITECTURE 3.3, 7.1.2). */
#ifndef ADRENO830_MACOS
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
#endif

uint64_t kNowUs()
{
    uint64_t ns=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    return ns/1000;
}
void kDelayUs(uint32_t us) {IODelay(us);}
void kSleepUs(uint32_t us)
{
    if(us>=1000)IOSleep(us/1000);
    if(us%1000)IODelay(us%1000);
}
struct KernelMap {volatile void *va;IOMemoryMap *map;};
KernelMap kernelMaps[48];
volatile void *kMap(uint64_t phys,uint64_t bytes,uint32_t mode)
{
    IODeviceMemory *memory=IODeviceMemory::withRange(phys,bytes);
    if(!memory)return nullptr;
    IOMemoryMap *map=memory->map(kIOMapAnywhere|(mode==Adreno830MapWriteCombined?kIOMapWriteCombineCache:kIOMapInhibitCache));
    memory->release();
    if(!map)return nullptr;
    volatile void *va=reinterpret_cast<volatile void *>(map->getVirtualAddress());
    for(auto &slot:kernelMaps)
        if(!slot.map) {slot={va,map};return va;}
    map->release();   /* more windows than slots: refuse rather than lose track of a mapping */
    return nullptr;
}
void kUnmap(volatile void *va,uint64_t)
{
    for(auto &slot:kernelMaps)
        if(slot.map && slot.va==va) {slot.map->release();slot={};return;}
}
void *kMutexAlloc() {return IOLockAlloc();}
void kMutexLock(void *mutex) {IOLockLock(static_cast<IOLock *>(mutex));}
void kMutexUnlock(void *mutex) {IOLockUnlock(static_cast<IOLock *>(mutex));}
void kWait(const void *event,uint32_t timeout_us)
{
    if(assert_wait_timeout(event_t(const_cast<void *>(event)),THREAD_UNINT,timeout_us,NSEC_PER_USEC)==THREAD_WAITING)
        thread_block(THREAD_CONTINUE_NULL);
}
void kWake(const void *event) {thread_wakeup(event_t(const_cast<void *>(event)));}
struct KernelBody {void (*body)(void *);void *argument;};
KernelBody kernelBodies[4];
void kThreadEntry(void *parameter,wait_result_t)
{
    KernelBody *slot=static_cast<KernelBody *>(parameter);
    slot->body(slot->argument);
}
IOReturn kThreadStart(void (*body)(void *),void *argument)
{
    for(auto &slot:kernelBodies)
        if(!slot.body) {
            slot={body,argument};
            thread_t thread=nullptr;
            if(kernel_thread_start(kThreadEntry,&slot,&thread)!=KERN_SUCCESS) {slot={};return kIOReturnNoResources;}
            thread_deallocate(thread);
            return kIOReturnSuccess;
        }
    return kIOReturnNoResources;
}
KernelBody kernelCalls[8];
void kCallEntry(thread_call_param_t param0,thread_call_param_t)
{
    KernelBody *slot=static_cast<KernelBody *>(param0);
    slot->body(slot->argument);
}
void *kCallAllocate(void (*function)(void *),void *argument)
{
    for(auto &slot:kernelCalls)
        if(!slot.body) {
            slot={function,argument};
            thread_call_t call=thread_call_allocate_with_options(kCallEntry,&slot,THREAD_CALL_PRIORITY_KERNEL_HIGH,
                                                                 THREAD_CALL_OPTIONS_ONCE);
            if(!call)slot={};
            return call;
        }
    return nullptr;
}
void kCallEnter(void *call) {if(call)thread_call_enter(static_cast<thread_call_t>(call));}
uint64_t kCounter() {return mach_absolute_time();}
uintptr_t kIrqDisable() {return uintptr_t(ml_set_interrupts_enabled(FALSE));}
void kIrqRestore(uintptr_t state) {ml_set_interrupts_enabled(boolean_t(state));}

#ifdef ADRENO830_MACOS
/* UniXNU-piano (macOS 27): no staged IOAcceleratorES. macOS's Metal discovery watches IOServiceMatching("IOAccelerator"); the native
 * kext's own AGXAccelerator (an ordinary IOAccelerator subclass of IOGraphicsFamily, package/PianoA830/board/PianoA830.cpp) is a
 * personality that matches the NativeAdrenoGpuPublished service native_adreno_publish registers below, and carries MetalPluginName /
 * MetalPluginClassName itself (UniXNU's macOS 27.3 path did the same switch: AdrenoMetal OriginalAcceleratorABI.h
 * NATIVE_ACCELERATOR_BASE). Nothing to prepare or attach here: the steps are reported as done so the S9 page reads as on iPadOS. */
IOReturn kMetalPrepare(Adreno830MetalReport *report)
{
    *report={};
    report->step=4;
    return kIOReturnSuccess;
}
IOReturn kMetalCommit(Adreno830MetalReport *report)
{
    report->step=5;
    return kIOReturnSuccess;
}
void kMetalDiscard() {}
#else
/* publishMetal (ARCHITECTURE 3.3, sheng GpuPowerProbe publishMetal): prepare stops before anything is attached. */
IOService *metalAccelerator;
IOReturn kMetalPrepare(Adreno830MetalReport *report)
{
    *report={};
    report->step=1;
    report->kext=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if(report->kext)return kIOReturnNotFound;
    const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    if(name)name->release();
    report->parent_bytes=parent?parent->getClassSize():0;
    if(report->parent_bytes!=136)return kIOReturnNotFound;
    report->step=2;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return kIOReturnNoResources;
    native_adreno_accelerator_static_init();
    const OSReturn loaded=OSMetaClass::postModLoad(handle);
    if(loaded)return kIOReturnError;
    report->instance_bytes=native_adreno_accelerator_instance_size();
    if(report->instance_bytes!=136)return kIOReturnInvalid;
    report->step=3;
    IOService *accelerator=native_adreno_accelerator_allocate();
    if(!accelerator)return kIOReturnNoMemory;
    if(!accelerator->init()) {accelerator->release();return kIOReturnError;}
    /* The two keys the original _MTLRegisterDevices reads, and the processes whose open of this accelerator the kernel's
     * sandbox grant (zzw-native-adreno-sandbox-grant.patch) lets through: the render server and every Metal client the seed lists
     * (NativeAdrenoSandboxGrantList, NativeAdrenoPublish.h, the poster extensions among them). No Apple device identity is claimed. */
    accelerator->setProperty("MetalPluginName","NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice");
    accelerator->setProperty("NativeSandboxGrantProcess",NativeAdrenoSandboxGrantList);
    /* Every process's open, with the optional zzwc-native-adreno-sandbox-grant-all.patch (inert without it; NativeAdrenoPublish.h). */
    if(NativeAdrenoSandboxGrantAll)accelerator->setProperty("NativeSandboxGrantAll",kOSBooleanTrue);
    metalAccelerator=accelerator;
    report->step=4;
    return kIOReturnSuccess;
}
IOReturn kMetalCommit(Adreno830MetalReport *report)
{
    if(!metalAccelerator)return kIOReturnNotReady;
    if(!metalAccelerator->attach(IOService::getPlatform())) {
        metalAccelerator->release();metalAccelerator=nullptr;
        return kIOReturnNotAttached;
    }
    metalAccelerator->registerService();
    report->step=5;
    return kIOReturnSuccess;
}
void kMetalDiscard()
{
    if(metalAccelerator) {metalAccelerator->release();metalAccelerator=nullptr;}
}
#endif

const Adreno830Os kernelOs={kNowUs,kDelayUs,kSleepUs,kMap,kUnmap,Sm8750ScmSmcInstruction,kMutexAlloc,kMutexLock,kMutexUnlock,
    kWait,kWake,kThreadStart,kCallAllocate,kCallEnter,armcpu_spi_attach,armcpu_spi_disable,armcpu_spi_enable,armcpu_spi_count,
    kCounter,kIrqDisable,kIrqRestore,kMetalPrepare,kMetalCommit,kMetalDiscard};
const Adreno830Os *osTable=&kernelOs;
#else
const Adreno830Os *osTable=nullptr;
#endif
const Adreno830Os &os() {return *osTable;}

/* ===== Small helpers ================================================================================================ */
uint64_t nowUs() {return osTable && osTable->now_us?osTable->now_us():0;}

/* ===== Bring-up register trace (pagani-gpu-trace, A830Trace.h, ARCHITECTURE 8.7) ====================================
 * Every table the orchestrator binds for the modules ends in rd/wr below, in the SMMU, GMU and A830Irq windows it wraps
 * when the trace may run, or in the SMC it wraps: while a scope is open (adreno830_start, a power or recovery operation)
 * each access gets its line before it is made. Outside a scope an access costs one relaxed load more; with the trace off
 * for the boot (pagani-gpu-trace=0, or no ramoops log) the SMMU, GMU, A830Irq and SMC tables are bound as before. */
const char *const traceNames[A830WinCount+1]={"GPU","RSCC","DBGC","CXM","GMU","GMUAO","GPUCC","GCC","SMMU","CMDDB","SMEM",
    "ZAP","QFP","DRV2","AOSS","IPCC","TSNS","LLC0","LLC1","LLC2","LLC3","LLCO","LLCA","SCM"};
static_assert(A830WinCount==23 && A830WinLlccAnd==22,"one line name per window, in Adreno830WindowId order");
constexpr unsigned TraceScm=A830WinCount;
/* Memory-like inputs of S0 (cmd-db and SMEM copied word by word, qfprom read once) and the ZAP region get no lines. */
constexpr uint32_t TraceQuietWindows=1u<<A830WinCmdDb|1u<<A830WinSmem|1u<<A830WinQfprom|1u<<A830WinZap;
constexpr int TraceStatusStep=-1;   /* a line carries adreno830_status.step */
A830Trace tracer;
bool traceAllowed;          /* adreno830_start: pagani-gpu-trace 1 or 2 and BootProgress's ramoops log runs */
uint32_t traceDepth;        /* open scopes (the bring-up, power and recovery operations): lines only while non-zero */
uint32_t traceLockWord;     /* serialises the tracer (thread context only, interrupts off while held) */
uint32_t traceQuiet;        /* non-zero: a timing-critical window, its accesses get no line (a note says so) */
uint32_t traceDrops;        /* lines lost to the lock */
uint64_t traceOpened;       /* nowUs() when adreno830_start opened the trace */

bool traceLive() {return __atomic_load_n(&traceDepth,__ATOMIC_RELAXED)!=0;}
/* Lines come from thread context only (the GMU, SMMU and GPU top halves and the SCM wait-queue interrupt use the same
 * tables) and never from a quiet window. */
bool traceHere()
{
#ifndef ADRENO830_HOST_TEST
    if(ml_at_interrupt_context())return false;
#endif
    return !__atomic_load_n(&traceQuiet,__ATOMIC_RELAXED);
}
void traceSink(const char *line) {boot_progress_trace(line);}
#ifdef ADRENO830_MACOS
/* UniXNU-piano: the board's synchronous trace (piano-gpu-trace=3, board/PianoA830.cpp) prints the lines the sink took here, after the
 * tracer's lock is given back (interrupts as the caller had them) and before the access is made. Weak no-op without a board. */
extern "C" __attribute__((weak)) void boot_progress_trace_sync(void) {}
#endif
/* The tracer's lock: interrupts off on this CPU (the holder is never preempted), a bounded spin, the line dropped after. */
class TraceLock {
    uintptr_t irq_;
    bool held_;
public:
    TraceLock():irq_(osTable && osTable->irq_disable?osTable->irq_disable():0),held_(false)
    {
        for(uint32_t spin=0;spin<(1u<<20);++spin)
            if(!__atomic_exchange_n(&traceLockWord,1u,__ATOMIC_ACQUIRE)) {held_=true;return;}
        __atomic_fetch_add(&traceDrops,1u,__ATOMIC_RELAXED);
    }
    ~TraceLock()
    {
        if(held_)__atomic_store_n(&traceLockWord,0u,__ATOMIC_RELEASE);
        if(osTable && osTable->irq_restore)osTable->irq_restore(irq_);
    }
    TraceLock(const TraceLock &)=delete;
    TraceLock &operator=(const TraceLock &)=delete;
    bool held() const {return held_;}
};
/* An access of block (a window id, or TraceScm) about to be made: its line, unless counted in a run. The token goes to
 * traceValue once a read or an SMC returned. step: the status step, or the power-up step of Sm8750GpuPowerOn/Off. */
A830TraceToken traceAccess(unsigned block,char op,uint64_t address,uint64_t value,int step=TraceStatusStep,uint64_t arg2=0,
                           uint64_t arg3=0)
{
    if((block<A830WinCount && (TraceQuietWindows>>block&1u)) || !traceHere())return {};   /* cmd-db: 32768 reads in S0 */
    A830TraceToken token={};
    {
        TraceLock lock;
        if(!lock.held())return {};
        const uint8_t stage=adreno830_status.stage;
        const uint16_t now=step<0?adreno830_status.step:uint16_t(step);
        token=tracer.access(stage,now,uint8_t(block),op,address,value,arg2,arg3);
    }
#ifdef ADRENO830_MACOS
    if(token.logged)boot_progress_trace_sync();   /* UniXNU-piano: the access's line on the console before the access */
#endif
    return token;
}
void traceValue(const A830TraceToken &token,uint64_t value,uint64_t result1=0,uint64_t result2=0)
{
    if(!token.op)return;
    TraceLock lock;
    if(lock.held())tracer.value(token,value,result1,result2);
}
/* g<S>.<step> note <text>[ <v1>[ <v2>]]. */
void traceNote(const char *text,unsigned values=0,uint64_t v1=0,uint64_t v2=0)
{
    if(!traceLive() || !traceHere())return;
    TraceLock lock;
    if(lock.held())tracer.note(adreno830_status.stage,adreno830_status.step,text,values,v1,v2);
}
/* g3.<step> note gpu aperture <state> x0=<x0>: the CP_SMMU_APERTURE outcome (A830Smmu::apertureState: 0 not run, 1 granted, 2
 * refused) and the x0 the SMC returned (hex, as every trace value), once per boot at the end of S3 (M14 P0). */
void traceAperture(uint32_t state,int64_t x0)
{
    char text[40]="gpu aperture ";
    unsigned n=13;
    text[n++]=char('0'+(state<9?state:9));
    for(const char *p=" x0=";*p;)text[n++]=*p++;
    char digits[16];
    unsigned count=0;
    uint64_t value=uint64_t(x0);
    do {digits[count++]="0123456789abcdef"[value&15];value>>=4;} while(value);
    while(count)text[n++]=digits[--count];
    text[n]=0;
    traceNote(text);
}
void traceStageBegin(uint8_t stage)
{
    if(!traceLive() || !traceHere())return;
    const uint64_t now=nowUs();
    TraceLock lock;
    if(lock.held())tracer.stageBegin(stage,adreno830_stage_name(stage),now);
}
void traceStageEnd(uint8_t stage,IOReturn result,const Adreno830Failure &failure,uint64_t elapsed_us,bool stopped)
{
    if(!traceLive() || !traceHere())return;
    TraceLock lock;
    if(!lock.held())return;
    const uint8_t outcome=result!=kIOReturnSuccess?A830TraceFail:stopped?A830TraceStop:A830TraceOk;
    tracer.stageEnd(stage,adreno830_stage_name(stage),outcome,uint32_t(result),failure.step,failure.reason,failure.reg,
                    failure.value,elapsed_us);
}
/* adreno830_start: the trace runs for this boot when the board asks for it (pagani-gpu-trace 1, or 2 for a line per
 * access) and BootProgress's ramoops log runs; the bring-up is the first scope. */
void traceOpen(const Adreno830Board *board)
{
    const bool known=board && board->magic==Adreno830BoardMagic && board->version==Adreno830BoardVersion;
    const uint32_t level=known?board->knobs.trace:0;
    traceAllowed=(level==1 || level==2) && boot_progress_trace_enabled()!=0;
    if(!traceAllowed)return;
    traceOpened=nowUs();
    {
        TraceLock lock;
        tracer.begin(traceSink,traceNames,A830WinCount+1,level==2);
        if(lock.held())tracer.opened(board->knobs.profile,board->knobs.stop,traceOpened);
    }
    __atomic_fetch_add(&traceDepth,1u,__ATOMIC_ACQ_REL);
}
void traceClose(IOReturn result)
{
    if(!traceAllowed)return;
    const uint64_t now=nowUs();
    {
        TraceLock lock;
        if(lock.held())tracer.closed(uint32_t(result),now-traceOpened,__atomic_load_n(&traceDrops,__ATOMIC_RELAXED));
    }
    __atomic_fetch_sub(&traceDepth,1u,__ATOMIC_ACQ_REL);
}
/* A scope of its own after the bring-up ("g op <name> begin/end"): open returns its start time; every open is closed once. */
uint64_t traceOpOpen(const char *name)
{
    __atomic_fetch_add(&traceDepth,1u,__ATOMIC_ACQ_REL);
    const uint64_t begin=nowUs();
    if(traceHere()) {
        TraceLock lock;
        if(lock.held())tracer.operation(name,true,0,0);
    }
    return begin;
}
void traceOpClose(const char *name,uint64_t begin,IOReturn result)
{
    const uint64_t now=nowUs();
    if(traceHere()) {
        TraceLock lock;
        if(lock.held())tracer.operation(name,false,uint32_t(result),now-begin);
    }
    __atomic_fetch_sub(&traceDepth,1u,__ATOMIC_ACQ_REL);
}
/* A power or recovery operation re-running a stage body (A830PowerOps, A830RecoveryOps). */
class TraceScope {
    const char *name_;
    uint64_t begin_;
    bool open_;
public:
    explicit TraceScope(const char *name):name_(name),begin_(0),open_(traceAllowed)
    {
        if(open_)begin_=traceOpOpen(name_);
    }
    TraceScope(const TraceScope &)=delete;
    TraceScope &operator=(const TraceScope &)=delete;
    IOReturn end(IOReturn result)
    {
        if(open_) {open_=false;traceOpClose(name_,begin_,result);}
        return result;
    }
    ~TraceScope() {end(kIOReturnSuccess);}
};
/* A whole recovery (A830Recovery, under the power mutex): from its gate going to Recovering to Open or Lost, so its own
 * accesses between the operations (the hang capture, RBBM_INT_0_MASK, the GBIF halt) get lines too. */
bool traceRecovering;
uint64_t traceRecoveryBegin;
void traceRecoveryGate(uint8_t gate)
{
    if(!traceAllowed)return;
    if(gate==A830GateRecovering) {
        if(!traceRecovering) {traceRecovering=true;traceRecoveryBegin=traceOpOpen("recovery");}
    } else if(traceRecovering) {
        traceRecovering=false;
        traceOpClose("recovery",traceRecoveryBegin,gate==A830GateOpen?kIOReturnSuccess:A830ReturnNotResponding);
    }
}
/* A timing-critical window (the CX timer seed measures its own writes with interrupts off): a note, no lines inside, a
 * note with what it returned. */
class TraceQuiet {
public:
    explicit TraceQuiet(const char *what)
    {
        traceNote(what);
        __atomic_fetch_add(&traceQuiet,1u,__ATOMIC_ACQ_REL);
    }
    TraceQuiet(const TraceQuiet &)=delete;
    TraceQuiet &operator=(const TraceQuiet &)=delete;
    ~TraceQuiet() {__atomic_fetch_sub(&traceQuiet,1u,__ATOMIC_ACQ_REL);}
};

/* The board windows, 32-bit (Device-nGnRE; a write is completed with dsb sy). step: see traceAccess. */
uint32_t rd(unsigned id,uint32_t offset,int step=TraceStatusStep)
{
    const Adreno830Mmio *window=&g830.win[id];
    if(!traceLive())return adreno830_read32(window,offset);
    const A830TraceToken token=traceAccess(id,A830TraceRead,window->phys+offset,0,step);
    const uint32_t value=adreno830_read32(window,offset);
    traceValue(token,value);
    return value;
}
void wr(unsigned id,uint32_t offset,uint32_t value,int step=TraceStatusStep)
{
    const Adreno830Mmio *window=&g830.win[id];
    if(traceLive())traceAccess(id,A830TraceWrite,window->phys+offset,value,step);
    adreno830_write32(window,offset,value);
}
bool mapped(unsigned id) {return g830.win[id].base!=nullptr;}

/* The SMMU io (A830Smmu, its VM table io, the orchestrator's own reads) over the one A830SmmuBindMmio bound. */
int smmuTraceRead32(void *,size_t offset,uint32_t *value)
{
    const ArmSmmu4KIo &raw=g830.smmu_raw;
    if(!traceLive())return raw.read32(raw.cookie,offset,value);
    const A830TraceToken token=traceAccess(A830WinSmmu,A830TraceRead,g830.win[A830WinSmmu].phys+offset,0);
    const int failed=raw.read32(raw.cookie,offset,value);
    traceValue(token,failed || !value?0xffffffffull:*value);
    return failed;
}
int smmuTraceRead64(void *,size_t offset,uint64_t *value)
{
    const ArmSmmu4KIo &raw=g830.smmu_raw;
    if(!traceLive())return raw.read64(raw.cookie,offset,value);
    const A830TraceToken token=traceAccess(A830WinSmmu,A830TraceRead64,g830.win[A830WinSmmu].phys+offset,0);
    const int failed=raw.read64(raw.cookie,offset,value);
    traceValue(token,failed || !value?~0ull:*value);
    return failed;
}
int smmuTraceWrite32(void *,size_t offset,uint32_t value)
{
    const ArmSmmu4KIo &raw=g830.smmu_raw;
    if(traceLive())traceAccess(A830WinSmmu,A830TraceWrite,g830.win[A830WinSmmu].phys+offset,value);
    return raw.write32(raw.cookie,offset,value);
}
int smmuTraceWrite64(void *,size_t offset,uint64_t value)
{
    const ArmSmmu4KIo &raw=g830.smmu_raw;
    if(traceLive())traceAccess(A830WinSmmu,A830TraceWrite64,g830.win[A830WinSmmu].phys+offset,value);
    return raw.write64(raw.cookie,offset,value);
}
void traceBindSmmu(Adreno830Device *d)
{
    if(!traceAllowed)return;
    d->smmu_raw=d->smmu_io;
    ArmSmmu4KIo &io=d->smmu_io;
    io.read32=io.read32?smmuTraceRead32:nullptr;
    io.read64=io.read64?smmuTraceRead64:nullptr;
    io.write32=io.write32?smmuTraceWrite32:nullptr;
    io.write64=io.write64?smmuTraceWrite64:nullptr;
}
/* The GMU io (A830Gmu, A830GmuFirmware, the CX timer and AHB timeouts) over the one A830GmuBindMmio bound. */
unsigned gmuTraceWindow(unsigned window)
{
    switch(window) {
    case A830GmuWindowGmuAo: return A830WinGmuAo;
    case A830GmuWindowRscc: return A830WinRscc;
    case A830GmuWindowGpu: return A830WinGpu;
    case A830GmuWindowCxMisc: return A830WinCxMisc;
    default: return A830WinGmu;
    }
}
int gmuTraceRead(void *,unsigned window,uint32_t offset,uint32_t *value)
{
    const A830GmuIo &raw=g830.gmu_raw;
    if(!traceLive())return raw.read32(raw.cookie,window,offset,value);
    const unsigned id=gmuTraceWindow(window);
    const A830TraceToken token=traceAccess(id,A830TraceRead,g830.win[id].phys+offset,0);
    const int failed=raw.read32(raw.cookie,window,offset,value);
    traceValue(token,failed || !value?0xffffffffull:*value);
    return failed;
}
int gmuTraceWrite(void *,unsigned window,uint32_t offset,uint32_t value)
{
    const A830GmuIo &raw=g830.gmu_raw;
    if(traceLive()) {
        const unsigned id=gmuTraceWindow(window);
        traceAccess(id,A830TraceWrite,g830.win[id].phys+offset,value);
    }
    return raw.write32(raw.cookie,window,offset,value);
}
void traceBindGmu(Adreno830Device *d)
{
    if(!traceAllowed)return;
    d->gmu_raw=d->gmu_io;
    d->gmu_io.read32=d->gmu_io.read32?gmuTraceRead:nullptr;
    d->gmu_io.write32=d->gmu_io.write32?gmuTraceWrite:nullptr;
}
/* A830Irq's windows (a830MmioDirect). Its top halves run in interrupt context and get no lines (traceHere). */
uint32_t irqTraceRead(void *cookie,uint32_t offset)
{
    const A830TraceMmio &m=*static_cast<const A830TraceMmio *>(cookie);
    if(!traceLive())return m.raw.read32(m.raw.cookie,offset);
    const A830TraceToken token=traceAccess(m.window,A830TraceRead,g830.win[m.window].phys+offset,0);
    const uint32_t value=m.raw.read32(m.raw.cookie,offset);
    traceValue(token,value);
    return value;
}
void irqTraceWrite(void *cookie,uint32_t offset,uint32_t value)
{
    const A830TraceMmio &m=*static_cast<const A830TraceMmio *>(cookie);
    if(traceLive())traceAccess(m.window,A830TraceWrite,g830.win[m.window].phys+offset,value);
    m.raw.write32(m.raw.cookie,offset,value);
}
A830Mmio traceIrqWindow(unsigned slot,unsigned window,const A830Mmio &raw)
{
    if(!traceAllowed || !raw.valid() || slot>=3)return raw;
    g830.irq_windows[slot]={raw,window};
    return {&g830.irq_windows[slot],irqTraceRead,irqTraceWrite};
}
/* The SMC instruction (Sm8750ScmOps, also A830Zap's and the SMMU aperture's calls): x0..x3 before, x0..x2 after. */
int scmTraceSmc(void *cookie,uint64_t regs[8])
{
    if(!traceLive())return osTable->smc(cookie,regs);
    const A830TraceToken token=traceAccess(TraceScm,A830TraceCall,regs[0],regs[1],TraceStatusStep,regs[2],regs[3]);
    const int failed=osTable->smc(cookie,regs);
    traceValue(token,regs[0],regs[1],regs[2]);
    return failed;
}
#ifdef ADRENO830_HOST_TEST
/* Host tests: the next adreno830_start begins without a trace. */
void traceReset()
{
    tracer=A830Trace{};
    traceAllowed=false;traceDepth=0;traceLockWord=0;traceQuiet=0;traceDrops=0;traceOpened=0;
    traceRecovering=false;traceRecoveryBegin=0;
}
#endif

IOReturn refuse(Adreno830Failure *failure,uint16_t step,uint16_t reason,uint64_t reg,uint64_t value,uint64_t expect=0,
                IOReturn result=kIOReturnBadArgument)
{
    failure->step=step;failure->reason=reason;failure->ior=result;failure->reg=reg;failure->value=value;failure->expect=expect;
    return result;
}

/* A board window mapped once (Device-nGnRE) and kept for the boot. */
IOReturn mapWindow(unsigned id,const Adreno830Window &window,Adreno830Failure *failure,uint16_t step)
{
    if(mapped(id))return kIOReturnSuccess;
    if(!window.phys || !window.bytes)return refuse(failure,step,A830ReasonArgument,window.phys,window.bytes);
    volatile void *va=os().map?os().map(window.phys,window.bytes,Adreno830MapDevice):nullptr;
    if(!va)return refuse(failure,step,A830ReasonNoMemory,window.phys,window.bytes,0,kIOReturnNoMemory);
    g830.win[id]={static_cast<volatile uint8_t *>(va),window.phys,window.bytes};
    return kIOReturnSuccess;
}

/* GPU-space dword offsets (A830Regs.h) to a window: GX/GBIF through the GPU window, GMUCX/GMUAO through the GMU window,
 * CX_MISC through its own (ARCHITECTURE 0.3). */
bool gpuDword(uint32_t dword,unsigned *id,uint32_t *offset)
{
    const uint64_t byte=4ull*dword;
    const uint64_t gmu=A830_GMU_WIN_DELTA,cx=A830_CX_MISC_WIN_DELTA;
    if(byte>=cx && byte<cx+A830_CX_MISC_BYTES && mapped(A830WinCxMisc)) {*id=A830WinCxMisc;*offset=uint32_t(byte-cx);return true;}
    if(byte>=gmu && byte<gmu+A830_GMU_BYTES && mapped(A830WinGmu)) {*id=A830WinGmu;*offset=uint32_t(byte-gmu);return true;}
    if(byte<A830_GPU_BYTES && mapped(A830WinGpu)) {*id=A830WinGpu;*offset=uint32_t(byte);return true;}
    return false;
}
bool readDword(uint32_t dword,uint32_t *value)
{
    unsigned id=0;uint32_t offset=0;
    if(!gpuDword(dword,&id,&offset))return false;
    *value=rd(id,offset);
    return true;
}
bool writeDword(uint32_t dword,uint32_t value)
{
    unsigned id=0;uint32_t offset=0;
    if(!gpuDword(dword,&id,&offset))return false;
    wr(id,offset,value);
    return true;
}
uint32_t dwordOr(uint32_t dword,uint32_t fallback) {uint32_t v=fallback;readDword(dword,&v);return v;}

/* ===== IO bindings for the modules ================================================================================== */
/* cmd-db and DRV-2 (Sm8750RpmhIo), AOSS QMP (same shape): the context is the window id. */
uint32_t rpmhRead(void *context,uint32_t offset) {return rd(unsigned(uintptr_t(context)),offset);}
void rpmhWrite(void *context,uint32_t offset,uint32_t value) {wr(unsigned(uintptr_t(context)),offset,value);}
void rpmhDelay(void *,unsigned us) {os().delay_us(us);}
void qmpDelay(void *,unsigned us) {os().sleep_us(us);}
/* SMEM and qfprom (Sm8750SocInfoIo): bounds-checked aligned reads. */
int socRead(void *cookie,uint32_t offset,uint32_t *value)
{
    const Adreno830Mmio &w=g830.win[unsigned(uintptr_t(cookie))];
    if(!w.base || (offset&3) || uint64_t(offset)+4>w.bytes)return 1;
    *value=adreno830_read32(&w,offset);
    return 0;
}
/* GCC and GPU_CC (Sm8750GpuPowerIo). The trace lines carry the power-up step (Sm8750GpuPowerTrace, the S1 failure step). */
int powerRead(void *,unsigned block,uint32_t offset,uint32_t *value)
{
    const unsigned id=block==Sm8750GpuGcc?A830WinGcc:A830WinGpuCc;
    if(!mapped(id) || uint64_t(offset)+4>g830.win[id].bytes)return 1;
    *value=rd(id,offset,int(g830.power.step&0xffff));
    return 0;
}
int powerWrite(void *,unsigned block,uint32_t offset,uint32_t value)
{
    const unsigned id=block==Sm8750GpuGcc?A830WinGcc:A830WinGpuCc;
    if(!mapped(id) || uint64_t(offset)+4>g830.win[id].bytes)return 1;
    wr(id,offset,value,int(g830.power.step&0xffff));
    return 0;
}
int powerDelay(void *,unsigned us) {os().delay_us(us);return 0;}
/* LLCC (Sm8750LlccIo): channel 0, broadcast OR, broadcast AND. */
unsigned llccWindow(unsigned block)
{
    return block==Sm8750LlccChannel0?A830WinLlcc0:block==Sm8750LlccBroadcastOr?A830WinLlccOr:A830WinLlccAnd;
}
int llccRead(void *,unsigned block,uint32_t offset,uint32_t *value)
{
    const unsigned id=llccWindow(block);
    if(!mapped(id) || uint64_t(offset)+4>g830.win[id].bytes)return 1;
    *value=rd(id,offset);
    return 0;
}
int llccWrite(void *,unsigned block,uint32_t offset,uint32_t value)
{
    const unsigned id=llccWindow(block);
    if(!mapped(id) || uint64_t(offset)+4>g830.win[id].bytes)return 1;
    wr(id,offset,value);
    return 0;
}
int llccDelay(void *,unsigned us) {os().delay_us(us);return 0;}
uint32_t tsensRead(void *,uint32_t offset) {return mapped(A830WinTsensTm) && offset+4<=g830.win[A830WinTsensTm].bytes?rd(A830WinTsensTm,offset):0;}
/* SCM (Sm8750ScmOps) from the kernel services. */
uint64_t scmNow(void *) {return nowUs();}
void scmSleep(void *,uint32_t us) {os().sleep_us(us);}
#ifdef ADRENO830_MACOS
/* UniXNU-piano: after the driver's own SCM mutex, the kernel-wide secure-call lock (other kexts call the secure world too). */
void scmLock(void *) {os().mutex_lock(g830.scm_mutex);adreno830_board_scm_hold(1);}
void scmUnlock(void *) {adreno830_board_scm_hold(0);os().mutex_unlock(g830.scm_mutex);}
#else
void scmLock(void *) {os().mutex_lock(g830.scm_mutex);}
void scmUnlock(void *) {os().mutex_unlock(g830.scm_mutex);}
#endif
void scmWait(void *,const void *event,uint32_t timeout_us) {os().wait(event,timeout_us);}
void scmWake(void *,const void *event) {os().wake(event);}
/* GPU registers by dword (NativeAdrenoRingIo, A830CpIo, NativeAdrenoTimeIo). */
int dwordWrite(void *,uint32_t reg,uint32_t value) {return writeDword(reg,value)?0:1;}
int dwordRead(void *,uint32_t reg,uint32_t *value) {return readDword(reg,value)?0:1;}
int dwordBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory");return 0;}
int ahbFenceStatus(void *,int clear,uint32_t *status)
{
    if(!readDword(A8XX_GMUAO_AHB_FENCE_STATUS,status))return 1;
    if(clear && *status)writeDword(A8XX_GMUAO_AHB_FENCE_STATUS_CLR,A830_AHB_FENCE_STATUS_CLR_VALUE);
    return 0;
}
void ringDelay(void *,uint32_t us) {os().delay_us(us);}
uint64_t clockNow(void *) {return nowUs();}
int clockIdle(void *) {os().sleep_us(100);return 0;}
void cpSleep(void *,uint32_t us) {os().sleep_us(us);}
void cpDelay(void *,uint32_t us) {os().delay_us(us);}
uint64_t cpNow(void *) {return nowUs();}
void mmioLock(void *) {os().mutex_lock(g830.mmio_mutex);}
void mmioUnlock(void *) {os().mutex_unlock(g830.mmio_mutex);}
int timeRead(void *,uint32_t address,uint32_t *value)
{
    if(!mapped(A830WinGmu) || uint64_t(address)+4>g830.win[A830WinGmu].bytes)return 1;
    /* The GMU AO counter is a CX register: under P1 full slumber CX is off, so the read first holds the GPU awake
     * (ARCHITECTURE 4.17; P0 never sleeps and accessBegin is two atomics). */
    A830PowerState *power=adreno830_power_state();
    bool woken=false;
    if(power && !power->accessBegin(false)) {
        if(power->acquire()!=kIOReturnSuccess)return 1;
        woken=true;
    }
    *value=rd(A830WinGmu,address);
    if(woken)power->release();
    else if(power)power->accessEnd(false);
    return 0;
}
/* HFI (A830HfiIo): GMU dwords, the HFI mutex (ARCHITECTURE 2.3), the fatal bits the GMU top halves saw. */
uint32_t hfiRead(void *,uint32_t dword) {uint32_t v=0;readDword(dword,&v);return v;}
void hfiWrite(void *,uint32_t dword,uint32_t value) {writeDword(dword,value);}
void hfiSleep(void *,uint32_t us) {os().sleep_us(us);}
uint64_t hfiNow(void *) {return nowUs();}
void hfiLock(void *) {os().mutex_lock(g830.hfi_mutex);}
void hfiUnlock(void *) {os().mutex_unlock(g830.hfi_mutex);}
bool hfiFaulted(void *) {return g830.gmu.fatalSeen();}
/* GMU services (A830GmuOs) and interrupt plumbing (A830GmuIrqOps). */
uint64_t gmuNow(void *) {return nowUs();}
void gmuSleep(void *,uint32_t us) {os().sleep_us(us);}
void gmuDelay(void *,uint32_t us) {os().delay_us(us);}
uint64_t gmuCounter(void *) {return os().counter?os().counter():0;}
uintptr_t gmuIrqDisable(void *) {return os().irq_disable?os().irq_disable():0;}
void gmuIrqRestore(void *,uintptr_t state) {if(os().irq_restore)os().irq_restore(state);}
void gmuWorkEntry(void *argument) {static_cast<A830Gmu *>(argument)->gmuWork();}
void *gmuWorkAllocate(A830Gmu *gmu) {return os().call_allocate?os().call_allocate(gmuWorkEntry,gmu):nullptr;}
void gmuWorkKick(void *work) {if(os().call_enter)os().call_enter(work);}
IOReturn spiAttach(uint32_t intid,bool edge,void (*handler)(void *),void *context,uint32_t *readback,uint64_t *router)
{
    return os().spi_attach?os().spi_attach(intid,edge,handler,context,readback,router):kIOReturnUnsupported;
}
IOReturn spiDisable(uint32_t intid) {return os().spi_disable?os().spi_disable(intid):kIOReturnUnsupported;}
IOReturn spiEnable(uint32_t intid) {return os().spi_enable?os().spi_enable(intid):kIOReturnUnsupported;}
/* GMU hooks: the HFI queues, the HFI mutex, the debug queue, global faults, the temporary GX vote of the force-off. */
int hookResetQueues(void *) {return g830.hfi.resetQueues()==kIOReturnSuccess?0:1;}
int hookDrainDebug(void *) {return g830.hfi.drainDebug()==kIOReturnSuccess?0:1;}
void hookGmuFault(void *,uint16_t reason,uint32_t info,uint32_t ao);
/* SMMU platform (A830SmmuPlatform). */
uint64_t smmuNow(void *) {return nowUs();}
void smmuDelay(void *,uint32_t us) {os().delay_us(us);}
void smmuFaultNotify(void *) {if(g830.smmu_fault_call && os().call_enter)os().call_enter(g830.smmu_fault_call);}
int smmuAperture(void *,uint32_t bank,int64_t *x0)
{
    Sm8750SmcResult result={};
    const Sm8750ScmStatus status=Sm8750ScmSetGpuSmmuAperture(&g830.scm,bank,&result);
    if(x0)*x0=result.x0;
    return status==Sm8750ScmOk?0:int(status);
}
/* The jobs in flight at a context fault, for its top half (M14 P0): the memptrs fence and the ring's last fence, two acquire loads
 * (0 and 0 before S4/S8 bound them: nothing in flight). */
void smmuFences(void *,uint32_t *completed,uint32_t *issued)
{
    *completed=g830.fence.completed();
    *issued=g830.ring.lastSubmitted();
}
/* The SMMU fault bottom half (ARCHITECTURE 4.12.3): CB0 faults go to the publish shell, which charges the connection whose window
 * holds FAR and counts how the jobs in flight at the fault (the fences the top half read) compare with it; CB2 faults and
 * unattributed ones are only counted (smmu_faults, by A830Smmu). */
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-21, 20261121-ws-crash CROSS-WINDOW.txt 4(a) D2): the hardware half of a CB0 fault's detail, taken by the bottom
 * half that found it: the fences now and, while GX is up (accessBegin: never wakes the GPU, never blocks), BR's and BV's IB1/IB2
 * through the hang snapshot's reads (A830Recovery::readIbs) and the BR ring read pointer. The publish shell adds the owners and the
 * command words (native_adreno_note_fault_detail). */
void noteFaultDetail(const A830SmmuFault &fault,uint32_t index)
{
    NativeAdrenoFaultDetail d{};
    d.far=fault.far;d.index=index;d.lines=A830SmmuFaultLines;
    d.completed=g830.fence.completed();d.issued=g830.ring.lastSubmitted();
    A830PowerState *power=adreno830_power_state();
    if(power && power->accessBegin(true)) {
        for(unsigned pipe=0;pipe<2;++pipe) {
            A830IbState ibs{};
            g830.recovery.readIbs(uint8_t(A8XX_PIPE_BR+pipe),&ibs);
            d.ib1[pipe]=ibs.ib1Base;d.ib1_rem[pipe]=ibs.ib1Rem;d.ib2[pipe]=ibs.ib2Base;d.ib2_rem[pipe]=ibs.ib2Rem;
        }
        uint32_t rptr=0;
        if(readDword(A8XX_CP_RB_RPTR,&rptr))d.rptr=rptr;
        power->accessEnd(true);
        d.ibs=true;
    }
    native_adreno_note_fault_detail(&d);
}
#endif
void smmuFaultSink(void *,const A830SmmuFault &fault)
{
    const bool gpu=fault.cb==g830.board->cb_gpu;
    if(gpu)native_adreno_note_page_fault(fault.far,fault.completed,fault.issued,fault.sampled!=0);
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (2026-11-21): the GPU bank's first A830SmmuFaultLines faults of a boot on the console whatever the diagnostic mode
     * (FSYNR0/1 say read or write and which GPU client, the SID which stream), each followed by its detail lines. */
    if(gpu && g830.fault_lines[1]<A830SmmuFaultLines) {
        char line[A830SmmuFaultLineBytes];
        A830SmmuFaultLine(line,fault);
        boot_progress_gpu_event(line+4);   /* "gpu fault cb0 ...": the event line brings its own "gpu " */
        noteFaultDetail(fault,++g830.fault_lines[1]);
        return;
    }
#endif
    if(g830.fault_lines[gpu]<A830SmmuFaultLines && adreno830_status_diag_lines()) {
        char line[A830SmmuFaultLineBytes];
        A830SmmuFaultLine(line,fault);
        boot_progress_trace(line);
        ++g830.fault_lines[gpu];
    }
}
void smmuFaultWork(void *) {g830.smmu.drainFaults(smmuFaultSink,nullptr);adreno830_status_publish();}
/* The SMMU's own register window for the few reads the orchestrator makes itself (LPAC route, CB fault state). */
uint32_t smmuRead(uint32_t offset)
{
    uint32_t v=0;
    if(g830.smmu_io.read32)g830.smmu_io.read32(g830.smmu_io.cookie,offset,&v);
    return v;
}
void smmuWrite(uint32_t offset,uint32_t value) {if(g830.smmu_io.write32)g830.smmu_io.write32(g830.smmu_io.cookie,offset,value);}
/* A context bank's 64-bit FAR, low word first (one deterministic order for the trace). */
uint64_t smmuFar(uint64_t bank)
{
    const uint64_t low=smmuRead(uint32_t(bank+A830_SMMU_CB_FAR));
    return low|uint64_t(smmuRead(uint32_t(bank+A830_SMMU_CB_FAR_HI)))<<32;
}
/* A830CpSmmu: the LPAC gate of S6 reads the LPAC slot's S2CR (functional bits only), and the sticky record. */
IOReturn cpLpacRoute(void *,uint32_t *readback)
{
    const A830SmmuStream lpac=g830.smmu.lpacStream();
    const uint32_t s2cr=smmuRead(A830_SMMU_GR0_S2CR(lpac.slot));
    if(readback)*readback=s2cr;
    return (s2cr&0x030303FFu)==(0x300u|lpac.cb)?kIOReturnSuccess:kIOReturnNotReady;
}
void cpRecordSticky(void *) {g830.smmu.poll();}
/* A830PowerIo: the four windows the power state machine and the recovery touch. */
unsigned powerWindow(uint8_t window)
{
    switch(window) {
    case A830PowerWinGpu: return A830WinGpu;
    case A830PowerWinGmu: return A830WinGmu;
    case A830PowerWinRscc: return A830WinRscc;
    default: return A830WinCxMisc;
    }
}
uint32_t powerWinRead(void *,uint8_t window,uint32_t offset)
{
    const unsigned id=powerWindow(window);
    return mapped(id) && uint64_t(offset)+4<=g830.win[id].bytes?rd(id,offset):0;
}
void powerWinWrite(void *,uint8_t window,uint32_t offset,uint32_t value)
{
    const unsigned id=powerWindow(window);
    if(mapped(id) && uint64_t(offset)+4<=g830.win[id].bytes)wr(id,offset,value);
}
void powerWinBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}
/* A830PowerEnv from the kernel services. */
uint64_t envNowNs(void *) {return nowUs()*1000;}
void envSleepUs(void *,uint32_t us) {os().sleep_us(us);}
void envDelayUs(void *,uint32_t us) {os().delay_us(us);}
void envLock(void *) {os().mutex_lock(g830.power_mutex);}
void envUnlock(void *) {os().mutex_unlock(g830.power_mutex);}
void envWait(void *,uint64_t deadline_ns)
{
    const uint64_t now=nowUs()*1000;
    const uint64_t us=deadline_ns>now?(deadline_ns-now+999)/1000:1;
    os().wait(&g830.power_event,uint32_t(us>4000000000ull?4000000000ull:us));
}
void envWake(void *) {os().wake(&g830.power_event);}
IOReturn envStartThread(void *,void (*body)(void *),void *argument)
{
    return os().thread_start?os().thread_start(body,argument):kIOReturnUnsupported;
}

/* ===== Board validation (S0, no hardware) =========================================================================== */
bool windowOk(const Adreno830Window &window)
{
    /* Page-aligned base, nonzero length, no wrap, below the 40-bit physical limit of SM8750. */
    return window.phys && window.bytes && !(window.phys&0xfff) && window.phys+window.bytes>window.phys &&
           window.phys+window.bytes<=(1ULL<<40);
}
bool spi(uint32_t intid) {return intid>=32 && intid<1020;}
uint64_t pinnedPrefix(const uint8_t sha256[32])
{
    uint64_t prefix=0;
    for(unsigned i=0;i<8;++i)prefix=prefix<<8|sha256[i];
    return prefix;
}

/* The structure is what this driver was built for, and every input a later stage dereferences or programs is plausible. */
IOReturn validateBoard(const Adreno830Board *board,Adreno830Failure *failure)
{
    if(!board)return refuse(failure,A830S0StepBoard,A830ReasonArgument,0,0,Adreno830BoardMagic);
    if(board->magic!=Adreno830BoardMagic)
        return refuse(failure,A830S0StepBoard,A830ReasonArgument,0,board->magic,Adreno830BoardMagic);
    if(board->version!=Adreno830BoardVersion)
        return refuse(failure,A830S0StepBoard,A830ReasonArgument,1,board->version,Adreno830BoardVersion);
    for(unsigned i=0;i<sizeof Adreno830BoardWindows/sizeof Adreno830BoardWindows[0];++i) {
        const Adreno830Window &window=board->*Adreno830BoardWindows[i];
        if(!windowOk(window))return refuse(failure,uint16_t(A830S0StepWindow+i),A830ReasonArgument,window.phys,window.bytes);
    }
    for(unsigned i=0;i<sizeof Adreno830BoardOptionalWindows/sizeof Adreno830BoardOptionalWindows[0];++i) {
        const Adreno830Window &window=board->*Adreno830BoardOptionalWindows[i];
        if((window.phys || window.bytes) && !windowOk(window))
            return refuse(failure,uint16_t(A830S0StepOptionalWindow+i),A830ReasonArgument,window.phys,window.bytes);
    }
    for(unsigned i=0;i<Adreno830LlccWindows;++i) {
        const Adreno830Window &window=board->llcc[i];
        if((window.phys || window.bytes) && !windowOk(window))
            return refuse(failure,uint16_t(A830S0StepLlccWindow+i),A830ReasonArgument,window.phys,window.bytes);
    }
    if(!board->chip_id)return refuse(failure,A830S0StepIdentity,A830ReasonArgument,0,board->chip_id);
    if(!board->soc_id)return refuse(failure,A830S0StepIdentity,A830ReasonArgument,1,board->soc_id);
    const uint32_t irqs[Adreno830BoardIrqCount]={board->irq_gpu,board->irq_cx_host,board->irq_hfi,board->irq_gmu,
        board->irq_smmu_global,board->irq_smmu_cb[0],board->irq_smmu_cb[1],board->irq_smmu_cb[2],board->irq_smmu_cb[3],
        board->irq_scm_waitq};
    for(unsigned i=0;i<Adreno830BoardIrqCount;++i)
        if(!spi(irqs[i]))return refuse(failure,uint16_t(A830S0StepIrq+i),A830ReasonArgument,i,irqs[i]);
    /* Three distinct streams on three distinct stream-match slots; the GPU and GMU contexts on different banks. */
    const bool streams=board->sid_gpu!=board->sid_lpac && board->sid_gpu!=board->sid_gmu && board->sid_lpac!=board->sid_gmu &&
        board->slot_gpu!=board->slot_lpac && board->slot_gpu!=board->slot_gmu && board->slot_lpac!=board->slot_gmu &&
        board->slot_gpu<128 && board->slot_lpac<128 && board->slot_gmu<128 &&
        board->cb_gpu!=board->cb_gmu && board->cb_gpu<128 && board->cb_gmu<128;
    if(!streams)
        return refuse(failure,A830S0StepStreams,A830ReasonArgument,
            board->sid_gpu|uint64_t(board->sid_lpac)<<16|uint64_t(board->sid_gmu)<<32,
            board->slot_gpu|uint64_t(board->slot_lpac)<<8|uint64_t(board->slot_gmu)<<16,
            board->cb_gpu|uint64_t(board->cb_gmu)<<8);
    if(!board->smmu_actlr)return refuse(failure,A830S0StepActlr,A830ReasonArgument,0,board->smmu_actlr);
    /* The power-level bins: each one present has a table of 1..Adreno830LevelMax levels, its SKUs fit, its initial level
     * (DT index) is inside it; the common count fits every bin; the DDR table has 1..16 levels. */
    unsigned bins=0,shortest=Adreno830LevelMax;
    for(unsigned i=0;i<Adreno830BinCount;++i) {
        if(!board->level_count[i] && !board->levels[i])continue;
        if(!board->levels[i] || !board->level_count[i] || board->level_count[i]>Adreno830LevelMax ||
           board->initial_level[i]>=board->level_count[i] || board->bin_sku_count[i]>Adreno830BinSkus)
            return refuse(failure,A830S0StepLevels,A830ReasonArgument,i,board->level_count[i],Adreno830LevelMax);
        ++bins;
        if(board->level_count[i]<shortest)shortest=board->level_count[i];
    }
    if(!bins || !board->common_level_count || board->common_level_count>shortest)
        return refuse(failure,A830S0StepLevels,A830ReasonArgument,0xff,board->common_level_count,shortest);
    if(!board->ddr_kbps || !board->ddr_levels || board->ddr_levels>A830VotesMaxDdr)
        return refuse(failure,A830S0StepLevels,A830ReasonArgument,0xfe,board->ddr_levels,A830VotesMaxDdr);
    /* The write-combined BO pool is DRAM reserved in S3 (halved until a reservation succeeds, NativeAdrenoGpuContext):
     * 64 MiB at least, never more than 2 GiB of the 6 GiB this phone has. */
    if(board->pool_mib<64 || board->pool_mib>2048)return refuse(failure,A830S0StepPool,A830ReasonArgument,0,board->pool_mib);
    const Adreno830Board::Adreno830Knobs &knobs=board->knobs;
    const uint32_t ddrTop=board->ddr_levels-1u;   /* highest DDR level of the bus table */
    const struct {Adreno830Knob knob;uint32_t value,limit;} knobLimits[]={
        {Adreno830KnobStatus,knobs.status,3},{Adreno830KnobAsync,knobs.async,1},{Adreno830KnobRails,knobs.rails,1},
        {Adreno830KnobDdrBoot,knobs.ddr_boot,ddrTop},{Adreno830KnobDdrFloor,knobs.ddr_floor,ddrTop},
        {Adreno830KnobCoherent,knobs.coherent,2},{Adreno830KnobDcvs,knobs.dcvs,1},{Adreno830KnobLlcc,knobs.llcc,1},
        {Adreno830KnobSpeedbin,knobs.speedbin,1},{Adreno830KnobAcd,knobs.acd,1},{Adreno830KnobDiag,knobs.diag,1},
        {Adreno830KnobQmp,knobs.qmp,1},{Adreno830KnobCohtest,knobs.cohtest,1},
        {Adreno830KnobSlumber,knobs.slumber,2},{Adreno830KnobIfpc,knobs.ifpc,1},{Adreno830KnobAhbTimeout,knobs.ahbto,1},
        {Adreno830KnobHfi,knobs.hfi,3},   /* bit0: Linux dependency vote, bit1: START after OOB; nothing else exists */
        {Adreno830KnobProfile,knobs.profile,A830ProfileCount-1u},
        {Adreno830KnobTrace,knobs.trace,2},   /* 0 off, 1 runs counted, 2 a line per access (A830Trace.h) */
        {Adreno830KnobMaxLevel,knobs.maxlevel,Adreno830LevelMax},   /* 0, or a GMU level (beyond the table: its top) */
        {Adreno830KnobIrq,knobs.irq,1},
        {Adreno830KnobTtarget,knobs.ttarget,100},  /* the thermal loop's target, C: 5 C under the 105 C emergency at most */
        {Adreno830KnobKopt,knobs.kopt,NativeAdrenoKoptAll},   /* NativeAdrenoKopt.h: a bit past these is no change this build has */
        {Adreno830KnobStop,knobs.stop==Adreno830StopNever?0:knobs.stop,A830SDcvs}};
    for(const auto &limit:knobLimits)
        if(limit.value>limit.limit)return refuse(failure,A830S0StepKnobs,A830ReasonArgument,limit.knob,limit.value,limit.limit);
    /* pagani-gpu-hwsched is reserved until the GMU hardware scheduler exists: RB is the only submission mode built, so any
     * other value is refused with its own reason instead of being ignored (kernel-sequences section 14 row 1). */
    if(knobs.hwsched)
        return refuse(failure,A830S0StepKnobs,A830ReasonUnsupported,Adreno830KnobHwsched,knobs.hwsched,0,kIOReturnUnsupported);
    /* The floor is what the host keeps of its boot DDR vote after the first GX_BW_PERF_VOTE answer (ARCHITECTURE 4.3.3):
     * never more than the boot level. */
    if(knobs.ddr_floor>knobs.ddr_boot)
        return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobDdrFloor,knobs.ddr_floor,knobs.ddr_boot);
    /* pagani-gpu-perf indexes the level table: 1..Adreno830LevelMax (0 is off); S0 narrows it to the selected table. */
    if(knobs.perf<1 || knobs.perf>Adreno830LevelMax)
        return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobPerf,knobs.perf,Adreno830LevelMax);
    if(knobs.va!=2 && knobs.va!=4 && knobs.va!=16 && knobs.va!=64 && knobs.va!=256)   /* UniXNU-piano: 64 (GPU-SLOTS), 256 (gpu/slots2) */
        return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobVa,knobs.va,4);
    if(knobs.dcvs_min_mhz>knobs.dcvs_max_mhz)
        return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobDcvsMin,knobs.dcvs_min_mhz,knobs.dcvs_max_mhz);
    if(!knobs.actlr)return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobActlr,knobs.actlr,1);
    if(knobs.ttarget<50)return refuse(failure,A830S0StepKnobs,A830ReasonArgument,Adreno830KnobTtarget,knobs.ttarget,50);
    /* Each embedded image must be the pinned one: a missing image (the weak zero-length default) or a different build of
     * the firmware stops here, before any of it reaches the GMU or the CP. */
    for(unsigned i=0;i<sizeof Adreno830BoardFirmware/sizeof Adreno830BoardFirmware[0];++i) {
        const Adreno830Firmware &image=board->*Adreno830BoardFirmware[i];
        NativeAdrenoFirmwareRules rules={image.length,{}};
        memcpy(rules.sha256,image.sha256,sizeof rules.sha256);
        NativeAdrenoFirmwareView view;
        const IOReturn result=NativeAdrenoFirmware::validate(image.bytes,image.length,&rules,&view);
        if(result!=kIOReturnSuccess) {
            adreno830_status.aux[0]|=A830S0FirmwareChecked;
            return refuse(failure,uint16_t(A830S0StepFirmware+i),A830ReasonFirmware,i,image.length,
                          pinnedPrefix(image.sha256),result);
        }
    }
    adreno830_status.aux[0]|=A830S0FirmwareChecked|A830S0FirmwareOk;
    return kIOReturnSuccess;
}

/* ===== S0: board inputs ============================================================================================== */
IOReturn stageInputs(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    IOReturn r=validateBoard(b,f);
    if(r!=kIOReturnSuccess)return r;
    Adreno830Status &s=adreno830_status;
    /* The bring-up profile, before any module reads it (the vote tables below may already depend on it), and the
     * performance knobs the RUN row will show. */
    adreno830_profile_value=b->knobs.profile;
    s.profile=uint8_t(b->knobs.profile);s.profile_set=1;
    adreno830_status_knobs(b->knobs.maxlevel,b->knobs.irq);
    NativeAdrenoMemory::setKopt(b->knobs.kopt);   /* before S3 reserves the pool and maintains it */
    /* cmd-db (4.1.1): aligned 32-bit copy into the static snapshot, then only the snapshot is parsed. */
    adreno830_status_step(A830S0StepCmdDb);
    if((r=mapWindow(A830WinCmdDb,b->cmd_db,f,A830S0StepMap))!=kIOReturnSuccess)return r;
    d->cmd_db_io={reinterpret_cast<void *>(uintptr_t(A830WinCmdDb)),rpmhRead,rpmhWrite,rpmhDelay};
    int rc=Sm8750CmdDbCopy(&d->cmd_db_io,&cmdDbSnapshot);
    if(rc==Sm8750RpmhOk)rc=Sm8750RpmhResourcesParse(&cmdDbSnapshot,&d->rpmh);
    if(rc!=Sm8750RpmhOk)
        return refuse(f,A830S0StepCmdDb,A830ReasonCmdDb,b->cmd_db.phys,uint64_t(int64_t(rc)),d->rpmh.failed,kIOReturnNotReady);
    s.aux[0]|=A830Votes::statusAux(d->rpmh,s.aux);
    /* SMEM socinfo (4.1.2): only the two readable ranges are ever touched. */
    adreno830_status_step(A830S0StepSocInfo);
    if((r=mapWindow(A830WinSmem,b->smem,f,A830S0StepMap+1))!=kIOReturnSuccess)return r;
    const Sm8750SocInfoIo smem={reinterpret_cast<void *>(uintptr_t(A830WinSmem)),socRead};
    const Sm8750SocInfoStatus soc=Sm8750SocInfoRead(&smem,&d->soc);
    if(soc!=Sm8750SocInfoOk)
        return refuse(f,A830S0StepSocInfo,A830ReasonSocInfo,b->smem.phys+d->soc.last_offset,soc,0,kIOReturnNotReady);
    s.soc_id=d->soc.id;s.aux[0]|=A830S0SocId;
    if(!(d->soc.flags&Sm8750SocInfoFlagNoFeature)) {s.feature_code=d->soc.feature_code;s.aux[0]|=A830S0FeatureCode;}
    if(d->soc.id!=b->soc_id)return refuse(f,A830S0StepSocId,A830ReasonSocInfo,b->smem.phys,d->soc.id,b->soc_id,kIOReturnUnsupported);
    /* Speed-bin fuse (4.1.3): read and recorded; a failed read leaves the common table (no bin can match). */
    d->speed_bin=A830VotesNoFuse;
    if(mapWindow(A830WinQfprom,b->qfprom,f,A830S0StepMap+2)==kIOReturnSuccess) {
        const Sm8750SocInfoIo qfprom={reinterpret_cast<void *>(uintptr_t(A830WinQfprom)),socRead};
        uint32_t bin=0;
        if(Sm8750SpeedBinRead(&qfprom,b->qfprom_speedbin_offset,0,b->speedbin_bits,&bin)==Sm8750SocInfoOk) {
            d->speed_bin=bin;s.speed_bin=bin;s.aux[0]|=A830S0SpeedBin;
        }
    }
    *f={};
    /* The level table (4.1.4): the common levels, plus the matched bin's faster ones with pagani-gpu-speedbin=1. */
    adreno830_status_step(A830S0StepLevelTable);
    Sm8750GpuBin bins[Adreno830BinCount]={};
    const unsigned count=A830Votes::boardBins(*b,bins);
    const Sm8750SocInfoStatus selected=Sm8750GpuLevelsSelect(bins,count,d->speed_bin,d->soc.soc_code,int(b->knobs.speedbin),
                                                             &d->levels);
    if(selected!=Sm8750SocInfoOk || !d->levels.count || d->levels.count<b->common_level_count)
        return refuse(f,A830S0StepLevelTable,A830ReasonArgument,count,selected,b->common_level_count);
    if(b->knobs.perf>d->levels.count)
        return refuse(f,A830S0StepLevelTable,A830ReasonArgument,Adreno830KnobPerf,b->knobs.perf,d->levels.count);
    /* The GX/CX/dependency votes, the GMU bandwidth table and the ACD table, kept for every later GMU boot (4.7.7).
     * pagani-gpu-maxlevel: the annibale profile keeps the whole selected table too (A830VotesFullTable). */
    adreno830_status_step(A830S0StepVotes);
    const uint32_t options=((b->knobs.hfi&1u)?uint32_t(A830VotesLinuxDependency):0u)|
                           (b->knobs.maxlevel?uint32_t(A830VotesFullTable):0u);
    if((r=A830Votes::build(d->rpmh,*b,d->levels,options,&d->votes))!=kIOReturnSuccess)
        return refuse(f,A830S0StepVotes,A830ReasonCmdDb,d->votes.error_resource,
                      uint64_t(d->votes.error_step)<<16|(d->votes.error_value&0xffff),0,r);
    /* gpu/fullopp1 (private/host-checks/20261122-gpu-fullopp/ANALYSIS.md 3.1, 3.5 item 3): Android's whole curve, bin 2 of the fuse
     * 0xe8 with its 14 levels (GX 15, 58 perf dwords), ACD allowed (required above 734 MHz), floor 160 or 222; every level checked
     * against bin 2's MHz. Anything else refuses the boot, as the 607 check does. */
    const bool fullopp=qualificationMode(b) && b->qualification_fullopp==1;
    if(fullopp) {
        const uint8_t top=qualificationTop(b);
        bool table=d->votes.levels==14 && d->votes.gx_count==15 && d->votes.cx_count==3 && d->votes.perf_dwords==58 &&
                   d->votes.initial==1 && d->votes.bw[A830BwLevelNum]==12;
        for(unsigned i=0;table && i<14;++i)table=d->votes.level[i].khz==uint32_t(FulloppMhz[i+1])*1000u;
        if(b->knobs.profile!=1 || b->knobs.speedbin!=1 || d->speed_bin!=0xe8 || b->knobs.acd>1 || (top>=10 && !b->knobs.acd) ||
           b->knobs.hfi || b->knobs.rails!=1 || b->knobs.ddr_boot!=11 || b->knobs.ddr_floor!=11 || !b->knobs.dcvs ||
           (b->knobs.dcvs_min_mhz!=160 && b->knobs.dcvs_min_mhz!=222) || !table)
            return refuse(f,A830S0StepVotes,A830ReasonArgument,1,d->votes.levels,14,kIOReturnBadArgument);
    } else if(qualificationMode(b)) {
        if(b->knobs.profile!=1 || b->knobs.speedbin || b->knobs.acd || b->knobs.hfi || b->knobs.rails!=1 ||
           b->knobs.ddr_boot!=11 || b->knobs.ddr_floor!=11 || !b->knobs.dcvs ||
           (b->knobs.dcvs_min_mhz!=160 && !qualificationResidency(b)) ||
           d->votes.levels!=11 || d->votes.gx_count!=12 || d->votes.cx_count!=3 || d->votes.perf_dwords!=49 ||
           d->votes.initial!=1 || d->votes.level[6].khz!=607000 || d->votes.bw[A830BwLevelNum]!=12)
            return refuse(f,A830S0StepVotes,A830ReasonArgument,0,d->votes.levels,11,kIOReturnBadArgument);
    }
    if(qualificationMode(b)) {
        const Sm8750RpmhArc *arcs[5]={&d->rpmh.gfx,&d->rpmh.mx,&d->rpmh.cx,&d->rpmh.gmxc,&d->rpmh.gbx};
        const char *names[5]={"gfx","mx","cx","gmxc","gbx"};
        for(unsigned a=0;a<5;++a) {
            const auto &arc=*arcs[a];qualLog("R3a ARC %s present=%u count=%u",names[a],arc.present,arc.count);
            for(unsigned i=0;i<arc.count;++i)qualLog("R3a ARC %s index=%u vlvl=%x",names[a],i,arc.vlvl[i]);
        }
        for(unsigned g=0;g<d->votes.gx_count;++g)qualLog("R3a GX %u khz=%u gx=%08x dep=%08x",g,
            d->votes.gx[g].khz,d->votes.gx[g].vote,d->votes.gx[g].dep);
        for(unsigned c=0;c<3;++c)qualLog("R3a CX %u khz=%u vote=%08x",c,d->votes.cx[c].khz,d->votes.cx[c].vote);
        const Sm8750RpmhBcm *bcms[4]={&d->rpmh.sh0,&d->rpmh.mc0,&d->rpmh.acv,&d->rpmh.cn0};
        for(unsigned i=0;i<4;++i)qualLog("R3a BCM %u addr=%x unit=%u width=%u vcd=%u",i,bcms[i]->addr,bcms[i]->unit,bcms[i]->width,bcms[i]->vcd);
        for(unsigned i=0;i<12;++i)qualLog("R3a DDR %u kbps=%u wait=%x words=%08x,%08x,%08x",i,d->votes.ddr_kbps[i],
            d->votes.bw_wait_mask,d->votes.bw[A830BwDdrData+i*8],d->votes.bw[A830BwDdrData+i*8+1],d->votes.bw[A830BwDdrData+i*8+2]);
        if(fullopp) {
            qualLog("R3a C5-FULL-v1 full-table verified M1=58 GX15 CX3 M2=160 profile1 speedbin1 bin=0x%x levels=14 top=%u ACD%s "
                    "acd-levels=%u host-rails1 DDR11",unsigned(d->speed_bin),unsigned(FulloppMhz[qualificationTop(b)])*1000u,
                    b->knobs.acd?"on":"off",unsigned(d->votes.acd_levels));
        } else qualLog("R3a C5-607-v1 full-table verified M1=49 GX12 CX3 M2=160 profile1 speedbin0 ACDoff host-rails1 DDR11");
    }
    if(!A830Votes::levelAtOrAbove(d->votes,b->knobs.dcvs_min_mhz) || !A830Votes::levelAtOrBelow(d->votes,b->knobs.dcvs_max_mhz))
        return refuse(f,A830S0StepLevelTable,A830ReasonArgument,Adreno830KnobDcvsMax,b->knobs.dcvs_max_mhz,b->knobs.dcvs_min_mhz);
    /* gen80000_gmu.bin blocks (4.7.1): the PREALLOC requests size the GMU memory of S3. */
    adreno830_status_step(A830S0StepGmuImage);
    if((r=A830GmuFirmware::parse(b->gmu_fw.bytes,size_t(b->gmu_fw.length),&d->gmu_blocks))!=kIOReturnSuccess)
        return refuse(f,A830S0StepGmuImage,A830ReasonFirmware,0,r,0,r);
    return kIOReturnSuccess;
}

/* ===== S1: power ===================================================================================================== */
void bindGmuServices(Adreno830Device *d);
void recordTcs(const Sm8750RpmhResult &result)
{
    uint64_t &w=adreno830_status.aux[5];
    w=(w&(A830S1Power|A830S1Scm))|A830S1Tcs|
      A830StatusTcs(result.tcs>15?0xf:result.tcs,result.status,result.command[0],result.command[1],result.command[2]);
}
uint16_t powerReason(Sm8750GpuPowerStatus status)
{
    switch(status) {
    case Sm8750GpuPowerTimeout: return A830ReasonTimeout;
    case Sm8750GpuPowerUnexpectedState: return A830ReasonUnexpectedState;
    case Sm8750GpuPowerIoError: return A830ReasonReadback;
    default: return A830ReasonArgument;
    }
}
void recordPower(bool cm3Valid,uint32_t cm3)
{
    const Sm8750GpuPowerTrace &t=g830.power;
    const uint32_t step=t.step>Sm8750GpuStepBranches?uint32_t(Sm8750GpuStepBranches):t.step;
    const uint32_t pll=mapped(A830WinGpuCc)?rd(A830WinGpuCc,SM8750_GPUCC_PLL0_MODE):0;
    Adreno830Status &s=adreno830_status;
    s.aux[0]=(step&0xff)|uint64_t(t.block&0xff)<<8|(cm3Valid?1ull<<16:0)|uint64_t(cm3&1)<<17|uint64_t(t.offset)<<32;
    s.aux[1]=t.value|uint64_t(pll)<<32;
    s.aux[5]|=A830S1Power;
}
IOReturn mapPowerWindows(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    const struct {unsigned id;const Adreno830Window *window;} windows[]={
        {A830WinGpu,&b->gpu},{A830WinRscc,&b->rscc},{A830WinCxDbgc,&b->cx_dbgc},{A830WinCxMisc,&b->cx_misc},
        {A830WinGmu,&b->gmu},{A830WinGmuAo,&b->gmu_ao},{A830WinGpuCc,&b->gpucc},{A830WinGcc,&b->gcc},{A830WinSmmu,&b->smmu},
        {A830WinRscDrv2,&b->rsc_drv2}};
    for(const auto &w:windows) {
        const IOReturn r=mapWindow(w.id,*w.window,f,uint16_t(A830S1StepMap+w.id));
        if(r!=kIOReturnSuccess)return r;
    }
    /* Optional windows of later stages: absent or unmappable ones leave their feature off (TSENS cap, LLCC). */
    Adreno830Failure ignored={};
    if(b->tsens_tm.phys)mapWindow(A830WinTsensTm,b->tsens_tm,&ignored,0);
    if(b->knobs.llcc)
        for(unsigned i=0;i<Adreno830LlccWindows;++i)
            if(b->llcc[i].phys)mapWindow(A830WinLlcc0+i,b->llcc[i],&ignored,0);
    return kIOReturnSuccess;
}
IOReturn stagePower(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    Adreno830Status &s=adreno830_status;
    /* Optional first step (4.17 P3 row): tell the AOP the GPU ACD state before the first GMU boot, as KGSL does. With
     * pagani-gpu-qmp=1 the message is val 0 (ACD off); pagani-gpu-acd=1 sends val 1 and enables ACD (HFI M3 in S5) only
     * once the AOP took it. The result is recorded; it never fails the stage. */
    d->acd_ready=false;
    if((b->knobs.qmp || b->knobs.acd) && b->aoss_msgram.phys && b->ipcc.phys) {
        adreno830_status_step(A830S1StepQmp);
        Adreno830Failure ignored={};
        if(mapWindow(A830WinAossMsgram,b->aoss_msgram,&ignored,0)==kIOReturnSuccess &&
           mapWindow(A830WinIpcc,b->ipcc,&ignored,0)==kIOReturnSuccess) {
            d->qmp_msgram={reinterpret_cast<void *>(uintptr_t(A830WinAossMsgram)),rpmhRead,rpmhWrite,qmpDelay};
            d->qmp_ipcc={reinterpret_cast<void *>(uintptr_t(A830WinIpcc)),rpmhRead,rpmhWrite,qmpDelay};
            Sm8750AossQmpInit(&d->qmp,&d->qmp_msgram,uint32_t(b->aoss_msgram.bytes),&d->qmp_ipcc);
            /* One message at a time on the channel PaganiAdsp and PaganiWifi share: their lock word, a 3 s wait, then this message anyway. */
            bool qmpLocked=false;
            for(unsigned waited=0;waited<3000 && !qmpLocked;++waited) {
                uint32_t expected=0;
                qmpLocked=__atomic_compare_exchange_n(&sm8750_aoss_qmp_owner,&expected,uint32_t(Sm8750RpmhLockOwnerGpu),false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
                if(!qmpLocked)qmpDelay(nullptr,1000);
            }
            const int rc=Sm8750AossQmpSendAcdState(&d->qmp,b->knobs.acd?1:0);
            if(qmpLocked) {
                uint32_t expected=uint32_t(Sm8750RpmhLockOwnerGpu);
                __atomic_compare_exchange_n(&sm8750_aoss_qmp_owner,&expected,0u,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
            }
            s.qmp=uint8_t(Sm8750AossQmpStatusCode(rc));
            d->acd_ready=b->knobs.acd && rc==Sm8750AossQmpOk;
        } else s.qmp=A830QmpRefused;
    }
    IOReturn r=mapPowerWindows(d,f);
    if(r!=kIOReturnSuccess)return r;
    /* The host mx/cx votes (4.3.3), mx first when raising: the table top (pagani-gpu-rails=1, measured) or LOW_SVS. */
    adreno830_status_step(A830S1StepRails);
    d->drv2_io={reinterpret_cast<void *>(uintptr_t(A830WinRscDrv2)),rpmhRead,rpmhWrite,rpmhDelay};
    Sm8750RpmhHostInit(&d->host,&d->drv2_io,&d->rpmh);
    int rc=Sm8750RpmhHostRails(&d->host,int(b->knobs.rails));
    recordTcs(d->host.last);
    if(rc!=Sm8750RpmhOk)
        return refuse(f,A830S1StepRails,A830ReasonRpmh,d->host.last.step,uint64_t(int64_t(rc)),d->host.last.tcs,
                      rc==Sm8750RpmhTimeout?kIOReturnTimeout:kIOReturnIOError);
    /* GCC/GPU_CC power-up (4.2.1). */
    d->power_io={nullptr,powerRead,powerWrite,powerDelay};
    d->power={};
    const Sm8750GpuPowerStatus power=Sm8750GpuPowerOn(&d->power_io,&d->power);
    if(power!=Sm8750GpuPowerOk) {
        recordPower(false,0);
        const uint64_t base=d->power.block==Sm8750GpuGcc?b->gcc.phys:b->gpucc.phys;
        return refuse(f,uint16_t(d->power.step),powerReason(power),base+d->power.offset,d->power.value,0,
                      power==Sm8750GpuPowerTimeout?kIOReturnTimeout:kIOReturnIOError);
    }
    /* CX_MISC AHB timeouts (annibale kernel-sequences 2.3 step 8, section 14 row 6; ARCHITECTURE 4.4.2): HUB runs at 200 MHz
     * and the branches are on, so CX_AHB_{AON,GMU,CP,VBIF_SMMU,HOST}_CNTL get 0x1B12 (2^19 HUB cycles, about 2.6 ms, under the
     * 3.4 ms NOC limit) and a stuck AHB access ends in an AHB error instead of a hung bus. The annibale profile reads each one
     * back exactly (a830CxAhbTimeout); the design profile writes them only with pagani-gpu-ahbto=1 (M13 c), without read
     * back. CX_MISC keeps them while CX stays up; a wake after CX collapsed writes them again (opCxPowerOn). */
    if(adreno830_annibale() || b->knobs.ahbto) {
        adreno830_status_step(A830S1StepAhbTimeout);
        bindGmuServices(d);
        if((r=a830CxAhbTimeout(d->cx_timer))!=kIOReturnSuccess)
            return refuse(f,A830S1StepAhbTimeout,A830ReasonReadback,d->cx_timer.ahb_reg,d->cx_timer.ahb_read,
                          A830_CX_AHB_CNTL_VALUE,r);
    }
    /* CM3_SYSRESET: recorded only (expected 1; B1 writes it anyway, 4.2.1 step 10). */
    adreno830_status_step(A830S1StepCm3);
    recordPower(true,dwordOr(A8XX_GMUCX_CM3_SYSRESET,0));
    return kIOReturnSuccess;
}

/* ===== S2: secure CX ================================================================================================= */
IOReturn stageSecureCx(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    Adreno830Status &s=adreno830_status;
    if(!d->scm_mutex)d->scm_mutex=os().mutex_alloc?os().mutex_alloc():nullptr;
    /* Every SMC of the boot (this stage, the ZAP of S7, the SMMU aperture) goes through this table; the trace wraps it. */
    d->scm_ops={nullptr,traceAllowed && os().smc?scmTraceSmc:os().smc,scmNow,scmSleep,d->scm_mutex?scmLock:nullptr,
                d->scm_mutex?scmUnlock:nullptr,scmWait,scmWake};
    Sm8750ScmInit(&d->scm,&d->scm_ops);
    /* The TZ wait-queue interrupt before the first SMC (4.12.2, optional: without it GET_WQ_CTX is polled every 1 ms). */
    adreno830_status_step(A830S2StepWaitq);
    uint32_t queues=0,intid=0;
    Sm8750SmcResult info={};
    if(Sm8750ScmWaitqInfo(&d->scm,&queues,&intid,&info)!=Sm8750ScmOk || intid<32 || intid>=1020)intid=b->irq_scm_waitq;
    uint32_t readback=0;uint64_t router=0;
    const IOReturn attached=spiAttach(intid,true,Sm8750ScmWaitqIrq,&d->scm,&readback,&router);
    if(attached==kIOReturnSuccess && (readback&A830IrqReadbackMask)==A830IrqReadbackEdge)Sm8750ScmSetIrqAttached(&d->scm,1);
    else adreno830_status_irq_off(A830IrqOffScmWaitq);
    adreno830_status_step(A830S2StepProbe);
    uint32_t available=0;
    Sm8750ScmProbeGpu(&d->scm,&available);   /* recorded in scm.available */
    /* GPU_INIT_REGS(ALWAYS_EN | TSENSE_EN), once per boot (4.4.1): x0 0 or -4; x1 is not looked at. */
    adreno830_status_step(A830S2StepInitRegs);
    Sm8750SmcResult init={};
    const Sm8750ScmStatus scm=Sm8750ScmGpuInitRegs(&d->scm,A830_SCM_GPU_INIT_REQ,&init);
    s.aux[2]=uint64_t(init.x0);s.aux[3]=uint32_t(init.x1)|uint64_t(init.calls)<<32;s.aux[5]|=A830S1Scm;
    if(scm!=Sm8750ScmOk)
        return refuse(f,A830S2StepInitRegs,A830ReasonSmc,SM8750_SCM_GPU_INIT_REGS,uint64_t(init.x0),init.x1,
                      scm==Sm8750ScmTimeout?kIOReturnTimeout:kIOReturnIOError);
    /* Host CN0 (keepalive 0x60000001) and the DDR boot vote (pagani-gpu-ddr, level 11 measured), ACV BIT(0) (4.3.3). */
    adreno830_status_step(A830S2StepCn0);
    int rc=Sm8750RpmhHostCn0(&d->host);
    recordTcs(d->host.last);
    if(rc!=Sm8750RpmhOk)
        return refuse(f,A830S2StepCn0,A830ReasonRpmh,d->host.last.step,uint64_t(int64_t(rc)),d->host.last.tcs,kIOReturnIOError);
    adreno830_status_step(A830S2StepDdr);
    const uint32_t kbps=adreno830_ddr_kbps(b->knobs.ddr_boot);
    rc=Sm8750RpmhHostDdr(&d->host,kbps);
    recordTcs(d->host.last);
    if(rc!=Sm8750RpmhOk)
        return refuse(f,A830S2StepDdr,A830ReasonRpmh,d->host.last.step,uint64_t(int64_t(rc)),kbps,kIOReturnIOError);
    /* The software fuses, recorded only (a non-zero value also shows TZ ran INIT_REGS, 4.4.2). TCM_RET_CNTL read only. */
    adreno830_status_step(A830S2StepFuse);
    const uint32_t fuse=dwordOr(A8XX_CX_MISC_SW_FUSE_VALUE,0);   /* in this order (the trace shows it) */
    s.aux[4]=fuse|uint64_t(dwordOr(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS,0))<<32;
    (void)dwordOr(A8XX_CX_MISC_TCM_RET_CNTL,0);
    return kIOReturnSuccess;
}

/* ===== S3: SMMU ====================================================================================================== */
IOReturn smmuFailure(Adreno830Failure *f,IOReturn r)
{
    const A830SmmuFailure &sf=g830.smmu.failure();
    return refuse(f,sf.step,sf.reason?sf.reason:A830ReasonSmmuRoute,sf.reg,sf.value,sf.expect,r);
}
IOReturn gmuMap(void *,NativeAdrenoVm *vm) {return g830.gmu_memory.mapInto(vm);}
/* A context bring-up failure: the programmer's own step (the 4.5.4 register steps) when the programmer failed, else the
 * context step under A830S3StepContext. */
IOReturn contextFailure(Adreno830Failure *f,const NativeAdrenoGmuContextReport &report,uint16_t bank,IOReturn r,
                        uint32_t programStep,uint32_t attachStep)
{
    if(report.step==programStep || report.step==attachStep) {
        const A830SmmuFailure &sf=g830.smmu.failure();
        if(sf.step)return smmuFailure(f,r);
    }
    const uint16_t reason=r==kIOReturnNoMemory || r==kIOReturnNoResources?A830ReasonNoMemory:A830ReasonSmmuRoute;
    return refuse(f,uint16_t(bank+A830S3StepContext),reason,report.step,uint64_t(uint32_t(r)),report.program.step,r);
}
IOReturn stageSmmu(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    Adreno830Status &s=adreno830_status;
    d->smmu_mmio={d->win[A830WinSmmu].base,size_t(d->win[A830WinSmmu].bytes)};
    IOReturn r=A830SmmuBindMmio(&d->smmu_mmio,&d->smmu_io);
    if(r!=kIOReturnSuccess)return refuse(f,A830S3StepIds,A830ReasonArgument,b->smmu.phys,b->smmu.bytes,0,r);
    traceBindSmmu(d);   /* A830Smmu copies the io in init: every SMMU access of the boot is traced from here on */
    if(!d->smmu_fault_call && os().call_allocate)d->smmu_fault_call=os().call_allocate(smmuFaultWork,nullptr);
    A830SmmuConfig config={};
    config.io=&d->smmu_io;config.phys=b->smmu.phys;
    config.platform={nullptr,smmuNow,smmuDelay,spiAttach,spiDisable,spiEnable,smmuFaultNotify,smmuAperture,smmuFences};
    config.actlr=b->knobs.actlr;config.cb_gpu=b->cb_gpu;config.cb_gmu=b->cb_gmu;
    config.slot_gpu=b->slot_gpu;config.slot_lpac=b->slot_lpac;config.slot_gmu=b->slot_gmu;
    config.sid_gpu=b->sid_gpu;config.sid_lpac=b->sid_lpac;config.sid_gmu=b->sid_gmu;
    for(unsigned i=0;i<4;++i)config.irq_cb[i]=b->irq_smmu_cb[i];
    config.irq_global=b->irq_smmu_global;config.status=&adreno830_status;
    if((r=d->smmu.init(config))!=kIOReturnSuccess)return refuse(f,A830S3StepIds,A830ReasonArgument,b->smmu.phys,r,0,r);
    /* 4.5.3: the read-only survey (IDs, CLIENTPD, no valid route to CB0/CB2 or SMR covering our SIDs, the slot choice). */
    adreno830_status_step(A830S3StepIds);
    A830SmmuSurvey survey={};
    r=d->smmu.survey(&survey);
    d->smmu.fillStatus(&s);
    s.smmu_slots=survey.slot_gpu|uint32_t(survey.slot_lpac)<<8|uint32_t(survey.slot_gmu)<<16;
    if(r!=kIOReturnSuccess)return smmuFailure(f,r);
    /* Steps 3 and 4: sACR CACHE_LOCK and sGFSR, written back and only recorded (the annibale profile of A830Smmu reads and
     * records them only, section 14 row 9). */
    adreno830_status_step(A830S3StepAcr);
    d->smmu.releaseCacheLock();
    adreno830_status_step(A830S3StepGfsr);
    d->smmu.clearGlobalFault();
    /* Both banks reset (SCTLR 0, FSR cleared) before their fault interrupts are attached (4.5.4). */
    adreno830_status_step(A830S3StepBankReset+A830S3Cb2);
    if((r=d->smmu.resetBank(b->cb_gmu))!=kIOReturnSuccess)return smmuFailure(f,r);
    adreno830_status_step(A830S3StepBankReset+A830S3Cb0);
    if((r=d->smmu.resetBank(b->cb_gpu))!=kIOReturnSuccess)return smmuFailure(f,r);
    adreno830_status_step(A830S3StepIrqs);
    d->smmu.attachIrqs();   /* 710/712, optional: failures are in irq_off and the banks are polled (annibale: never attached) */
    d->smmu_claimed=true;
    /* The write-combined BO pool (pagani-gpu-pool, 512 MiB) while memory is still unfragmented, before the context tables:
     * halved until a reservation succeeds, 64 MiB at least; none at all only costs scattered BOs (the GPU context keeps it). */
    adreno830_status_step(A830S3StepPool);
    const unsigned oas=survey.output_bits>40?40:survey.output_bits;
    uint32_t reserved=0;
    /* UniXNU-piano (2026-11-23, GPU-PAGED P1, private P1-GATE.md): piano-gpu-mem=paged reserves no pool (every user BO is its own
     * pageable named entry; the VM's tables come on demand). */
    const bool paged=__atomic_load_n(&native_adreno_gpu_mem_paged,__ATOMIC_ACQUIRE)!=0;
    if(!paged && (r=NativeAdrenoMemory::reservePoolBestEffort(oas?oas:40,b->pool_mib,64,true,&reserved))!=kIOReturnSuccess)
        return refuse(f,A830S3StepPool,A830ReasonNoMemory,0,b->pool_mib,reserved,r);
    /* The GMU context (CB2, slot 1, SID 5, ASID 3) with every region of the firmware's PREALLOC plan mapped: the GMU fetches
     * its code from DDR through it (4.7.1-4.7.2). */
    adreno830_status_step(A830S3StepContext+A830S3Cb2);
    NativeAdrenoGmuPlan plan={};
    if((r=A830GmuFirmware::plan(d->gmu_blocks,NativeAdrenoGmuPageBytes,&plan))!=kIOReturnSuccess)
        return refuse(f,A830S3StepContext+A830S3Cb2,A830ReasonFirmware,0,r,0,r);
    if((r=d->gmu_memory.allocate(plan,oas?oas:40))!=kIOReturnSuccess)
        return refuse(f,A830S3StepContext+A830S3Cb2,A830ReasonNoMemory,plan.count,r,plan.cache_bytes,r);
    const NativeAdrenoGmuContextConfig gmu={d->smmu.programmer(),b->cb_gmu,unsigned(b->cb_gmu)+1u,survey.slot_gmu,b->sid_gmu,0,
                                            gmuMap,nullptr};
    if((r=d->gmu_context.bringUp(gmu,nullptr,&d->gmu_report))!=kIOReturnSuccess)
        return contextFailure(f,d->gmu_report,A830S3Cb2,r,NativeAdrenoGmuContext::Program,NativeAdrenoGmuContext::Attach);
    /* The GPU context (CB0, slot 0, SID 0, ASID 1) with the BO pool reserved first, the TTBR1 guard, the LPAC stream (SID 1
     * on slot 3, before CP_SQE_CNTL = 1, 4.5.5) and the kernel buffers of 4.6.2. */
    adreno830_status_step(A830S3StepContext+A830S3Cb0);
    NativeAdrenoGpuContextConfig gpu={};
    gpu.programmer=d->smmu.programmer();gpu.bank=b->cb_gpu;gpu.asid=unsigned(b->cb_gpu)+1u;
    gpu.slot=survey.slot_gpu;gpu.lpac_slot=survey.slot_lpac;gpu.sid=b->sid_gpu;gpu.lpac_sid=b->sid_lpac;gpu.output_bits=0;
    gpu.aperture_gib=b->knobs.va;gpu.pool_mib=paged?0:b->pool_mib;gpu.write_combined_pool=true;
    gpu.paged=paged;
    gpu.cap_bytes=uint64_t(__atomic_load_n(&native_adreno_gpu_mem_cap_mib,__ATOMIC_ACQUIRE))<<20;
    gpu.reserve_bytes=uint64_t(__atomic_load_n(&native_adreno_gpu_mem_reserve_mib,__ATOMIC_ACQUIRE))<<20;
    if((r=d->gpu_context.bringUp(gpu,nullptr,&d->gpu_report))!=kIOReturnSuccess) {
        const uint16_t bank=d->gpu_report.step==NativeAdrenoGpuContext::Lpac?A830S3Lpac:A830S3Cb0;
        return contextFailure(f,d->gpu_report,bank,r,NativeAdrenoGpuContext::Program,NativeAdrenoGpuContext::Lpac);
    }
    /* The aperture the context built, in GiB, and its table pages (hex, as every trace value): pagani-gpu-va=16 falls back to 4 GiB
     * without a 34-bit UBS or the 32 MiB of tables. In both profiles: only the design profile's CB0 TCR (T0SZ 30) shows it otherwise,
     * the annibale profile writes T0SZ 25 for any aperture. */
    traceNote("gpu aperture GiB, table pages",2,d->gpu_context.apertureBytes()>>30,d->gpu_report.table_pages);
    if((r=d->gpu_context.buffers(d->buffers))!=kIOReturnSuccess)
        return refuse(f,A830S3StepContext+A830S3Cb0,A830ReasonNoMemory,NativeAdrenoGpuContext::Ready,r,0,r);
    /* 4.5.6: CP_SMMU_APERTURE for CB0, recorded only: on the S3 page (" AP<state>"), in one note per boot ("gpu aperture <state>
     * x0=<x0>"; the SCM lines of the trace go round the events ring long before anybody reads them) and on the meter line (M14 P0). */
    adreno830_status_step(A830S3StepAperture);
    d->smmu.grantAperture(b->cb_gpu);
    d->smmu.fillStatus(&s);
    int64_t x0=0;
    const uint32_t aperture=d->smmu.apertureState(&x0);
    adreno830_status_aperture(aperture);
    traceAperture(aperture,x0);
    return kIOReturnSuccess;
}

/* ===== S4: firmware memory =========================================================================================== */
void bindGmuServices(Adreno830Device *d)
{
    d->gmu_mmio={};
    const unsigned ids[A830GmuWindows]={A830WinGmu,A830WinGmuAo,A830WinRscc,A830WinGpu,A830WinCxMisc};
    for(unsigned i=0;i<A830GmuWindows;++i) {d->gmu_mmio.base[i]=d->win[ids[i]].base;d->gmu_mmio.bytes[i]=uint32_t(d->win[ids[i]].bytes);}
    A830GmuBindMmio(&d->gmu_mmio,&d->gmu_io);
    traceBindGmu(d);
    d->gmu_os={nullptr,gmuNow,gmuSleep,gmuDelay,gmuCounter,gmuIrqDisable,gmuIrqRestore};
    d->gmu_irq_ops={spiAttach,spiDisable,spiEnable,gmuWorkAllocate,gmuWorkKick};
    d->cx_timer.io=&d->gmu_io;d->cx_timer.os=&d->gmu_os;
}
IOReturn stageFirmware(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    Adreno830Status &s=adreno830_status;
    bindGmuServices(d);
    /* 4.4.2: the CX always-on counter seeded from the CPU counter, before the GMU firmware is loaded; once per boot (a830CxTimerSeed
     * is a no-op once seeded, so a recovery with CX up never reseeds it). */
    adreno830_status_step(A830S4StepTimer);
    IOReturn r;
    {   /* the seed times its own AO_COUNTER writes with interrupts off: no lines inside */
        TraceQuiet quiet("cx timer seed: AO_COUNTER 3d9e200/204 writes not logged");
        r=a830CxTimerSeed(d->cx_timer);
    }
    traceNote("cx timer seed",2,uint32_t(r),d->cx_timer.seeds);
    if(r!=kIOReturnSuccess)return refuse(f,A830S4StepTimer,A830ReasonUnexpectedState,A830_CX_MISC_PHYS+0x200,r,0,r);
    s.aux[0]=d->cx_timer.seed;s.aux[1]=d->cx_timer.skew;
    /* The DDR blocks of the GMU image into the regions CB2 maps; the log and HFI regions zeroed (the persistent one only at
     * allocation). The TCM blocks follow at B4 of every cold boot. */
    adreno830_status_step(A830S4StepGmuFirmware);
    A830GmuLoadCheck check={};
    if((r=A830GmuFirmware::load(d->gmu_blocks,d->gmu_io,d->gmu_memory,A830GmuLoadDdr,&check))!=kIOReturnSuccess) {
        /* The annibale profile compares every DDR block with the image (section 14 row 16): the word that differed. */
        if(check.mismatch || check.read_failed)
            return refuse(f,A830S4StepGmuFirmware,A830ReasonReadback,check.addr,check.read,check.expect,r);
        return refuse(f,A830S4StepGmuFirmware,r==kIOReturnBadArgument?A830ReasonFirmware:r==kIOReturnNotReady?A830ReasonNoMemory:
                      A830ReasonReadback,0,r,0,r);
    }
    d->gmu_memory.clear(1u<<NativeAdrenoGmuRegionLog|1u<<NativeAdrenoGmuRegionHfi);
    uint32_t tcm=0;
    for(uint32_t i=d->gmu_blocks.metadata;i<d->gmu_blocks.count;++i)
        if(d->gmu_blocks.block[i].target!=A830GmuTargetDdr)tcm+=d->gmu_blocks.block[i].size;
    s.aux[2]=d->gmu_blocks.count|uint64_t(d->gmu_memory.layout().cache_bytes)<<32;s.aux[4]=tcm;
    /* SQE and AQE without their first dword into the GPU buffers at 0x100000 / 0x180000 (4.9). */
    size_t sqe=0,aqe=0;
    adreno830_status_step(A830S4StepSqe);
    const NativeAdrenoGpuBuffer &sqeBuffer=d->buffers[GpuBufferSqe],&aqeBuffer=d->buffers[GpuBufferAqe];
    if((r=A830Cp::stageMicrocode(b->sqe_fw.bytes,b->sqe_fw.length,const_cast<void *>(sqeBuffer.cpu),size_t(sqeBuffer.bytes),&sqe))
       !=kIOReturnSuccess)
        return refuse(f,A830S4StepSqe,A830ReasonFirmware,sqeBuffer.iova,b->sqe_fw.length,sqeBuffer.bytes,r);
    adreno830_status_step(A830S4StepAqe);
    if((r=A830Cp::stageMicrocode(b->aqe_fw.bytes,b->aqe_fw.length,const_cast<void *>(aqeBuffer.cpu),size_t(aqeBuffer.bytes),&aqe))
       !=kIOReturnSuccess)
        return refuse(f,A830S4StepAqe,A830ReasonFirmware,aqeBuffer.iova,b->aqe_fw.length,aqeBuffer.bytes,r);
    s.aux[3]=sqe|uint64_t(aqe)<<32;
    /* The ring and memptrs (cleared by the ring object), and a zero power-up list page (filled after the ZAP, 4.11 step 10). */
    adreno830_status_step(A830S4StepBuffers);
    d->ring_io={nullptr,dwordWrite,dwordRead,dwordBarrier,nullptr,ahbFenceStatus,ringDelay};
    d->ring_cache={nullptr,nullptr,nullptr};
    d->clock={nullptr,clockNow,clockIdle};
    const NativeAdrenoGpuBuffer &ring=d->buffers[GpuBufferRing],&memptrs=d->buffers[GpuBufferMemptrs],&pwrup=d->buffers[GpuBufferPwrup];
    if(!d->ring.attached() &&
       (r=d->ring.attach(&d->ring_io,&d->ring_cache,const_cast<void *>(ring.cpu),size_t(ring.bytes),ring.iova,
                         const_cast<void *>(memptrs.cpu),size_t(memptrs.bytes),memptrs.iova))!=kIOReturnSuccess)
        return refuse(f,A830S4StepBuffers,A830ReasonArgument,ring.iova,r,0,r);
    memset(const_cast<void *>(pwrup.cpu),0,size_t(pwrup.bytes));
    __asm__ volatile("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}

/* ===== S5: GMU and HFI =============================================================================================== */
enum : uint32_t {AnnibaleFirstVoteMhz=160};   /* annibale profile: the frequency of the first GX_BW_PERF_VOTE (6.1) */
uint8_t busOfLevel(uint8_t level)
{
    const A830VoteTables &v=g830.votes;
    return level>=1 && level<=v.levels?v.level[level-1].bus:2;
}
/* The late HFI drain's counters (A830HfiStats late_*, since the boot) into hfi_late (A830StatusLate packing). */
void recordLate()
{
    const A830HfiStats &st=g830.hfi.stats();
    adreno830_status.hfi_late=A830StatusLate(st.late_drains,st.late_messages,st.late_acks,st.late_errors);
}
/* annibale (field-notes 9-1, K/redwood_gpu_dispatch.c:166-178): the GMU often reports a failure after the driver stopped
 * waiting, so before a failure of S6-S8 or a hang verdict is written, its msg and dbg queues are read for 3 s more
 * (A830Hfi::drainLate; the design profile returns at once). Only while the GMU runs: one forced off or in reset sends nothing
 * (and after a CX collapse its registers are gone). A failed HFI command of S5 has drained itself. What it found goes to
 * hfi_late, which the S5, S6/S7 and S8 pages show. */
void drainHfiLate()
{
    if(g830.hfi_initialized && g830.gmu.booted())(void)g830.hfi.drainLate();
    recordLate();
}
/* The S5 page (Adreno830Status.h) and the GMU's global words: the annibale fields (TCM words read back at B4, GENERAL_11
 * before B6, RSC_STATUS0_DRV0 at B3) stay 0 in the design profile; the AO status is the sticky word the page shows. */
void recordGmu()
{
    Adreno830Status &s=adreno830_status;
    const A830GmuReport &r=g830.gmu.report();
    const A830Hfi &h=g830.hfi;
    auto u16=[](uint32_t v) {return uint64_t(v<0xffffu?v:0xffffu);};
    s.aux[0]=r.core|uint64_t(r.hfi)<<32;
    s.aux[1]=r.init_result|u16(r.init_us/1000)<<32|u16(r.tcm_verified)<<48;
    s.aux[2]=h.status(A830HfiM1Perf)|uint64_t(h.status(A830HfiM2Bw))<<16|uint64_t(h.status(A830HfiM6CoreFwStart))<<32|
             uint64_t(h.status(A830HfiM8Start))<<48;
    s.aux[3]=h.status(A830HfiM9Vote)|uint64_t(h.status(A830HfiM5Ifpc))<<16|uint64_t(r.oob_us)<<32;
    s.aux[4]=r.gx_pwrclk|uint64_t(r.rpmh_state)<<32;
    s.aux[5]=r.rsc_status|uint64_t(r.info_last)<<32;
    s.gmu_general11=r.general11;
    adreno830_status_sticky(A830StickyGmuAo,r.ao_last);
    recordLate();
}
IOReturn gmuFailure(Adreno830Failure *f,IOReturn r)
{
    const A830GmuFailure &gf=g830.gmu.failure();
    return refuse(f,gf.step,gf.reason?gf.reason:A830ReasonTimeout,gf.reg,gf.value,gf.expect,r);
}
IOReturn hfiFailure(Adreno830Failure *f,IOReturn r)
{
    const A830HfiFailure &hf=g830.hfi.failure();
    return refuse(f,hf.step,hf.reason?hf.reason:A830ReasonHfiStatus,hf.hdr,hf.value,hf.expect,hf.ior?hf.ior:r);
}
IOReturn bindGmu(Adreno830Device *d,Adreno830Failure *f)
{
    if(d->gmu_bound)return kIOReturnSuccess;
    const Adreno830Board *b=d->board;
    if(!d->hfi_mutex)d->hfi_mutex=os().mutex_alloc?os().mutex_alloc():nullptr;
    if(!d->hfi_mutex)return refuse(f,A830S5StepBind,A830ReasonNoMemory,0,0,0,kIOReturnNoMemory);
    d->hfi_io={nullptr,hfiRead,hfiWrite,hfiSleep,hfiNow,hfiLock,hfiUnlock,hfiFaulted};
    d->hfi.setIo(d->hfi_io);
    d->gx_voter={};d->gx_voter.drv2=&d->drv2_io;d->gx_voter.res=&d->rpmh;
    d->gmu_hooks={nullptr,hookResetQueues,hfiLock,hfiUnlock,hookDrainDebug,hookGmuFault,A830GmuGxVote,&d->gx_voter};
    A830GmuConfig config={};
    A830GmuDefaultConfig(&config);
    config.diag=b->knobs.diag;config.hfi_irq=b->irq_hfi;config.gmu_irq=b->irq_gmu;
    const IOReturn r=d->gmu.bind(d->gmu_io,d->gmu_os,config,&d->gmu_blocks,&d->gmu_memory,&d->gmu_hooks);
    if(r!=kIOReturnSuccess)return refuse(f,A830S5StepBind,A830ReasonArgument,0,r,0,r);
    d->gmu_bound=true;
    return kIOReturnSuccess;
}
IOReturn hfiInit(Adreno830Device *d,A830HfiLayout layout,Adreno830Failure *f)
{
    NativeAdrenoMemory *shared=d->gmu_memory.hfi();
    const IOReturn r=shared && shared->mapping()?
        d->hfi.init(static_cast<volatile uint32_t *>(shared->mapping()),NativeAdrenoGmuHfiIova,layout):kIOReturnNotReady;
    if(r!=kIOReturnSuccess)return refuse(f,A830S5StepHfiInit,A830ReasonArgument,NativeAdrenoGmuHfiIova,r,0,r);
    d->hfi_initialized=true;
    return kIOReturnSuccess;
}
/* One GMU start after the firmware DDR blocks are in place: B1-B8, the HFI start messages (M9 = level/bus), the host DDR
 * floor once per boot, OOB_GPU and the GX power confirmation, the GMU-side counters. *fatal (when not null) says a retry
 * cannot help: a host resource request (reason 22) or the GX confirmation (no GX register may be touched, 4.7.8). */
IOReturn gmuStartOnce(Adreno830Device *d,uint8_t level,uint8_t bus,bool ifpc,Adreno830Failure *f,bool *fatal)
{
    const Adreno830Board *b=d->board;
    if(fatal)*fatal=false;
    uint32_t init=0;
    IOReturn r=d->gmu.coldBoot(&init);
    if(r!=kIOReturnSuccess)return gmuFailure(f,r);
    /* 336/337 after B8 (4.12.2, optional: a failure is in irq_off and the fatal bits are polled). In the annibale profile
     * A830Gmu attaches neither and polls both (section 14 row 18). */
    if(!d->gmu.interruptsWired() && !d->gmu.interruptsOff()) {
        adreno830_status_step(A830S5StepIrqs);
        d->gmu.attachInterrupts(d->gmu_irq_ops);
    }
    A830HfiStartOptions options={};
    options.flags=A830HfiStartDefault|((b->knobs.hfi&2u)?A830HfiStartDeferStart:0u)|(ifpc?A830HfiStartIfpc:0u)|
                  (d->acd_ready && d->votes.acd_levels?A830HfiStartAcd:0u);
    options.level=level;options.bus=bus;options.ifpc_hysteresis=0x1680;options.hfi_version=d->gmu.report().hfi;
    adreno830_status_step(A830S5StepM1);
    if((r=d->hfi.start(d->votes,options))!=kIOReturnSuccess) {
        if(fatal)*fatal=d->hfi.failure().reason==A830ReasonGmuHostResource;
        return hfiFailure(f,r);
    }
    d->acd_running=(options.flags&A830HfiStartAcd)!=0;
    /* After the first GX_BW_PERF_VOTE answer: the host DDR floor (default = the boot level: nothing is sent), then the
     * software fuse once more (4.3.3, 4.7.6). */
    if(!d->floor_done) {
        adreno830_status_step(A830S5StepDdrFloor);
        if(b->knobs.ddr_floor<b->knobs.ddr_boot) {
            const int rc=Sm8750RpmhHostDdr(&d->host,adreno830_ddr_kbps(b->knobs.ddr_floor));
            if(rc!=Sm8750RpmhOk) {
                if(fatal)*fatal=true;
                return refuse(f,A830S5StepDdrFloor,A830ReasonRpmh,d->host.last.step,uint64_t(int64_t(rc)),
                              adreno830_ddr_kbps(b->knobs.ddr_floor),kIOReturnIOError);
            }
        }
        (void)dwordOr(A8XX_CX_MISC_SW_FUSE_VALUE,0);
        d->floor_done=true;
    }
    adreno830_status_step(A830S5StepOob);
    uint32_t ack=0;
    if((r=d->gmu.oobSet(&ack))!=kIOReturnSuccess)return gmuFailure(f,r);
    if(b->knobs.hfi&2u) {   /* KGSL non-hwsched order: START after the OOB (pagani-gpu-hfi bit1) */
        adreno830_status_step(A830S5StepM1+7);
        if((r=d->hfi.sendStart())!=kIOReturnSuccess)return hfiFailure(f,r);
    }
    adreno830_status_step(A830S5StepGx);
    if((r=d->gmu.waitGxOn(A830_T_GX_ON_US))!=kIOReturnSuccess) {
        if(fatal)*fatal=true;
        return gmuFailure(f,r);
    }
    adreno830_status_step(A830S5StepCounters);
    if((r=d->gmu.writeCounters())!=kIOReturnSuccess) {
        if(fatal)*fatal=true;
        return gmuFailure(f,r);
    }
    return kIOReturnSuccess;
}
/* gpu/fullopp1: tell the AOP the ACD state again (val 0 once the GMU refused the ACD table), with S1's lock protocol. */
int resendAcdState(Adreno830Device *d,int value)
{
    if(!mapped(A830WinAossMsgram) || !mapped(A830WinIpcc))return -1;
    bool locked=false;
    for(unsigned waited=0;waited<3000 && !locked;++waited) {
        uint32_t expected=0;
        locked=__atomic_compare_exchange_n(&sm8750_aoss_qmp_owner,&expected,uint32_t(Sm8750RpmhLockOwnerGpu),false,__ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED);
        if(!locked)qmpDelay(nullptr,1000);
    }
    const int rc=Sm8750AossQmpSendAcdState(&d->qmp,value);
    if(locked) {
        uint32_t expected=uint32_t(Sm8750RpmhLockOwnerGpu);
        __atomic_compare_exchange_n(&sm8750_aoss_qmp_owner,&expected,0u,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    return rc;
}
IOReturn stageGmu(Adreno830Device *d,Adreno830Failure *f)
{
    Adreno830Status &s=adreno830_status;
    IOReturn r=bindGmu(d,f);
    if(r!=kIOReturnSuccess)return r;
    /* The HFI queues are built while the GMU is held in reset (4.7.5). */
    adreno830_status_step(A830S5StepHfiInit);
    A830HfiLayout layout=A830HfiLayoutLinux;
    if((r=hfiInit(d,layout,f))!=kIOReturnSuccess)return r;
    /* The level of the first GX_BW_PERF_VOTE (HFI M9): the board's initial level (design), or the 160 MHz level every K90
     * bin starts from (annibale: {1, 1, 2}, level 1 and its DDR level 2 on PKX110; kernel-sequences 6.1, section 14 rows 2
     * and 24). The DCVS starts from it in both profiles. */
    d->level=d->votes.initial?d->votes.initial:2;
    if(adreno830_annibale()) {
        const uint8_t slowest=A830Votes::levelAtOrAbove(d->votes,AnnibaleFirstVoteMhz);
        d->level=slowest?slowest:1;
    }
    d->bus=busOfLevel(d->level);
    /* B1-B8, M1..M9, OOB, GX: at most three cold boots with a forced power-off between them (4.7.3); the last failure keeps
     * its state for the diagnosis. */
    for(uint16_t attempt=1;;++attempt) {
        s.attempts=attempt;
        bool fatal=false;
        *f={};
        r=gmuStartOnce(d,d->level,d->bus,false,f,&fatal);
        recordGmu();
        if(r==kIOReturnSuccess)return kIOReturnSuccess;
        /* gpu/fullopp1: the ACD feature or table refused (HFI M3, step 0x13): the next attempt starts without ACD, so the GPU runs
         * under the DCVS's ACD-off 734 MHz cap instead of not at all (ANALYSIS.md 3.6 R1). */
        if(!fatal && d->acd_ready && d->board->qualification_fullopp==1 && d->hfi.failure().step==A830S5StepM1+2) {
            d->acd_ready=false;
            const int rc=resendAcdState(d,0);
            qualLog("R3b ACD refused at HFI M3 (attempt %u, value %#x): AOP told val 0 (rc %d), the next GMU start runs without ACD "
                    "(cap 734 MHz)",unsigned(attempt),unsigned(d->hfi.failure().value),rc);
        }
        if(fatal || attempt>=3)return r;
        /* Not one acknowledgement with the Linux queue layout: the KGSL layout on the next attempt (4.7.5). */
        const bool kgsl=layout==A830HfiLayoutLinux && f->step>=A830S5StepM1 && f->step<=A830S5StepM9 && !d->hfi.firstAckSeen();
        d->gmu.forceOff();
        d->hfi.stop();
        if(kgsl) {
            layout=A830HfiLayoutKgsl;
            Adreno830Failure ignored={};
            hfiInit(d,layout,&ignored);
        }
    }
}

/* ===== S6: command processor ========================================================================================= */
IOReturn cpFailure(Adreno830Failure *f,IOReturn r)
{
    const A830CpFailure &cf=g830.cp.failure();
    return refuse(f,cf.step,cf.reason?cf.reason:A830ReasonCpFault,cf.reg,cf.value,cf.expect,r);
}
/* A830CpHealth, the annibale profile's checks that need other modules (kernel-sequences 9.7 steps 5, 6, 9 and 10; A830Cp
 * calls neither with the MMIO lock held, the design profile never):
 *  - global_fault: sGFSR read through the SMMU window, never written back (the hypervisor emulates the global space and
 *    A830Smmu never stores it in this profile either); 0 on a completed read;
 *  - gmu_fault: one HFI-style poll as annibale makes in every round of its waits, GMU2HOST_INTR_INFO bit 15 (CM3 fault) or an
 *    NMI bit of CM3_FW_INIT_RESULT, read only (the interrupt bits stay for the HFI and pollFatal; 0xFFFFFFFF is no answer, as
 *    A830Gmu reads it), else a fatal bit a GMU top half or pollFatal already took (A830Gmu::fatalSeen). The nonzero result is
 *    the value the CP failure records: the INFO or INIT_RESULT word, 1 for the earlier fatal. */
int cpGlobalFault(void *,uint32_t *sgfsr)
{
    return g830.smmu_io.read32?g830.smmu_io.read32(g830.smmu_io.cookie,A830_SMMU_GR0_SGFSR,sgfsr):1;
}
int cpGmuFault(void *)
{
    uint32_t info=0,init=0;
    if(readDword(A8XX_GMUCX_GMU2HOST_INTR_INFO,&info) && info!=0xffffffffu && (info&A830_GMU2HOST_CM3_FAULT))
        return static_cast<int>(info);
    if(readDword(A8XX_GMUCX_CM3_FW_INIT_RESULT,&init) && init!=0xffffffffu && (init&A830_GMU_FW_INIT_NMI_MASK))
        return static_cast<int>(init);
    return g830.gmu.fatalSeen()?1:0;
}
IOReturn bindCp(Adreno830Device *d,Adreno830Failure *f)
{
    if(d->cp.attached())return kIOReturnSuccess;
    if(!d->mmio_mutex)d->mmio_mutex=os().mutex_alloc?os().mutex_alloc():nullptr;
    d->cp_io={nullptr,dwordWrite,dwordRead,dwordBarrier,cpSleep,cpDelay,cpNow,d->mmio_mutex?mmioLock:nullptr,
              d->mmio_mutex?mmioUnlock:nullptr};
    d->cp_smmu={nullptr,cpLpacRoute,cpRecordSticky};
    const IOReturn r=d->cp.attach(&d->cp_io,&d->ring,&d->cp_smmu);
    if(r!=kIOReturnSuccess)return refuse(f,A830S6StepAttach,A830ReasonArgument,0,r,0,r);
    const A830CpHealth health={nullptr,cpGlobalFault,cpGmuFault};
    d->cp.setHealth(&health);
    return kIOReturnSuccess;
}
/* hw_init, microcode bases, ring, SQE start and CP_ME_INIT (4.8, 4.9): the idle criterion within 2 s in the design profile;
 * annibale's criterion and its gates within 2000 polls of 100 us, 200 ms (kernel-sequences 9.7 step 9, section 14 row 33). */
IOReturn cpStartOnce(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    A830HwInitConfig config=A830HwInitDefaults();
    config.ubwc_mode=b->ubwc_mode;config.highest_bank_bit=b->highest_bank_bit;config.min_access_length=b->min_access_length;
    config.gmem_base=A830_GMEM_BASE;config.uche_trap_base=A830_UCHE_TRAP_ADDR;config.llcc_scid=d->llcc_scid;
    IOReturn r=d->cp.hwInit(config);
    if(r!=kIOReturnSuccess)return cpFailure(f,r);
    if((r=d->cp.loadMicrocode(GpuSqeIova,GpuAqeIova))!=kIOReturnSuccess)return cpFailure(f,r);
    const NativeAdrenoGpuBuffer &ring=d->buffers[GpuBufferRing],&memptrs=d->buffers[GpuBufferMemptrs];
    if((r=d->cp.startRing(ring.iova,ring.bytes,memptrs.iova))!=kIOReturnSuccess)return cpFailure(f,r);
    uint32_t fence=0;
    if((r=d->cp.startCp(GpuPwrupIova,&fence))!=kIOReturnSuccess) {
        d->cp.dumpStall(&adreno830_status);
        return cpFailure(f,r);
    }
    return kIOReturnSuccess;
}
IOReturn stageCp(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    /* M11: the LLCC slices 9 (GPU) and 11 (GPUHTW), pagani-gpu-llcc=1 only; any failure leaves LLCC off (4.15). */
    d->llcc_scid=0;
    if(b->knobs.llcc && mapped(A830WinLlcc0) && mapped(A830WinLlccOr) && mapped(A830WinLlccAnd)) {
        adreno830_status_step(A830S6StepLlcc);
        d->llcc_io={nullptr,llccRead,llccWrite,llccDelay};
        if(Sm8750LlccProbe(&d->llcc_io,&d->llcc)==Sm8750LlccOk && Sm8750LlccEnableGpu(&d->llcc_io,&d->llcc)==Sm8750LlccOk) {
            const int scid=Sm8750LlccGpuScid(&d->llcc);
            d->llcc_scid=scid>0?uint32_t(scid):0;
        }
    }
    IOReturn r=bindCp(d,f);
    if(r!=kIOReturnSuccess)return r;
    adreno830_status_step(A830CpStepClearHalt);
    if((r=cpStartOnce(d,f))!=kIOReturnSuccess)return r;
    d->cp.dumpStall(&adreno830_status);   /* the S6 page: RPTR/WPTR, RBBM, SQE PCs (record) */
    return kIOReturnSuccess;
}

/* ===== S7: ZAP and secure exit ======================================================================================= */
IOReturn stageZap(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    adreno830_status_step(A830S7StepZapInit);
#ifndef ADRENO830_HOST_TEST
    const A830ZapMemoryOps *memory=A830ZapKernelMemory();
#else
    const A830ZapMemoryOps *memory=nullptr;
#endif
    IOReturn r=d->zap.init(&d->scm,memory);
    if(r!=kIOReturnSuccess)return refuse(f,A830S7StepZapInit,A830ReasonArgument,0,r,0,r);
    /* 4.11 steps 1-7: MDT checks, metadata, PAS_INIT_IMAGE, MEM_SETUP, segment copy, AUTH_AND_RESET; once per boot. */
    adreno830_status_step(A830ZapStepParse);
#ifdef ADRENO830_MACOS
    adreno830_board_scm_hold(1);   /* UniXNU-piano: PAS 13's init_image .. auth_and_reset, one secure-call critical section */
#endif
    r=d->zap.load(b->zap_fw.bytes,size_t(b->zap_fw.length),b->zap.phys,b->zap.bytes);
#ifdef ADRENO830_MACOS
    adreno830_board_scm_hold(0);
#endif
    d->zap.noteStatus();
    if(r!=kIOReturnSuccess) {
        A830ZapFailure zf={};
        d->zap.failure(&zf);
        return refuse(f,zf.step,zf.reason?zf.reason:A830ReasonSmc,zf.reg,zf.value,zf.expect,r);
    }
    /* Steps 8-11: CP_SET_SECURE_MODE 0 and idle, RB_GC_GMEM_PROTECT, the power-up list (first time), HWCG. */
    adreno830_status_step(A830ZapStepSecureMode);
    if((r=d->cp.finishSecure(const_cast<void *>(d->buffers[GpuBufferPwrup].cpu),A830_T_CP_IDLE_US))!=kIOReturnSuccess) {
        d->cp.dumpStall(&adreno830_status);
        return cpFailure(f,r);
    }
    /* Step 12: OOB_GPU cleared (IFPC off: GX stays powered, 4.7.8). The annibale profile keeps the OOB_GPU of S5 set until
     * the next cold boot, as the K90 port does (kernel-sequences 6.2, section 14 row 28: first round, clearing is a later
     * single variable). */
    if(!adreno830_annibale()) {
        adreno830_status_step(A830ZapStepOobClear);
        d->gmu.oobClear();
    }
    d->zap.noteStatus();
    return kIOReturnSuccess;
}

/* ===== S8: interrupts, fences, coherence ============================================================================= */
uint8_t faultCause(uint8_t kind)
{
    switch(kind) {
    case A830IrqFaultHang: return A830FaultHang;
    case A830IrqFaultCpHwError: return A830FaultCpHwError;
    case A830IrqFaultWatchdog: return A830FaultWatchdog;
    case A830IrqFaultGmu: return A830FaultGmu;
    case A830FaultWptr: return A830FaultWptrFence;
    case A830IrqFaultSelfTest: return A830FaultSelfTest;
    default: return A830FaultHfi;
    }
}
void irqDeviceLost(void *,const A830FaultReport *report)
{
    g830.recovery.report(faultCause(report->kind),report->detail);
}
void hookGmuFault(void *,uint16_t reason,uint32_t info,uint32_t ao)
{
    const uint8_t cause=(ao&1u) && reason!=A830ReasonF2hError?A830FaultGmuWatchdog:A830FaultGmu;
    g830.recovery.report(cause,uint64_t(info)|uint64_t(ao)<<32);
}
void irqRetire(void *,uint32_t) {native_adreno_retire_completed();}
void irqSmmuSnapshot(void *,uint32_t *fsr,uint64_t *far)
{
    const uint64_t cb=g830.smmu.contextOffset(g830.board->cb_gpu);
    *fsr=smmuRead(uint32_t(cb+A830_SMMU_CB_FSR));
    *far=smmuFar(cb);
}
void irqErrorNoted(void *,uint32_t) {}
/* The self-test's view of the GPU address space (V4 and V9, 4.14.2): one cacheable probe page at a time, mapped at a fixed
 * kernel address below the GMEM window. */
enum : uint64_t {ProbeIova=0x00310000};
IOReturn probeAllocate(void *,A830SelfTestPage *page)
{
    for(auto &memory:g830.probe)
        if(!memory.backed()) {
            const unsigned bits=g830.gpu_context.outputBits();
            const IOReturn r=memory.allocate(NativeAdrenoGmuPageBytes,bits?bits:40,true,NativeAdrenoGmuPageBytes);
            if(r!=kIOReturnSuccess)return r;
            page->cpu=static_cast<volatile uint32_t *>(memory.mapping());
            page->physical=memory.spans()?memory.spans()[0].physical:0;
            page->owner=&memory;
            return page->cpu?kIOReturnSuccess:kIOReturnNoMemory;
        }
    return kIOReturnNoResources;
}
void probeRelease(void *,A830SelfTestPage *page)
{
    if(page->owner)static_cast<NativeAdrenoMemory *>(page->owner)->release();
    page->owner=nullptr;page->cpu=nullptr;
}
IOReturn probeMap(void *,A830SelfTestPage *page,unsigned mode)
{
    NativeAdrenoVm *vm=g830.gpu_context.vm();
    if(!vm || !page->owner)return kIOReturnNotReady;
    unsigned permissions=ArmSmmu4KRead,writeback=0,shareability=2;
    if(mode==A830SelfTestMapCoherentRo) {writeback=1;shareability=3;}
    else if(mode==A830SelfTestMapCoherentRw) {permissions|=ArmSmmu4KWrite;writeback=1;shareability=3;}
    const bool walk=vm->tableWalkCoherent();
    if(mode==A830SelfTestMapCachedTable)vm->setTableWalkCoherent(true);   /* V4: the new leaf with dsb ishst only */
    page->iova=ProbeIova+uint64_t(mode)*NativeAdrenoGmuPageBytes;
    const IOReturn r=vm->map(static_cast<NativeAdrenoMemory *>(page->owner),0,page->iova,permissions,writeback,shareability,
                             &page->handle);
    if(mode==A830SelfTestMapCachedTable)vm->setTableWalkCoherent(walk);
    return r;
}
IOReturn probeUnmap(void *,A830SelfTestPage *page)
{
    NativeAdrenoVm *vm=g830.gpu_context.vm();
    return vm?vm->unmap(page->handle):kIOReturnNotReady;
}
void probeMaintain(void *,A830SelfTestPage *page,unsigned op)
{
    const NativeAdrenoCacheOperation operation=op==A830SelfTestClean?NativeAdrenoClean:
        op==A830SelfTestInvalidate?NativeAdrenoInvalidate:NativeAdrenoCleanInvalidate;
    NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(page->cpu),NativeAdrenoGmuPageBytes,operation);
}
uint32_t probeFaults(void *,uint64_t *far)
{
    const uint64_t cb=g830.smmu.contextOffset(g830.board->cb_gpu);
    const uint32_t fsr=smmuRead(uint32_t(cb+A830_SMMU_CB_FSR))&A830_SMMU_FSR_FAULT_MASK;
    if(fsr) {
        if(far)*far=smmuFar(cb);
        smmuWrite(uint32_t(cb+A830_SMMU_CB_FSR),fsr);
        adreno830_status_sticky(A830StickyCb0Fsr,fsr);
    }
    return fsr;
}
void probePolicy(void *,const A830CoherenceResult *result)
{
    NativeAdrenoVm::setCoherence(result->level);
    adreno830_status.coh_limit=result->limit;
    /* M10 (4.6.1): a proved coherent table walk publishes page-table changes with dsb ishst only. */
    if(result->tables_cacheable && g830.gpu_context.vm())g830.gpu_context.vm()->setTableWalkCoherent(true);
}
IOReturn ringSubmit(void *ring,const uint32_t *words,size_t count,uint32_t *fence)
{
    return static_cast<NativeAdrenoRing *>(ring)->submit(words,count,fence);
}
uint64_t aoCounter(void *timer)
{
    uint64_t ticks=0;
    return static_cast<NativeAdrenoTimer *>(timer)->read(&ticks)==kIOReturnSuccess?ticks:0;
}
IOReturn selfTestRecover(void *,uint16_t)
{
    return g830.recovery.recover(A830FaultSelfTest,0);
}
/* S8 of the annibale profile (kernel-sequences section 14 row 36, first round): 332 is not attached (the K90 port wires no
 * GPU, GMU or SMMU interrupt, and A830Cp leaves RBBM_INT_0_MASK at 0 there), so it cannot be a gate. This is
 * A830SelfTest::run without its attach step: CP_MEM_WRITE passes on the fence its waiter polls (16 ms slice) and on the
 * scratch read-back, while its interrupt criteria (count + 1, woken by the interrupt) are only recorded; the counters and
 * the coherence checks follow as in run(). Attaching 332 is the later single variable: pagani-gpu-irq=1 attaches it first
 * (A830Irq unmasks the fence interrupt only, own_mask) and keeps it when the CP_MEM_WRITE check also met the interrupt
 * criteria; a refused attach (irq_off bit 0 already set) or missed criteria (A830Irq::fallBack) leave the polled fences of
 * the first round, and S8 goes on either way. */
IOReturn selfTestPolled(Adreno830Device *d,A830SelfTest &test,const A830SelfTestEnv &env,A830SelfTestFailure *failure)
{
    A830SelfTestReport &report=d->selftest;
    report={};*failure={};
    if(d->board->knobs.irq) {
        adreno830_status_step(A830S8StepAttach);
        report.attach_result=uint32_t(d->irq.attach(d->board->irq_gpu));
    }
    const bool live=d->irq.attached() && !d->irq.off();
    IOReturn r=test.memWrite(env.scratch_iova+A830SelfTestScratchMemWrite,env.scratch+A830SelfTestScratchMemWrite/4,
                             A830SelfTestScratchValue,&report,failure);
    if(r!=kIOReturnSuccess && (failure->step==A830S8StepIrq || failure->step==A830S8StepWake)) {
        if(live)d->irq.fallBack();   /* attached, but its fence interrupt did not arrive or did not wake the waiter */
        r=kIOReturnSuccess;*failure={};
    }
    test.timestamps(&report.cp_counter,&report.ao_counter);
    if(r==kIOReturnSuccess)r=test.coherence(env.cohtest,&report.coherence,failure,&report.recovered);
    else {report.coherence.limit=A830CoherenceOff;report.coherence.level=A830CoherenceOff;}
    A830SelfTest::record(report);
    a830_selftest_last=report;
    return r;
}
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
struct TierProbe {
    Adreno830Device *device;
    bool blocked=false;
};
// Persistent storage in case a poisoned VM retains the probe's mapping reference.
NativeAdrenoMemory tierProbePage;
IOMemoryDescriptor *tierProbeDescriptor;

bool tierProbe(uint64_t pa,unsigned run,void *cookie)
{
    auto &probe=*static_cast<TierProbe *>(cookie);
    if(probe.blocked)return false;
    auto &d=*probe.device;
    auto *vm=d.gpu_context.vm();
    if(!vm)return false;

    constexpr uint64_t iova=ProbeIova+0x10000;
    constexpr uint64_t pageBytes=NativeAdrenoGmuPageBytes;
    IOAddressRange range={pa,pageBytes};
    IOMemoryDescriptor *descriptor=IOMemoryDescriptor::withAddressRanges(&range,1,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryTypePhysical64)|kIOMemoryMapperNone,
        nullptr);
    if(!descriptor)return false;
    IOMemoryMap *view=descriptor->createMappingInTask(kernel_task,0,kIOMapAnywhere|kIOMapWriteCombineCache);
    if(!view) {
        descriptor->release();
        return false;
    }

    // Give the two endpoints different words, including for a run consisting of a single slab.
    const uint32_t value=0xa8300000u | (run<<12) | uint32_t((pa>>12)&0x1ff);
    auto *word=reinterpret_cast<volatile uint32_t *>(view->getAddress());
    *word=~value;
    __asm__ volatile("dsb sy":::"memory");
    (void)probeFaults(nullptr,nullptr);

    uint64_t handle=0;
    IOReturn result=tierProbePage.importDevice(descriptor,0,pageBytes);
    const char *stage="backing";
    if(!result) {
        stage="map";
        result=vm->map(&tierProbePage,0,iova,ArmSmmu4KRead|ArmSmmu4KWrite,0,2,&handle);
    }
    bool quiesced=true;
    uint32_t fault=0,viewWord=0;
    if(!result) {
        uint32_t words[4], fence=0;
        const size_t count=A830SelfTest::memWriteWords(iova,value,words,4);
        stage="submit";
        result=d.ring.submit(words,count,&fence);
        if(!result) {
            stage="fence";
            result=d.fence.waitNs(fence,250000000);
        }
        fault=probeFaults(nullptr,nullptr);
        if(result && (fence || d.ring.poisoned()))
            quiesced=d.recovery.recover(A830FaultSelfTest,pa)==kIOReturnSuccess;
        if(!result) {
            __asm__ volatile("dsb sy":::"memory");
            viewWord=*word;
        }
    }

    view->release();
    uint32_t seen=0;
    if(!result) {
        stage="readback";
        seen=ml_phys_read_word_64(pa);
        if(fault || seen!=value || viewWord!=value)result=kIOReturnIOError;
    }
    const IOReturn unmap=handle && quiesced?vm->unmap(handle):kIOReturnSuccess;
    if(!quiesced || unmap || tierProbePage.release()!=kIOReturnSuccess) {
        tierProbeDescriptor=descriptor;  // keep the descriptor alive while the VM might still reference the page
        probe.blocked=true;
    } else descriptor->release();
    if(result || probe.blocked)
        IOLog("PianoA830: gpu tier: run %u page %#llx %s result=%#x seen=%#x view=%#x expected=%#x fsr=%#x cleanup=%#x blocked=%u\n",
            run,(unsigned long long)pa,stage,result,seen,viewWord,value,fault,unmap,probe.blocked);
    return !result && !probe.blocked;
}
void tierProbeLine(const char *line,void *)
{
    IOLog("PianoA830: %s\n",line);
}
#endif

void bindPower(Adreno830Device *d);
IOReturn stageFence(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    adreno830_status_step(A830S8StepStart);
    A830IrqConfig irq={};
    irq.gpu=traceIrqWindow(0,A830WinGpu,a830MmioDirect(d->win[A830WinGpu].base));
    irq.gmu=traceIrqWindow(1,A830WinGmu,a830MmioDirect(d->win[A830WinGmu].base));
    irq.cx_misc=traceIrqWindow(2,A830WinCxMisc,a830MmioDirect(d->win[A830WinCxMisc].base));
    irq.platform=nullptr;irq.fence=&d->fence;
    irq.hooks={nullptr,d->mmio_mutex?mmioLock:nullptr,d->mmio_mutex?mmioUnlock:nullptr,irqRetire,irqDeviceLost,irqSmmuSnapshot,
               irqErrorNoted};
    /* annibale with pagani-gpu-irq=1: its hw_init masks every RBBM interrupt; A830Irq unmasks the fence one itself while 332
     * is attached (after the attach below, again after every re-initialization of the device side). */
    irq.own_mask=adreno830_annibale() && b->knobs.irq?uint32_t(A830_INT_MASK_FENCE_ONLY):0u;
    irq.softwareFaultSnapshot=b->qualification_r3b==1;
    IOReturn r=d->irq.start(irq);
    if(r!=kIOReturnSuccess)return refuse(f,A830S8StepStart,A830ReasonNoMemory,0,r,0,r);
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: piano-gpu-hang-ms (adreno830_board_hang_ms, 0: the 2000 ms default) for the fence watchdog, as for the recovery's. */
    const A830FenceConfig fence={static_cast<volatile uint32_t *>(const_cast<void *>(d->buffers[GpuBufferMemptrs].cpu)),&d->irq,
                                 0,adreno830_board_hang_ms(),0};
#else
    const A830FenceConfig fence={static_cast<volatile uint32_t *>(const_cast<void *>(d->buffers[GpuBufferMemptrs].cpu)),&d->irq,
                                 0,0,0};
#endif
    if((r=d->fence.start(fence))!=kIOReturnSuccess)return refuse(f,A830S8StepStart,A830ReasonNoMemory,1,r,0,r);
    /* The completion waits, and the queue meter's stamp of every fenced submission (ARCHITECTURE 4.13.5). */
    const NativeAdrenoRingWaiter waiter={&d->fence,A830Fence::ringWaiter,A830Fence::ringStamp};
    d->ring.setWaiter(&waiter);
    /* The GMU AO counter for GetTimestamp and the S8 counter comparison (1.3 #5). */
    adreno830_status_step(A830S8StepTimer);
    d->time_io={nullptr,timeRead};
    if(!d->timer.attached() && (r=d->timer.attach(&d->time_io,NativeAdrenoTimeGmu))!=kIOReturnSuccess)
        return refuse(f,A830S8StepTimer,A830ReasonArgument,b->gmu.phys+NativeAdrenoTimer::gmuLowOffset(),r,0,r);
    /* The minimal recovery (M10) exists from here on: a V9b/V9c timeout of a cohtest image recovers instead of stopping. */
    bindPower(d);
    /* 332 (the only hard interrupt dependency), CP_MEM_WRITE with an IRQ fence, the counters, V4 and V9a; V9b/V9c only with
     * pagani-gpu-cohtest=1 (4.14.2). The annibale profile leaves 332 out and runs the same checks on polled fences
     * (pagani-gpu-irq=1: 332 attached first, kept only when its fence interrupt works; selfTestPolled). */
    A830SelfTestEnv env={};
    env.irq=&d->irq;env.fence=&d->fence;env.ring=&d->ring;env.submit=ringSubmit;
    env.scratch=static_cast<volatile uint32_t *>(const_cast<void *>(d->buffers[GpuBufferScratch].cpu));
    env.scratch_iova=GpuScratchIova;
    env.vm={nullptr,probeAllocate,probeRelease,probeMap,probeUnmap,probeMaintain,probeFaults,probePolicy};
    env.timer=&d->timer;env.ao_counter=aoCounter;
    env.recovery=nullptr;env.recover=selfTestRecover;
    env.coherent=b->knobs.coherent;env.coherent_given=(b->flags&(1u<<Adreno830KnobCoherent))!=0;
    env.cohtest=b->knobs.cohtest!=0;env.timeout_us=0;
    A830SelfTest test(env);
    A830SelfTestFailure failure={};
    r=adreno830_annibale()?selfTestPolled(d,test,env,&failure):test.run(b->irq_gpu,&d->selftest,&failure);
    if(r!=kIOReturnSuccess)
        return refuse(f,failure.step,failure.reason?failure.reason:A830ReasonIrqWiring,failure.reg,failure.value,failure.expect,r);
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
    TierProbe tier{d};
    NativeAdrenoMemory::qualifyTier(d->gpu_context.outputBits(),native_adreno_wc_enabled,
        b->knobs.coherent==2 || d->selftest.coherence.level>=A830CoherenceFull,tierProbe,&tier,tierProbeLine);
    if(tier.blocked)
        return refuse(f,A830S8StepStart,A830ReasonSmmuFault,ProbeIova,0,0,kIOReturnIOError);
#endif
    /* 112 (GPU_CC frequency-limit events), optional. */
    adreno830_status_step(A830S8StepCxHost);
    d->irq.attachCxHost(b->irq_cx_host);
    return kIOReturnSuccess;
}

/* ===== S9: Metal service ============================================================================================= */
IOReturn stageMetal(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    Adreno830Status &s=adreno830_status;
    /* DeviceInfo of ARCHITECTURE 6.4. */
    adreno830_status_step(A830S9StepInfo);
    NativeAdrenoAbiDeviceInfo &info=d->info;
    info={};
    info.chip_id=b->chip_id;info.gmem_bytes=A830_GMEM_BYTES;info.gmem_base=A830_GMEM_BASE;info.uche_trap_base=A830_UCHE_TRAP_ADDR;
    info.va_start=A830_GPU_VA_CLIENT_BASE;
    /* The aperture the GPU context actually built (pagani-gpu-va=16 falls back to 4 GiB without a 34-bit UBS or its 32 MiB of tables):
     * the publish shell picks its client windows from this VA (NativeAdrenoClientWindowsFor). */
    const uint64_t aperture=d->gpu_context.apertureBytes();
    /* UniXNU-piano (GPU-SLOTS): above 16 GiB the 64 GiB aperture's end (pagani-gpu-va=64), whose table the shell then picks; gpu/slots2:
     * above 64 GiB the 256 GiB one's (pagani-gpu-va=256 in paged mode). */
    info.va_size=(aperture>A830_GPU_VA_CLIENT_END_64G?A830_GPU_VA_CLIENT_END_256G:
        aperture>A830_GPU_VA_CLIENT_END_16G?A830_GPU_VA_CLIENT_END_64G:aperture>A830_GPU_VA_CLIENT_END_4G?A830_GPU_VA_CLIENT_END_16G:
        b->knobs.va==2?A830_GPU_VA_CLIENT_END_2G:A830_GPU_VA_CLIENT_END_4G)-A830_GPU_VA_CLIENT_BASE;
    info.highest_bank_bit=b->highest_bank_bit;info.ubwc_swizzle=A830_UBWC_SWIZZLE;info.macrotile_mode=A830_UBWC_MACROTILE_MODE;
    info.ubwc_version=b->ubwc_mode;
    info.flags=NativeAdrenoDeviceClientIova|(d->selftest.coherence.level>=A830CoherenceFull?NativeAdrenoDeviceCachedCoherent:0u);
    info.reserved=0;
    adreno830_status_step(A830S9StepSubmit);
    NativeAdrenoVm *vm=d->gpu_context.vm();
    IOReturn r=d->submit.attach(vm,&d->ring);
    if(r!=kIOReturnSuccess)return refuse(f,A830S9StepSubmit,A830ReasonPublish,0,r,0,r);
    /* publishMetal up to the properties first: whatever can fail does before any service is registered (3.3). */
    adreno830_status_step(A830S9StepPrepare);
    r=os().metal_prepare?os().metal_prepare(&d->metal):kIOReturnUnsupported;
    s.aux[1]=d->metal.step|uint64_t(uint32_t(d->metal.kext))<<32;
    if(r!=kIOReturnSuccess) {
        if(os().metal_discard)os().metal_discard();
        return refuse(f,uint16_t(A830S9StepPrepare+d->metal.step),A830ReasonPublish,d->metal.parent_bytes,r,d->metal.instance_bytes,r);
    }
    adreno830_status_step(A830S9StepPublish);
    r=native_adreno_publish(vm,&d->submit,&d->ring,&d->clock,&d->timer,&info,d->gpu_context.inputBits(),d->smmu.outputBits());
    s.aux[0]=uint32_t(r);
    if(r!=kIOReturnSuccess) {
        if(os().metal_discard)os().metal_discard();
        return refuse(f,A830S9StepPublish,A830ReasonPublish,0,r,0,r);
    }
    d->published=true;   /* the direct service owns the VM, ring and submit object from here on */
    /* S3's note again, where a run keeps it: the events ring has turned over S3's lines by the first UFS round, this one is the only proof of the
     * aperture the windows above were cut for (no log of a run since 2026-10-31 holds S3's). */
    traceNote("gpu aperture GiB, table pages",2,d->gpu_context.apertureBytes()>>30,d->gpu_report.table_pages);
    adreno830_status_step(A830S9StepCommit);
    r=os().metal_commit?os().metal_commit(&d->metal):kIOReturnUnsupported;
    s.aux[0]|=uint64_t(uint32_t(r))<<32;
    s.aux[1]=d->metal.step|uint64_t(uint32_t(d->metal.kext))<<32;
    if(r!=kIOReturnSuccess) {
        /* NativeAdrenoGpu is registered but the accelerator is not: the direct service must not stay usable on its own. */
        native_adreno_device_lost(A830ReasonPublish);
        return refuse(f,uint16_t(A830S9StepCommit+d->metal.step),A830ReasonPublish,0,r,0,r);
    }
    return kIOReturnSuccess;
}

/* ===== S10: DCVS (and the power/recovery bindings, from S8 on) ======================================================= */
/* A830PowerOps (ARCHITECTURE 4.17 P1, 4.13.4): the stage bodies again, without their once-per-boot parts. The ones that
 * re-run a stage body open a trace scope ("g op <name>", A830Trace.h): their accesses get lines as the bring-up's did. The
 * OOB, interrupt-line and counter operations (P2 calls them around GX accesses) do not. */
IOReturn opCxPowerOn(void *)
{
    TraceScope scope("cxPowerOn");
    g830.power={};
    if(Sm8750GpuPowerOn(&g830.power_io,&g830.power)!=Sm8750GpuPowerOk)return scope.end(kIOReturnIOError);
    /* CX_MISC lost the AHB timeouts with CX: the annibale profile writes them again as S1 did, read back exactly (the design's
     * pagani-gpu-ahbto=1 is A830PowerState's armAhbTimeout after this operation). */
    if(adreno830_annibale() && a830CxAhbTimeout(g830.cx_timer)!=kIOReturnSuccess)return scope.end(kIOReturnIOError);
    return scope.end(kIOReturnSuccess);
}
IOReturn opCxPowerOff(void *,uint32_t *warnings)
{
    TraceScope scope("cxPowerOff");
    g830.power={};
    const Sm8750GpuPowerStatus status=Sm8750GpuPowerOff(&g830.power_io,&g830.power);
    if(warnings)*warnings=g830.power.warnings;
    return scope.end(status==Sm8750GpuPowerOk?kIOReturnSuccess:kIOReturnIOError);
}
IOReturn opSmmuRestore(void *)
{
    TraceScope scope("smmuRestore");
    return scope.end(g830.smmu.restore());
}
void opCxTimerSeed(void *)
{
    TraceScope scope("cxTimerSeed");
    a830CxTimerLost(g830.cx_timer);
    IOReturn r;
    {
        TraceQuiet quiet("cx timer seed: AO_COUNTER 3d9e200/204 writes not logged");
        r=a830CxTimerSeed(g830.cx_timer);
    }
    traceNote("cx timer seed",2,uint32_t(r),g830.cx_timer.seeds);
}
IOReturn opGmuStart(void *,const A830GmuStartArgs *args,uint16_t *step)
{
    TraceScope scope("gmuStart");
    Adreno830Failure f={};
    /* S4 reload: the DDR blocks again only after CX lost power (the TCM blocks are written by B4 of every boot). */
    if(args->cxLost) {
        const IOReturn r=A830GmuFirmware::load(g830.gmu_blocks,g830.gmu_io,g830.gmu_memory,A830GmuLoadDdr);
        if(r!=kIOReturnSuccess) {if(step)*step=A830S4StepGmuFirmware;return scope.end(r);}
    }
    g830.gmu_memory.clear(1u<<NativeAdrenoGmuRegionLog|1u<<NativeAdrenoGmuRegionHfi);
    Adreno830Failure ignored={};
    hfiInit(&g830,g830.hfi.layout(),&ignored);
    const IOReturn r=gmuStartOnce(&g830,args->level,args->bus,args->ifpc,&f,nullptr);
    recordGmu();
    if(r!=kIOReturnSuccess) {
        if(step)*step=f.step;
        g830.failure=f;g830.failure.stage=A830SGmu;
        return scope.end(r);
    }
    const uint8_t ackLevel=gQualification.r3b()?g830.hfi.currentLevel():args->level;
    const uint8_t ackBus=gQualification.r3b()?g830.hfi.currentBus():args->bus;
    if(args->ackLevel)*args->ackLevel=ackLevel;
    if(args->ackBus)*args->ackBus=ackBus;
    g830.level=ackLevel;g830.bus=ackBus;
    if(gQualificationEnabled) {
        uint64_t highAcks=0;
        for(unsigned i=4;i<=Adreno830LevelMax;++i)highAcks+=g830.hfi.stats().level_acks[i];
        qualLog("R3a fresh-M9 ACK level=%u bus=%u header=%08x at-us=%llu elapsed-us=%llu high-acks=%llu",ackLevel,ackBus,
            g830.hfi.header(A830HfiM9Vote),nowUs(),nowUs()-gQualification.failedAt()/1000,highAcks);
    }
    /* P2: with IFPC on (FEATURE_CTRL{IFPC,1,0x1680}) the GPU top half keeps GX up and polls the AHB fence (4.12.1, 4.17). */
    g830.irq.setIfpc(args->ifpc);
    return scope.end(kIOReturnSuccess);
}
/* The last CX gate check's words, for the low-failed console line (the exit journal holds them too, but it dies with a reset;
 * gpu-r3d-c's console delivered lines at +1.9 and +2.8 ms after the expiry). flags (A830CxGate.h): bit0 cxOn, bit1 the three
 * windows mapped, bit2/3/4 the GDSC/AHB/CX_GMU read answered, bit5 AHB on only by its running clock (ENABLE clear, CLK_OFF clear),
 * bit8 accessible; seq counts the checks, at-us is when the last one ran. */
static uint32_t gCxLastGdsc,gCxLastAhb,gCxLastClock,gCxLastFlags,gCxLastSeq;
static uint64_t gCxLastAtUs;
bool qualificationCxAccessible()
{
    uint32_t gdsc=0,ahb=0,clock=0;
    a830ExitTrace.stage(A830ExitStage::CxBegin);
    const bool cxOn=g830.power_state.cxOn(),
        windows=mapped(A830WinGmu) && mapped(A830WinGpu) && mapped(A830WinRscc);
    uint32_t flags=(cxOn?1u:0u)|(windows?2u:0u);
    /* In order, short-circuit (a word not reached stays 0 and its read bit clear). AHB_CBCR counts as on when its clock runs, by
     * our enable or another voter's (A830CxGate.h: gpu-607d1 read 08000000 at the expiry, ENABLE clear with the clock running,
     * and the ENABLE-only test sent the exit strict and then unsafe). */
    bool accessible=cxOn && windows;
    if(accessible) {
        const bool read=!powerRead(nullptr,Sm8750GpuGpuCc,SM8750_GPUCC_CX_GDS_HW_CTRL,&gdsc) && gdsc!=UINT32_MAX;
        flags|=read?A830CxFlagReadGdsc:0u;
        accessible=read && (gdsc&SM8750_GDSC_PWR_ON);
    }
    if(accessible) {
        const bool read=!powerRead(nullptr,Sm8750GpuGpuCc,SM8750_GPUCC_AHB_CBCR,&ahb) && ahb!=UINT32_MAX;
        flags|=read?A830CxFlagReadAhb:0u;
        accessible=read && a830CxAhbOn(ahb);
        flags|=accessible && a830CxAhbByRunningClock(ahb)?A830CxFlagAhbRunning:0u;
    }
    if(accessible) {
        const bool read=!powerRead(nullptr,Sm8750GpuGpuCc,SM8750_GPUCC_CX_GMU_CBCR,&clock) && clock!=UINT32_MAX;
        flags|=read?A830CxFlagReadCxGmu:0u;
        accessible=read && a830CxGmuOn(clock);
    }
    flags|=accessible?A830CxFlagAccessible:0u;
    gCxLastGdsc=gdsc;gCxLastAhb=ahb;gCxLastClock=clock;gCxLastFlags=flags;gCxLastAtUs=nowUs();++gCxLastSeq;
    a830ExitTrace.stage(A830ExitStage::CxEnd,accessible?0u:uint32_t(kIOReturnNotReady),gdsc,ahb,clock);
    return accessible;
}

IOReturn opGmuForceOff(void *)
{
    TraceScope scope("gmuForceOff");
    IOReturn r;
    if(gQualification.r3b() && !gQualification.isDaily()) {
        r=qualificationCxAccessible()?g830.gmu.forceOffStrict(A830GmuGxVoteStrict,&g830.gx_voter):kIOReturnNotReady;
        const auto &f=g830.gmu.failure();
        if(r!=kIOReturnSuccess && r!=kIOReturnNotReady)
            a830ExitTrace.stage(A830ExitStage::StrictHwError,uint32_t(r),f.step,f.reg,f.value,f.expect);
        qualLog("R3b3 strict result=%x step=%u reg=%llx value=%llx want=%llx at-us=%llu",
            unsigned(r),r==kIOReturnSuccess?0:f.step,f.reg,f.value,f.expect,nowUs());
    } else {
        r=g830.gmu.forceOff(gQualificationEnabled);
        g830.hfi.stop();
    }
    if(gQualificationEnabled)qualLog("R3a strict-off result=%x at-us=%llu elapsed-us=%llu",unsigned(r),nowUs(),
        nowUs()-gQualification.failedAt()/1000);
    return scope.end(r);
}
IOReturn opRsccSleep(void *)
{
    TraceScope scope("rsccSleep");
    return scope.end(g830.gmu.rsccSleep(gQualification.r3b()));
}
IOReturn opOobSet(void *) {uint32_t ack=0;return g830.gmu.oobSet(&ack);}
void opOobClear(void *) {g830.gmu.oobClear();}
IOReturn opHfiSend(void *,uint8_t id,const uint32_t *payload,uint8_t dwords)
{
    TraceScope scope("hfiSend");
    return scope.end(g830.hfi.send(id,payload,dwords,nullptr));
}
IOReturn opCpStart(void *,uint32_t fence,uint16_t *step)
{
    TraceScope scope("cpStart");
    Adreno830Failure f={};
    uint32_t last=0;
    /* The fence is about to be forced (ring.recover, then fence below) over the jobs the recovery failed: the queue meter
     * skips them (4.13.5). UniXNU-piano (2026-11-22, replay): fence is the recovery's restart fence, at or before the last issued
     * one; the jobs after it are replayed and measured again, so only the seqnos up to it are skipped (without a replay fence is
     * the last issued one, as before). */
    g830.fence.queueSkip(fence);
    g830.ring.recover(&last,&fence);
    IOReturn r=cpStartOnce(&g830,&f);
    if(r!=kIOReturnSuccess) {
        if(step)*step=f.step;
        g830.failure=f;g830.failure.stage=A830SCp;
        return scope.end(r);
    }
    /* startRing cleared the memptrs: the completed fences go back to the last issued seqno before any waiter looks. */
    volatile uint32_t *memptrs=static_cast<volatile uint32_t *>(const_cast<void *>(g830.buffers[GpuBufferMemptrs].cpu));
    __atomic_store_n(&memptrs[3],fence,__ATOMIC_RELEASE);
    __atomic_store_n(&memptrs[1],fence,__ATOMIC_RELEASE);
    __asm__ volatile("dsb sy" ::: "memory");
    g830.fence.wakeAll();
    return scope.end(kIOReturnSuccess);
}
IOReturn opSecureResume(void *,uint16_t *step)
{
    TraceScope scope("secureResume");
    IOReturn r=g830.zap.resume(A830ZapResumeSecureMode);
    if(r!=kIOReturnSuccess) {if(step)*step=A830ZapStepRemoteState;return scope.end(r);}
    if((r=g830.cp.finishSecure(nullptr,A830_T_CP_IDLE_US))!=kIOReturnSuccess) {
        if(step)*step=g830.cp.failure().step;
        return scope.end(r);
    }
    /* As S7: the annibale profile keeps the OOB_GPU of this start's gmuStart set (A830PowerState releases it on slumber). */
    if(!adreno830_annibale())g830.gmu.oobClear();
    return scope.end(kIOReturnSuccess);
}
void opIrqEnable(void *,uint32_t lines,bool enable)
{
    if(lines&A830PowerIrqGpu) {
        if(enable)g830.irq.restore();
        else g830.irq.quiesce(true);
    }
    if(lines&A830PowerIrqCxHost) {
        if(enable)g830.irq.cxGuard().resume();
        else g830.irq.cxGuard().suspend();
    }
    if((lines&(A830PowerIrqHfi|A830PowerIrqGmu)) && enable)g830.gmu.reenableInterrupts();
}
uint32_t opLastSubmitted(void *) {return g830.ring.lastSubmitted();}
uint32_t opRetired(void *) {return g830.fence.completed();}
/* A830RecoveryOps: the publish shell's gate and fault accounting, the fence, the interrupt lines, the CP pipe registers. */
uint32_t ropFailInflight(void *) {return g830.ring.outstanding();}
/* The publish shell halts the ring and faults the connections with work in flight whatever the cause; only a hang a client's job can
 * produce (A830FaultBlamesClient) is also charged to the process whose job the CP stood in. UniXNU-piano (2026-11-22): a replaying
 * recovery only halts the shell here; the plan charges and faults the culprit alone. */
void ropGate(void *,uint8_t gate,uint8_t cause)
{
    if(gate==A830GateRecovering) {
        traceRecoveryGate(gate);
        if(g830.recovery.replaying())native_adreno_recovery_begin_replay();
        else native_adreno_recovery_begin(A830FaultBlamesClient(cause));
    }
    else if(gate==A830GateOpen) {native_adreno_recovery_end(true);traceRecoveryGate(gate);}
    else {native_adreno_recovery_end(false);traceRecoveryGate(gate);}
    if(gQualificationEnabled)qualLog("R3a gate=%u cause=%u ACK-level=%u bus=%u at-us=%llu elapsed-us=%llu sticky=%u",gate,cause,
        g830.power_state.votedLevel(),g830.power_state.votedBus(),nowUs(),nowUs()-gQualification.failedAt()/1000,
        g830.power_state.qualificationLatched());
}
/* The replay ops (A830RecoveryOps::plan/replay): the shell's plan over the submit object; before the shell is published (no client
 * job exists) the restart is the last issued fence and nothing is replayed. */
uint32_t ropPlan(void *,uint8_t cause,uint32_t *restart)
{
    *restart=g830.ring.lastSubmitted();
    return native_adreno_recovery_plan(A830FaultBlamesClient(cause),restart);
}
IOReturn ropReplay(void *,uint32_t *replayed) {return native_adreno_recovery_replay(replayed);}
void ropFenceSignal(void *) {g830.fence.wakeAll();}
void ropIrqRestore(void *)
{
    TraceScope scope("irqRestore");
    g830.irq.restore();
    g830.gmu.reenableInterrupts();
}
IOReturn ropReadPipe(void *,uint8_t pipe,uint32_t reg,uint32_t *value) {return g830.cp.readPipe(pipe,reg,value);}
/* The hang verdict's snapshot (A830Recovery, before the status block is written and before the GMU is forced off): the CP pipe
 * and SMMU bank registers, then (annibale, CX up) the GMU's late report drained into hfi_late. */
void ropSnapshot(void *,A830HangSnapshot *snapshot)
{
    TraceScope scope("snapshot");
    for(uint8_t pipe=0;pipe<2;++pipe) {
        g830.cp.readPipe(uint8_t(pipe+1),A8XX_CP_HW_FAULT_STATUS_PIPE,&snapshot->pipeHwFault[pipe]);
        g830.cp.readPipe(uint8_t(pipe+1),A8XX_CP_INTERRUPT_STATUS_PIPE,&snapshot->pipeIntStatus[pipe]);
        g830.cp.readPipe(uint8_t(pipe+1),A8XX_CP_PIPE_STATUS_PIPE,&snapshot->pipeStatus[pipe]);
        g830.cp.readPipe(uint8_t(pipe+1),A8XX_CP_PROTECT_STATUS_PIPE,&snapshot->pipeProtect[pipe]);
    }
    const uint8_t banks[2]={g830.board->cb_gpu,g830.board->cb_gmu};
    for(unsigned i=0;i<2;++i) {
        const uint64_t cb=g830.smmu.contextOffset(banks[i]);
        snapshot->smmuFsr[i]=smmuRead(uint32_t(cb+A830_SMMU_CB_FSR));
        snapshot->smmuFar[i]=smmuFar(cb);
    }
    /* UniXNU-piano (2026-11-22, gpu-recover2 RECOVER2-CLASSIFY.md): the GMU's late report only for a fault the GMU or HFI can explain.
     * drainLate polls the HFI queues for 3 s (annibale); a client's hang (hang detect, CP_HW_ERROR, the software watchdog) leaves the
     * GMU healthy, and those 3 s were 3.025 of gpu-recover2's 3.046 s recoveries, with the gate closed and every client stalled
     * (gpu/daily3c's line skipped the drain under its qualification: 22 ms). */
    /* lead/v131y7: and, as gpu/daily3c, never under the qualification (the R3b line: 22 ms recoveries). */
    if(snapshot->cxOn && !gQualificationEnabled && !A830FaultBlamesClient(snapshot->cause))drainHfiLate();
#ifdef ADRENO830_MACOS
    native_adreno_note_hang_snapshot(snapshot);   /* UniXNU-piano: the snapshot on the console (NativeAdrenoPublish.cpp) */
#endif
}
IOReturn ropQualificationLow(void *,bool drop)
{
    if(!qualificationCxAccessible())return kIOReturnNotReady;
    const IOReturn result=g830.hfi.confirmThermalExit(drop);
    const auto &h=g830.hfi.stats();
    qualLog("R3b3 low vote result=%x level=%u bus=%u request=%08x ack=%08x drop=%08x dropped=%u at-us=%llu",
        unsigned(result),h.last_level,h.last_bw,h.last_request,h.last_ack,h.drop_header,h.dropped_acks,nowUs());
    return result;
}
IOReturn ropQualificationQuiesce(void *)
{
    // Qualification selects annibale: GMU AO and HFI device IRQs are masked, with no host handlers.
    if(!adreno830_annibale())return kIOReturnUnsupported;
    g830.hfi.stop();
    const IOReturn irq=g830.irq.quiesceForRecovery();
    if(irq!=kIOReturnSuccess)return irq;
    for(unsigned waited=0;!g830.irq.recoveryUsersDrained();waited+=50) {
        if(waited>=100000)return kIOReturnTimeout;
        os().sleep_us(50);
    }
    return kIOReturnSuccess;
}
bool ropQualificationGxOff(void *)
{
    return qualificationCxAccessible() && g830.gmu.gxOffProven();
}
void ropQualificationStage(void *,const char *stage,IOReturn result)
{
    /* low-failed (the vote ran) and low-failed-phase (not Active or no vote callback: no CX check this time): the last CX gate words
     * on the same console line, with their age; a fresh check (cx-seq moved, cx-age small) and accessible=0 names the predicate. */
    if(!strncmp(stage,"low-failed",10)) {
        const uint64_t now=nowUs();
        qualLog("R3b3 exit %s result=%x at-us=%llu sticky=%u voted=%u/%u phase=%u cx gdsc=%08x ahb=%08x clock=%08x flags=%03x seq=%u "
            "age-us=%llu",stage,unsigned(result),now,g830.power_state.qualificationLatched(),g830.power_state.votedLevel(),
            g830.power_state.votedBus(),unsigned(g830.power_state.phase()),gCxLastGdsc,gCxLastAhb,gCxLastClock,gCxLastFlags,
            gCxLastSeq,gCxLastSeq?now-gCxLastAtUs:0ull);
        return;
    }
    qualLog("R3b3 exit %s result=%x at-us=%llu sticky=%u voted=%u/%u",stage,unsigned(result),
        nowUs(),g830.power_state.qualificationLatched(),g830.power_state.votedLevel(),g830.power_state.votedBus());
}
void powerFaultSink(void *,uint8_t cause,uint64_t detail) {g830.recovery.report(cause,detail);}
/* The power state machine and the recovery, bound once (S8; started in S10). */
void bindPower(Adreno830Device *d)
{
    if(d->power_bound)return;
    d->power_bound=true;
    d->power_mutex=os().mutex_alloc?os().mutex_alloc():nullptr;
#ifdef ADRENO830_MACOS
    if(!d->host_mutex)d->host_mutex=os().mutex_alloc?os().mutex_alloc():nullptr;
#endif
    d->power_win={nullptr,powerWinRead,powerWinWrite,powerWinBarrier};
    d->power_env={nullptr,envNowNs,envSleepUs,envDelayUs,envLock,envUnlock,envWait,envWake,envStartThread};
    d->power_ops={nullptr,opCxPowerOn,opCxPowerOff,opSmmuRestore,opCxTimerSeed,opGmuStart,opGmuForceOff,opRsccSleep,opOobSet,
                  opOobClear,opHfiSend,opCpStart,opSecureResume,opIrqEnable,opLastSubmitted,opRetired};
    d->recovery_ops={nullptr,ropFailInflight,ropGate,ropFenceSignal,ropIrqRestore,ropReadPipe,ropSnapshot,ropPlan,ropReplay,
                     ropQualificationLow,ropQualificationQuiesce,ropQualificationGxOff,ropQualificationStage};
    /* P0 by default (ARCHITECTURE 4.17): no slumber, no IFPC; the machine still owns the transitions the recovery runs.
     * pagani-gpu-slumber selects P1 (1: full collapse, 2: keeping CX), pagani-gpu-ifpc P2 and pagani-gpu-ahbto the CX_AHB
     * timeout detection of each GMU start (IMPLEMENTATION_PLAN M13 c, e, f). */
    const Adreno830Board::Adreno830Knobs &knobs=d->board->knobs;
    A830PowerConfig power;
    power.slumber=knobs.slumber==1?A830SlumberFull:knobs.slumber==2?A830SlumberCxKeep:A830SlumberOff;
    power.ifpc=knobs.ifpc!=0;power.ahbTimeout=knobs.ahbto!=0;power.defaultLevel=d->level;power.defaultBus=d->bus;
    /* pagani-screenoff bit 1: slumber while the display is off (bit 3: the full collapse instead of keeping CX). */
    power.screenSlumber=!(knobs.screenoff&2u)?A830SlumberOff:(knobs.screenoff&8u)?A830SlumberFull:A830SlumberCxKeep;
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (A830Idle.h): piano-gpu-idle=2 with its slumber is P1's full collapse after piano-gpu-idle-ms, unless
     * piano-gpu-slumber chose a mode itself. No touch pre-wake from level 1 on: nothing on piano counts touches for it
     * (pagani_input_touch_events), and asleep the thread then looked at that counter every 16 ms for nothing. */
    gIdle=A830IdleConfig{};
    adreno830_board_idle(&gIdle);
    if(gIdle.mode>=1)power.touchPrewake=false;
    if(gIdle.mode>=2 && gIdle.slumber && !(d->board->flags&(1u<<Adreno830KnobSlumber))) {
        power.slumber=A830SlumberFull;
        power.idleMs=gIdle.idleMs;
    }
#endif
    d->power_state.init(power,d->power_win,d->power_env,d->power_ops);
    d->power_state.setFaultSink(powerFaultSink,nullptr);
    A830RecoveryConfig recovery;
    recovery.enabled=true;   /* M10: a global loss is recovered first */
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: piano-gpu-hang-ms, the no-progress time the software watchdog calls a hang (the board keeps it in 1000..60000). A
     * full-panel pass at the annibale clocks (160-342 MHz) can run past the 2 s that suit the 13T's iPadOS work. */
    if(const uint32_t hang_ms=adreno830_board_hang_ms())recovery.watchdogLimitMs=hang_ms;
#endif
    d->recovery.init(recovery,d->power_win,d->power_env,d->power_ops,d->recovery_ops,&d->power_state);
    d->power_state.start(d->level,d->bus);
}
IOReturn dcvsPerfVote(void *,uint8_t level,uint32_t bw)
{
    const uint8_t bus=uint8_t(bw&0xff);
    if(bw&0x100u) {
        const uint32_t top=adreno830_ddr_kbps(g830.board->ddr_levels-1u);
        const uint64_t ab=uint64_t(bw>>16)*top*4ull/65536ull;
        return g830.hfi.perfVote(level,bus,uint32_t(ab));
    }
    return g830.hfi.perfVote(level,bus);
}
#ifdef ADRENO830_MACOS
/* UniXNU-piano (A830Suspend.h): the host-vote mutex (none in a run without the kernel's mutexes: the host tests' doubles). */
struct HostVotes {
    HostVotes() {if(g830.host_mutex)os().mutex_lock(g830.host_mutex);}
    ~HostVotes() {if(g830.host_mutex)os().mutex_unlock(g830.host_mutex);}
    HostVotes(const HostVotes &)=delete;
    HostVotes &operator=(const HostVotes &)=delete;
};
bool suspendGateShut() {return g830.power_bound && g830.power_state.suspended();}
#endif
IOReturn dcvsHostDdr(void *,uint32_t kbps)
{
#ifdef ADRENO830_MACOS
    HostVotes votes;
    if(suspendGateShut())return kIOReturnNotReady;   /* UniXNU-piano: the suspend owns the host votes until it resumed */
#endif
    return Sm8750RpmhHostDdr(&g830.host,kbps)==Sm8750RpmhOk?kIOReturnSuccess:kIOReturnIOError;
}
#ifdef ADRENO830_MACOS
/* ===== UniXNU-piano: the idle votes (A830Idle.h, piano-gpu-idle=2) ============================================================ */
A830IdleVotes gIdleVotes;
uint32_t gIdleDdrFloorKbps;   /* the host DDR vote while the GPU is not idle: S5's floor, or in mode 2 the display's (stageDcvs) */
struct IdleReport {
    uint64_t periodStart=0,downSince=0,downNs=0;
    uint32_t downs=0,ups=0,slumbers=0,wakes=0,maxWakeUs=0;   /* in the period */
    uint32_t seenEntries=0,seenWakes=0;
    uint64_t seenSleptNs=0;
    bool slumberOffSaid=false;
} gIdleReport;
uint32_t railVlvl(const Sm8750RpmhArc &arc,uint32_t hlvl) {return hlvl<arc.count?arc.vlvl[hlvl]:0;}
void idleEvent(A830IdleEvent event)
{
    const Sm8750RpmhHost &h=g830.host;
    event.ddrKbps=h.ddr_kbps;
    event.ddrLevel=0;
    for(uint8_t level=0;g830.board && level<g830.board->ddr_levels;++level)
        if(adreno830_ddr_kbps(level)==h.ddr_kbps)event.ddrLevel=level;
    event.cxVlvl=h.res?railVlvl(h.res->cx,h.cx_hlvl):0;
    event.mxVlvl=h.res?railVlvl(h.res->mx,h.mx_hlvl):0;
    adreno830_board_idle_event(&event);
}
/* Down: the DDR vote to the display's floor (only lower than what is held), then the rails to the idle level (lowering only, cx first:
 * Sm8750RpmhHostRailsIdle). Up: the rails as S1 chose them (pagani-gpu-rails: the top), then the DDR floor S5 left (pagani-gpu-ddr-floor,
 * = the boot level). Every one of them goes through Sm8750RpmhWriteActive, which takes the DRV-2 lock, as for every vote of this driver. */
int idleVotesDown()
{
    const uint32_t floor=adreno830_ddr_kbps(gIdle.ddrLevel);
    int rc=Sm8750RpmhOk;
    if(floor && floor<g830.host.ddr_kbps)rc=Sm8750RpmhHostDdr(&g830.host,floor);
    if(!rc)rc=Sm8750RpmhHostRailsIdle(&g830.host,gIdle.railVlvl);
    return rc;
}
int idleVotesUp()
{
    const Adreno830Board *b=g830.board;
    int rc=Sm8750RpmhHostRails(&g830.host,int(b->knobs.rails));
    const int ddr=g830.host.ddr_kbps==gIdleDdrFloorKbps?int(Sm8750RpmhOk):Sm8750RpmhHostDdr(&g830.host,gIdleDdrFloorKbps);
    return rc?rc:ddr;
}
uint64_t dcvsIdleTick(void *,uint64_t now,bool quiet,uint64_t lastSubmit)
{
    /* UniXNU-piano (A830Suspend.h): while a system suspend has the GPU (its gate shut), its votes are the suspend's: nothing here.
     * Looked at again under the host-vote mutex, so a vote of this tick and the suspend's never interleave. */
    HostVotes votes;
    if(suspendGateShut())return 0;
    IdleReport &r=gIdleReport;
    if(!r.periodStart)r.periodStart=now;
    /* The slumbers and wakes A830PowerState counted since the last look (its own counters; their lines from here). */
    const A830PowerStats &ps=g830.power_state.stats();
    while(r.seenEntries<ps.entries) {
        ++r.seenEntries;++r.slumbers;
        if(A830IdleLogged(r.seenEntries)) {
            A830IdleEvent e{};e.kind=A830IdleEventSlumber;e.count=r.seenEntries;e.us=ps.lastEntryUs;e.ms=gIdle.idleMs;
            idleEvent(e);
        }
    }
    while(r.seenWakes<ps.wakes) {
        ++r.seenWakes;++r.wakes;
        if(ps.lastWakeUs>r.maxWakeUs)r.maxWakeUs=ps.lastWakeUs;
        if(A830IdleLogged(r.seenWakes)) {
            A830IdleEvent e{};e.kind=A830IdleEventWake;e.count=r.seenWakes;e.us=ps.lastWakeUs;
            e.ms=(ps.sleptNs-r.seenSleptNs)/1000000u;   /* asleep since the last look (one sleep but for a burst) */
            idleEvent(e);
        }
    }
    r.seenSleptNs=ps.sleptNs;
    if(ps.slumberBlocked && !r.slumberOffSaid) {
        r.slumberOffSaid=true;
        A830IdleEvent e{};e.kind=A830IdleEventSlumberOff;e.count=ps.failures;e.result=int32_t(ps.lastError);e.us=ps.lastStep;
        idleEvent(e);
    }
    uint64_t next=0;
    const A830IdleAction action=gIdleVotes.step(now,quiet,lastSubmit,gIdle.idleMs,&next);
    if(action==A830IdleDown) {
        const int rc=idleVotesDown();
        gIdleVotes.done(action,rc==Sm8750RpmhOk,now,lastSubmit);
        A830IdleEvent e{};
        if(rc==Sm8750RpmhOk) {
            ++r.downs;r.downSince=now;
            e.kind=A830IdleEventDown;e.count=gIdleVotes.downs;e.ms=(now-lastSubmit)/1000000u;
            if(A830IdleLogged(e.count))idleEvent(e);
        } else {
            (void)idleVotesUp();   /* best effort: what went down comes back */
            e.kind=A830IdleEventFailed;e.result=rc;e.count=gIdleVotes.failures;
            idleEvent(e);
        }
    } else if(action==A830IdleUp) {
        const int rc=idleVotesUp();
        gIdleVotes.done(action,rc==Sm8750RpmhOk,now,lastSubmit);
        A830IdleEvent e{};
        if(r.downSince) {r.downNs+=now-r.downSince;e.ms=(now-r.downSince)/1000000u;r.downSince=0;}
        if(rc==Sm8750RpmhOk) {
            ++r.ups;
            e.kind=A830IdleEventUp;e.count=gIdleVotes.ups;
            if(A830IdleLogged(e.count))idleEvent(e);
        } else {
            e.kind=A830IdleEventFailed;e.result=rc;e.count=gIdleVotes.failures;
            idleEvent(e);
        }
    }
    /* Once a minute, when anything happened in it: the summary (every transition past the logged ones is only counted here). */
    if(now-r.periodStart>=60000000000ull) {
        uint64_t down=r.downNs;
        if(r.downSince)down+=now-r.downSince;
        if(r.downs || r.ups || r.slumbers || r.wakes) {
            A830IdleEvent e{};e.kind=A830IdleEventSummary;e.ms=(now-r.periodStart)/1000000u;
            e.downs=r.downs;e.ups=r.ups;e.slumbers=r.slumbers;e.wakes=r.wakes;e.maxWakeUs=r.maxWakeUs;
            e.downPermille=uint32_t(down*1000u/(now-r.periodStart));
            idleEvent(e);
        }
        r.periodStart=now;r.downNs=0;if(r.downSince)r.downSince=now;
        r.downs=r.ups=r.slumbers=r.wakes=r.maxWakeUs=0;
    }
    return next;
}
#endif
void dcvsPollFaults(void *)
{
    if(g830.gmu.interruptsOff() || !g830.gmu.interruptsWired())g830.gmu.pollFatal();
    g830.smmu.poll();
    /* Without 332 (annibale profile, or turned off by the storm guard) nobody runs the fence interrupt's retire call:
     * completed jobs are retired here, once a second, so they never pin buffers or job slots (waiters poll on their own). */
    if(!g830.irq.attached() || g830.irq.off())native_adreno_retire_completed();
}
void dcvsMemoryRow(void *,uint32_t *pool,uint32_t *used,uint32_t *imports,uint32_t *coherent)
{
    *pool=g830.gpu_context.poolMib();
    NativeAdrenoVm *vm=g830.gpu_context.vm();
    /* used: the BO pool's MiB in use (it was always 0 here, which hid how much the clients hold); the VM's bytes go to the row's vm. */
    uint32_t pool_mib=0,used_mib=0;
    NativeAdrenoMemory::usage(&pool_mib,&used_mib,nullptr);
    *used=used_mib;*imports=vm?uint32_t(vm->objectCount()):0;
    adreno830_status_vm(vm?vm->mappedBytes():0);
    *coherent=native_adreno_coherent_imports();
    /* The publish shell's second: orphaned cores retried, then who holds the GPU memory (the MEM row's tail, ARCHITECTURE 8.4). */
    Adreno830StatusClients clients{};
    adreno830_status_clients(native_adreno_client_tick(&clients)?&clients:nullptr);
}
/* Once a second: the ring's queue over the last period (A830Fence's meter) for the run page's IRQ and MEM rows (ARCHITECTURE
 * 4.13.5), then the ramoops meter line's window (on the diagnostic pages at most one line a minute, only a changed one; M14 P0). */
void dcvsQueueRow(void *)
{
    Adreno830StatusQueue queue={};
    g830.fence.takeQueue(&queue);
    adreno830_status_queue(&queue);
    adreno830_status_meter();
    if(gQualificationEnabled) {
        const auto &s=g830.dcvs.stats();const auto &h=g830.hfi.stats();const auto &r=g830.recovery.stats();
        qualLog("R3a COUNTERS at-us=%llu vote=%u failures=%u sent=%u ack=%u timeout=%u errors=%u stale=%u malformed=%u queuefull=%u dropped=%u drop=%08x req=%08x requested=%u/%u forceoff=%u recover=%u recoverfail=%u gbif=%u level=%u bus=%u sticky=%u reason=%u ready=%u high=%u",
            nowUs(),s.votes,s.voteFailures,h.sent,h.acks,h.timeouts,h.errors,h.stale_acks,h.malformed,h.queue_full,h.dropped_acks,h.drop_header,
            h.last_request,h.last_level,h.last_bw,g830.gmu.report().force_offs,r.recoveries,r.failures,r.gbifTimeouts,
            g830.power_state.votedLevel(),g830.power_state.votedBus(),g830.power_state.qualificationLatched(),
            gQualification.reason(),gQualification.ready(),gQualification.high());
        qualLog("R3a RES at-us=%llu windows=%u busy=%u ceiling=%u l1=%u l2=%u l3=%u l4=%u l5=%u l6=%u l7=%u l8=%u l9=%u l10=%u l11=%u",
            nowUs(),s.windows,s.busyPermille,s.ceiling,s.residency[1],s.residency[2],s.residency[3],s.residency[4],s.residency[5],
            s.residency[6],s.residency[7],s.residency[8],s.residency[9],s.residency[10],s.residency[11]);
        if(gQualification.r3b())qualLog("R3b LIVE at-us=%llu l0=%llu l1=%llu l2=%llu l3=%llu l4=%llu l5=%llu l6=%llu l7=%llu l8=%llu l9=%llu l10=%llu l11=%llu high-long-us=%llu l7-long-us=%llu",
            nowUs(),s.residencyTotalNs[0]/1000,s.residencyTotalNs[1]/1000,s.residencyTotalNs[2]/1000,s.residencyTotalNs[3]/1000,
            s.residencyTotalNs[4]/1000,s.residencyTotalNs[5]/1000,s.residencyTotalNs[6]/1000,s.residencyTotalNs[7]/1000,
            s.residencyTotalNs[8]/1000,s.residencyTotalNs[9]/1000,s.residencyTotalNs[10]/1000,s.residencyTotalNs[11]/1000,s.residencyHighLongestNs/1000,s.residency607LongestNs/1000);
        if(gQualification.r3b())qualLog("R3b WORK at-us=%llu submitted=%u completed=%u missed=%u q-us=%u q-max=%u depth=%u polled=%u rs-jobs=%u rs-us=%u ext-jobs=%u ext-busy-us=%u ext-max-us=%u",
            nowUs(),queue.submitted,queue.completed,queue.missed,queue.avg_us,queue.max_us,queue.depth,queue.polled,
            queue.rs_jobs,queue.rs_avg_us,queue.ext_jobs,queue.ext_busy_us,queue.ext_max_us);
        qualLog("R3a ACK at-us=%llu l1=%u l2=%u l3=%u l4=%u l5=%u l6=%u l7=%u l8=%u l9=%u l10=%u l11=%u last=%08x hfi-vote-fail=%u host-sent=%u host-fail=%u host-held=%x mx=%u cx=%u ddr=%u gpu-peak=%d",
            nowUs(),h.level_acks[1],h.level_acks[2],h.level_acks[3],h.level_acks[4],h.level_acks[5],h.level_acks[6],h.level_acks[7],h.level_acks[8],
            h.level_acks[9],h.level_acks[10],h.level_acks[11],h.last_ack,h.vote_failures,g830.host.sent,g830.host.failures,g830.host.held,
            g830.host.mx_hlvl,g830.host.cx_hlvl,g830.host.ddr_kbps,g830.dcvs.thermal().peak_deci);
    }
}
void qualificationVoteFault(void *,uint8_t requested,uint8_t bus,IOReturn error)
{
    const auto &h=g830.hfi.failure();
    // A last-moment R3b cap may have lowered the caller's cached target before enqueue.
    if(gQualification.r3b()) {requested=uint8_t(g830.hfi.stats().last_level);bus=uint8_t(g830.hfi.stats().last_bw);}
    qualLog("R3a vote-unknown request=%08x seq=%u requested=%u/%u oldACK=%u/%u error=%x reason=%u at-us=%llu",
        h.hdr,A830_HFI_HDR_SEQ(h.hdr),requested,bus,g830.power_state.votedLevel(),g830.power_state.votedBus(),unsigned(error),h.reason,nowUs());
    gQualification.fault(A830QualVote,nowUs()*1000,A830ExitContext::Dcvs);
    if(gQualification.isDaily())g830.recovery.report(A830FaultHfi,uint32_t(error));   // daily: fault() does not report a vote
}
/* gpu/throttle1 (private/host-checks/20261124-gpu-throttle/TABLE.md): the skin table the guard uses (0 Android's normal scene, 1 the
 * docked table; the board sets it from piano-gpu-skin-table and machdep.piano_gpu_skin_table), the external ceiling's holder (the
 * board's slots: 0 PianoThermal's passive, 1 the power profile = Low Power Mode, 2 the sweep) and the state machdep.piano_gpu_state
 * reads: two buffers, the qualification thread writes the one not published, then publishes it (a once-a-second diagnostic). */
uint32_t gSkinTable,gCeilingHolder=4;
char gGpuState[2][288];uint32_t gGpuStateAt;
const char *ceilingHolderName(uint32_t client)
{
    static const char *const names[]={"thermal","lpm","sweep","client3"};
    return client<4?names[client]:"ceiling";
}
void qualificationThread(void *)
{
    uint64_t logAt=0;uint32_t lastReason=0;
    for(;;) {
        const uint64_t now=nowUs()*1000;
        if(g830.power_state.qualificationLatched() && !gQualification.reason())gQualification.fault(A830QualVote,now);
        Sm8750TsensSample gpu={};Sm8750TsensReadGpu(&g830.tsens_io,&gpu);
        if(gQualification.r3b())gQualification.sample(now,gpu,A830ExitContext::Guard); // raw exit before board lookup, filtering, logging or HFI
        piano_gpu_temps board={};board.version=PIANO_GPU_TEMPS_VERSION;
        const bool ok=adreno830_qualification_board(&board);
        const auto test=__atomic_load_n(&gQualificationExitTest,__ATOMIC_ACQUIRE);
        bool injectTrip=false;
        if(test==3 || test==4) {
            const bool accepted=a830ExitTestAllowed(test,gpu,board,ok,gQualification,g830.power_state,g830.recovery);
            injectTrip=accepted && test==3;
            qualLog("R3b4 inject command=%u accepted=%u raw=%d msm=%d quiet=%d period=%u age-ms=%u,%u phase=%u level=%u at-us=%llu",
                test,accepted,gpu.max_deci,board.milli_c[0],board.milli_c[1],board.period_ms,board.age_ms[0],board.age_ms[1],
                unsigned(g830.power_state.phase()),g830.power_state.votedLevel(),now/1000);
            if(accepted && test==4)g830.recovery.requestQualificationLow(A830QualBoardHot,now,true);
            __atomic_store_n(&gQualificationExitTest,test-2,__ATOMIC_RELEASE);
        }
        /* gpu/fullopp1: the virtual skin sensor (PianoThermal's round is 5 s: read it once a second) */
        static piano_skin_temps skin;static bool skinOk;static unsigned skinTicks;
        if(gQualification.isFullopp() && (skinTicks++%10)==0) {
            skin={};skin.version=PIANO_SKIN_TEMPS_VERSION;skinOk=adreno830_qualification_skin(&skin);
        }
        gQualification.thermal().skinTable=gQualification.isFullopp()?uint8_t(__atomic_load_n(&gSkinTable,__ATOMIC_ACQUIRE)==1):0;
        gQualification.tick(now,gpu,board,ok,injectTrip,gQualification.isFullopp() && skinOk?&skin:nullptr);
        static bool dailyOpenLogged;
        if(gQualification.isDaily() && !dailyOpenLogged && gQualification.openedAt()) {
            dailyOpenLogged=true;qualLog("R3b DAILY open at-us=%llu",gQualification.openedAt()/1000);
        }
        if(gQualification.deadlineMissed(now))qualificationUnsafe(nullptr,A830ExitOrigin::ProtectionDeadline);
        const bool event=gQualification.r3b() && (gQualification.thermal().events || gQualification.reason()!=lastReason);
        if(event) {
            const auto &t=gQualification.thermal();
            qualLog("R3b EVENT at-us=%llu event=%u step=%u board-cap=%u effective=%u raw=%d filtered=%d reason=%u failed-us=%llu",
                now/1000,t.events,t.step,t.boardCap,g830.power_state.qualificationEffectiveCap(A830ExitContext::Guard),t.raw,t.filter16/16,gQualification.reason(),gQualification.failedAt()/1000);
        }
        lastReason=gQualification.reason();
        if(!gQualification.r3b() || now>=logAt || event) {
            const uint64_t age0=board.age_ms[0],age1=board.age_ms[1];
            qualLog("R3a TEMP at-us=%llu gpu-valid=%02x gpu-max=%d gpu-raw=%x,%x,%x,%x,%x,%x,%x,%x board-ok=%u state=%u valid=%x disabled=%x rounds=%u period=%u msm=%d quiet=%d age-ms=%llu,%llu sample-us=%llu,%llu ready=%u high=%u sticky=%u reason=%u opened-us=%llu failed-us=%llu inhibit=%u",
                now/1000,gpu.valid,gpu.max_deci,gpu.raw[0],gpu.raw[1],gpu.raw[2],gpu.raw[3],gpu.raw[4],gpu.raw[5],gpu.raw[6],gpu.raw[7],
                ok,board.state,board.valid,board.disabled,board.rounds,board.period_ms,board.milli_c[0],board.milli_c[1],age0,age1,
                board.sample_us[0],board.sample_us[1],gQualification.ready(),gQualification.high(),g830.power_state.qualificationLatched(),
                gQualification.reason(),gQualification.openedAt()/1000,gQualification.failedAt()/1000,gQualification.inhibit());
            if(gQualification.r3b()) {
                auto &t=gQualification.thermal();
                qualLog("R3b THERM at-us=%llu seq=%llu reads=%u min-valid=%x gap-us=%llu peaks=%d,%d,%d,%d,%d,%d,%d,%d raw=%d filtered=%d trend=%d step=%u board-cap=%u effective=%u downs=%u ups=%u healthy=%u",
                    now/1000,t.sequence,t.reads,t.minValid,t.maxGap/1000,t.peaks[0],t.peaks[1],t.peaks[2],t.peaks[3],t.peaks[4],t.peaks[5],t.peaks[6],t.peaks[7],
                    t.raw,t.filter16/16,t.trend,t.step,t.boardCap,g830.power_state.qualificationEffectiveCap(A830ExitContext::Guard),t.downs,t.ups,t.healthy);
                t.resetPeaks();
            }
            if(gQualification.isFullopp()) {
                /* gpu/fullopp1: one short line a second (the console keeps ~255 bytes of a line): the voted level and its MHz, the
                 * guard's cap and why, the silicon and skin readings, ACD; then the residency above 607 and the HFI ACKs per level. */
                const auto &t=gQualification.thermal();
                const uint8_t lv=g830.power_state.votedLevel();
                /* gpu/throttle1: the cap from the qualified top (the probe started at 7 before) and the limit that holds it */
                /* min(knob/ACD-off, DCVS thermal, external slot) under the guard: A830Dcvs::ceiling() without its boost cap (inline
                 * accessors only: the stage tests link this file without A830Dcvs.cpp) */
                uint8_t cap=g830.dcvs.knobCeiling();
                if(g830.dcvs.thermalCeiling() && g830.dcvs.thermalCeiling()<cap)cap=g830.dcvs.thermalCeiling();
#ifdef ADRENO830_MACOS
                const uint8_t external=g830.dcvs.externalCeiling();
#else
                const uint8_t external=0;
#endif
                if(external && external<cap)cap=external;
                cap=g830.power_state.qualificationEffectiveCap(A830ExitContext::Guard,cap?cap:1);
                static const char *const causes[]={"none","ladder","skin25","skin45","xo","skin-unknown","hard","skinband"};
                A830BindIn bi;
                bi.voted=lv;bi.cap=cap;bi.top=gQualification.top();bi.guardCap=g830.power_state.qualificationCap();
                bi.guardStep=uint8_t(gQualification.thermalSnapshot());bi.guardCause=t.cause;
                bi.latched=g830.power_state.qualificationLatched();bi.opened=gQualification.openedAt()!=0;bi.high=gQualification.high();
                bi.external=external;
                bi.externalName=ceilingHolderName(__atomic_load_n(&gCeilingHolder,__ATOMIC_ACQUIRE));
                bi.knob=g830.dcvs.knobCeiling();
                const char *bind=a830BindName(bi),*why=a830BindWhy(bi);
                qualLog("R3b LV at-us=%llu lvl=%u mhz=%u cap=%u cause=%s raw=%d filt=%d skin=%d skin-ok=%u xo=%d acd=%u bind=%s why=%s tab=%u",
                    now/1000,unsigned(lv),unsigned(lv<=14?FulloppMhz[lv]:0),unsigned(cap),causes[t.cause<=7?t.cause:0],t.raw,
                    t.filter16/16,t.skinMilli/100,unsigned(t.skinValid),t.xoMilli/100,unsigned(g830.acd_running),bind,why,
                    unsigned(t.skinTable));
                {
                    const uint32_t next=(__atomic_load_n(&gGpuStateAt,__ATOMIC_ACQUIRE)+1)&1u;
                    snprintf(gGpuState[next],sizeof gGpuState[next],
                        "at_us=%llu lvl=%u mhz=%u cap=%u cap_mhz=%u why=%s bind=%s want=%u table=%s band=%u step=%u ext=%u "
                        "skin_dC=%d skin_ok=%u raw_dC=%d filt_dC=%d xo_dC=%d latched=%u high=%u",
                        now/1000,unsigned(lv),unsigned(lv<=14?FulloppMhz[lv]:0),unsigned(cap),unsigned(cap<=14?FulloppMhz[cap]:0),
                        why,bind,unsigned(g830.dcvs.policyLevel()),t.skinTable?"docked":"android",unsigned(t.band),unsigned(t.step),
                        unsigned(bi.external),t.skinMilli/100,unsigned(t.skinValid),t.raw,t.filter16/16,t.xoMilli/100,
                        unsigned(bi.latched),unsigned(bi.high));
                    __atomic_store_n(&gGpuStateAt,next,__ATOMIC_RELEASE);
                }
                const A830DcvsStats &ds=g830.dcvs.stats();
                qualLog("R3b RESF at-us=%llu ms l7=%llu l8=%llu l9=%llu l10=%llu l11=%llu l12=%llu l13=%llu l14=%llu",now/1000,
                    ds.residencyTotalNs[7]/1000000,ds.residencyTotalNs[8]/1000000,ds.residencyTotalNs[9]/1000000,
                    ds.residencyTotalNs[10]/1000000,ds.residencyTotalNs[11]/1000000,ds.residencyTotalNs[12]/1000000,
                    ds.residencyTotalNs[13]/1000000,ds.residencyTotalNs[14]/1000000);
                const A830HfiStats &hs=g830.hfi.stats();
                qualLog("R3b ACKS at-us=%llu a1=%u a2=%u a3=%u a4=%u a5=%u a6=%u a7=%u a8=%u a9=%u a10=%u a11=%u a12=%u a13=%u a14=%u fail=%u",
                    now/1000,hs.level_acks[1],hs.level_acks[2],hs.level_acks[3],hs.level_acks[4],hs.level_acks[5],hs.level_acks[6],
                    hs.level_acks[7],hs.level_acks[8],hs.level_acks[9],hs.level_acks[10],hs.level_acks[11],hs.level_acks[12],
                    hs.level_acks[13],hs.level_acks[14],hs.vote_failures);
            }
        logAt=now+1000000000ull;
        }
        os().sleep_us(gQualification.r3b()?100000:1000000);
    }
}
void qualificationService(void *,uint64_t now) {
    gQualification.checkLease(now,A830ExitContext::Dcvs);
    const uint64_t at=__atomic_load_n(&gQualificationInjectAt,__ATOMIC_ACQUIRE);
    if(at && now>=at && __atomic_exchange_n(&gQualificationInjectAt,0ull,__ATOMIC_ACQ_REL)) {
        if(gQualification.high() && g830.power_state.votedLevel()>3 &&
           (!gQualification.r3b() || g830.power_state.qualificationEffectiveCap(A830ExitContext::Dcvs)>3) && g830.hfi.armDropAck()) {
            qualLog("R3a injection armed from ACK level=%u at-us=%llu",g830.power_state.votedLevel(),nowUs());
            g830.dcvs.forceQualificationVote();
        } else qualLog("R3a injection refused: high ACK/permit missing at deadline");
    }
}
void qualificationSample(void *,uint64_t now,const Sm8750TsensSample *gpu) {gQualification.sample(now,*gpu);}
IOReturn stageDcvs(Adreno830Device *d,Adreno830Failure *f)
{
    const Adreno830Board *b=d->board;
    adreno830_status_step(A830S10StepPower);
    bindPower(d);
#ifdef ADRENO830_MACOS
    if(qualificationMode(b) && gIdle.mode!=1)return refuse(f,A830S10StepPower,A830ReasonArgument,0,gIdle.mode,1,kIOReturnBadArgument);
#endif
    adreno830_status_step(A830S10StepDcvs);
    A830DcvsConfig config;
    config.qualification607=qualificationMode(b);
    config.qualificationR3b=b->qualification_r3b==1;
    if(b->qualification_exit_test && (!config.qualificationR3b || b->qualification_exit_test!=1))
        return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->qualification_exit_test,1,kIOReturnBadArgument);
    if(b->qualification_r3b && (!config.qualification607 || b->qualification_r3b!=1))
        return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->qualification_r3b,1,kIOReturnBadArgument);
    /* pagani-gpu-daily=1 only on the R3b line with the governor's own floor (160 MHz): not with the exit test or the 607 residency */
    /* gpu/fullopp1: with pagani-gpu-fullopp=1 the floor may also be Android's 222 MHz (min_pwrlevel 12). */
    const bool fullopp=b->qualification_fullopp==1;
    if(b->qualification_daily && (b->qualification_daily!=1 || !config.qualificationR3b || b->qualification_exit_test ||
       qualificationResidency(b) || (b->knobs.dcvs_min_mhz!=160 && !(fullopp && b->knobs.dcvs_min_mhz==222))))
        return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->qualification_daily,1,kIOReturnBadArgument);
    if(b->qualification_fullopp && (!fullopp || !config.qualificationR3b))
        return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->qualification_fullopp,1,kIOReturnBadArgument);
    config.governor=b->knobs.dcvs!=0;
    /* gpu/fullopp1 (ANALYSIS.md 3.2): pagani-gpu-governor=2, KGSL's open constants: 10 ms windows on the timer (no TE windows), the idle
     * rule at 80 ms (the idle timer), no 734 MHz sustain limit, the race to the top without needing the input floor. */
    if(b->dcvs_governor) {
        if(b->dcvs_governor!=2)return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->dcvs_governor,2,kIOReturnBadArgument);
        config.kgsl=true;config.windowMs=10;config.idleMs=80;config.sustainKhz=0;
    }
    if(b->input_floor_mhz && (b->input_floor_mhz<160 || b->input_floor_mhz>1100))
        return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->input_floor_mhz,1100,kIOReturnBadArgument);
    config.inputFloorKhz=b->input_floor_mhz*1000u;
    // Use the 342 MHz control without a load generator; thermal and sticky limits still apply.
    config.minMhz=b->qualification_exit_test?342:b->knobs.dcvs_min_mhz;
    config.maxMhz=b->knobs.dcvs_max_mhz;
    config.maxGiven=(b->flags&(1u<<Adreno830KnobDcvsMax))!=0;
    config.maxLevel=uint8_t(b->knobs.maxlevel);   /* pagani-gpu-maxlevel: the ceiling level (S0 checked <= 16) */
    config.thermalTargetDeci=int32_t(b->knobs.ttarget)*10;   /* pagani-gpu-ttarget (S0 checked 50..100 C) */
    config.fixedLevel=uint8_t(b->knobs.perf);
    config.acd=d->acd_running;   /* P3 (pagani-gpu-acd=1): the AOP took val 1 and the GMU acknowledged the ACD table */
    config.abVote=d->hfi.abSupported();
    config.initialLevel=d->level;config.initialBus=d->bus;
    config.hostBootKbps=adreno830_ddr_kbps(b->knobs.ddr_boot);
    config.hostFloorKbps=adreno830_ddr_kbps(b->knobs.ddr_floor);
    const A830DcvsTable table={d->votes.level,d->votes.levels,b->ddr_kbps,b->ddr_levels};
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (A830Idle.h): from piano-gpu-idle=1 on no wake boost (an isolated frame runs at the floor) and the idle window;
     * from 2 on the idle votes. */
    if(gIdle.mode>=1) {config.wakeBoostKhz=0;config.idleWindowMs=gIdle.windowMs;}
    gIdleVotes=A830IdleVotes{};gIdleReport=IdleReport{};
    gIdleDdrFloorKbps=config.hostFloorKbps;
    /* Mode 2: the boot bandwidth vote goes now that the GMU votes the GPU's own (S5, and at every wake): the host DDR vote falls to
     * the display's floor for the rest of the boot, unless pagani-gpu-ddr-floor chose a floor. The DCVS thread is not running yet:
     * this thread still owns the host votes. A failed vote keeps the boot vote (the line says so). */
    if(gIdle.mode>=2 && !(b->flags&(1u<<Adreno830KnobDdrFloor))) {
        const uint32_t floor=adreno830_ddr_kbps(gIdle.ddrLevel);
        if(floor && floor<d->host.ddr_kbps) {
            A830IdleEvent e{};e.kind=A830IdleEventFloor;
            e.result=Sm8750RpmhHostDdr(&d->host,floor);
            if(e.result==Sm8750RpmhOk)gIdleDdrFloorKbps=config.hostFloorKbps=floor;
            idleEvent(e);
        }
    }
    A830DcvsOps ops={nullptr,dcvsPerfVote,dcvsHostDdr,dcvsPollFaults,dcvsMemoryRow,dcvsQueueRow,
                           gIdle.mode>=2?dcvsIdleTick:nullptr};
#else
    A830DcvsOps ops={nullptr,dcvsPerfVote,dcvsHostDdr,dcvsPollFaults,dcvsMemoryRow,dcvsQueueRow};
#endif
    d->tsens_io={nullptr,tsensRead};
    if(config.qualification607) {
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
        /* the board allocated it under the same rule (A830QualificationTop.h, board/PianoA830.cpp PianoA830::start) */
        if(!adreno830_qualification_log_ready())return refuse(f,A830S10StepDcvs,A830ReasonNoMemory,0,0,0,kIOReturnNoMemory);
#endif
        gQualificationEnabled=true;
        d->recovery.qualification(qualificationUnsafe,nullptr);
        const bool residency607=qualificationResidency(b);
        gQualification.init(&d->power_state,&d->recovery,config.qualificationR3b,qualificationUnsafe,residency607);
        if(fullopp)gQualification.fullopp();   /* Android's thermal table, tops to 14 (before setTop) */
        if(!gQualification.setTop(qualificationTop(b)))return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,b->knobs.maxlevel,9,kIOReturnBadArgument);
        qualLog("R3b TOP v1 level=%u mhz=%u bus=%u",unsigned(gQualification.top()),unsigned(b->knobs.dcvs_max_mhz),
                unsigned(A830Qualification::defaultBus(gQualification.top())));
        if(config.qualificationR3b)a830ExitTrace.start(d->power_env.nowNs,d->power_env.context);
        if(residency607)qualLog("R3b residency qualification window-s=625 floor-mhz=607");
        if(b->qualification_daily) {
            gQualification.daily();
            qualLog("R3b DAILY v1 auto-arm=1 window=none floor-mhz=%u max-mhz=%u",unsigned(config.minMhz),unsigned(config.maxMhz));
        }
        if(config.qualificationR3b)qualLog("R3b CONFIG R3b-607-v1 soft=800 cool=750 hard=850 halt=1050 floor=3 guard-ms=100 lease-ms=250 down-ms=500 up-ms=2000 board-hold=62000,37000 board-release=60000,35000 board-hard=65000,40000");
        if(config.qualificationR3b)qualLog("R3b CXGATE v2 ahb=enable-or-running gmu=clk-or-noc gdsc=pwr-on");
        if(fullopp) {
            const auto &t=gQualification.thermal();
            qualLog("R3b FULL v1 top=%u mhz=%u acd=%s soft=%d cool=%d hard=%d halt=1050 skin25=%u,%d,%d skin45=%u,%d,%d xo=%u,%d,%d "
                    "xo-hard=%d governor=%s floor-mhz=%u",unsigned(gQualification.top()),unsigned(b->knobs.dcvs_max_mhz),
                    d->acd_running?"on":b->knobs.acd?"refused":"off",int(t.softDeci),int(t.coolDeci),int(t.hardDeci),
                    unsigned(t.Skin25Level),int(t.Skin25On),int(t.Skin25Off),unsigned(t.Skin45Level),int(t.Skin45On),int(t.Skin45Off),
                    unsigned(t.Skin45Level),int(t.XoOn),int(t.XoOff),int(t.XoHard),
                    config.kgsl?"kgsl":"r3b",unsigned(config.minMhz));
        }
        d->hfi.qualification(true);ops.qualificationFault=qualificationVoteFault;
        if(config.qualificationR3b) {
            d->hfi.qualificationLimiter([](void *p,uint8_t &level,uint8_t &bus) {
                static_cast<A830PowerState *>(p)->limitQualification(level,bus,A830ExitContext::Hfi);
            },&d->power_state);
            ops.qualificationAck=[](void *,uint8_t &level,uint8_t &bus) {level=g830.hfi.currentLevel();bus=g830.hfi.currentBus();};
        }
        ops.qualificationThermal=[](void *) {return gQualification.thermalSnapshot();};
        ops.qualificationService=qualificationService;ops.qualificationSample=qualificationSample;
        if(!os().thread_start || os().thread_start(qualificationThread,nullptr)!=kIOReturnSuccess)
            return refuse(f,A830S10StepDcvs,A830ReasonNoMemory,0,0,0,kIOReturnNoMemory);
    }
    IOReturn r=d->dcvs.init(config,table,d->power_win,d->power_env,ops,&d->power_state,&d->recovery,
                            mapped(A830WinTsensTm)?&d->tsens_io:nullptr);
    if(r!=kIOReturnSuccess)return refuse(f,A830S10StepDcvs,A830ReasonArgument,0,r,0,r);
    /* pagani-gpu-dcvs=0: one GX_BW_PERF_VOTE {perf, bus} and the level stays; =1: the governor. The service thread starts
     * in both modes (recovery, watchdog, fault polls, the status rows). */
    if((r=d->dcvs.start())!=kIOReturnSuccess)
        return refuse(f,A830S10StepDcvs,A830ReasonHfiStatus,b->knobs.perf,r,busOfLevel(uint8_t(b->knobs.perf)),r);
    return kIOReturnSuccess;
}

using Stage=IOReturn (*)(Adreno830Device *,Adreno830Failure *);
const Stage stages[A830StageCount]={stageInputs,stagePower,stageSecureCx,stageSmmu,stageFirmware,stageGmu,stageCp,stageZap,
                                    stageFence,stageMetal,stageDcvs};

bool stopAfter(const Adreno830Board *board,uint8_t stage) {return board->knobs.stop!=Adreno830StopNever && stage>=board->knobs.stop;}

/* A failure after S3: the claimed streams back to FAULT and the banks disabled, so no live route points at tables nobody
 * maintains (4.5.7). Not after S9 published: the services own the device then. */
void rollbackStreams(uint8_t stage)
{
    if(g830.smmu_claimed && stage>=A830SSmmu && !g830.published) {
        g830.smmu.releaseAll();
        g830.smmu_claimed=false;
    }
}

/* S0..S10 in order: the first failure stops the bring-up (written once the late GMU report and the stream rollback are
 * done), pagani-gpu-stop stops after its stage, else the GPU runs. Each stage gets its begin and end trace lines. */
IOReturn runStages(const Adreno830Board *board)
{
    for(uint8_t stage=A830SInputs;stage<A830StageCount;++stage) {
        adreno830_status_begin(stage);
        traceStageBegin(stage);
        const uint64_t begin=nowUs();
        Adreno830Failure failure={};
        const IOReturn result=stages[stage](&g830,&failure);
        const uint64_t elapsed=nowUs()-begin;
        adreno830_status_end(stage,uint32_t(elapsed>UINT32_MAX?UINT32_MAX:elapsed));
        const bool stop=result==kIOReturnSuccess && stopAfter(board,stage);
        traceStageEnd(stage,result,failure,elapsed,stop);
        if(result!=kIOReturnSuccess) {
            failure.stage=stage;failure.ior=result;
            g830.failure=failure;
            /* S6-S8 (annibale): the GMU's late report is drained while its stream still routes, before the streams FAULT
             * and the failure is written. */
            if(stage>=A830SCp && stage<=A830SFence)drainHfiLate();
            rollbackStreams(stage);
            adreno830_status_fail(stage,failure.step,failure.reason,result,failure.reg,failure.value,failure.expect);
            return result;
        }
        if(stop) {
            adreno830_status_stop(stage,A830ReasonStopped);
            return kIOReturnSuccess;
        }
    }
    g830.running=true;
    adreno830_status_running();
    return kIOReturnSuccess;
}
}

/* ===== Entry and accessors =========================================================================================== */
extern "C" IOReturn adreno830_start(const Adreno830Board *board)
{
    if(__atomic_exchange_n(&g830.started,true,__ATOMIC_ACQ_REL))return kIOReturnBusy;
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
    if(!cmdDbHeap)cmdDbHeap=IONewZeroData(Sm8750CmdDb,1);
    if(!cmdDbHeap)return kIOReturnNoMemory;
#endif
    g830.board=board;g830.started_us=nowUs();
    adreno830_status_reset();
    const bool known=board && board->magic==Adreno830BoardMagic && board->version==Adreno830BoardVersion;
    adreno830_status_mode(known?board->knobs.status:1);
    adreno830_status_diag(known && board->system_diag);
    if(!osTable || !osTable->now_us || !osTable->map) {
        Adreno830Failure failure={};
        refuse(&failure,A830S0StepBoard,A830ReasonArgument,0,0,0,kIOReturnNotReady);
        adreno830_status_fail(A830SInputs,failure.step,failure.reason,failure.ior,0,0,0);
        return kIOReturnNotReady;
    }
    /* pagani-gpu-trace: the bring-up is the first trace scope (A830Trace.h); its last line names the outcome. */
    traceOpen(board);
    const IOReturn result=runStages(board);
    traceClose(result);
    return result;
}

const Adreno830Os *adreno830_os(void) {return osTable;}
const Adreno830Mmio *adreno830_window(unsigned id) {return id<A830WinCount?&g830.win[id]:nullptr;}
const Adreno830Board *adreno830_board(void) {return g830.board;}
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
const Sm8750CmdDb *adreno830_cmd_db(void) {return cmdDbHeap && cmdDbSnapshot.bytes?&cmdDbSnapshot:nullptr;}
#else
const Sm8750CmdDb *adreno830_cmd_db(void) {return cmdDbSnapshot.bytes?&cmdDbSnapshot:nullptr;}
#endif
const Sm8750RpmhResources *adreno830_rpmh_resources(void) {return g830.rpmh.gfx.present?&g830.rpmh:nullptr;}
const Sm8750SocInfo *adreno830_soc_info(void) {return g830.soc.id?&g830.soc:nullptr;}
const Sm8750GpuLevelTable *adreno830_levels(void) {return g830.levels.count?&g830.levels:nullptr;}
const Sm8750RpmhIo *adreno830_drv2_io(void) {return g830.drv2_io.read?&g830.drv2_io:nullptr;}
Sm8750RpmhHost *adreno830_rpmh_host(void) {return g830.host.drv2?&g830.host:nullptr;}
const Sm8750GpuPowerIo *adreno830_power_io(void) {return g830.power_io.read32?&g830.power_io:nullptr;}
Sm8750Scm *adreno830_scm(void) {return g830.scm.ops?&g830.scm:nullptr;}
uint32_t adreno830_ddr_kbps(unsigned level)
{
    const Adreno830Board *b=g830.board;
    return b && b->ddr_kbps && level<b->ddr_levels?b->ddr_kbps[level]:0;
}
const Adreno830Failure *adreno830_last_failure(void) {return &g830.failure;}
bool adreno830_running(void) {return __atomic_load_n(&g830.running,__ATOMIC_ACQUIRE);}

#ifdef ADRENO830_MACOS
/* ===== UniXNU-piano: the system suspend (A830Suspend.h, piano-gpu-suspend=1) ================================================= */
namespace {
/* The host votes as the suspend found them at PreSleep, put back at PostResume (or kOn). */
struct SuspendSaved {bool valid;uint32_t held,mx,cx,ddr;} gSaved;
/* (os() here, inside the unnamed namespace: at file scope the kernel's namespace os makes the name ambiguous) */
void suspendSleepUs(uint32_t us) {os().sleep_us(us);}
void reportVotes(A830SuspendReport *r,bool after)
{
    const Sm8750RpmhHost &h=g830.host;
    const uint32_t ddr=h.held&Sm8750RpmhHeldDdr?h.ddr_kbps:0;
    const uint32_t cx=h.res && (h.held&Sm8750RpmhHeldRails)?railVlvl(h.res->cx,h.cx_hlvl):0;
    const uint32_t mx=h.res && (h.held&Sm8750RpmhHeldRails)?railVlvl(h.res->mx,h.mx_hlvl):0;
    if(after) {r->ddrAfter=ddr;r->cxAfter=cx;r->mxAfter=mx;}
    else {r->ddrBefore=ddr;r->cxBefore=cx;r->mxBefore=mx;}
}
}

/* Step 1 (kOff): the gate, the quiet, the full slumber (A830PowerState::suspend). */
extern "C" int32_t adreno830_suspend_gpu(uint32_t idleWaitMs,A830SuspendReport *report)
{
    A830SuspendReport local{};
    A830SuspendReport *r=report?report:&local;
    *r=A830SuspendReport{};
    if(!g830.power_bound || !adreno830_running()) {r->result=kIOReturnNotReady;return r->result;}
    /* A recovery reported and not yet run (the DCVS thread runs it, and it would bring the GPU up again under the shut gate): waited
     * out first, within the same quiet time. */
    const uint64_t begin=nowUs(),budgetUs=uint64_t(idleWaitMs)*1000u;
    while(g830.recovery.pending() && nowUs()-begin<budgetUs)suspendSleepUs(1000);
    const uint64_t spent=nowUs()-begin;
    if(g830.recovery.pending()) {
        r->result=kIOReturnBusy;r->waitedUs=uint32_t(spent);r->powerPhase=g830.power_state.phase();
        return r->result;
    }
    A830PowerState::SuspendInfo info{};
    IOReturn result=g830.power_state.suspend(uint32_t(budgetUs>spent?budgetUs-spent:1000u),&info);
    if(result==kIOReturnSuccess && g830.recovery.pending()) {
        /* Reported while the GPU went down: the gate open again and the recovery's turn; this suspend goes on without the GPU. */
        (void)g830.power_state.resume(false,nullptr);
        result=kIOReturnBusy;
    }
    info.waitedUs+=uint32_t(spent);
    r->result=result;r->waitedUs=info.waitedUs;r->entryUs=info.entryUs;r->warnings=info.warnings;
    r->alreadyAsleep=info.alreadyAsleep;
    r->cxCollapsed=result==kIOReturnSuccess && !g830.power_state.cxOn() && !(info.warnings&Sm8750GpuOffCxStillOn);
    r->powerPhase=g830.power_state.phase();
    reportVotes(r,false);reportVotes(r,true);
    return result;
}

/* Step 2 (PreSleep): the DDR to the keepalive minimum or the display's floor, then the rails (A830Suspend.h). Only with the GPU
 * suspended; a failure puts back what went down. */
extern "C" int32_t adreno830_suspend_votes(uint8_t scanout,const A830SuspendConfig *config,A830SuspendReport *report)
{
    A830SuspendReport local{};
    A830SuspendReport *r=report?report:&local;
    *r=A830SuspendReport{};
    Sm8750RpmhHost &h=g830.host;
    if(!config || !g830.power_bound || !h.drv2 || !h.res || !g830.power_state.suspended()) {r->result=kIOReturnNotReady;return r->result;}
    HostVotes votes;
    const uint64_t begin=nowUs();
    reportVotes(r,false);
    gSaved={true,h.held,h.mx_hlvl,h.cx_hlvl,h.ddr_kbps};
    int rc=Sm8750RpmhOk;
    if(h.held&Sm8750RpmhHeldDdr) {
        const uint32_t ddr=A830SuspendDdrKbps(h.ddr_kbps,scanout,adreno830_ddr_kbps(config->ddrFloorLevel));
        if(ddr!=h.ddr_kbps)rc=Sm8750RpmhHostDdr(&h,ddr);
    }
    if(!rc && (h.held&Sm8750RpmhHeldRails)) {
        const uint32_t vlvl=A830SuspendRailVlvl(config->railVlvl,scanout);
        const uint32_t mx=A830SuspendRailHlvl(h.res->mx.vlvl,h.res->mx.count,h.mx_hlvl,vlvl);
        const uint32_t cx=A830SuspendRailHlvl(h.res->cx.vlvl,h.res->cx.count,h.cx_hlvl,vlvl);
        rc=Sm8750RpmhHostRailsSet(&h,mx,cx);
    }
    if(rc) {
        /* What went down comes back (rails first, then the DDR), best effort; the suspend goes on with the votes as they were. */
        (void)Sm8750RpmhHostRailsSet(&h,gSaved.mx,gSaved.cx);
        if((gSaved.held&Sm8750RpmhHeldDdr) && h.ddr_kbps!=gSaved.ddr)(void)Sm8750RpmhHostDdr(&h,gSaved.ddr);
        gSaved.valid=false;
        r->rpmh=rc;r->result=kIOReturnIOError;
    }
    r->votesUs=uint32_t(nowUs()-begin);
    reportVotes(r,true);
    r->powerPhase=g830.power_state.phase();
    return r->result;
}

/* Step 3 (PostResume) or 4 (kOn): the rails as found, then the DDR. Nothing to do when nothing went down. */
extern "C" int32_t adreno830_resume_votes(A830SuspendReport *report)
{
    A830SuspendReport local{};
    A830SuspendReport *r=report?report:&local;
    *r=A830SuspendReport{};
    Sm8750RpmhHost &h=g830.host;
    if(!gSaved.valid) {reportVotes(r,false);reportVotes(r,true);return kIOReturnSuccess;}
    HostVotes votes;
    const uint64_t begin=nowUs();
    reportVotes(r,false);
    int rc=Sm8750RpmhHostRailsSet(&h,gSaved.mx,gSaved.cx);
    if(!rc && (gSaved.held&Sm8750RpmhHeldDdr) && h.ddr_kbps!=gSaved.ddr)rc=Sm8750RpmhHostDdr(&h,gSaved.ddr);
    if(!rc)gSaved.valid=false;
    r->rpmh=rc;r->result=rc?kIOReturnIOError:kIOReturnSuccess;
    r->votesUs=uint32_t(nowUs()-begin);
    reportVotes(r,true);
    return r->result;
}

/* Step 4 (kOn): the gate open, the GPU woken now with `wake` (A830PowerState::resume). */
extern "C" int32_t adreno830_resume_gpu(bool wake,A830SuspendReport *report)
{
    A830SuspendReport local{};
    A830SuspendReport *r=report?report:&local;
    *r=A830SuspendReport{};
    if(!g830.power_bound) {r->result=kIOReturnNotReady;return r->result;}
    uint32_t us=0;
    r->result=g830.power_state.resume(wake,&us);
    r->wakeUs=us;
    r->powerPhase=g830.power_state.phase();
    reportVotes(r,false);reportVotes(r,true);
    return r->result;
}

extern "C" void adreno830_host_votes(uint32_t *ddrKbps,uint32_t *cxVlvl,uint32_t *mxVlvl,bool *votesDown)
{
    A830SuspendReport r{};
    reportVotes(&r,true);
    if(ddrKbps)*ddrKbps=r.ddrAfter;
    if(cxVlvl)*cxVlvl=r.cxAfter;
    if(mxVlvl)*mxVlvl=r.mxAfter;
    if(votesDown)*votesDown=gSaved.valid;
}
#endif

#ifdef ADRENO830_HOST_TEST
/* Host tests: a fresh device (the static object is rebuilt in place) and the host's kernel services. */
extern "C" void adreno830_start_reset(void)
{
    g830.~Adreno830Device();
    new (&g830) Adreno830Device();
    gQualification=A830Qualification{};gQualificationEnabled=false;
    gQualificationInjectAt=0;gQualificationInjectionScheduled=false;gQualificationExitTest=0;
#ifdef ADRENO830_MACOS
    gSaved=SuspendSaved{};   /* UniXNU-piano */
#endif

    memset(&cmdDbSnapshot,0,sizeof cmdDbSnapshot);
    traceReset();
}
void adreno830_set_os(const Adreno830Os *table) {osTable=table;}
Adreno830Device *adreno830_device_for_test(void) {return &g830;}
#endif

// Flags: enabled/ready/high/sticky, then reason in bits 8+. Commands 3/4 need the R3b exit-test boot arg.
extern "C" int adreno830_qual607_control(int command)
{
    if(!gQualificationEnabled)return -1;
    if((command==1 || command==2) && g830.board->qualification_exit_test)return -2;
    if(command==1) {if(!gQualification.arm())return -2;qualLog("R3a permit requested");}
    else if(command==2) {
        if(!gQualification.high() || g830.power_state.votedLevel()<=3 ||
           __atomic_exchange_n(&gQualificationInjectionScheduled,true,__ATOMIC_ACQ_REL))return -2;
        // Schedule on the device clock. SSH latency cannot shorten the600-second normal soak or consume its2-second tail.
        const uint64_t at=gQualification.openedAt()+600000000000ull;
        __atomic_store_n(&gQualificationInjectAt,at,__ATOMIC_RELEASE);
        qualLog("R3a injection scheduled for-us=%llu",at/1000);
    } else if(command==3 || command==4) {
        if(!g830.board->qualification_exit_test)return -2;
        uint32_t expected=uint32_t(command-3);
        if(!__atomic_compare_exchange_n(&gQualificationExitTest,&expected,uint32_t(command),false,
                                       __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -2;
    } else if(command!=0)return -2;
    return 1|(gQualification.ready()?2:0)|(gQualification.high()?4:0)|(g830.power_state.qualificationLatched()?8:0)|int(gQualification.reason()<<8);
}

extern "C" void adreno830_qualification_display(const PianoGpuQualificationDisplay *d) {
    if(!d || !gQualificationEnabled || !gQualification.r3b())return;
    qualLog("R3b DISPLAY at-us=%llu period-us=%llu submitted=%llu shown=%llu dropped=%llu copies=%llu mean-us=%llu max-us=%llu prepare-us=%llu read-us=%llu convert-us=%llu write-us=%llu",
        d->atUs,d->periodUs,d->submitted,d->shown,d->dropped,d->copies,d->meanUs,d->maxUs,d->prepareUs,d->readUs,d->convertUs,d->writeUs);
}

/* gpu/throttle1: the board's side of the skin table, the ceiling holder and machdep.piano_gpu_state (A830Bind.h). */
extern "C" void adreno830_set_skin_table(uint32_t table) {__atomic_store_n(&gSkinTable,table==1?1u:0u,__ATOMIC_RELEASE);}
extern "C" uint32_t adreno830_skin_table(void) {return __atomic_load_n(&gSkinTable,__ATOMIC_ACQUIRE);}
extern "C" void adreno830_note_ceiling_holder(uint32_t client) {__atomic_store_n(&gCeilingHolder,client,__ATOMIC_RELEASE);}
extern "C" size_t adreno830_gpu_state(char *out,size_t size)
{
    if(!out || !size)return 0;
    const uint32_t at=__atomic_load_n(&gGpuStateAt,__ATOMIC_ACQUIRE)&1u;
    if(!gGpuState[at][0])snprintf(out,size,"none (the fullopp guard has not reported yet)");
    else {strlcpy(out,gGpuState[at],size);}
    return strlen(out);
}
