/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_STATUS_H
#define NATIVE_ADRENO_STATUS_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* IOReturn values the shared (host-tested) code returns beyond the host fixture's set. They carry the kernel's own values
 * (IOKit/IOReturn.h); host tests compare against these names, the kernel build checks them against IOReturn.h below. */
static constexpr IOReturn NativeAdrenoReturnNotResponding=IOReturn(0xe00002edu);   /* kIOReturnNotResponding: the device is lost */
static constexpr IOReturn NativeAdrenoReturnAborted=IOReturn(0xe00002ebu);          /* kIOReturnAborted: the connection is closing */
static constexpr IOReturn NativeAdrenoReturnNotPermitted=IOReturn(0xe00002e2u);     /* kIOReturnNotPermitted: a name refused for hangs */
#if defined(kIOReturnNotResponding) && defined(kIOReturnAborted)
static_assert(NativeAdrenoReturnNotResponding==kIOReturnNotResponding && NativeAdrenoReturnAborted==kIOReturnAborted,
              "NativeAdreno status codes are IOReturn.h's");
#endif
#if defined(kIOReturnNotPermitted)
static_assert(NativeAdrenoReturnNotPermitted==kIOReturnNotPermitted,"NativeAdreno status codes are IOReturn.h's");
#endif
/* The client ranges a recovery or a fault names, bit r for range r (NativeAdrenoSubmit.h's NativeAdrenoSubmitRanges, the publish shell's
 * connection slots). UniXNU-piano (2026-11-22, GPU-SLOTS): 64 bits on macOS (64 slots), 32 upstream. */
#ifdef ADRENO830_MACOS
typedef uint64_t NativeAdrenoRangeMask;
#else
typedef uint32_t NativeAdrenoRangeMask;
#endif

/* Monotonic fault and suspend counters of ONE connection, for Turnip's device_check_status and device_get_suspend_count (GetStatus).
 * Turnip compares the fault count with the value it saw last: a change means its context is lost (vk_device_set_lost). Counts are
 * per connection from M7 on (ARCHITECTURE 4.13.4, 6.3): a GPU page fault is counted only on the connection whose GPU window holds
 * the faulting address, a global fault (hang class) on every connection, and a recovery counts a suspend on every connection and a
 * fault on each one that had work in flight. A connection starts at zero when it is mounted (attachStatus). This object only counts
 * what the publish shell tells it; it performs no detection, reset or recovery and never clears a count, so a client cannot hide a
 * fault by re-reading. Interrupt bottom halves count while GetStatus reads, so every access is a relaxed atomic. */
struct NativeAdrenoStatusCounts {
    uint64_t faults;
    uint64_t suspends;
};
class NativeAdrenoStatus final {
    uint64_t faults_=0,suspends_=0;
public:
    void noteFault() {__atomic_fetch_add(&faults_,1,__ATOMIC_RELAXED);}
    void noteSuspend() {__atomic_fetch_add(&suspends_,1,__ATOMIC_RELAXED);}
    uint64_t faults() const {return __atomic_load_n(&faults_,__ATOMIC_RELAXED);}
    uint64_t suspends() const {return __atomic_load_n(&suspends_,__ATOMIC_RELAXED);}
    NativeAdrenoStatusCounts counts() const {return {faults(),suspends()};}
};

/* Device-level fault plumbing of the publish shell (NativeAdrenoPublish.cpp), for the GPU interrupt and SMMU bottom halves, the
 * watchdog and the recovery (ARCHITECTURE 4.12.3, 4.13.4). All of them run in thread context (thread_call or a kernel thread),
 * never in an interrupt handler: they take the publish shell's locks. Before native_adreno_publish() they only count. */
#ifdef __cplusplus
extern "C" {
#endif
/* An SMMU context fault on the GPU bank (CB0) at GPU address far: the connection whose window holds far gets faults += 1 (that
 * process alone loses its device; the GPU keeps serving everybody else). Returns true when a connection took it, false for an
 * address below 0x20000000 (kernel buffers), outside every window, or in a free window: those are only recorded (smmu_faults in
 * adreno830_status is the SMMU code's). Faults on the GMU bank (CB2) must not come here. Never makes the device lost. completed and
 * issued: the completed fence and the last one issued as the interrupt's top half read them (sampled; false for a fault the poll
 * found): the jobs in flight between them are compared with the FAR window and counted (M14 P0, NativeAdrenoFaultVerdictFor,
 * native_adreno_fault_attribution), never charged. */
bool native_adreno_note_page_fault(uint64_t far,uint32_t completed,uint32_t issued,bool sampled);
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-21, 20261121-ws-crash CROSS-WINDOW.txt 4(a) D2): the state at the bottom half that found a CB0 fault (the poll:
 * up to a tick after the fault, FAR is sticky and the GPU kept running), for its "gpu fault detail" lines: the fences now and, when GX
 * was up (ibs), BR's [0] and BV's [1] IB1/IB2 base and dwords left and the BR ring read pointer. index: the fault's line number this
 * boot (1..lines). native_adreno_note_fault_detail adds whose jobs are in flight, what is mapped at FAR and FAR - 4 GiB and what the
 * command words at the IBs hold; it prints only, after native_adreno_note_page_fault, in the same thread context. */
struct NativeAdrenoFaultDetail {
    uint64_t far;
    uint32_t index,lines,completed,issued;
    bool ibs;
    uint32_t rptr;
    uint64_t ib1[2],ib2[2];
    uint32_t ib1_rem[2],ib2_rem[2];
};
void native_adreno_note_fault_detail(const NativeAdrenoFaultDetail *detail);
#endif
/* Global device loss, hang class only (RBBM HANG_DETECT, CP_HW_ERROR, the 2 s software watchdog, GMU CM3 fault / watchdog bite,
 * the WPTR fenced write failing 200 times; ARCHITECTURE 4.13.4): every connection's faults += 1, every later submission fails with
 * kIOReturnNotResponding, waiters return it, and newUserClient refuses new GPU connections. reason is an A830Reason (recorded).
 * From M10 the orchestrator tries native_adreno_recovery_begin/end first and calls this only when recovery is not possible. */
void native_adreno_device_lost(uint32_t reason);
/* M10 minimal recovery (ARCHITECTURE 4.13.4), around the GPU restart that A830Recovery performs:
 * begin: no new submission reaches the ring (submitters wait, at most their 200 ms bound, then get Busy), new connections wait, and
 *        every connection with a job in flight gets faults += 1 (its jobs are lost), whatever the cause. Call it before halting GBIF.
 *        charge_culprit: the cause is one a client's job can produce (RBBM hang detect, CP_HW_ERROR, the software watchdog;
 *        A830FaultBlamesClient), so the process whose job the CP stood in (NativeAdrenoSubmit::halt) is charged one hang against
 *        NativeAdrenoClientHangLimit; a GMU, HFI, fenced-write, slumber, wake or self-test fault charges nobody.
 * end(true): after the ring was restarted and the memptrs fence set to the last issued seqno: every job's references are released
 *        (the GPU was power cycled, nothing of them runs any more), every connection gets suspends += 1, and submissions and
 *        connections are accepted again. end(false): the recovery failed or was the third in 60 s; the loss becomes permanent
 *        (native_adreno_device_lost semantics). */
void native_adreno_recovery_begin(bool charge_culprit);
void native_adreno_recovery_end(bool recovered);
/* UniXNU-piano (2026-11-22, private 20261122-render/ANALYSIS.md "ONE change"; Linux msm recover_worker is the behaviour reference): the
 * replaying recovery (A830RecoveryOps::plan/replay) in place of begin's faulting:
 * begin_replay: begin without faulting or charging anybody (the halt and the gate only).
 * plan: once the GPU stopped (NativeAdrenoSubmit::plan): for charge_culprit, the process whose job the CP stood in is charged one hang
 *        (never the render server) and its connection alone gets faults += 1 (Turnip: that process's device is lost); *restart gets
 *        the fence S6 restarts at (untouched before the shell is published). Every other job in flight is kept for the replay; when it
 *        cannot be (a poisoned submit object, a kernel fence among them, no room) every connection with a job in flight is faulted as
 *        begin does. Returns the jobs that will not run again. One console line names the culprit, the faulted and the replayed.
 * replay: after S7 and the interrupt restore (NativeAdrenoSubmit::replay): the kept jobs go into the ring again with their own fences;
 *        *replayed gets how many. end(true) after a replaying begin releases nothing (replay did) and resumes as before. */
void native_adreno_recovery_begin_replay(void);
uint32_t native_adreno_recovery_plan(bool charge_culprit,uint32_t *restart);
IOReturn native_adreno_recovery_replay(uint32_t *replayed);
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-22, test only: piano-gpu-fault-test=1, machdep.piano_gpu_fault_hold): the next submission of any live connection
 * of process pid starts with the hold (NativeAdrenoSubmit::armHold): the CP stops in that job, a real hang whose culprit is known.
 * Returns the connections armed (0: none of that process; nothing armed); name (optional, 20 bytes) gets the process's name. */
unsigned native_adreno_fault_hold(int pid,char *name);
#endif
/* retire_call of the GPU interrupt (fence interrupt, ARCHITECTURE 4.12.3 step 3 and 5.4 "回收"): releases the BO references of every
 * job the CP has passed, so completed work never pins buffers or job slots. Cheap when nothing completed. */
void native_adreno_retire_completed(void);
#ifdef __cplusplus
}
#endif
#endif
