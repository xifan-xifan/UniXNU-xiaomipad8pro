/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address and IB_SIZE in dwords (bits 0..19), exactly as the
 * pinned adreno_pm4.xml domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxBuffers=(uint32_t)NativeAdrenoSubmitMaxBuffers,
    SubmitMaxIbBytes=0xfffffu*4u,
    /* UniXNU-piano (2026-11-22): the hold packet, CP_WAIT_TIMESTAMP (0x14, A7XX-; adreno_pm4.xml: dword 0 = 0 is TS_WAIT_GE_32B on RAM,
     * "wait for the value at the address to be >= SRC_0", signed), the packet the ring's own context entry and framed fence use. */
    SubmitWaitTimestamp=0x14,
    /* Ring words a replay keeps free beyond the jobs and the gap (the restart's CP_ME_INIT and secure-mode packets). */
    SubmitReplayMarginWords=256};
static_assert(NativeAdrenoSubmitJobWords>=SubmitMaxBuffers*(1+SubmitIbWords)+NativeAdrenoSubmitHoldWords,"a job's words");
static_assert(NativeAdrenoSubmitJobs==NativeAdrenoSubmitRanges+NativeAdrenoSubmitSharedJobs &&
              NativeAdrenoSubmitRanges<=sizeof(NativeAdrenoRangeMask)*8,
              "one reserved job slot per client range, the rest shared");

namespace {
/* Ring fences are 32-bit sequence numbers: a is at or after b. */
bool atOrAfter(uint32_t a,uint32_t b) {return a-b<0x80000000u;}
}
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-21): submit()'s refusal reason into its record (null: none), the status returned unchanged. */
static IOReturn refuseSubmit(NativeAdrenoSubmitRefusal *out,uint8_t why,IOReturn status,size_t index=UINT32_MAX,
    const NativeAdrenoSubmitBuffer *buffer=nullptr,uint64_t mapped=0,uint64_t mapped_bytes=0,uint64_t a=0)
{
    if(out)*out={why,uint32_t(index),0,buffer?buffer->handle:0,buffer?buffer->iova:0,buffer?uint64_t(buffer->bytes):0,mapped,mapped_bytes,
                 a,0,status};
    return status;
}
#define SUBMIT_REFUSE(why,status) refuseSubmit(refusal,NativeAdrenoSubmitWhy##why,status)
#define SUBMIT_REFUSE_ENTRY(why,status,i,buffer,mapped,mapped_bytes,a) \
    refuseSubmit(refusal,NativeAdrenoSubmitWhy##why,status,i,&(buffer),mapped,mapped_bytes,a)
#else
#define SUBMIT_REFUSE(why,status) (status)
#define SUBMIT_REFUSE_ENTRY(why,status,i,buffer,mapped,mapped_bytes,a) (status)
#endif

IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
uint32_t NativeAdrenoSubmit::completedValue() const
{
    /* memptrs + 4, the BR fence the CACHE_CLEAN timestamp writes (ARCHITECTURE 4.13.1): Normal-NC on both sides, so an acquire load
     * is the whole read; it orders this CPU's later accesses after the fence it saw. */
    const volatile uint32_t *memptrs=ring_?ring_->memptrs():nullptr;
    return memptrs?__atomic_load_n(const_cast<const uint32_t *>(&memptrs[1]),__ATOMIC_ACQUIRE):0;
}
NativeAdrenoRingFence NativeAdrenoSubmit::state(uint32_t fence,uint32_t *observed) const
{
    const uint32_t value=completedValue();
    if(observed)*observed=value;
    if(!ring_ || !fence || !atOrAfter(ring_->lastSubmitted(),fence))return NativeAdrenoRingFenceArgument;
    return atOrAfter(value,fence)?NativeAdrenoRingFenceComplete:NativeAdrenoRingFenceTimeout;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::findLocked(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::slotLocked(uint32_t range)
{
    if(!jobs_[range].active)return &jobs_[range];
    /* Admission control (ARCHITECTURE 4.17, 5.4): only the render server's range takes the shared slots. */
    if(range)return nullptr;
    for(uint32_t i=NativeAdrenoSubmitRanges;i<NativeAdrenoSubmitJobs;++i)if(!jobs_[i].active)return &jobs_[i];
    return nullptr;
}
bool NativeAdrenoSubmit::rangeOutstandingLocked(uint32_t first,uint32_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstandingLocked() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
void NativeAdrenoSubmit::dropReferences(const uint64_t *handles,size_t count)
{
    while(count) {vm_->retireJobAfterCompletion(handles[--count]);++stats_.references_retired;}
}
IOReturn NativeAdrenoSubmit::releaseLocked(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    for(uint32_t i=0;i<job.count;++i) {
        const IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    job.active=false;job.count=0;job.fence=0;job.queue=0;job.word_count=0;job.context=0;job.source=0;job.kept=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompletedLocked()
{
    /* One read of the fence decides every job: the ring completes in order. */
    const uint32_t observed=completedValue();
    for(auto &job:jobs_) {
        if(!job.active || !job.fence || !atOrAfter(observed,job.fence))continue;
        const IOReturn status=releaseLocked(job);
        if(status)return status;
        ++stats_.completed;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};
    for(auto &job:jobs_) {job.active=false;job.count=0;job.fence=0;job.queue=0;job.word_count=0;job.context=0;job.source=0;job.kept=false;}
    for(auto &entry:history_)entry=0;
    hold_ranges_=0;held_fence_=0;plan_=NativeAdrenoSubmitPlan{};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(poisoned_ || outstandingLocked())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::attachLocks(const NativeAdrenoSubmitLocks *locks)
{
    /* No lock is held here: the hooks change only while nothing can be using them (no job outstanding, before publication). */
    if(outstandingLocked())return kIOReturnBusy;
    if(locks && (!locks->lock || !locks->unlock))return kIOReturnBadArgument;
    locks_=locks?*locks:NativeAdrenoSubmitLocks{};
    return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return rangeOutstandingLocked(first,count);
}
size_t NativeAdrenoSubmit::outstanding() const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return outstandingLocked();
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,uint32_t queue,uint32_t *fence,
    uint32_t *retry_fence,uint32_t context,uint32_t source
#ifdef ADRENO830_MACOS
    ,NativeAdrenoSubmitRefusal *refusal
#endif
    )
{
    if(!fence)return SUBMIT_REFUSE(Other,kIOReturnBadArgument);
    *fence=0;
    if(retry_fence)*retry_fence=0;
    if(!buffers || !count || count>SubmitMaxBuffers)return SUBMIT_REFUSE(Count,kIOReturnBadArgument);
    const uint32_t range=queue/NativeAdrenoSubmitRangeQueues;
    if(range>=NativeAdrenoSubmitRanges)return SUBMIT_REFUSE(Queue,kIOReturnBadArgument);
    NativeAdrenoSubmitGuard ring_lock(this,NativeAdrenoSubmitLockRing);
    if(!attached_ || poisoned_)return SUBMIT_REFUSE(NotOpen,kIOReturnNotReady);
    if(halted_) {
        /* A recovery holds the ring: a caller with work in flight waits for its fence (the recovery completes it), any other sleeps. */
        if(rangeOutstandingLocked(range*NativeAdrenoSubmitRangeQueues,NativeAdrenoSubmitRangeQueues)) {
            ++stats_.busy;return SUBMIT_REFUSE(QueueFull,kIOReturnBusy);
        }
        ++stats_.retries;
        return SUBMIT_REFUSE(QueueFull,NativeAdrenoSubmitRetry);
    }
    /* Every buffer is validated against the live VM before any reference: the IB check is the one thing the submit path keeps. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    static_assert(sizeof(scratch_words_)/sizeof(scratch_words_[0])>=SubmitMaxBuffers*(1+SubmitIbWords)+NativeAdrenoSubmitHoldWords,
                  "ring words");
    /* UniXNU-piano (test only, armHold): a held range's submission starts with the wait nothing ever satisfies. */
    const bool hold=(hold_ranges_&NativeAdrenoRangeBit(range))!=0;
    if(hold) {
        const uint64_t at=ring_->memptrsIova()+NativeAdrenoSubmitHoldOffset;
        if(AdrenoPM4Type7(SubmitWaitTimestamp,4,&words[0])!=AdrenoPM4Success)return SUBMIT_REFUSE(Other,poison());
        words[1]=0;words[2]=(uint32_t)at;words[3]=(uint32_t)(at>>32);words[4]=1;
        word_count=NativeAdrenoSubmitHoldWords;
    }
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle)return SUBMIT_REFUSE_ENTRY(NotMapped,kIOReturnBadArgument,i,buffer,0,0,0);
        if(!buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)return SUBMIT_REFUSE_ENTRY(Bytes,kIOReturnBadArgument,i,buffer,0,0,0);
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        const IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return SUBMIT_REFUSE_ENTRY(NotMapped,status,i,buffer,iova,bytes,permissions);
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova || buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))
            return SUBMIT_REFUSE_ENTRY(Range,kIOReturnBadArgument,i,buffer,iova,bytes,permissions);
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)return SUBMIT_REFUSE(Other,poison());
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    /* A job slot: the range's own one, or (range 0) a shared one. Completed jobs are retired here only when that finds none: the
     * fence interrupt's retire_call keeps the slots clean, the submit path does not walk the jobs (ARCHITECTURE 5.4). */
    Job *job=slotLocked(range);
    if(!job) {
        ++stats_.sweeps;
        const IOReturn swept=retireCompletedLocked();
        if(swept)return SUBMIT_REFUSE(Other,swept);
        job=slotLocked(range);
    }
    if(!job) {
        /* The range's reserved slot holds the range's own job: the caller has work in flight and waits for its oldest fence. */
        ++stats_.busy;
        return SUBMIT_REFUSE(QueueFull,kIOReturnBusy);
    }
    for(size_t i=0;i<handle_count;++i) {
        const IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            dropReferences(handles,i);
            return SUBMIT_REFUSE(Other,status);
        }
        ++stats_.references_held;
    }
    /* GPU-read-only imports that are not mapped coherent (the maintained list; empty from coherence level 1, ARCHITECTURE 4.14.1). */
    IOReturn status=vm_->cleanForSubmission();
    if(status) {
        dropReferences(handles,handle_count);
        return SUBMIT_REFUSE(Other,status);
    }
    if(context)ring_->useContext(context);
    uint32_t value=0;
    status=ring_->submitFramed(words,word_count,&value,source);
    if(status==kIOReturnBusy) {
        /* No room in the ring now. Room comes back as the CP consumes: the oldest outstanding submission's fence is the event. */
        dropReferences(handles,handle_count);
        if(rangeOutstandingLocked(range*NativeAdrenoSubmitRangeQueues,NativeAdrenoSubmitRangeQueues)) {
            ++stats_.busy;return SUBMIT_REFUSE(QueueFull,kIOReturnBusy);
        }
        /* The ring completes in order: the next fence to complete is the one after the CP's completed fence. */
        const uint32_t done=completedValue(),newest=ring_->lastSubmitted();
        if(retry_fence)*retry_fence=atOrAfter(done,newest)?newest:done+1;
        ++stats_.retries;
        return SUBMIT_REFUSE(QueueFull,NativeAdrenoSubmitRetry);
    }
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the referenced buffers cannot be retired here: the job is
         * kept (with no fence) until recover() after a GPU reset. */
        if(ring_->poisoned()) {
            job->fence=0;job->queue=queue;job->count=(uint32_t)handle_count;job->active=true;
            job->word_count=0;job->context=context;job->source=source;job->kept=false;   /* never replayed: it has no fence */
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            ++stats_.poisoned_jobs;
            return SUBMIT_REFUSE(Other,poison());
        }
        /* kIOReturnNoResources: not even the drained ring takes it (only a larger submission than this object builds). */
        dropReferences(handles,handle_count);
        return SUBMIT_REFUSE(Other,status);
    }
    job->fence=value;job->queue=queue;job->count=(uint32_t)handle_count;job->active=true;
    /* The packets as the ring got them, for a recovery's replay (2 KiB at most, into the replay store; the ring copied them already). */
    job->word_count=(uint32_t)word_count;job->context=context;job->source=source;job->kept=store_!=nullptr;
    if(store_) {uint32_t *kept=keptWords(*job);for(size_t i=0;i<word_count;++i)kept[i]=words[i];}
    if(hold) {hold_ranges_=0;held_fence_=value;++stats_.holds;}
    /* Whose job this fence is, for a page fault's attribution (rangesInFlight): kept after the job retires. */
    history_[value%NativeAdrenoSubmitHistory]=uint64_t(value)|uint64_t(range)<<32|uint64_t(1)<<63;
    for(size_t i=0;i<handle_count;++i) {
        job->handles[i]=handles[i];
        /* The fence of the buffer's last use, for the VM's idle check before an unmap (the job's reference covers it until then). */
        (void)vm_->noteUse(handles[i],value);
    }
    ++stats_.submissions;
    const size_t now_outstanding=outstandingLocked();
    if(now_outstanding>stats_.peak_jobs)stats_.peak_jobs=now_outstanding;
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    const uint64_t began=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            if(!attached_ || poisoned_)return kIOReturnNotReady;
            const NativeAdrenoRingFence result=state(fence,nullptr);
            if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
            if(result==NativeAdrenoRingFenceComplete) {
                if(Job *job=findLocked(fence)) {
                    const IOReturn status=releaseLocked(*job);
                    if(status)return status;
                    ++stats_.completed;
                }
                return kIOReturnSuccess;
            }
            if(!timeout_us || !clock || !clock->now_us || clock->now_us(clock->cookie)-began>=timeout_us) {
                ++stats_.timeouts;
                return kIOReturnTimeout;
            }
        }
        /* The pause is the clock's (until the next GPU interrupt or a short sleep), with no lock held. */
        if(!clock->idle || clock->idle(clock->cookie)) {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            ++stats_.timeouts;
            return kIOReturnTimeout;
        }
    }
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    const uint64_t began=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            if(!attached_ || poisoned_)return kIOReturnNotReady;
            const IOReturn swept=retireCompletedLocked();
            if(swept)return swept;
            if(!rangeOutstandingLocked(first,count))return kIOReturnSuccess;
            if(!clock || !clock->now_us || clock->now_us(clock->cookie)-began>=timeout_us) {
                ++stats_.timeouts;
                return kIOReturnTimeout;
            }
        }
        if(!clock->idle || clock->idle(clock->cookie)) {
            NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
            ++stats_.timeouts;
            return kIOReturnTimeout;
        }
    }
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    return waitRange(0,UINT32_MAX,clock,timeout_us);
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    return retireCompletedLocked();
}
bool NativeAdrenoSubmit::issued(uint64_t fence) const
{
    return attached_ && ring_ && fence && fence<=UINT32_MAX && atOrAfter(ring_->lastSubmitted(),(uint32_t)fence);
}
bool NativeAdrenoSubmit::completed(uint32_t fence) const
{
    return attached_ && state(fence,nullptr)==NativeAdrenoRingFenceComplete;
}
uint32_t NativeAdrenoSubmit::lastSubmitted() const
{
    return attached_ && ring_?ring_->lastSubmitted():0;
}
IOReturn NativeAdrenoSubmit::rangesInFlight(uint32_t completed,uint32_t issued,NativeAdrenoRangeMask *ranges) const
{
    if(!ranges)return kIOReturnBadArgument;
    *ranges=0;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    const uint32_t count=issued-completed;
    if(count>NativeAdrenoSubmitHistory)return kIOReturnUnsupported;   /* also a completed fence past the issued one */
    NativeAdrenoRangeMask mask=0;
    for(uint32_t i=1;i<=count;++i) {
        const uint32_t fence=completed+i;
        const uint64_t entry=history_[fence%NativeAdrenoSubmitHistory];
        if(!(entry>>63) || uint32_t(entry)!=fence)return kIOReturnUnsupported;
        mask|=NativeAdrenoRangeBit(uint32_t(entry>>32)&(NativeAdrenoSubmitRanges-1));
    }
    *ranges=mask;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::halt(NativeAdrenoRangeMask *ranges_in_flight,uint32_t *oldest_range)
{
    if(ranges_in_flight)*ranges_in_flight=0;
    if(oldest_range)*oldest_range=UINT32_MAX;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    __atomic_store_n(&halted_,true,__ATOMIC_RELEASE);
    /* Jobs the CP already passed are not lost: retire them first (a poisoned object has nothing to retire safely). */
    if(!poisoned_)(void)retireCompletedLocked();
    NativeAdrenoRangeMask ranges=0;
    uint32_t oldest=UINT32_MAX,distance=UINT32_MAX;
    const uint32_t done=completedValue();
    for(const auto &job:jobs_) {
        if(!job.active)continue;
        ranges|=NativeAdrenoRangeBit(job.queue/NativeAdrenoSubmitRangeQueues);
        /* Fences are sequence numbers: the job the CP stands in is the one nearest after the completed fence. A job the CP already
         * passed (a poisoned object retires nothing) and a poisoned commit's job (fence 0, never issued) are not in flight on the CP:
         * never chosen, and with no other job there is no culprit. */
        if(!job.fence || atOrAfter(done,job.fence))continue;
        const uint32_t ahead=job.fence-done;
        if(ahead<distance) {distance=ahead;oldest=job.queue/NativeAdrenoSubmitRangeQueues;}
    }
    if(ranges_in_flight)*ranges_in_flight=ranges;
    if(oldest_range)*oldest_range=oldest;
    return kIOReturnSuccess;
}
void NativeAdrenoSubmit::resume()
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    __atomic_store_n(&halted_,false,__ATOMIC_RELEASE);
}
void NativeAdrenoSubmit::dropAllLocked(NativeAdrenoRangeMask *ranges,IOReturn *result)
{
    for(auto &job:jobs_) {
        if(!job.active)continue;
        *ranges|=NativeAdrenoRangeBit(job.queue/NativeAdrenoSubmitRangeQueues);
        const IOReturn status=releaseLocked(job);
        if(status && !*result)*result=status;
        if(!status)++stats_.abandoned;
    }
}
IOReturn NativeAdrenoSubmit::recover(NativeAdrenoRangeMask *abandoned_ranges)
{
    if(abandoned_ranges)*abandoned_ranges=0;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    NativeAdrenoRangeMask ranges=0;
    IOReturn result=kIOReturnSuccess;
    dropAllLocked(&ranges,&result);
    plan_=NativeAdrenoSubmitPlan{};
    if(abandoned_ranges)*abandoned_ranges=ranges;
    if(result)return poison();
    poisoned_=false;
    return kIOReturnSuccess;
}
uint32_t NativeAdrenoSubmit::orderLocked(uint32_t from,uint32_t *order) const
{
    /* Insertion by distance from `from` (at most NativeAdrenoSubmitJobs jobs; fences are sequence numbers). */
    uint32_t n=0;
    for(uint32_t i=0;i<NativeAdrenoSubmitJobs;++i) {
        const Job &job=jobs_[i];
        if(!job.active || !job.fence || atOrAfter(from,job.fence))continue;
        const uint32_t ahead=job.fence-from;
        uint32_t at=n++;
        while(at && jobs_[order[at-1]].fence-from>ahead) {order[at]=order[at-1];--at;}
        order[at]=i;
    }
    return n;
}
IOReturn NativeAdrenoSubmit::plan(bool charge,NativeAdrenoSubmitPlan *out)
{
    if(!out)return kIOReturnBadArgument;
    NativeAdrenoSubmitPlan p{};
    p.culprit_range=UINT32_MAX;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_) {*out=p;return kIOReturnNotReady;}
    p.done=completedValue();p.issued=ring_->lastSubmitted();
    /* The GPU stopped: the jobs the CP passed completed (a poisoned object retires nothing: it falls back below). */
    if(!poisoned_)
        for(auto &job:jobs_) {
            if(!job.active || !job.fence || !atOrAfter(p.done,job.fence))continue;
            if(releaseLocked(job))break;   /* poisons: the fallback below */
            ++stats_.completed;++p.retired;
        }
    uint32_t order[NativeAdrenoSubmitJobs];
    const uint32_t n=poisoned_?0:orderLocked(p.done,order);
    /* The culprit, for a client's cause only: the job the CP stood in, the one right after the completed fence (the one ring runs in
     * order); never a job the CP passed, never a kernel fence (then there is no job at done + 1 and the plan falls back). */
    if(charge && n && jobs_[order[0]].fence==p.done+1) {
        p.culprit_range=jobs_[order[0]].queue/NativeAdrenoSubmitRangeQueues;
        p.culprit_fence=jobs_[order[0]].fence;
    }
    const uint32_t skip=p.culprit_fence?1u:0u;
    if(poisoned_)p.fallback=NativeAdrenoSubmitFallbackPoisoned;
    /* Every fence in (done, issued] must be a job of this object: the replayed jobs complete them, the last one the newest. */
    else if(p.issued-p.done!=n)p.fallback=NativeAdrenoSubmitFallbackGap;
    else {
        for(uint32_t i=skip;i<n;++i)if(!jobs_[order[i]].kept)p.fallback=NativeAdrenoSubmitFallbackNoStore;
    }
    if(!p.fallback) {
        /* The words the replay writes: as the ring frames them, a context entry for the first job and at each change of context. */
        uint64_t words=0;
        uint32_t context=0;
        for(uint32_t i=skip;i<n;++i) {
            const Job &job=jobs_[order[i]];
            const bool reset=i==skip || (job.context && job.context!=context);
            if(job.context)context=job.context;
            words+=NativeAdrenoRing::framedWords(job.word_count,reset);
        }
        if(words+NativeAdrenoRingGapWords+SubmitReplayMarginWords>ring_->ringBytes()/4)p.fallback=NativeAdrenoSubmitFallbackRoom;
        else p.replay_words=uint32_t(words);
    }
    if(p.fallback) {
        /* The behaviour before: the CP restarts at the newest fence, every job in flight fails and its range is faulted. */
        p.mode=NativeAdrenoSubmitPlanFailAll;p.restart=p.issued;
        for(const auto &job:jobs_)
            if(job.active) {p.fault_ranges|=NativeAdrenoRangeBit(job.queue/NativeAdrenoSubmitRangeQueues);++p.failed;}
        ++stats_.fallbacks;
    } else {
        p.mode=NativeAdrenoSubmitPlanReplay;
        p.restart=p.culprit_fence?p.culprit_fence:p.done;
        if(p.culprit_fence) {p.fault_ranges=NativeAdrenoRangeBit(p.culprit_range);p.failed=1;}
        for(uint32_t i=skip;i<n;++i) {
            const Job &job=jobs_[order[i]];
            const uint32_t range=job.queue/NativeAdrenoSubmitRangeQueues;
            p.replay_ranges|=NativeAdrenoRangeBit(range);
            if(p.replay_count[range]<UINT8_MAX)++p.replay_count[range];
            ++p.replay_jobs;
            if(!p.first_replay)p.first_replay=job.fence;
            p.last_replay=job.fence;
        }
    }
    plan_=p;
    *out=p;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::replay(uint32_t *replayed)
{
    if(replayed)*replayed=0;
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    const NativeAdrenoSubmitPlan p=plan_;
    plan_=NativeAdrenoSubmitPlan{};
    if(p.mode!=NativeAdrenoSubmitPlanReplay) {
        /* FailAll, or no plan: recover()'s drop (the CP restarted at the newest fence). */
        NativeAdrenoRangeMask ranges=0;
        IOReturn result=kIOReturnSuccess;
        dropAllLocked(&ranges,&result);
        if(result)return poison();
        poisoned_=false;
        return kIOReturnSuccess;
    }
    /* The CP restarted at p.restart: the culprit's job (at it) is done for, its references go; no other job ran yet. */
    for(auto &job:jobs_) {
        if(!job.active || !job.fence || !atOrAfter(p.restart,job.fence))continue;
        const IOReturn status=releaseLocked(job);
        if(status)return status;   /* poisoned */
        ++stats_.skipped;
    }
    uint32_t order[NativeAdrenoSubmitJobs];
    const uint32_t n=orderLocked(p.restart,order);
    uint32_t after=p.restart;
    for(uint32_t i=0;i<n;++i) {
        const Job &job=jobs_[order[i]];
        if(job.context)ring_->useContext(job.context);
        if(!job.kept) {poisoned_=true;return kIOReturnNotReady;}   /* the plan saw every one kept; a store detached since */
        const IOReturn status=ring_->replayFramed(keptWords(job),job.word_count,job.fence,after,job.source);
        if(status) {poisoned_=true;return status;}   /* the device is lost then (A830Recovery) */
        after=job.fence;
        ++stats_.replayed;
        if(replayed)++*replayed;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::armHold(NativeAdrenoRangeMask ranges)
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(!attached_)return kIOReturnNotReady;
    if(ranges && ring_->memptrsBytes()<NativeAdrenoSubmitHoldOffset+4)return kIOReturnUnsupported;
    hold_ranges_=ranges;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::attachReplayStore(uint32_t *words,size_t count)
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    if(outstandingLocked())return kIOReturnBusy;
    if(words && count<NativeAdrenoSubmitReplayStoreWords)return kIOReturnBadArgument;
    store_=words;
    return kIOReturnSuccess;
}
NativeAdrenoRangeMask NativeAdrenoSubmit::holdArmed() const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return hold_ranges_;
}
uint32_t NativeAdrenoSubmit::heldFence() const
{
    NativeAdrenoSubmitGuard guard(this,NativeAdrenoSubmitLockRing);
    return held_fence_;
}
