/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * R3a is an operator qualification, not a daily frequency policy. One heartbeat owns the sensor state;
 * requests, the ceiling and the fault latch cross threads atomically. Never waits for the power/HFI mutex.
 */
#ifndef A830_QUALIFICATION_H
#define A830_QUALIFICATION_H
#include "A830Recovery.h"
#include "A830R3bThermal.h"
#include "Sm8750Tsens.h"
#include "../../include/PianoBoardTemps.h"

enum A830QualReason : uint32_t {A830QualNone,A830QualVote,A830QualGpuHot,A830QualBoardHot,A830QualSensor,A830QualExpired};
// Current pre-arm blockers, distinct from the first armed-run fault (reason). Diagnostic only.
enum A830QualInhibit : uint32_t {A830QualGpuInvalid=1,A830QualBoardInvalid=2,A830QualGpuOver=4,
    A830QualBoardOver=8,A830QualNeedRounds=16};
class A830Qualification {
public:
    void init(A830PowerState *power,A830Recovery *recovery,bool r3b=false,void (*unsafe)(void *,A830ExitOrigin)=nullptr,
              bool residency607=false) {
        power_=power;recovery_=recovery;r3b_=r3b;unsafe_=unsafe;power_->setQualificationCeiling(3,3);
        // Bound the residency run from permit open, allowing the four thermally guarded up-steps before timing level 7.
        windowNs_=r3b && residency607?625000000000ull:602000000000ull;
        if(r3b) {
            recovery_->qualificationR3b();
            power_->qualificationGuard([](void *p,uint64_t now,A830ExitContext source) {
                static_cast<A830Qualification *>(p)->checkLease(now,source);
            },this);
        }
    }
    bool r3b() const {return r3b_;}
    /* gpu/daily4: the qualified ceiling, GMU level 7 (607 MHz), 8 (660) or 9 (734), with its default bus (Sm8750GpuLevels, GPU-ANDROID.md
     * table: 607 and 660 bus 6, 734 bus 8). Set once after init(), before the guard runs. */
    static constexpr uint8_t TopMin=7,TopMax=9;
    /* gpu/fullopp1 (ANALYSIS.md 3.1): with fullopp() the top may be any bin-2 level from 7 to 14 (607 .. 1100 MHz). */
    static constexpr uint8_t TopMaxFull=14;
    static uint8_t defaultBus(uint8_t level) {
        static const uint8_t buses[TopMaxFull+1]={0,2,2,3,4,4,4,6,6,8,10,10,11,11,11};
        return buses[level<=TopMaxFull?level:TopMaxFull];
    }
    bool setTop(uint8_t level) {
        if(level<TopMin || level>(fullopp_?TopMaxFull:TopMax))return false;
        top_=level;thermal_.top=level;return true;
    }
    uint8_t top() const {return top_;}
    /* gpu/fullopp1: Android's thermal table in the guard (A830R3bThermal::useAndroid) and tops up to 14. Before setTop(). */
    void fullopp() {fullopp_=true;thermal_.useAndroid();}
    bool isFullopp() const {return fullopp_;}
    uint64_t skinSnapshot() const {return thermal_.publishedSkin();}
    /* Daily mode (pagani-gpu-daily=1, gpu/daily1): the same guard as the daily frequency policy. The permit arms itself once the
     * guard is ready (two fresh board rounds, GPU and board sensors healthy and cool) and has no expiry; every limit of the R3b
     * contract stays: the soft ladder, the board hold, the hard exits (raw 85 C, board 65/40 C, sensor/lease, an unacknowledged
     * vote) through the low transaction, raw 105 C to the unsafe exit. A hard exit still ends high levels for the boot (<= 342). */
    static constexpr uint64_t DailyWindowNs=1000000000000000000ull;   /* about 31.7 years: no expiry within a boot */
    void daily() {daily_=true;windowNs_=DailyWindowNs;if(recovery_)recovery_->qualificationDaily();}
    bool isDaily() const {return daily_;}
    A830R3bThermal &thermal() {return thermal_;}
    uint64_t thermalSnapshot() const {return thermal_.published();}
    bool ready() const {return __atomic_load_n(&ready_,__ATOMIC_ACQUIRE);}
    uint32_t inhibit() const {return __atomic_load_n(&inhibit_,__ATOMIC_ACQUIRE);}
    bool high() const {return __atomic_load_n(&high_,__ATOMIC_ACQUIRE) && !power_->qualificationLatched();}
    bool arm() {
        if(!ready() || power_->qualificationLatched() || __atomic_exchange_n(&armed_,true,__ATOMIC_ACQ_REL))return false;
        return true;
    }
    uint32_t reason() const {return uint32_t(__atomic_load_n(&firstFailure_,__ATOMIC_ACQUIRE)&7);}
    uint64_t failedAt() const {return __atomic_load_n(&firstFailure_,__ATOMIC_ACQUIRE)>>3;}
    uint64_t openedAt() const {return __atomic_load_n(&openedAt_,__ATOMIC_ACQUIRE);}
    void fault(uint32_t reason,uint64_t now,A830ExitContext source=A830ExitContext::Guard) {
        // One publication for the incident and its timestamp. A later real HFI fault still needs recovery.
        uint64_t expected=0;
        const bool first=__atomic_compare_exchange_n(&firstFailure_,&expected,now<<3|reason,false,
                                                     __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        const uint8_t cap=first && r3b_?power_->qualificationCap():0;
        __atomic_store_n(&high_,false,__ATOMIC_RELEASE);
        if(reason==A830QualSensor || reason==A830QualGpuHot || (r3b_ && reason==A830QualBoardHot))
            power_->setQualificationCeiling(1,2);
        if(r3b_ && first) {
            const uint64_t opened=openedAt();
            a830ExitTrace.firstCause(now,reason,source,opened,opened?opened+windowNs_:0,
                                     __atomic_load_n(&heartbeatAt_,__ATOMIC_ACQUIRE),cap);
        }
        if(r3b_ && reason!=A830QualVote) {
            if(first)recovery_->requestQualificationLow(reason,now);
            return;
        }
        if(!r3b_)__atomic_store_n(&legacyProtectionAt_,now,__ATOMIC_RELEASE);
        if(!r3b_)recovery_->closeQualification();
        // daily: a latch from a recovery is not a new fault, and the vote path reports its own failure (qualificationVoteFault)
        if(daily_)return;
        recovery_->report(A830FaultHfi,reason);
    }
    // Called every second by a separate kernel thread. 600 s soak + at most 2 s for the last, dropped ACK.
    // A missing callback/service, partial VALID mask, implausible reading or stalled sample is never a permit.
    void tick(uint64_t now,const Sm8750TsensSample &gpu,const piano_gpu_temps &board,bool boardOk,bool injectBoardHard=false,
              const piano_skin_temps *skin=nullptr) {
        if(r3b_) {tickR3b(now,gpu,board,boardOk,injectBoardHard,skin);return;}
        __atomic_store_n(&heartbeatAt_,now,__ATOMIC_RELEASE);
        if(power_->qualificationLatched() && !reason())fault(A830QualVote,now);
        bool gpuOk=gpu.valid==255;
        for(unsigned i=0;i<8;++i)if(gpu.deci[i]<-400 || gpu.deci[i]>1250)gpuOk=false;
        const bool boardGood=boardOk && board.version==PIANO_GPU_TEMPS_VERSION && board.state==PIANO_BOARD_TEMPS_RUNNING &&
            board.valid==3 && !board.disabled && board.period_ms==5000 && board.age_ms[0]<=10000 && board.age_ms[1]<=10000 &&
            board.sample_us[0] && board.sample_us[1] && board.milli_c[0]>=-40000 && board.milli_c[1]>=-40000 &&
            board.milli_c[0]<=125000 && board.milli_c[1]<=125000;
        const bool gpuHot=gpu.valid && gpu.max_deci>=700;
        const bool boardHot=boardGood && (board.milli_c[0]>=65000 || board.milli_c[1]>=40000);
        if(boardGood && gpuOk && !gpuHot && !boardHot) {
            if(board.rounds!=lastRound_ && board.sample_us[0]>lastSamples_[0] && board.sample_us[1]>lastSamples_[1]) {
                if(rounds_<2)++rounds_;
                lastRound_=board.rounds;lastSamples_[0]=board.sample_us[0];lastSamples_[1]=board.sample_us[1];
            }
        } else rounds_=0;
        __atomic_store_n(&inhibit_,(!gpuOk?A830QualGpuInvalid:0u)|(!boardGood?A830QualBoardInvalid:0u)|
            (gpuHot?A830QualGpuOver:0u)|(boardHot?A830QualBoardOver:0u)|(rounds_<2?A830QualNeedRounds:0u),__ATOMIC_RELEASE);
        __atomic_store_n(&ready_,gpuOk && boardGood && !gpuHot && !boardHot && rounds_>=2,__ATOMIC_RELEASE);
        if(high()) {
            if(gpuHot)fault(A830QualGpuHot,now);
            else if(boardHot)fault(A830QualBoardHot,now);
            else if(!gpuOk || !boardGood)fault(A830QualSensor,now);
            else if(now-openedAt()>=windowNs_)fault(A830QualExpired,now);
        }
        if(!openedAt() && __atomic_load_n(&armed_,__ATOMIC_ACQUIRE) && ready() && !power_->qualificationLatched()) {
            __atomic_store_n(&openedAt_,now,__ATOMIC_RELEASE);
            __atomic_store_n(&high_,true,__ATOMIC_RELEASE);
        }
        const uint64_t end=openedAt()+windowNs_;
        power_->setQualificationLease(now+2500000000ull<end?now+2500000000ull:end);
        // After the boot latch, cool readings may still reach 342; unknown/hot must always be able to reach 160.
        power_->setQualificationCeiling(!gpuOk || gpuHot?1:high()?top_:3,!gpuOk || gpuHot?2:high()?defaultBus(top_):3);
    }
    void checkLease(uint64_t now,A830ExitContext source=A830ExitContext::Guard) {
        const uint64_t at=__atomic_load_n(&heartbeatAt_,__ATOMIC_ACQUIRE);
        if(r3b_ && high() && openedAt() && now>=openedAt() && now-openedAt()>=windowNs_)
            fault(A830QualExpired,now,source);
        else if(__atomic_load_n(&high_,__ATOMIC_ACQUIRE) && (r3b_?(now<at || now-at>A830R3bThermal::Lease):(now>=at && now-at>=2500000000ull)))
            fault(A830QualSensor,now,source);
        else if(power_->qualificationLatched() && !reason())fault(A830QualVote,now,source);
        if(r3b_ && openedAt() && (now<at || now-at>A830R3bThermal::Lease))power_->setQualificationCeiling(1,2);
    }
    void sample(uint64_t now,const Sm8750TsensSample &gpu,A830ExitContext source=A830ExitContext::Dcvs) {
        if(r3b_) {
            bool valid=gpu.valid==255,hot=false;
            for(unsigned i=0;i<8;++i) {
                if(gpu.deci[i]<-400 || gpu.deci[i]>1250)valid=false;
                if(gpu.valid&(1u<<i)) {
                    if(gpu.deci[i]>=1050 && unsafe_) {unsafe_(nullptr,A830ExitOrigin::Raw105);break;}
                    if(gpu.deci[i]>=thermal_.hardDeci)hot=true;
                }
            }
            if(!reason() && hot)fault(A830QualGpuHot,now,source);
            else if(high() && !valid)fault(A830QualSensor,now,source);
            return; // no filter/cap update here: the sole 100 ms owner does that in tickR3b
        }
        if(!openedAt())return;
        if(gpu.valid && gpu.max_deci>=700 && !power_->qualificationLatched())fault(A830QualGpuHot,now);
        else if(gpu.valid!=255 && !power_->qualificationLatched())fault(A830QualSensor,now);
    }
    bool deadlineMissed(uint64_t now) const {
        if(daily_)return false;   // daily: no protection-deadline unsafe exit (gpu-daily1); PianoThermal's shutdown stays independent
        const uint64_t at=r3b_?recovery_->protectionAt():__atomic_load_n(&legacyProtectionAt_,__ATOMIC_ACQUIRE);
        return at && now>=at && now-at>=5000000000ull && !recovery_->safeOff() && !recovery_->qualificationSafeLow();
    }
private:
    void tickR3b(uint64_t now,const Sm8750TsensSample &gpu,const piano_gpu_temps &board,bool boardOk,bool injectBoardHard,
                 const piano_skin_temps *skin) {
        // Last protection runs before any board call/log/fault path can stall.
        for(unsigned i=0;i<8;++i)if((gpu.valid&(1u<<i)) && gpu.deci[i]>=1050 && unsafe_) {unsafe_(nullptr,A830ExitOrigin::Raw105);break;}
        checkLease(now); // never renew a missing guard's expired lease
        thermal_.tick(now,gpu,board,boardOk,high(),skin);
        __atomic_store_n(&heartbeatAt_,now,__ATOMIC_RELEASE);
        bool gpuOk=gpu.valid==255;
        for(unsigned i=0;i<8;++i)if(gpu.deci[i]<-400 || gpu.deci[i]>1250)gpuOk=false;
        __atomic_store_n(&ready_,thermal_.ready,__ATOMIC_RELEASE);
        __atomic_store_n(&inhibit_,(!gpuOk?A830QualGpuInvalid:0u)|(!thermal_.healthy?A830QualBoardInvalid:0u)|
            (thermal_.raw>=thermal_.softDeci?A830QualGpuOver:0u)|(thermal_.boardCap || thermal_.xoHard?A830QualBoardOver:0u)|
            (thermal_.entryRounds<2?A830QualNeedRounds:0u),__ATOMIC_RELEASE);
        /* gpu/fullopp1: Android's table has no board exit; its XO shutdown trip (90 C) is the only board-side hard limit. */
        const bool hardBoard=injectBoardHard || (fullopp_?thermal_.xoHard:
            boardOk && board.valid==3 && board.state==PIANO_BOARD_TEMPS_RUNNING &&
            (board.milli_c[0]>=65000 || board.milli_c[1]>=40000));
        if(high() || (!reason() && ((gpuOk && thermal_.raw>=thermal_.hardDeci) || hardBoard))) {
            if(thermal_.raw>=thermal_.hardDeci)fault(A830QualGpuHot,now);
            else if(hardBoard)fault(A830QualBoardHot,now);
            else if(!thermal_.healthy)fault(A830QualSensor,now);
            else if(now-openedAt()>=windowNs_)fault(A830QualExpired,now);
        }
        if(daily_ && !openedAt() && ready() && !reason() && !power_->qualificationLatched())
            __atomic_store_n(&armed_,true,__ATOMIC_RELEASE);   // daily: the permit arms itself (no operator, no sysctl)
        if(!openedAt() && __atomic_load_n(&armed_,__ATOMIC_ACQUIRE) && ready() && !power_->qualificationLatched()) {
            __atomic_store_n(&openedAt_,now,__ATOMIC_RELEASE);__atomic_store_n(&high_,true,__ATOMIC_RELEASE);
            a830ExitTrace.add(A830ExitStage::Permit,0,now+windowNs_,0,0,0,now);
        }
        const uint64_t end=openedAt()+windowNs_;
        power_->setQualificationLease(now+A830R3bThermal::Lease<end?now+A830R3bThermal::Lease:end);
        uint8_t cap=hardBoard?1:uint8_t(thermal_.published());
        if(!high() && cap>3)cap=3;
        if(cap>top_)cap=top_;
        if(thermal_.events&12)a830ExitTrace.board(thermal_.boardCap,now,cap);
        power_->setQualificationCeiling(cap,defaultBus(cap));
        if(deadlineMissed(now) && unsafe_)unsafe_(nullptr,A830ExitOrigin::ProtectionDeadline);
    }
    bool r3b_=false,daily_=false,fullopp_=false;
    uint8_t top_=7;
    void (*unsafe_)(void *,A830ExitOrigin)=nullptr;
    A830R3bThermal thermal_{};
    A830PowerState *power_=nullptr;
    A830Recovery *recovery_=nullptr;
    bool ready_=false,high_=false,armed_=false;
    uint32_t rounds_=0,lastRound_=0,inhibit_=A830QualNeedRounds;
    uint64_t firstFailure_=0,legacyProtectionAt_=0;
    uint64_t openedAt_=0,lastSamples_[2]={};
    uint64_t heartbeatAt_=0;
    uint64_t windowNs_=602000000000ull;
};
#endif
