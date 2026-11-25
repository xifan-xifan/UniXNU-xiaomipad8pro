/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 */
#ifndef A830_EXIT_TRACE_H
#define A830_EXIT_TRACE_H

#include <stdint.h>

enum class A830ExitContext : uint32_t { Guard=1, Dcvs, Power, Hfi, Operator };
enum class A830ExitOrigin : uint32_t { Raw105=1, ProtectionDeadline, StrictOffFailed, RestartReoffFailed, DeclareLostOffFailed };
enum class A830ExitVote : uint32_t { None, BoardHold, Low160, RestartM9, Recovery342 };
enum class A830ExitStage : uint32_t {
    Ready=1, Permit, BoardHold, BoardRelease, First, Request,
    PowerWait, PowerLocked, LowBegin, LowDeferred, CxBegin, CxEnd,
    HfiWait, HfiLocked, EnqueueBegin, Enqueued, MatchingAck, VoteResult, LowResult, Open,
    DrainBegin, DrainEnd, StrictBegin, QuiesceBegin, QuiesceEnd, StrictEnd,
    Cm3Begin, Cm3End, GxBegin, GxEnd, RsccBegin, RsccEnd, RestartBegin, RestartEnd,
    Terminal, StrictHwError
};

class A830ExitTrace {
public:
    struct Record {
        uint64_t at, a, b, c, d;
        uint32_t stage, status;
    };
    static constexpr unsigned Capacity=96;
    static_assert(sizeof(Record)==48,"exit record ABI");

    void start(uint64_t (*clock)(void *),void *context) {
        if(boot())return;
        clock_=clock;
        context_=context;
        __atomic_store_n(&boot_,clock(context),__ATOMIC_RELEASE);
        add(A830ExitStage::Ready);
    }
    uint64_t boot() const {return __atomic_load_n(&boot_,__ATOMIC_ACQUIRE);}
    uint64_t first() const {return __atomic_load_n(&first_,__ATOMIC_ACQUIRE);}
    uint64_t ack() const {return __atomic_load_n(&ack_,__ATOMIC_ACQUIRE);}
    uint32_t head() const {return __atomic_load_n(&head_,__ATOMIC_ACQUIRE);}
    bool tracking() const {return first() && !__atomic_load_n(&closed_,__ATOMIC_ACQUIRE);}
    void noteAck(uint64_t ack) {__atomic_store_n(&ack_,ack,__ATOMIC_RELEASE);}
    void firstCause(uint64_t now,uint32_t reason,A830ExitContext source,uint64_t opened,uint64_t deadline,
                    uint64_t heartbeat,uint32_t cap) {
        if(!boot())return;
        __atomic_store_n(&first_,now<<3|reason,__ATOMIC_RELEASE);
        add(A830ExitStage::First,uint32_t(source)<<8|reason,opened,deadline,heartbeat,
            ack()|(uint64_t(cap)<<48),now);
    }
    void board(bool hold,uint64_t now,uint32_t cap) {
        __atomic_store_n(&boardHold_,hold,__ATOMIC_RELEASE);
        // One marker per edge is enough to distinguish this reversible path from a permit exit.
        bool *seen=hold?&holdSeen_:&releaseSeen_;
        if(!__atomic_exchange_n(seen,true,__ATOMIC_ACQ_REL))
            add(hold?A830ExitStage::BoardHold:A830ExitStage::BoardRelease,0,cap,ack(),0,0,now);
    }
    bool boardDown(uint8_t old,uint8_t level) {
        return old>3 && level<=3 && !first() && __atomic_load_n(&boardHold_,__ATOMIC_ACQUIRE) &&
            !__atomic_exchange_n(&boardVote_,true,__ATOMIC_ACQ_REL);
    }
    void opened(bool confirmed) {
        if(!tracking())return;
        if(confirmed)__atomic_store_n(&lookFor342_,true,__ATOMIC_RELEASE);
        __atomic_store_n(&closed_,true,__ATOMIC_RELEASE);
    }
    bool recoveryVote(uint8_t level) {
        return level>1 && level<=3 && __atomic_exchange_n(&lookFor342_,false,__ATOMIC_ACQ_REL);
    }
    void stage(A830ExitStage stage,uint32_t status=0,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
        if(tracking())add(stage,status,a,b,c,d);
    }
    void add(A830ExitStage stage,uint32_t status=0,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,
             uint64_t at=0) {
        if(!boot())return;
        const auto seq=__atomic_fetch_add(&head_,1u,__ATOMIC_RELAXED)+1;
        // Preserve the beginning of this one-shot transaction even if stages outnumber the slots.
        // Dropped sequence numbers are visible in head(). Slots are written once and never reused.
        if(seq>Capacity)return;
        switch(stage) {
        case A830ExitStage::PowerLocked: case A830ExitStage::CxEnd: case A830ExitStage::HfiLocked:
        case A830ExitStage::Enqueued: case A830ExitStage::MatchingAck: case A830ExitStage::VoteResult:
        case A830ExitStage::LowResult: case A830ExitStage::Open: case A830ExitStage::DrainEnd:
        case A830ExitStage::QuiesceEnd: case A830ExitStage::Cm3End: case A830ExitStage::GxEnd:
        case A830ExitStage::RsccEnd: case A830ExitStage::StrictEnd: case A830ExitStage::RestartEnd:
            __atomic_store_n(&completed_,uint32_t(stage),__ATOMIC_RELEASE);
            break;
        default: break;
        }
        publish(slots_[seq-1],seq,{at?at:clock_(context_),a,b,c,d,uint32_t(stage),status});
    }
    void terminal(A830ExitOrigin origin,uint64_t protectionAt,bool safeLow,bool safeOff) {
        if(!boot() || __atomic_exchange_n(&terminalClaimed_,true,__ATOMIC_ACQ_REL))return;
        const uint64_t state=uint64_t(__atomic_load_n(&completed_,__ATOMIC_ACQUIRE))<<2 |
            uint64_t(safeOff)<<1 | uint64_t(safeLow);
        const auto seq=__atomic_fetch_add(&head_,1u,__ATOMIC_RELAXED)+1;
        // A reserved slot keeps the first direct PSCI origin even if the stage journal is full.
        publish(terminal_,seq,{clock_(context_),first(),protectionAt,state,ack(),
                               uint32_t(A830ExitStage::Terminal),uint32_t(origin)});
    }
    bool copy(uint32_t seq,Record &record) const {
        if(!seq || seq>Capacity)return false;
        if(__atomic_load_n(&terminal_.commit,__ATOMIC_ACQUIRE)==seq)return copyTerminal(record)==seq;
        return copySlot(slots_[seq-1],seq,record);
    }
    uint32_t copyTerminal(Record &record) const {
        const auto seq=__atomic_load_n(&terminal_.commit,__ATOMIC_ACQUIRE);
        return seq && copySlot(terminal_,seq,record)?seq:0;
    }

private:
    struct Slot {Record record;uint32_t commit;};
    static void publish(Slot &slot,uint32_t seq,const Record &record) {
        slot.record=record;
        __atomic_store_n(&slot.commit,seq,__ATOMIC_RELEASE);
    }
    static bool copySlot(const Slot &slot,uint32_t seq,Record &record) {
        if(__atomic_load_n(&slot.commit,__ATOMIC_ACQUIRE)!=seq)return false;
        record=slot.record;
        return true;
    }

    Slot slots_[Capacity]={},terminal_={};
    uint64_t (*clock_)(void *)=nullptr;
    void *context_=nullptr;
    uint64_t boot_=0,first_=0,ack_=0;
    uint32_t head_=0,completed_=0;
    bool closed_=false,boardHold_=false,holdSeen_=false,releaseSeen_=false,boardVote_=false;
    bool lookFor342_=false,terminalClaimed_=false;
};

// Production uses one external object. Apple's kext flags give inline variables
// separate local storage in each translation unit.
#ifdef ADRENO830_HOST_TEST
inline A830ExitTrace a830ExitTrace;
#else
extern A830ExitTrace a830ExitTrace;
#endif

#endif
