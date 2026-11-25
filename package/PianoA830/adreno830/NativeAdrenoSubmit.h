/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SUBMIT_H
#define NATIVE_ADRENO_SUBMIT_H
#include "NativeAdrenoRing.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoStatus.h"

/* IBs of one submission: a whole frame, so the ring prologue (cache flush, idle wait, thread switch) runs once per frame. */
enum : size_t {NativeAdrenoSubmitMaxBuffers=128};
/* Job slots of the one shared ring (ARCHITECTURE 5.4): every client range (queue / NativeAdrenoSubmitRangeQueues = 0..31, the
 * connection slots of the publish shell, NativeAdrenoMaxClients) owns one reserved slot, the other twelve are shared. Range 0, the
 * render server's, may also take the shared ones; every other range has at most its reserved job in flight (admission control in
 * place of preemption, ARCHITECTURE 4.17). So a caller with nothing in flight always finds its own slot free and is never told Busy
 * for want of a slot. 32 ranges since 2026-10-30 (every app may be a Metal client); a recovery's range masks are 32 bits.
 * UniXNU-piano (2026-11-22, private GPU-SLOTS.md): 64 ranges on macOS, one per connection slot of the publish shell (an installed session
 * refused every app after 24 GPU connections), so the masks are 64 bits (NativeAdrenoRangeMask) and the ring has 64 reserved + 12
 * shared jobs; the upstream build keeps 32 and 44. */
#ifdef ADRENO830_MACOS
enum : uint32_t {NativeAdrenoSubmitRangeCount=64};   /* NativeAdrenoRangeMask (NativeAdrenoStatus.h) is 64 bits */
#else
enum : uint32_t {NativeAdrenoSubmitRangeCount=32};
#endif
enum : uint32_t {
    NativeAdrenoSubmitRanges=NativeAdrenoSubmitRangeCount,
    NativeAdrenoSubmitJobs=NativeAdrenoSubmitRanges+12,
    NativeAdrenoSubmitRangeQueues=8,                       /* NativeAdrenoAbiMaxQueues: one client's queues */
    NativeAdrenoSubmitSharedJobs=NativeAdrenoSubmitJobs-NativeAdrenoSubmitRanges,
    NativeAdrenoSubmitRetryLimitUs=200000,                 /* the longest in-kernel wait for ring space (ARCHITECTURE 5.4) */
    NativeAdrenoSubmitHistory=64,                          /* fences whose client range is remembered (page-fault attribution) */
    /* UniXNU-piano (2026-11-22, test only): the hold packet in front of a held job (armHold): CP_WAIT_TIMESTAMP (header + 4) on the
     * memptrs word at NativeAdrenoSubmitHoldOffset, which nothing writes (0), waited for >= 1. */
    NativeAdrenoSubmitHoldWords=5,
    NativeAdrenoSubmitHoldOffset=0x800,
    /* A job's packets as the ring got them (kept for a recovery's replay): one CP_INDIRECT_BUFFER (4 words) per buffer, the hold. */
    NativeAdrenoSubmitJobWords=NativeAdrenoSubmitMaxBuffers*4+NativeAdrenoSubmitHoldWords,
    /* The replay store attachReplayStore() takes: every job slot's words (157 KiB on macOS, 91 KiB upstream). Outside the object, so the
     * kext's static data stays as it was (gpu-recover2's first KC: 0x43180 bytes of __DATA found no gap in the kext collection). */
    NativeAdrenoSubmitReplayStoreWords=NativeAdrenoSubmitJobs*NativeAdrenoSubmitJobWords,
};
/* UniXNU-piano (2026-11-22, private 20261122-render/ANALYSIS.md "ONE change"): a recovery's plan for the jobs in flight, made once the GPU
 * stopped (NativeAdrenoSubmit::plan). Replay (the behaviour of Linux msm recover_worker): the jobs the CP passed retire, the job the CP
 * stood in (fence done + 1) is the culprit when the cause is a client's (charge): its fence counts as completed (restart = its fence)
 * and only its range is faulted; every other job in flight is replayed with its own fence (replay()). FailAll (the behaviour before):
 * restart = the last fence issued, every range with a job in flight is faulted and every job dropped; taken when the submit object is
 * poisoned, when a fence in (restart, issued] has no job here (a kernel submission: nothing could complete it), or when the jobs would
 * not fit the empty ring again, or a job in flight has no kept words (no replay store attached: attachReplayStore). */
enum : uint8_t {NativeAdrenoSubmitPlanNone,NativeAdrenoSubmitPlanReplay,NativeAdrenoSubmitPlanFailAll};
enum : uint8_t {NativeAdrenoSubmitFallbackNone,NativeAdrenoSubmitFallbackPoisoned,NativeAdrenoSubmitFallbackGap,
                NativeAdrenoSubmitFallbackRoom,NativeAdrenoSubmitFallbackNoStore};
struct NativeAdrenoSubmitPlan {
    uint8_t mode,fallback;
    uint32_t done,issued,restart;          /* the CP's completed fence at the plan, the last issued, the fence the CP restarts at */
    uint32_t culprit_range,culprit_fence;  /* UINT32_MAX / 0: no culprit */
    uint32_t retired,failed,replay_jobs,replay_words;
    uint32_t first_replay,last_replay;     /* fences of the first and last job replayed (0: none) */
    NativeAdrenoRangeMask fault_ranges,replay_ranges;
    uint8_t replay_count[NativeAdrenoSubmitRangeCount];   /* jobs replayed per range */
};
/* Bit r of a range mask (r < NativeAdrenoSubmitRanges). */
static inline NativeAdrenoRangeMask NativeAdrenoRangeBit(uint32_t range) {return NativeAdrenoRangeMask(1)<<range;}
static_assert(NativeAdrenoSubmitRanges<=sizeof(NativeAdrenoRangeMask)*8 && !(NativeAdrenoSubmitRanges&(NativeAdrenoSubmitRanges-1)),
              "a range mask holds every range; the history keeps the range in log2(ranges) bits");
/* Internal status, never returned to user space: the ring has no room for this submission now and the caller has nothing in flight,
 * so it must not be told Busy (Turnip could only spin 64 x 100 us and then declare the device lost). The caller of submit() drops
 * every lock, sleeps until *retry_fence completes (or its 200 ms bound passes) and submits again; NativeAdrenoSubmitWithRetry below is
 * that loop. The value is kIOReturnNoSpace's, so a leak would still be an error, never a success. */
static constexpr IOReturn NativeAdrenoSubmitRetry=IOReturn(0xe00002dbu);
#if defined(kIOReturnNoSpace)
static_assert(NativeAdrenoSubmitRetry==kIOReturnNoSpace,"NativeAdrenoSubmitRetry is kIOReturnNoSpace's value");
#endif
struct NativeAdrenoSubmitBuffer {
    uint64_t handle;
    uint64_t iova;
    size_t bytes;      /* Complete indirect-buffer range in bytes. */
};
struct NativeAdrenoSubmitStats {
    uint32_t submissions,completed,timeouts,poisoned_jobs;
    uint32_t busy,retries,abandoned,sweeps;   /* Busy answers, NativeAdrenoSubmitRetry answers, jobs dropped by recover(), on-demand retires */
    size_t references_held,references_retired,peak_jobs;
    /* UniXNU-piano (2026-11-22): jobs replayed by recoveries, culprit jobs skipped, plans that fell back to failing every job, holds. */
    uint32_t replayed,skipped,fallbacks,holds;
};
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-21, 20261121-ws-crash: "WindowServer slot 0 Submit -> BadArgument" said nothing of which check refused): why a
 * submission was refused, for the console's line after its "call failed" line (NativeAdrenoSubmitRefusalLine). index is the refused
 * entry of the request (UINT32_MAX: none) with its handle, iova and bytes; mapped and mapped_bytes the mapping or object the entry was
 * checked against (0: none found); a and b named per code. The user-client core fills what it checks itself and the entry's BO handle,
 * submit() (its refusal argument) the VM's view: there handle is the VM's mapping handle. */
enum NativeAdrenoSubmitWhy : uint8_t {
    NativeAdrenoSubmitWhyNone,
    NativeAdrenoSubmitWhyNotOpen,     /* the connection is closed, or the submit object not attached or poisoned */
    NativeAdrenoSubmitWhyCount,       /* count the buffer_count, a reserved: no entry, more than the most, or reserved nonzero */
    NativeAdrenoSubmitWhyQueue,       /* a the queue: not one of the connection's open queues */
    NativeAdrenoSubmitWhyWaitFence,   /* a the wait_fence: never issued */
    NativeAdrenoSubmitWhyBytes,       /* the entry's bytes 0, not a multiple of 4 or past the largest IB */
    NativeAdrenoSubmitWhyHandle,      /* the entry's handle is no live object of the connection; a the next handle it gives out */
    NativeAdrenoSubmitWhyNotMapped,   /* a live object that is not GPU-mapped (mapped_bytes its size), or the VM's mapping is unknown or
                                       * not wholly mapped */
    NativeAdrenoSubmitWhyRange,       /* the IB is not inside its mapping [mapped, +mapped_bytes) or the mapping is not GPU-readable (a
                                       * its permissions) */
    NativeAdrenoSubmitWhyQueueFull,   /* no job slot or no ring room, or a recovery holds the ring */
    NativeAdrenoSubmitWhyOther,       /* a device reference, the submission's maintenance or the ring refused (status) */
};
struct NativeAdrenoSubmitRefusal {uint8_t why;uint32_t index,count;uint64_t handle,iova,bytes,mapped,mapped_bytes,a,b;IOReturn status;};
#endif
/* The M10 lock split (ARCHITECTURE 5.4): the publish shell installs IOLock hooks; without hooks nothing is serialised here (host
 * tests, or a caller that serialises everything itself). Ring covers this object's job slots and every submission into the ring
 * (held for microseconds; never while sleeping). Memory covers the BO pool behind NativeAdrenoMemory, which the user-client cores
 * share (a pooled BO's allocation and release), and user imports; a BO outside the pool is allocated, write-combined and released
 * without it (NativeAdrenoMemory::allocatePooled: it touches nothing shared). The VM serialises itself (NativeAdrenoVm's own lock)
 * and is called with or without these. Order: a connection's own lock, then Ring or Memory (never both), then the VM's. */
enum NativeAdrenoSubmitLock : uint32_t {NativeAdrenoSubmitLockRing,NativeAdrenoSubmitLockMemory};
struct NativeAdrenoSubmitLocks {
    void *cookie;
    void (*lock)(void *cookie,NativeAdrenoSubmitLock which);
    void (*unlock)(void *cookie,NativeAdrenoSubmitLock which);
};
/* Kernel-side execbuf: turns validated indirect buffers into one framed ring submission and owns a device reference per distinct
 * buffer until the timestamp fence is actually observed (the CP-written memptrs fence, read with acquire semantics; the one ring
 * completes fences in sequence). Each buffer's mapping also learns the fence of its use (NativeAdrenoVm::noteUse). This is not a
 * userspace ABI and not a scheduler: one in-order ring, no reordering or isolation between queues. Buffers may be sub-ranges of a
 * live VM mapping but never a different IOVA, an unmapped handle, a table pool or a range that includes unreadable bytes. The
 * submit path does no TLB invalidation, no logging, no CPU wait and no cache maintenance beyond the VM's maintained list (GPU-read-
 * only imports mapped non-cacheable, empty from coherence level 1; ARCHITECTURE 2.2, 4.14.1, 5.4): IB validation, references,
 * the ring words and the fenced WPTR write. A failed ring commit that poisons the ring keeps every reference and the submit object
 * unusable until recover() (M10). */
class NativeAdrenoSubmit final {
    struct Job {
        uint32_t fence=0;
        uint32_t queue=0;
        uint32_t count=0;
        bool active=false;
        uint64_t handles[NativeAdrenoSubmitMaxBuffers]{};
        /* UniXNU-piano (2026-11-22): what the ring got, for a recovery's replay: the packets (in the replay store's slice of this job
         * slot, kept says they are there), the client's GPU context, the source. */
        uint32_t word_count=0,context=0,source=0;
        bool kept=false;
    } jobs_[NativeAdrenoSubmitJobs];
    NativeAdrenoVm *vm_=nullptr;
    NativeAdrenoRing *ring_=nullptr;
    NativeAdrenoSubmitLocks locks_{};
    NativeAdrenoSubmitStats stats_{};
    bool attached_=false,poisoned_=false,halted_=false;
    /* submit()'s distinct buffers and ring words, used under the Ring lock: too large for the kernel stack at 128 IBs */
    uint64_t scratch_handles_[NativeAdrenoSubmitMaxBuffers]{};
    uint32_t scratch_words_[NativeAdrenoSubmitJobWords]{};
    /* UniXNU-piano (2026-11-22): the ranges whose next submission is held (armHold), the fence of the last held job; the plan of the
     * running recovery (plan() to replay()). Under the Ring lock. */
    NativeAdrenoRangeMask hold_ranges_=0;
    uint32_t held_fence_=0;
    NativeAdrenoSubmitPlan plan_{};
    uint32_t *store_=nullptr;   /* attachReplayStore: NativeAdrenoSubmitJobWords words per job slot */
    uint32_t *keptWords(const Job &job) const {return store_+size_t(&job-jobs_)*NativeAdrenoSubmitJobWords;}
    /* The active jobs in fence order after `from` (sequence numbers), into order[]; returns how many. */
    uint32_t orderLocked(uint32_t from,uint32_t *order) const;
    void dropAllLocked(NativeAdrenoRangeMask *ranges,IOReturn *result);
    /* The client range of the latest NativeAdrenoSubmitHistory fences this object issued, at fence % NativeAdrenoSubmitHistory:
     * fence | range << 32 | 1 << 63 (0: none). Written and read under the Ring lock; kept across completion and recovery. */
    uint64_t history_[NativeAdrenoSubmitHistory]{};
    /* The unlocked bodies; every public method takes the Ring lock around one of them. */
    Job *findLocked(uint32_t fence);
    Job *slotLocked(uint32_t range);
    bool rangeOutstandingLocked(uint32_t first,uint32_t count) const;
    size_t outstandingLocked() const;
    IOReturn releaseLocked(Job &);
    IOReturn retireCompletedLocked();
    IOReturn poison();
    void dropReferences(const uint64_t *handles,size_t count);
    /* The CP's completed fence (memptrs + 4) and a fence's state without waiting: Complete, Timeout (not yet) or Argument (never
     * issued). Lock-free reads: the ring publishes its issued count atomically. */
    uint32_t completedValue() const;
    NativeAdrenoRingFence state(uint32_t fence,uint32_t *observed) const;
public:
    NativeAdrenoSubmit()=default;
    ~NativeAdrenoSubmit()=default;
    NativeAdrenoSubmit(const NativeAdrenoSubmit &)=delete;
    NativeAdrenoSubmit &operator=(const NativeAdrenoSubmit &)=delete;
    IOReturn attach(NativeAdrenoVm *,NativeAdrenoRing *);
    /* Refuses while jobs are outstanding or poisoned; the caller must have stopped the GPU and observed every fence before calling this. */
    IOReturn detach();
    /* Installs (or with null clears) the lock hooks; only while no job is outstanding. */
    IOReturn attachLocks(const NativeAdrenoSubmitLocks *);
    void lock(NativeAdrenoSubmitLock which) const {if(locks_.lock)locks_.lock(locks_.cookie,which);}
    void unlock(NativeAdrenoSubmitLock which) const {if(locks_.unlock)locks_.unlock(locks_.cookie,which);}
    /* One submission into the ring for client range queue / NativeAdrenoSubmitRangeQueues (0..NativeAdrenoSubmitRanges - 1, i.e. 0..63;
     * queue is bookkeeping, it never reorders). Returns:
     *   kIOReturnSuccess, *fence nonzero;
     *   kIOReturnBusy: no job slot or no ring room, and the caller's range HAS a job in flight (Turnip waits for its oldest fence);
     *   NativeAdrenoSubmitRetry: no ring room and the caller's range has nothing in flight; *retry_fence (when given) is the fence
     *     whose completion frees room: sleep on it without any lock, then submit again (at most NativeAdrenoSubmitRetryLimitUs);
     *   kIOReturnNoResources: the submission cannot fit even into the empty ring (never for <= 128 IBs and a ring >= 8 KiB);
     *   kIOReturnBadArgument / NotReady / IOError (poisoned) as before. Nothing is referenced or written unless it succeeds.
     * context (nonzero) is the submitting client's GPU context: NativeAdrenoRing::useContext runs under the Ring lock right before
     * the words, so a client switch carries the ring's context entry exactly in front of that client's submission. source
     * (NativeAdrenoRingSourceRender for the render server's connections, else External) goes with the words to the ring's stamp:
     * the queue meter's attribution (ARCHITECTURE 4.13.5); it changes nothing else. UniXNU-piano: refusal (optional) receives why
     * a refused submission was refused (NativeAdrenoSubmitRefusal; untouched on success). */
    IOReturn submit(const NativeAdrenoSubmitBuffer *,size_t,uint32_t queue,uint32_t *fence,uint32_t *retry_fence=nullptr,
        uint32_t context=0,uint32_t source=NativeAdrenoRingSourceExternal
#ifdef ADRENO830_MACOS
        ,NativeAdrenoSubmitRefusal *refusal=nullptr
#endif
        );
    bool queueOutstanding(uint32_t queue) const;
    /* Queues [first, first+count): whether any of their jobs is outstanding, and a wait for every one of them (oldest first) that
     * leaves other queues' jobs alone. Each client of the shared ring owns one such range. The waits poll with the clock's idle hook
     * between zero-timeout checks and hold no lock while idle (connection teardown only; fence waits of clients sleep on the fence
     * event in the publish shell instead). */
    bool rangeOutstanding(uint32_t first,uint32_t count) const;
    IOReturn waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Retires (without waiting) every job whose fence the CP has already passed, so completed work never pins buffers or job slots
     * just because nobody called wait(). The fence interrupt's retire_call runs it (native_adreno_retire_completed). */
    IOReturn retireCompleted();
    /* Waits for every outstanding job in submission order and retires its references. */
    IOReturn waitAll(const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Checks the fence (timeout_us 0: once, which is all the kernel's WaitFence does after its lock-free sleep) and on completion
     * retires the job's references. A fence whose job was already retired succeeds once the CP has passed it; a fence the ring never
     * issued is kIOReturnBadArgument. */
    IOReturn wait(uint32_t fence,const NativeAdrenoRingClock *,uint32_t timeout_us);
    /* Whether the ring issued this fence (nonzero, at most the newest handed out): the only check a submission's wait_fence needs, as
     * the one ring executes in order and every framed submission starts with the BR/BV sync (ARCHITECTURE 4.13.3). */
    bool issued(uint64_t fence) const;
    /* Whether the CP has passed the fence (false for a fence never issued). */
    bool completed(uint32_t fence) const;
    uint32_t lastSubmitted() const;
    /* The jobs in flight when a GPU page fault's top half read the completed fence and the last one issued (M14 P0, ARCHITECTURE
     * 4.12.3): *ranges gets bit r for the client range of every fence in (completed, issued] (0 when the interval is empty: nothing
     * was in flight). kIOReturnUnsupported when the interval is longer than NativeAdrenoSubmitHistory or holds a fence this object
     * did not issue or no longer remembers (a kernel submission, an older numbering): nothing can be said then. Ring lock; thread
     * context only (the fault's bottom half). */
    IOReturn rangesInFlight(uint32_t completed,uint32_t issued,NativeAdrenoRangeMask *ranges) const;
    /* M10 recovery (ARCHITECTURE 4.13.4). halt(): from now on submit() queues nothing (NativeAdrenoSubmitRetry without a retry
     * fence, or Busy for a range with a job in flight) until resume(); *ranges_in_flight gets bit r for every client range with a job
     * in flight at that moment, under the same lock, so no submission slips between the snapshot and the halt. recover(), after the
     * GPU was power cycled and the ring restarted (nothing of the outstanding jobs runs any more): every job is dropped and its
     * references released, and a poisoned submit object becomes usable again; *abandoned_ranges gets bit r for every client range
     * that lost a job. Both are refused (NotReady) while not attached. *oldest_range (optional) gets the range of the job the CP stands
     * in: the nearest one after the completed fence (on the one in-order ring, the job a hang stopped in), never a job the CP already
     * passed nor a poisoned commit's (fence 0, never issued); UINT32_MAX without such a job. A poisoned object retires nothing first,
     * so its passed jobs and the poisoned commit still count in *ranges_in_flight (recover() drops them). */
    IOReturn halt(NativeAdrenoRangeMask *ranges_in_flight,uint32_t *oldest_range=nullptr);
    void resume();
    bool halted() const {return __atomic_load_n(&halted_,__ATOMIC_ACQUIRE);}
    IOReturn recover(NativeAdrenoRangeMask *abandoned_ranges);
    /* UniXNU-piano (2026-11-22, replay; NativeAdrenoSubmitPlan above). plan(): once the GPU stopped (no fence moves any more), while
     * halted: the jobs the CP passed retire, the culprit is found (charge: the job at the completed fence + 1, never a kernel fence) and
     * *out says what the restart does; nothing is dropped yet. replay(): after the CP restarted at out->restart (NativeAdrenoRing::
     * recover(last, &restart), CP_ME_INIT, the secure resume): Replay retires the culprit's job (its fence passed) and puts every
     * other job into the ring again in fence order with its own fence and context (NativeAdrenoRing::replayFramed); FailAll drops
     * every job as recover() does. *replayed gets the jobs resubmitted. An error leaves the object poisoned (the device is lost then).
     * Both refused (NotReady) while not attached; replay() without a plan is recover(). */
    IOReturn plan(bool charge,NativeAdrenoSubmitPlan *out);
    IOReturn replay(uint32_t *replayed);
    /* UniXNU-piano (2026-11-22, test only: piano-gpu-fault-test=1, machdep.piano_gpu_fault_hold): the next submission of any range in
     * ranges starts with the hold packet (NativeAdrenoSubmitHoldWords): the CP stops in that job for good, a real hang whose culprit
     * is known (the watchdogs find it, the recovery skips it). One shot: the first held submission disarms every range. 0 disarms.
     * Unsupported when the memptrs page has no room for the hold word. */
    IOReturn armHold(NativeAdrenoRangeMask ranges);
    /* The replay store (UniXNU-piano 2026-11-22): words[NativeAdrenoSubmitReplayStoreWords] the caller owns (the publish shell's heap
     * allocation; a host test's array) where submit() keeps each job's packets; null detaches. Only while no job is outstanding.
     * Without one nothing is kept and every plan falls back to failing the jobs in flight (NativeAdrenoSubmitFallbackNoStore). */
    IOReturn attachReplayStore(uint32_t *words,size_t count);
    bool replayStore() const {return store_!=nullptr;}
    NativeAdrenoRangeMask holdArmed() const;
    uint32_t heldFence() const;
    bool poisoned() const {return poisoned_;}
    size_t outstanding() const;
    NativeAdrenoSubmitStats stats() const {return stats_;}
};
/* RAII hold of one of the submit object's locks (Memory for the user-client core's pooled BOs and imports; a null submit object
 * holds none, for a BO outside the pool). */
class NativeAdrenoSubmitGuard final {
    const NativeAdrenoSubmit *submit_;
    NativeAdrenoSubmitLock which_;
public:
    NativeAdrenoSubmitGuard(const NativeAdrenoSubmit *submit,NativeAdrenoSubmitLock which):submit_(submit),which_(which)
    {if(submit_)submit_->lock(which_);}
    ~NativeAdrenoSubmitGuard() {if(submit_)submit_->unlock(which_);}
    NativeAdrenoSubmitGuard(const NativeAdrenoSubmitGuard &)=delete;
    NativeAdrenoSubmitGuard &operator=(const NativeAdrenoSubmitGuard &)=delete;
};
/* The bounded wait of ARCHITECTURE 5.4 around one submission: call(&retry_fence) submits under the caller's locks and returns the
 * status; on NativeAdrenoSubmitRetry, sleep(retry_fence, deadline_us) waits without any lock (on the fence event in the kernel) and
 * the submission is made again, until it is something else or limit_us passed since the first attempt, which turns it into
 * kIOReturnBusy. now() is microseconds. NativeAdrenoSubmitRetry never leaves this function. */
template<class Call,class Sleep,class Now>
static inline IOReturn NativeAdrenoSubmitWithRetry(Call call,Sleep sleep,Now now,uint64_t limit_us=NativeAdrenoSubmitRetryLimitUs)
{
    const uint64_t began=now();
    for(;;) {
        uint32_t retry_fence=0;
        const IOReturn status=call(&retry_fence);
        if(status!=NativeAdrenoSubmitRetry)return status;
        const uint64_t at=now();
        if(at-began>=limit_us)return kIOReturnBusy;
        sleep(retry_fence,began+limit_us);
    }
}
#endif
