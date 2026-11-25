/* SPDX-License-Identifier: MIT */
#ifndef A830_RECOVERY_H
#define A830_RECOVERY_H
#include "A830PowerState.h"

/* Hang detection and recovery of the A830 (ARCHITECTURE 4.13.4, IMPLEMENTATION_PLAN M10 and M13 b).
 *
 * Global device loss (hang-class faults only; SMMU page faults never come here, they cost the one connection its FAR
 * belongs to): RBBM_INT_0_STATUS HANG_DETECT or CP_HW_ERROR, the software watchdog (2 s without progress while work is
 * in flight), a GMU fatal (CM3 fault bit 15, WDOG_BITE), a WPTR fenced write dropped 200 times, a failed slumber or
 * wake. Every such report goes to the minimal recovery first:
 *   gate (no submission, no new connection) -> RBBM_INT_0_MASK = 0 -> snapshot into the status block -> the in-flight
 *   jobs fail and their connections get faults++ -> GBIF halt (Linux a8xx_bus_clear_pending_transactions: GX side,
 *   then client, then arbiter, then release) -> A830Gmu::forceOff -> [CX powered again only if it was off] -> S4/S5
 *   (GMU firmware reload; the CX timer is not re-seeded while CX stayed up; cold boot with OOB and the GX power check)
 *   -> S6 (hw_init + CP_ME_INIT, the ring empty and the memptrs fence at the last submitted seqno) -> S7 resume
 *   (CP_SET_SECURE_MODE 0 only: the ZAP stays resident; GMEM_PROTECT, HWCG, OOB clear; the annibale profile keeps
 *   OOB_GPU set, as its first bring-up did) -> fence waiters woken ->
 *   interrupt lines and the storm-guarded ones re-enabled (armcpu_spi_enable) -> gate open, "GPU RUN".
 * The SMMU contexts are not touched while CX stayed up. A failed recovery, or the 3rd report within 60 s, makes the
 * loss permanent: the gate stays closed (newUserClient refuses, every connection is faulted), "GPU LOST" stays.
 * In-flight jobs are failed, not replayed (design 4.13.4) -- unless the replay ops are there (UniXNU-piano 2026-11-22, private
 * 20261122-render/ANALYSIS.md; Linux msm recover_worker is the behaviour reference): then nobody is faulted at the gate; once the GMU
 * is forced off the plan op names the culprit (a client's cause only: the job the CP stood in), faults its connection alone and
 * gives the fence S6 restarts at (the culprit's job counts as completed); after S7 and the interrupt restore the replay op puts
 * every other job in flight into the ring again, with its own fence, before the gate opens. Innocent clients (WindowServer first)
 * keep their device and their frames. adreno830_recovery_set_replay(0) (piano-gpu-recovery-replay=0) restores the old behaviour.
 *
 * report() may be called from any context, interrupt context included (two atomics and env.wake); the recovery itself
 * runs on the service thread (A830Dcvs owns it) or synchronously through recover() (S8 self-test timeout, tests). */

/* Global registers captured before the reset (GX only when it is powered), plus what A830RecoveryOps::snapshot adds. */
struct A830HangSnapshot {
    uint64_t whenNs;
    uint8_t cause;
    bool cxOn,gxOn;
    uint8_t reserved;
    uint32_t fence,lastSubmitted;
    /* GMU (CX) */
    uint32_t gfxPwrClkStatus,rpmhPowerState,cxBusyStatus,aoStatus,gmu2hostInfo,cm3InitResult,ahbFenceStatus,intUnmasked;
    /* GX */
    uint32_t rbbmStatus,rbbmStatus1,int0Status,cpRptr,cpWptr,cpIntGlobal,cpRlError[2];
    uint64_t ib1Base,ib2Base;
    uint32_t ib1Rem,ib2Rem;
    /* A830RecoveryOps::snapshot: BR and BV pipes, SMMU CB0 and CB2 */
    uint32_t pipeHwFault[2],pipeIntStatus[2],pipeStatus[2],pipeProtect[2];
    uint32_t smmuFsr[2];
    uint64_t smmuFar[2];
    uint64_t detail;
};

/* One pipe's IB1 and IB2 (CP_IB1/IB2_BASE and _REM_SIZE through CP_APERTURE_CNTL_HOST): base and the dwords left. A830Recovery::readIbs
 * fills it, for the hang snapshot (BR) and the fault detail (UniXNU-piano, 2026-11-21: BR and BV at the poll that finds a CB0 fault). */
struct A830IbState {uint64_t ib1Base,ib2Base;uint32_t ib1Rem,ib2Rem;};

/* The gate of the ABI shell (NativeAdrenoPublish/UserClient). */
enum A830Gate : uint8_t {A830GateOpen,A830GateRecovering,A830GateLost};

/* Whether a fault is one a client's job can cause, so the hang is charged to the process whose job the CP stood in
 * (native_adreno_recovery_begin, NativeAdrenoClientHangLimit): RBBM HANG_DETECT, CP_HW_ERROR (a protected register or an
 * illegal packet in an IB) and the software watchdog. A GMU fault or watchdog bite, an HFI exchange, the fenced WPTR write,
 * a slumber or wake and the S8 self-test are the driver's own (unlike hangClass, which counts the self-test as a hang). */
static inline bool A830FaultBlamesClient(uint8_t cause)
{
    return cause==A830FaultHang || cause==A830FaultCpHwError || cause==A830FaultWatchdog;
}

struct A830RecoveryOps {
    void *context;
    /* NativeAdrenoSubmit + NativeAdrenoUserClient: every job not retired fails; each owning connection gets faults++.
     * Returns the jobs failed. */
    uint32_t (*failInflight)(void *context);
    /* NativeAdrenoPublish: Open accepts submissions and new connections; Recovering refuses both (and counts a
     * suspend); Lost refuses both for good and faults every connection. cause is the A830FaultCause of the recovery
     * or loss the gate belongs to (the shell charges a hang only for A830FaultBlamesClient causes). */
    void (*gate)(void *context,uint8_t gate,uint8_t cause);
    void (*fenceSignal)(void *context);                               /* A830Fence::signal: wake every waiter */
    void (*irqRestore)(void *context);                                /* A830Irq: armcpu_spi_enable the storm-guarded lines */
    /* A830Cp: read a _PIPE register through CP_APERTURE_CNTL_HOST under the MMIO lock (aperture written back to 0). */
    IOReturn (*readPipe)(void *context,uint8_t pipe,uint32_t reg,uint32_t *value);
    /* A830Cp/A830Smmu: per-pipe CP fault registers, CB0/CB2 FSR/FAR into the snapshot (may be null). */
    void (*snapshot)(void *context,A830HangSnapshot *snapshot);
    /* UniXNU-piano (2026-11-22): the replay (both or neither; neither = every job in flight fails, as before). plan, once the GPU
     * stopped (after the GMU was forced off, before S4): the jobs the CP passed retire; for a cause a client's job can produce
     * (A830FaultBlamesClient) the job right after the completed fence is the culprit: its connection alone gets faults++ (and a hang
     * against its name); *restart gets the fence S6 restarts at (the culprit's, else the completed one; the last submitted one when
     * the plan falls back to failing every job). Returns the jobs that will not run again. replay, after S7, the fence signal and the
     * interrupt restore: every other job in flight goes into the ring again in fence order with its own fence; *replayed the jobs
     * resubmitted. An error makes the loss final. */
    uint32_t (*plan)(void *context,uint8_t cause,uint32_t *restart);
    IOReturn (*replay)(void *context,uint32_t *replayed);
    /* R3b runs these under the power mutex. The vote must prove a new 160 MHz / bus 2 ACK. */
    IOReturn (*qualificationLowVote)(void *context,bool dropAck)=nullptr;
    IOReturn (*qualificationQuiesce)(void *context)=nullptr;
    bool (*qualificationGxOff)(void *context)=nullptr;
    void (*qualificationStage)(void *context,const char *stage,IOReturn result)=nullptr;
};

struct A830RecoveryConfig {
    bool enabled=true;              /* M10: a global loss is recovered first; false = M7-M9 behaviour (loss is final) */
    bool replay=true;               /* the replay ops are used (and adreno830_recovery_set_replay did not turn it off) */
    uint32_t windowMs=60000;        /* the 3rd report within this window is final */
    uint8_t maxInWindow=2;          /* recoveries allowed within the window */
    uint32_t watchdogPeriodMs=500;  /* progress check period while work is in flight */
    uint32_t watchdogLimitMs=2000;  /* no progress for this long = hang */
    uint32_t gbifTimeoutUs=100000;  /* each GBIF halt acknowledgement */
};

struct A830RecoveryStats {
    uint32_t reports,coalesced,recoveries,failures,watchdogHangs,jobsFailed;
    uint32_t lastMs,maxMs;
    uint8_t lastCause,lostCause;
    uint16_t lostStep;
    IOReturn lostResult;
    uint32_t gbifTimeouts;
    /* UniXNU-piano (2026-11-22): recoveries that replayed (the replay ops ran), jobs replayed, gate-to-open time of the last and the
     * longest recovery in microseconds. */
    uint32_t replayRecoveries,jobsReplayed,lastUs,maxUs;
};
/* The step a failed replay is lost at (stage S6, A830Recovery::recoverLocked): after A830WakeStepIrq. */
enum : uint16_t {A830RecoveryStepReplay=0x18};

class A830Recovery {
public:
    IOReturn init(const A830RecoveryConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops,
                  const A830RecoveryOps &recoveryOps,A830PowerState *power);
    /* Any context: remember the first cause and wake the service thread. Reports during a recovery are coalesced. */
    void report(uint8_t cause,uint64_t detail);
    bool pending() const {return faultPending() || __atomic_load_n(&lowRequest_,__ATOMIC_ACQUIRE);}
    void requestQualificationLow(uint32_t reason,uint64_t now,bool dropAck=false);
    uint64_t protectionAt() const {return __atomic_load_n(&protectionAt_,__ATOMIC_ACQUIRE);}
    bool qualificationLowConfirmed() const {return qualificationSafeLow();}
    /* Service thread: run the pending recovery, if any. */
    IOReturn runPending();
    /* Synchronous recovery of `cause` (takes the power mutex). */
    IOReturn recover(uint8_t cause,uint64_t detail);
    /* Final loss without a recovery attempt (also the M7-M9 path). Takes the power mutex. */
    void declareLost(uint8_t cause,uint64_t detail,IOReturn result);
    /* Service thread, every watchdogPeriodMs while work is in flight (A830Dcvs calls it); returns true on a new hang. */
    bool watchdogTick(uint64_t now);
    bool lost() const {return __atomic_load_n(&lost_,__ATOMIC_ACQUIRE);}
    bool busy() const {return __atomic_load_n(&running_,__ATOMIC_ACQUIRE);}   /* a recovery is running */
    /* The running (or last) recovery replays: decided once at its start (config, ops, adreno830_recovery_set_replay). The gate op
     * reads it to halt the shell without faulting anybody. */
    bool replaying() const {return __atomic_load_n(&replaying_,__ATOMIC_ACQUIRE);}
    const A830HangSnapshot &snapshot() const {return snapshot_;}
    const A830RecoveryStats &stats() const {return stats_;}
    const A830RecoveryConfig &config() const {return config_;}
    void qualification(void (*unsafe)(void *,A830ExitOrigin),void *context) {qualification_=true;unsafe_=unsafe;unsafeContext_=context;}
    // No power or HFI mutex: the independent heartbeat may close the actual submit ring.
    void closeQualification(uint8_t cause=A830FaultHfi);
    void qualificationR3b() {qualificationR3b_=true;}
    /* Daily mode (pagani-gpu-daily=1, gpu-daily2 after gpu-daily1): recovery is the daily line's. A GPU hang/watchdog/CP error
     * keeps the guard open (no close, no latch) and restarts the GPU; no recovery uses the strict force-off; a failed force-off or
     * restart declares the GPU lost instead of the native unsafe exit (PianoThermal's independent shutdown stays). The guard's own
     * exits still take the low transaction (an ordinary blocking 160 MHz vote); raw 105 C stays the only native unsafe exit. */
    void qualificationDaily() {daily_=true;}
    bool qualificationIsDaily() const {return daily_;}
    bool safeOff() const {return __atomic_load_n(&safeOff_,__ATOMIC_ACQUIRE);}
    bool qualificationSafeLow() const {return __atomic_load_n(&qualificationSafeLow_,__ATOMIC_ACQUIRE);}

    /* GBIF halt (4.13.4; KGSL gen8_halt_gbif / Linux a8xx_bus_clear_pending_transactions): returns the acknowledgements
     * that timed out (bit 0 GX, 1 client, 2 arbiter); halts are released in any case. */
    uint32_t haltGbif(bool gx);
    /* Capture the global registers (GX ones only when GFX_PWR_CLK_STATUS says GX is on). */
    void capture(uint8_t cause,uint64_t detail,A830HangSnapshot *out);
    /* A pipe's (A8XX_PIPE_BR, A8XX_PIPE_BV) IB1/IB2 through the readPipe hook; the caller holds GX up (GX registers). A read that
     * fails leaves its field 0; without the hook (before init) everything is 0. Any thread context (readPipe takes the MMIO lock). */
    void readIbs(uint8_t pipe,A830IbState *out) const;

private:
    static constexpr uint8_t Publishing=A830FaultCount;
    bool faultPending() const {
        const auto cause=__atomic_load_n(&pendingCause_,__ATOMIC_ACQUIRE);
        return cause!=A830FaultNone && cause!=Publishing;
    }
    void stage(const char *name,IOReturn result=kIOReturnSuccess) {
        if(rops_.qualificationStage)rops_.qualificationStage(rops_.context,name,result);
    }
    IOReturn recoverLocked(uint8_t cause,uint64_t detail);
    bool qualificationAsleep() const {
        return power_->phase()==A830PowerSlumber && power_->rsccSlept() &&
            (!power_->cxOn() || (rops_.qualificationGxOff && rops_.qualificationGxOff(rops_.context)));
    }
    IOReturn strictOff(bool provenAsleep);
    void minimalSnapshot(uint8_t cause,uint64_t detail);
    void loseLocked(uint8_t cause,uint8_t stage,uint16_t step,IOReturn result);
    bool allowed(uint64_t now);
    uint32_t rd(uint8_t window,uint32_t offset) const {return io_.read32(io_.context,window,offset);}
    void wr(uint8_t window,uint32_t offset,uint32_t value) const {io_.write32(io_.context,window,offset,value);}
    bool pollGpu(uint32_t dword,uint32_t mask,uint32_t timeoutUs);

    A830RecoveryConfig config_{};
    A830PowerIo io_{};
    A830PowerEnv env_{};
    A830PowerOps ops_{};
    A830RecoveryOps rops_{};
    A830PowerState *power_=nullptr;
    A830HangSnapshot snapshot_{};
    A830RecoveryStats stats_{};
    uint8_t pendingCause_=A830FaultNone;
    uint64_t pendingDetail_=0;
    bool lost_=false,running_=false,initialized_=false,replaying_=false;
    bool qualification_=false,qualificationClosed_=false,qualificationR3b_=false,daily_=false;
    bool strictMode() const {return qualificationR3b_ && !daily_;}
    bool closesQualification(uint8_t cause) const {return qualification_ && !(daily_ && A830FaultBlamesClient(cause));}
    uint64_t protectionAt_=0;
    uint32_t lowRequest_=0;
    bool safeOff_=false;
    bool qualificationSafeLow_=false;
    void (*unsafe_)(void *,A830ExitOrigin)=nullptr;
    void *unsafeContext_=nullptr;
    uint64_t history_[4]={};            /* start times of the recent recoveries */
    /* watchdog */
    uint64_t wdLastProgress_=0;
    uint32_t wdFence_=0,wdRptr_=0,wdIb1Rem_=0;
    uint64_t wdIb1_=0;
    bool wdArmed_=false;
};

extern "C" {
/* Fault entry for the other modules (A830Irq fault_call, A830Gmu gmuWork/aoIrq, NativeAdrenoRing fenced write, the S8
 * self-test): forwards to the recovery of the running device; a no-op before S10. Interrupt-safe. */
void adreno830_recovery_report(uint8_t cause,uint64_t detail);
/* 0 open, 1 recovering, 2 lost (A830Gate): what the ABI shell checks before a submission or a new connection. */
uint8_t adreno830_recovery_gate(void);
/* UniXNU-piano (2026-11-22): 1 (the default) the recoveries replay the jobs of innocent clients, 0 every job in flight fails as before
 * (piano-gpu-recovery-replay=0, machdep.piano_gpu_recovery_replay: an A/B switch). Read at each recovery's start; any context. */
void adreno830_recovery_set_replay(uint32_t on);
uint32_t adreno830_recovery_replay_mode(void);
/* The running device's recovery counters (false before init): machdep.piano_gpu_recovery. */
bool adreno830_recovery_stats(A830RecoveryStats *out);
#ifdef ADRENO830_MACOS
/* UniXNU-piano (macOS 27): the hang verdict's snapshot on the console (NativeAdrenoPublish.cpp; a weak no-op in A830Recovery.cpp),
 * called by the orchestrator's snapshot hook once the CP pipe and SMMU registers are in. Thread context (the recovery's). */
void native_adreno_note_hang_snapshot(const A830HangSnapshot *snapshot);
#endif
}

#endif
