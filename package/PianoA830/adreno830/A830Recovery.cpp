/* SPDX-License-Identifier: MIT */
#include "A830Recovery.h"
#include "A830Regs.h"
#include "Adreno830Status.h"

namespace {
constexpr uint64_t NsPerMs=1000000;
A830Recovery *gRecovery;
uint32_t gReplayWanted=1;   /* adreno830_recovery_set_replay */

constexpr uint32_t gpuReg(uint32_t dword) {return A830_GPU_REG_BYTE(dword);}
constexpr uint32_t gmuReg(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}

/* 8.3 reason of a fault cause. */
uint16_t reasonOf(uint8_t cause)
{
    switch(cause) {
    case A830FaultHang: case A830FaultWatchdog: case A830FaultSelfTest: return A830ReasonHang;
    case A830FaultCpHwError: return A830ReasonCpFault;
    case A830FaultGmu: case A830FaultGmuWatchdog: return A830ReasonGmuFault;
    case A830FaultWptrFence: return A830ReasonTimeout;
    case A830FaultHfi: return A830ReasonHfiStatus;
    default: return A830ReasonUnexpectedState;   /* slumber, wake */
    }
}
bool hangClass(uint8_t cause) {return cause==A830FaultHang || cause==A830FaultWatchdog || cause==A830FaultSelfTest;}

void sinkReport(void *context,uint8_t cause,uint64_t detail) {static_cast<A830Recovery *>(context)->report(cause,detail);}

struct Progress {A830Recovery *self;uint64_t begin;const A830PowerEnv *env;};
void showProgress(void *context,uint16_t step)
{
    auto *progress=static_cast<Progress *>(context);
    const uint64_t now=progress->env->nowNs(progress->env->context);
    adreno830_status_recovering(A830PowerStepStage(step),uint32_t((now-progress->begin)/NsPerMs));
}
}

IOReturn A830Recovery::init(const A830RecoveryConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops,
                            const A830RecoveryOps &recoveryOps,A830PowerState *power)
{
    if(!power || !io.read32 || !io.write32 || !io.barrier || !env.nowNs || !env.sleepUs || !env.wake)return kIOReturnBadArgument;
    if(!ops.gmuForceOff || !ops.gmuStart || !ops.cpStart || !ops.secureResume || !ops.lastSubmitted || !ops.retired)
        return kIOReturnBadArgument;
    if(!recoveryOps.failInflight || !recoveryOps.gate)return kIOReturnBadArgument;
    if(!recoveryOps.plan!=!recoveryOps.replay)return kIOReturnBadArgument;   /* the replay ops come as a pair */
    if(!config.windowMs || !config.watchdogPeriodMs || config.watchdogLimitMs<config.watchdogPeriodMs || !config.gbifTimeoutUs)
        return kIOReturnBadArgument;
    config_=config;io_=io;env_=env;ops_=ops;rops_=recoveryOps;power_=power;
    snapshot_={};stats_={};
    pendingCause_=A830FaultNone;pendingDetail_=0;lost_=running_=replaying_=false;
    qualification_=qualificationClosed_=qualificationR3b_=daily_=false;protectionAt_=0;lowRequest_=0;unsafe_=nullptr;unsafeContext_=nullptr;
    safeOff_=false;
    qualificationSafeLow_=false;
    for(auto &start:history_)start=0;
    wdArmed_=false;
    power_->setFaultSink(sinkReport,this);
    initialized_=true;
    __atomic_store_n(&gRecovery,this,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

/* ---- reports --------------------------------------------------------------------------------------------------- */
void A830Recovery::requestQualificationLow(uint32_t reason,uint64_t now,bool dropAck)
{
    power_->blockQualification(true);
    power_->holdQualificationLow(true);
    power_->latchQualification();
    __atomic_store_n(&protectionAt_,now,__ATOMIC_RELEASE);
    __atomic_store_n(&qualificationSafeLow_,false,__ATOMIC_RELEASE);
    a830ExitTrace.stage(A830ExitStage::Request,reason,now,now+5000000000ull,1|(2u<<8),dropAck);
    __atomic_store_n(&lowRequest_,reason|(dropAck?0x100u:0),__ATOMIC_RELEASE);
    env_.wake(env_.context);
}

void A830Recovery::closeQualification(uint8_t cause)
{
    if(!qualification_)return;
    power_->blockQualification(true);
    power_->latchQualification();
    if(qualificationR3b_)power_->holdQualificationLow(true);
    if(!__atomic_exchange_n(&qualificationClosed_,true,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&safeOff_,false,__ATOMIC_RELEASE);
        __atomic_store_n(&qualificationSafeLow_,false,__ATOMIC_RELEASE);
        rops_.gate(rops_.context,A830GateRecovering,cause);
    }
}

void A830Recovery::report(uint8_t cause,uint64_t detail)
{
    if(!initialized_ || cause==A830FaultNone || cause>=A830FaultCount)return;
    __atomic_fetch_add(&stats_.reports,1u,__ATOMIC_RELAXED);
    if(__atomic_load_n(&lost_,__ATOMIC_ACQUIRE))return;
    uint8_t expected=A830FaultNone;
    if(__atomic_load_n(&running_,__ATOMIC_ACQUIRE) ||
       !__atomic_compare_exchange_n(&pendingCause_,&expected,Publishing,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&stats_.coalesced,1u,__ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&pendingDetail_,detail,__ATOMIC_RELAXED);
    if(qualificationR3b_ && !__atomic_load_n(&lowRequest_,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&protectionAt_,env_.nowNs(env_.context),__ATOMIC_RELEASE);
        __atomic_store_n(&qualificationSafeLow_,false,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&pendingCause_,cause,__ATOMIC_RELEASE);
    env_.wake(env_.context);
}

IOReturn A830Recovery::runPending()
{
    if(!pending())return kIOReturnSuccess;
    a830ExitTrace.stage(A830ExitStage::PowerWait);
    power_->lock();
    a830ExitTrace.stage(A830ExitStage::PowerLocked);
    const uint32_t request=__atomic_load_n(&lowRequest_,__ATOMIC_ACQUIRE);
    IOReturn low=kIOReturnSuccess;
    if(__atomic_load_n(&pendingCause_,__ATOMIC_ACQUIRE)==Publishing) {
        power_->unlock();
        return kIOReturnSuccess;
    }
    if(request && !faultPending() && !lost()) {
        bool voted=false;   /* low-failed: the vote ran; low-failed-phase: not Active or no vote callback */
        a830ExitTrace.stage(A830ExitStage::LowBegin,request);
        stage("low-begin");
        if(qualificationAsleep() && !(request&0x100u)) {
            power_->pinQualificationWake();
            a830ExitTrace.stage(A830ExitStage::LowDeferred);
            stage("low-deferred");
        } else if(power_->phase()==A830PowerActive && rops_.qualificationLowVote) {
            voted=true;
            low=rops_.qualificationLowVote(rops_.context,(request&0x100u)!=0);
            if(low==kIOReturnSuccess) {
                power_->setTarget(1,2);
                power_->noteVoted(1,2);
                power_->holdQualificationLow(false);
                a830ExitTrace.stage(A830ExitStage::LowResult,0,1,2);
                stage("low-confirmed");
            }
        } else low=kIOReturnNotReady;
        if(low!=kIOReturnSuccess) {
            a830ExitTrace.stage(A830ExitStage::LowResult,uint32_t(low));
            stage(voted?"low-failed":"low-failed-phase",low);
        }
    }
    const uint8_t cause=__atomic_load_n(&pendingCause_,__ATOMIC_ACQUIRE);
    IOReturn result=kIOReturnSuccess;
    if(cause!=A830FaultNone && cause!=Publishing) {
        const uint64_t detail=__atomic_load_n(&pendingDetail_,__ATOMIC_RELAXED);
        __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
        __atomic_store_n(&pendingCause_,uint8_t(A830FaultNone),__ATOMIC_RELEASE);
        result=recoverLocked(cause,detail);
    } else if(request && low!=kIOReturnSuccess && cause!=Publishing) {
        result=recoverLocked(A830FaultHfi,request&255u);
    } else if(request && !lost() && cause!=Publishing) {
        __atomic_store_n(&qualificationSafeLow_,true,__ATOMIC_RELEASE);
        power_->blockQualification(false);
        a830ExitTrace.stage(A830ExitStage::Open,0,qualificationSafeLow(),safeOff());
        a830ExitTrace.opened(power_->votedLevel()==1 && power_->votedBus()==2);
    }
    if(request)__atomic_store_n(&lowRequest_,0u,__ATOMIC_RELEASE);
    power_->unlock();
    return result;
}

IOReturn A830Recovery::recover(uint8_t cause,uint64_t detail)
{
    if(!initialized_ || cause==A830FaultNone || cause>=A830FaultCount)return kIOReturnBadArgument;
    power_->lock();
    /* A report of the same incident that is still pending is answered by this run. */
    __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
    if(faultPending())__atomic_store_n(&pendingCause_,uint8_t(A830FaultNone),__ATOMIC_RELEASE);
    const IOReturn result=recoverLocked(cause,detail);
    power_->unlock();
    return result;
}

void A830Recovery::declareLost(uint8_t cause,uint64_t detail,IOReturn result)
{
    if(!initialized_)return;
    power_->lock();
    if(!lost()) {
        __atomic_store_n(&replaying_,false,__ATOMIC_RELEASE);   /* a final loss replays nothing: the shell faults as before */
        __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
        if(strictMode()) {
            const bool asleep=qualificationAsleep();
            closeQualification(cause);
            const IOReturn drained=power_->beginRecovery();
            minimalSnapshot(cause,detail);
            stats_.jobsFailed+=rops_.failInflight(rops_.context);
            const IOReturn off=drained==kIOReturnSuccess?strictOff(asleep):drained;
            __atomic_store_n(&safeOff_,off==kIOReturnSuccess,__ATOMIC_RELEASE);
            loseLocked(cause,A830SRunning,cause,result);
            if(off!=kIOReturnSuccess && unsafe_)unsafe_(unsafeContext_,A830ExitOrigin::DeclareLostOffFailed);
        } else {
            rops_.gate(rops_.context,A830GateRecovering,cause);
            power_->beginRecovery();
            capture(cause,detail,&snapshot_);
            if(snapshot_.gxOn) {wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_MASK),0);io_.barrier(io_.context);}
            if(rops_.snapshot)rops_.snapshot(rops_.context,&snapshot_);
            stats_.jobsFailed+=rops_.failInflight(rops_.context);
            if(qualification_) {
                power_->blockQualification(true);
                power_->latchQualification();
                const bool safe=ops_.gmuForceOff(ops_.context)==kIOReturnSuccess;
                __atomic_store_n(&safeOff_,safe,__ATOMIC_RELEASE);
                if(!safe && unsafe_ && !daily_)unsafe_(unsafeContext_,A830ExitOrigin::DeclareLostOffFailed);
            }
            loseLocked(cause,A830SRunning,cause,result);
        }
        adreno830_status_add32(&adreno830_status.faults,1);
        if(hangClass(cause))adreno830_status_add32(&adreno830_status.hangs,1);
    }
    power_->unlock();
}

bool A830Recovery::allowed(uint64_t now)
{
    /* history_ keeps the start times of the recent recoveries; the 3rd report within windowMs is final. */
    const uint64_t window=uint64_t(config_.windowMs)*NsPerMs;
    unsigned recent=0;
    for(const uint64_t start:history_)
        if(start && now-start<window)++recent;
    if(recent>=config_.maxInWindow)return false;
    unsigned oldest=0;
    for(unsigned i=1;i<sizeof history_/sizeof history_[0];++i)
        if(history_[i]<history_[oldest])oldest=i;
    history_[oldest]=now?now:1;
    return true;
}

/* ---- the recovery --------------------------------------------------------------------------------------------- */

bool A830Recovery::pollGpu(uint32_t dword,uint32_t mask,uint32_t timeoutUs)
{
    for(uint32_t waited=0;;waited+=10) {
        if((rd(A830PowerWinGpu,gpuReg(dword))&mask)==mask)return true;
        if(waited>=timeoutUs)return false;
        env_.sleepUs(env_.context,10);
    }
}

uint32_t A830Recovery::haltGbif(bool gx)
{
    /* Linux a8xx_bus_clear_pending_transactions: GX side first (only with GX up), then new client requests, then the
     * AXI arbiter, then the GBIF halt released. RBBM_GBIF_HALT stays: hw_init step 1 writes it back to 0. */
    uint32_t timeouts=0;
    if(gx) {
        wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_GBIF_HALT),A8XX_RBBM_GBIF_HALT_GX);
        io_.barrier(io_.context);
        if(!pollGpu(A8XX_RBBM_GBIF_HALT_ACK,A8XX_RBBM_GBIF_HALT_GX,config_.gbifTimeoutUs))timeouts|=1u;
    }
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_CLIENT);
    io_.barrier(io_.context);
    if(!pollGpu(A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_CLIENT,config_.gbifTimeoutUs))timeouts|=2u;
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),A8XX_GBIF_HALT_ARB);
    io_.barrier(io_.context);
    if(!pollGpu(A8XX_GBIF_HALT_ACK,A8XX_GBIF_HALT_ARB,config_.gbifTimeoutUs))timeouts|=4u;
    wr(A830PowerWinGpu,gpuReg(A8XX_GBIF_HALT),0);
    io_.barrier(io_.context);
    stats_.gbifTimeouts+=uint32_t(__builtin_popcount(timeouts));
    return timeouts;
}

void A830Recovery::capture(uint8_t cause,uint64_t detail,A830HangSnapshot *out)
{
    A830HangSnapshot s={};
    s.whenNs=env_.nowNs(env_.context);
    s.cause=cause;s.detail=detail;
    s.fence=ops_.retired(ops_.context);
    s.lastSubmitted=ops_.lastSubmitted(ops_.context);
    s.cxOn=power_->cxOn();
    if(s.cxOn) {
        s.gfxPwrClkStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS));
        s.rpmhPowerState=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_RPMH_POWER_STATE));
        s.cxBusyStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_GPU_CX_BUSY_STATUS));
        s.aoStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS));
        s.gmu2hostInfo=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO));
        s.cm3InitResult=rd(A830PowerWinGmu,gmuReg(A8XX_GMUCX_CM3_FW_INIT_RESULT));
        s.ahbFenceStatus=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS));
        s.intUnmasked=rd(A830PowerWinGmu,gmuReg(A8XX_GMUAO_RBBM_INT_UNMASKED_STATUS_SHADOW));
        s.gxOn=!(s.gfxPwrClkStatus&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF);
    }
    if(s.gxOn) {
        /* GX registers only with GX powered: an access to a collapsed GX is a synchronous external abort (4.7.8). */
        s.rbbmStatus=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_STATUS));
        s.rbbmStatus1=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_STATUS1));
        s.int0Status=rd(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_STATUS));
        s.cpRptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_RPTR));
        s.cpWptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_WPTR));
        s.cpIntGlobal=rd(A830PowerWinGpu,gpuReg(A8XX_CP_INTERRUPT_STATUS_GLOBAL));
        s.cpRlError[0]=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RL_ERROR_DETAILS_0));
        s.cpRlError[1]=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RL_ERROR_DETAILS_1));
        if(rops_.readPipe) {
            A830IbState br{};
            readIbs(A8XX_PIPE_BR,&br);
            s.ib1Base=br.ib1Base;s.ib1Rem=br.ib1Rem;s.ib2Base=br.ib2Base;s.ib2Rem=br.ib2Rem;
        }
    }
    *out=s;
    /* Status block: what a photo of the overlay needs to place the loss (8.2 reg/value/expect). */
    adreno830_status.reg=A830_GPU_REG_PHYS(A8XX_RBBM_STATUS);
    adreno830_status.value=uint64_t(s.int0Status)<<32|s.rbbmStatus;
    adreno830_status.expect=uint64_t(s.lastSubmitted)<<32|s.fence;
    if(s.aoStatus)adreno830_status_sticky(A830StickyGmuAo,s.aoStatus);
}

void A830Recovery::readIbs(uint8_t pipe,A830IbState *out) const
{
    if(!out)return;
    *out={};
    if(!rops_.readPipe)return;
    uint32_t lo=0,hi=0;
    if(rops_.readPipe(rops_.context,pipe,A8XX_CP_IB1_BASE,&lo)==kIOReturnSuccess &&
       rops_.readPipe(rops_.context,pipe,A8XX_CP_IB1_BASE_HI,&hi)==kIOReturnSuccess)
        out->ib1Base=uint64_t(hi)<<32|lo;
    rops_.readPipe(rops_.context,pipe,A8XX_CP_IB1_REM_SIZE,&out->ib1Rem);
    lo=hi=0;
    if(rops_.readPipe(rops_.context,pipe,A8XX_CP_IB2_BASE,&lo)==kIOReturnSuccess &&
       rops_.readPipe(rops_.context,pipe,A8XX_CP_IB2_BASE_HI,&hi)==kIOReturnSuccess)
        out->ib2Base=uint64_t(hi)<<32|lo;
    rops_.readPipe(rops_.context,pipe,A8XX_CP_IB2_REM_SIZE,&out->ib2Rem);
}

void A830Recovery::loseLocked(uint8_t cause,uint8_t stage,uint16_t step,IOReturn result)
{
    __atomic_store_n(&lost_,true,__ATOMIC_RELEASE);
    stats_.lostCause=cause;stats_.lostStep=step;stats_.lostResult=result;
    power_->markLost();
    rops_.gate(rops_.context,A830GateLost,cause);
    adreno830_status_lost(stage,step,reasonOf(cause),result);
    __atomic_store_n(&running_,false,__ATOMIC_RELEASE);
}

void A830Recovery::minimalSnapshot(uint8_t cause,uint64_t detail)
{
    snapshot_={};
    snapshot_.whenNs=env_.nowNs(env_.context);
    snapshot_.cause=cause;
    snapshot_.detail=detail;
    snapshot_.cxOn=power_->cxOn();
    snapshot_.lastSubmitted=ops_.lastSubmitted(ops_.context);
    snapshot_.fence=ops_.retired(ops_.context);
}

IOReturn A830Recovery::strictOff(bool provenAsleep)
{
    a830ExitTrace.stage(A830ExitStage::StrictBegin,0,provenAsleep);
    stage("strict-begin");
    IOReturn result=kIOReturnSuccess;
    if(!provenAsleep) {
        result=kIOReturnNotReady;
        if(power_->cxOn() && rops_.qualificationQuiesce) {
            a830ExitTrace.stage(A830ExitStage::QuiesceBegin);
            result=rops_.qualificationQuiesce(rops_.context);
            a830ExitTrace.stage(A830ExitStage::QuiesceEnd,uint32_t(result));
            if(result==kIOReturnSuccess) {
                stage("strict-quiesced");
                result=ops_.gmuForceOff(ops_.context);
            }
        }
    }
    a830ExitTrace.stage(A830ExitStage::StrictEnd,uint32_t(result));
    stage("strict-end",result);
    return result;
}

IOReturn A830Recovery::recoverLocked(uint8_t cause,uint64_t detail)
{
    if(lost()) {__atomic_store_n(&running_,false,__ATOMIC_RELEASE);return A830ReturnNotResponding;}
    /* Replay or fail (decided once, before the gate: the gate op asks replaying()). lead/v131y7 (gpu/recover2 on the R3b line): also in
     * the daily mode (the daily line's recovery); never in the strict qualification mode (R3b without daily), whose recovery is the
     * minimal snapshot and the safe-off exit with every job in flight failed, as before. */
    const bool replay=config_.replay && rops_.plan && rops_.replay && !strictMode() && __atomic_load_n(&gReplayWanted,__ATOMIC_ACQUIRE);
    __atomic_store_n(&replaying_,replay,__ATOMIC_RELEASE);
    __atomic_store_n(&running_,true,__ATOMIC_RELEASE);
    const uint64_t begin=env_.nowNs(env_.context);
    stats_.lastCause=cause;
    if(cause==A830FaultWatchdog)++stats_.watchdogHangs;
    if(hangClass(cause))adreno830_status_add32(&adreno830_status.hangs,1);
    /* No submission, no new connection; holders drained (a stuck one does not stop the recovery). */
    if(closesQualification(cause))closeQualification(cause);
    else rops_.gate(rops_.context,A830GateRecovering,cause);
    const bool asleep=strictMode() && qualificationAsleep();
    a830ExitTrace.stage(A830ExitStage::DrainBegin,0,cause,detail);
    const IOReturn drained=power_->beginRecovery();
    a830ExitTrace.stage(A830ExitStage::DrainEnd,uint32_t(drained));
    if(strictMode())minimalSnapshot(cause,detail);
    else {
        capture(cause,detail,&snapshot_);
        if(snapshot_.gxOn) {wr(A830PowerWinGpu,gpuReg(A8XX_RBBM_INT_0_MASK),0);io_.barrier(io_.context);}
        if(rops_.snapshot)rops_.snapshot(rops_.context,&snapshot_);
    }
    /* Without the replay the jobs in flight fail here (the gate faulted their connections); with it the plan decides below. */
    if(!replay)stats_.jobsFailed+=rops_.failInflight(rops_.context);
    adreno830_status_add32(&adreno830_status.faults,1);
    adreno830_status_lost(A830SRunning,cause,reasonOf(cause),A830ReturnNotResponding);
    const bool restartAllowed=config_.enabled && allowed(begin);
    if(!qualification_ && !restartAllowed) {
        /* Final: the Lost gate faults every connection, so every job in flight is lost with it. */
        if(replay)stats_.jobsFailed+=rops_.failInflight(rops_.context);
        loseLocked(cause,A830SRunning,cause,A830ReturnNotResponding);
        return A830ReturnNotResponding;
    }
    Progress progress={this,begin,&env_};
    if(strictMode()) {
        const IOReturn off=drained==kIOReturnSuccess?strictOff(asleep):drained;
        if(off!=kIOReturnSuccess) {
            stage("strict-unsafe",off);
            ++stats_.failures;
            loseLocked(cause,A830SGmu,0,off);
            if(unsafe_)unsafe_(unsafeContext_,A830ExitOrigin::StrictOffFailed);
            return off;
        }
        power_->setRsccSlept(true);
        __atomic_store_n(&safeOff_,true,__ATOMIC_RELEASE);
    } else if(power_->cxOn()) {
        /* CX stayed up: stop the GPU's bus traffic, then the GMU (KGSL order: fence, CM3 reset, GBIF, GX, RSCC sleep). */
        adreno830_status_recovering(A830SGmu,0);
        haltGbif(snapshot_.gxOn);
        const IOReturn off=ops_.gmuForceOff(ops_.context);
        if(off==kIOReturnSuccess) {power_->setRsccSlept(true);__atomic_store_n(&safeOff_,true,__ATOMIC_RELEASE);}
        else if(qualification_ && !daily_) {
            ++stats_.failures;
            if(replay)stats_.jobsFailed+=rops_.failInflight(rops_.context);   /* final: lost with the Lost gate */
            loseLocked(cause,A830SGmu,0,off);
            if(unsafe_)unsafe_(unsafeContext_,A830ExitOrigin::StrictOffFailed);
            return off;
        }
    }
    if(!restartAllowed) {
        if(replay)stats_.jobsFailed+=rops_.failInflight(rops_.context);   /* final: lost with the Lost gate */
        loseLocked(cause,A830SGmu,0,A830ReturnNotResponding);
        return A830ReturnNotResponding;
    }
    /* The GPU stopped: no fence moves now. The replay's plan: the culprit (a client's cause only) and the fence S6 restarts at. */
    uint32_t restart=0;
    if(replay)stats_.jobsFailed+=rops_.plan(rops_.context,cause,&restart);
    /* S4/S5/S6/S7 again (plus S1/S3/S4 restore when CX was off); the GMU starts at the DCVS target. */
    const uint8_t level=qualification_?1:power_->targetLevel(),bus=qualification_?2:power_->targetBus();
    if(qualification_)power_->setTarget(1,2);
    __atomic_store_n(&safeOff_,false,__ATOMIC_RELEASE);
    uint16_t step=0,subStep=0;
    a830ExitTrace.stage(A830ExitStage::RestartBegin,0,level,bus);
    const IOReturn result=power_->bringUp(level,bus,!power_->cxOn(),&step,&subStep,showProgress,&progress,replay?&restart:nullptr);
    a830ExitTrace.stage(A830ExitStage::RestartEnd,uint32_t(result),step,subStep);
    if(result!=kIOReturnSuccess) {
        if(qualification_) {
            const IOReturn off=strictMode()?strictOff(false):ops_.gmuForceOff(ops_.context);
            const bool safe=off==kIOReturnSuccess;
            __atomic_store_n(&safeOff_,safe,__ATOMIC_RELEASE);
            if(!safe && unsafe_ && !daily_)unsafe_(unsafeContext_,A830ExitOrigin::RestartReoffFailed);
        }
        /* "GPU LOST S<stage of the failing step>.<its own step>": e.g. S6.41 for a CP_ME_INIT that never finished. */
        ++stats_.failures;
        loseLocked(cause,A830PowerStepStage(step),subStep?subStep:step,result);
        return result;
    }
    /* Waiters see the fence at the last submitted seqno (cpStart wrote it), with the replay at the restart fence (the culprit's
     * waiters wake to its fault); interrupts the storm guard turned off. */
    if(rops_.fenceSignal)rops_.fenceSignal(rops_.context);
    if(rops_.irqRestore)rops_.irqRestore(rops_.context);
    if(replay) {
        /* After the interrupt restore (it clears what the reset left pending): the replayed jobs' completions interrupt as usual. */
        adreno830_status_recovering(A830SCp,uint32_t((env_.nowNs(env_.context)-begin)/NsPerMs));
        uint32_t replayed=0;
        const IOReturn replayResult=rops_.replay(rops_.context,&replayed);
        stats_.jobsReplayed+=replayed;
        if(replayResult!=kIOReturnSuccess) {
            ++stats_.failures;
            loseLocked(cause,A830SCp,A830RecoveryStepReplay,replayResult);
            return replayResult;
        }
        ++stats_.replayRecoveries;
    }
    power_->endRecovery(true,level,bus);
    ++stats_.recoveries;
    adreno830_status_add32(&adreno830_status.recoveries,1);
    const uint64_t end=env_.nowNs(env_.context);
    stats_.lastMs=uint32_t((end-begin)/NsPerMs);
    if(stats_.lastMs>stats_.maxMs)stats_.maxMs=stats_.lastMs;
    stats_.lastUs=uint32_t((end-begin)/1000);
    if(stats_.lastUs>stats_.maxUs)stats_.maxUs=stats_.lastUs;
    wdArmed_=false;
    if(qualification_)__atomic_store_n(&qualificationSafeLow_,true,__ATOMIC_RELEASE);
    if(qualificationR3b_)power_->holdQualificationLow(false);
    power_->blockQualification(false);
    rops_.gate(rops_.context,A830GateOpen,cause);
    a830ExitTrace.stage(A830ExitStage::Open,0,qualificationSafeLow(),safeOff());
    a830ExitTrace.opened(true);
    __atomic_store_n(&qualificationClosed_,false,__ATOMIC_RELEASE);
    adreno830_status_running();
    __atomic_store_n(&running_,false,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

/* ---- software watchdog (4.13.4) ---------------------------------------------------------------------------------- */

bool A830Recovery::watchdogTick(uint64_t now)
{
    if(!initialized_ || lost() || __atomic_load_n(&running_,__ATOMIC_ACQUIRE) || pending())return false;
    const uint32_t fence=ops_.retired(ops_.context);
    if(power_->phase()!=A830PowerActive || fence==ops_.lastSubmitted(ops_.context)) {wdArmed_=false;return false;}
    if(!power_->accessBegin(true))return false;
    const uint32_t rptr=rd(A830PowerWinGpu,gpuReg(A8XX_CP_RB_RPTR));
    uint32_t lo=0,hi=0,rem=0;
    if(rops_.readPipe) {
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE,&lo);
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_BASE_HI,&hi);
        rops_.readPipe(rops_.context,A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,&rem);
    }
    power_->accessEnd(true);
    const uint64_t ib1=uint64_t(hi)<<32|lo;
    /* Any change of the fence, the ring read pointer or the BR IB1 position is progress. */
    if(!wdArmed_ || fence!=wdFence_ || rptr!=wdRptr_ || ib1!=wdIb1_ || rem!=wdIb1Rem_) {
        wdArmed_=true;wdFence_=fence;wdRptr_=rptr;wdIb1_=ib1;wdIb1Rem_=rem;wdLastProgress_=now;
        return false;
    }
    if(now-wdLastProgress_<uint64_t(config_.watchdogLimitMs)*NsPerMs)return false;
    wdArmed_=false;
    report(A830FaultWatchdog,uint64_t(rptr)<<32|fence);
    return true;
}

/* ---- C entry points ------------------------------------------------------------------------------------------------ */

extern "C" void adreno830_recovery_report(uint8_t cause,uint64_t detail)
{
    if(A830Recovery *recovery=__atomic_load_n(&gRecovery,__ATOMIC_ACQUIRE))recovery->report(cause,detail);
}

extern "C" uint8_t adreno830_recovery_gate(void)
{
    A830Recovery *recovery=__atomic_load_n(&gRecovery,__ATOMIC_ACQUIRE);
    if(!recovery)return A830GateOpen;
    if(recovery->lost())return A830GateLost;
    if(recovery->pending() || recovery->busy())return A830GateRecovering;
    return A830GateOpen;
}

extern "C" void adreno830_recovery_set_replay(uint32_t on) {__atomic_store_n(&gReplayWanted,on?1u:0u,__ATOMIC_RELEASE);}
extern "C" uint32_t adreno830_recovery_replay_mode(void) {return __atomic_load_n(&gReplayWanted,__ATOMIC_ACQUIRE);}
extern "C" bool adreno830_recovery_stats(A830RecoveryStats *out)
{
    A830Recovery *recovery=__atomic_load_n(&gRecovery,__ATOMIC_ACQUIRE);
    if(!out || !recovery)return false;
    *out=recovery->stats();   /* plain reads: a counter being written may be one behind */
    return true;
}
