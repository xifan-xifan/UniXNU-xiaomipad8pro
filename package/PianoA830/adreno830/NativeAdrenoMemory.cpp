/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "A830GpuTier.h"
#include "NativeAdrenoMeter.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
/* pagani-gpu-kopt and the call meter's figures (NativeAdrenoKopt.h, NativeAdrenoMeter.h): here because every file that reads or writes
 * them links this one. */
extern "C" {
volatile uint32_t native_adreno_kopt=NativeAdrenoKoptAll;
volatile uint32_t native_adreno_meter_calls[Adreno830CallKinds],native_adreno_meter_waits[Adreno830WaitKinds];
volatile uint32_t native_adreno_meter_zero[2],native_adreno_meter_contig[3],native_adreno_meter_fence;
}
void NativeAdrenoMemory::setKopt(uint32_t bits) {native_adreno_kopt=bits;}
#include "NativeAdrenoPoolBypass.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <string.h>
#ifdef ADRENO830_HOST_TEST
#include <stdio.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#else
#include <libkern/libkern.h>   /* snprintf */
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <mach/thread_policy.h>
#endif
/* UniXNU-piano (2026-11-22, GPU-PAGED P0b): the pool bypass and its figures (NativeAdrenoPoolBypass.h). */
extern "C" {
volatile uint32_t native_adreno_pool_bypass=0,native_adreno_pool_bypass_flips=0;
uint64_t (*native_adreno_p0b_now_us)(void)=nullptr;
volatile uint64_t native_adreno_p0b_alloc[NativeAdrenoPaths][5];
volatile uint64_t native_adreno_p0b_alloc_us[NativeAdrenoPaths][NativeAdrenoP0bTimeWords];
volatile uint64_t native_adreno_p0b_wire_us[2][NativeAdrenoP0bTimeWords];
volatile uint64_t native_adreno_p0b_lookup[2][3];
volatile uint64_t native_adreno_p0b_map_us[NativeAdrenoP0bMapKinds][NativeAdrenoP0bTimeWords];
volatile uint64_t native_adreno_p0b_map_pages[NativeAdrenoP0bMapKinds];
volatile uint64_t native_adreno_p0b_tlbi[4];
volatile uint64_t native_adreno_p0b_submit[3];
}

/* ---- UniXNU-piano (2026-11-23, GPU-PAGED P1b): the parallel wiring of a split user BO's parts --------------------------------------
 * One queue of batches (a BoAlloc's parts), worker threads started on first use (up to native_adreno_wire_workers), the calling thread
 * wiring parts of its own batch too and then waiting until every part answered. A batch lives on the caller's stack: it leaves the
 * queue (under the lock) before the caller returns, and a worker only touches it under the lock or between taking a part and
 * counting it done. */
extern "C" {volatile uint32_t native_adreno_wire_workers;volatile uint64_t native_adreno_wire_stats[6];
volatile uint32_t native_adreno_wire_priority=NativeAdrenoWirePriorityDefault;}
namespace {
struct WireBatch {IOBufferMemoryDescriptor **parts;IOReturn *results;unsigned count,next,done;WireBatch *link;};
WireBatch *wireHead;
unsigned wireThreads;
#ifdef ADRENO830_HOST_TEST
std::mutex wireMutex;std::condition_variable wireCv;
struct WireLocked {std::unique_lock<std::mutex> l{wireMutex};};
void wireWaitLocked(WireLocked &w) {wireCv.wait(w.l);}
void wireWakeLocked() {wireCv.notify_all();}
bool wireReady() {return true;}
#else
IOLock *wireLock;
struct WireLocked {WireLocked() {IOLockLock(wireLock);} ~WireLocked() {IOLockUnlock(wireLock);}};
void wireWaitLocked(WireLocked &) {IOLockSleep(wireLock,&wireHead,THREAD_UNINT);}
void wireWakeLocked() {IOLockWakeup(wireLock,&wireHead,false);}
bool wireReady()
{
    if(__atomic_load_n(&wireLock,__ATOMIC_ACQUIRE))return true;
    IOLock *lock=IOLockAlloc();
    if(!lock)return false;
    IOLock *none=nullptr;
    if(!__atomic_compare_exchange_n(&wireLock,&none,lock,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(lock);
    return true;
}
#endif
/* The first batch with a part nobody took (spent batches stay linked until their caller unlinks them). */
WireBatch *wireOpen()
{
    for(WireBatch *b=wireHead;b;b=b->link)if(b->next<b->count)return b;
    return nullptr;
}
void wireUnlink(WireBatch *batch)
{
    for(WireBatch **at=&wireHead;*at;at=&(*at)->link)if(*at==batch) {*at=batch->link;return;}
}
/* Under the lock: one part taken, wired without it, counted done under it again. */
void wireOne(WireLocked &w,WireBatch *b,bool worker)
{
    const unsigned i=b->next++;
    IOBufferMemoryDescriptor *part=b->parts[i];
#ifdef ADRENO830_HOST_TEST
    w.l.unlock();
    const IOReturn r=part->prepare();
    w.l.lock();
#else
    (void)w;
    IOLockUnlock(wireLock);
    const IOReturn r=part->prepare();
    IOLockLock(wireLock);
#endif
    b->results[i]=r;
    if(worker)__atomic_fetch_add(&native_adreno_wire_stats[5],1u,__ATOMIC_RELAXED);
    if(++b->done==b->count)wireWakeLocked();
}
/* The worker's own priority (P1d): THREAD_PRECEDENCE_POLICY's importance is relative to kernel_task's BASEPRI_KERNEL. */
uint32_t wireWanted()
{
    const uint32_t want=__atomic_load_n(&native_adreno_wire_priority,__ATOMIC_RELAXED);
    return !want || want>NativeAdrenoWirePriorityKernel?uint32_t(NativeAdrenoWirePriorityKernel):want;
}
uint32_t wireApplyPriority(uint32_t applied)
{
    const uint32_t want=wireWanted();
    if(want==applied)return applied;
#ifndef ADRENO830_HOST_TEST
    thread_precedence_policy_data_t policy={integer_t(want)-integer_t(NativeAdrenoWirePriorityKernel)};
    /* a refusal leaves the thread where it was; the wanted value is still recorded, so it is not retried part by part */
    (void)thread_policy_set(current_thread(),THREAD_PRECEDENCE_POLICY,reinterpret_cast<thread_policy_t>(&policy),
        THREAD_PRECEDENCE_POLICY_COUNT);
#endif
    return want;
}
void wireWorkerLoop()
{
    uint32_t applied=NativeAdrenoWirePriorityKernel;   /* kernel_thread_start's */
    applied=wireApplyPriority(applied);
    WireLocked w;
    for(;;) {
        WireBatch *b=wireOpen();
        if(!b) {wireWaitLocked(w);continue;}
        if(wireWanted()!=applied) {
            /* outside the lock: thread_policy_set may block */
#ifdef ADRENO830_HOST_TEST
            w.l.unlock();applied=wireApplyPriority(applied);w.l.lock();
#else
            IOLockUnlock(wireLock);applied=wireApplyPriority(applied);IOLockLock(wireLock);
#endif
            continue;
        }
        wireOne(w,b,true);
    }
}
#ifndef ADRENO830_HOST_TEST
void wireWorkerMain(void *,wait_result_t) {wireWorkerLoop();}
#endif
/* Up to `wanted` workers running (never fewer once started). */
void wireStart(unsigned wanted)
{
    if(wanted>NativeAdrenoWireMaxWorkers)wanted=NativeAdrenoWireMaxWorkers;
    for(;;) {
        unsigned have=__atomic_load_n(&wireThreads,__ATOMIC_ACQUIRE);
        if(have>=wanted)return;
        if(!__atomic_compare_exchange_n(&wireThreads,&have,have+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))continue;
#ifdef ADRENO830_HOST_TEST
        std::thread(wireWorkerLoop).detach();
#else
        thread_t thread=nullptr;
        if(kernel_thread_start(wireWorkerMain,nullptr,&thread)!=KERN_SUCCESS) {__atomic_fetch_sub(&wireThreads,1u,__ATOMIC_ACQ_REL);return;}
        thread_deallocate(thread);
#endif
        __atomic_fetch_add(&native_adreno_wire_stats[4],1u,__ATOMIC_RELAXED);
    }
}
/* Every part prepared (results[i] each), by the workers and this thread. */
void wireParts(IOBufferMemoryDescriptor **parts,IOReturn *results,unsigned count,unsigned workers)
{
    if(!workers || !wireReady()) {for(unsigned i=0;i<count;++i)results[i]=parts[i]->prepare();return;}
    wireStart(workers);
    WireBatch b={parts,results,count,0,0,nullptr};
    WireLocked w;
    WireBatch **tail=&wireHead;while(*tail)tail=&(*tail)->link;
    *tail=&b;
    wireWakeLocked();
    while(b.next<b.count)wireOne(w,&b,false);
    while(b.done<b.count)wireWaitLocked(w);
    wireUnlink(&b);
}
}

static bool userUnmapped;
static uint64_t ownedMapped,ownedUnmapped,importBytes,mappedPeak,backingAllocs,backingFrees;
void NativeAdrenoMemory::setUnmappedUser(bool enabled) {userUnmapped=enabled;}
bool NativeAdrenoMemory::unmappedUser() {return userUnmapped;}
#ifndef ADRENO830_HOST_TEST
IOMemoryDescriptor *NativeAdrenoMemory::retainCpuView()
{
    if(!unmapped_ || !prepared_ || quarantined_)return nullptr;
    if(multi_) {multi_->retain();return multi_;}
    if(!descriptor_)return nullptr;
    descriptor_->retain();return descriptor_;
}
#endif
IOBufferMemoryDescriptor *NativeAdrenoMemory::retainVmCpuDescriptor()
{
    if(!unmapped_ || !prepared_ || quarantined_ || !descriptor_)return nullptr;
    descriptor_->retain();return descriptor_;
}
NativeAdrenoBackingTotals NativeAdrenoMemory::backingTotals()
{
    return {__atomic_load_n(&ownedMapped,__ATOMIC_RELAXED),__atomic_load_n(&ownedUnmapped,__ATOMIC_RELAXED),
        __atomic_load_n(&importBytes,__ATOMIC_RELAXED),__atomic_load_n(&mappedPeak,__ATOMIC_RELAXED),
        __atomic_load_n(&backingAllocs,__ATOMIC_RELAXED),__atomic_load_n(&backingFrees,__ATOMIC_RELAXED)};
}
static void accountBacking(size_t bytes,bool unmapped)
{
    const uint64_t current=__atomic_add_fetch(unmapped?&ownedUnmapped:&ownedMapped,bytes,__ATOMIC_RELAXED);
    if(!unmapped) {
        uint64_t peak=__atomic_load_n(&mappedPeak,__ATOMIC_RELAXED);
        while(current>peak && !__atomic_compare_exchange_n(&mappedPeak,&peak,current,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    }
    __atomic_fetch_add(&backingAllocs,1ull,__ATOMIC_RELAXED);
}
static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* The pool (see reservePool): a free list of page extents over one contiguous block; best fit, neighbours merged on release. */
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The pool's write-combine (writeCombinePool): bit 31 once tried, the low bits its status. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}   /* [0] pool MiB, [1] BOs from the pool, [2] fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans */
/* The pool's shape: [0] free extents now, [1] their peak, [2] pooled BOs alive, [3] allocations sent past the pool because
 * PoolExtentCapacity - 1 pooled BOs were alive, [4] releases whose extent found no room (never: see Pool), [5] the MiB they lost. */
extern "C" {volatile uint32_t native_adreno_pool_shape[6];}
#ifdef ADRENO830_MACOS
/* UniXNU-piano: [0] pooled BOs cut from several free extents (poolTakeMany), [1] the extents they took, [2] BOs the free extents
 * could not cover within NativeAdrenoPoolMaxPieces (or the gaps bound), sent past the pool. */
extern "C" {volatile uint32_t native_adreno_pool_split[3];}
#endif
namespace {
/* The free extents are kept sorted by address and fully merged, so every one of them is a gap between two pooled BOs (or an end of
 * the pool): with L pooled BOs alive there are at most L + 1. The table holds PoolExtentCapacity of them and a BO is taken from the
 * pool only while fewer than PoolExtentCapacity - 1 pooled BOs are alive, so a release always finds its slot or merges. The table of
 * 512 before this held too few: about 1000 live BOs of mixed sizes fragment the pool into some 500 gaps, and from then on every
 * release that needed a slot, a merge with a neighbour included, was dropped, so the pool drained for the rest of the boot while
 * its "used" figure went down, and every later BO fell back to contiguous or scattered pages (a contiguous search of the VM, page
 * attributes changed one page at a time under the Memory lock, wired memory of the system, one GPU span per page). Extents are in
 * pages: a pool is at most 2 GiB (pagani-gpu-pool <= 2048). */
enum : unsigned {PoolExtentCapacity=16384};
struct PoolExtent {uint32_t page,pages;};
struct Pool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *virt=nullptr;uint64_t physical=0;size_t bytes=0,used=0,peak=0;
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
    /* UniXNU-piano (k27 native kext): the 128 KiB extent table on the heap, allocated with the pool (reservePool): the k27 kernel
     * collection has no room for it in a native kext's __DATA. Nothing reads it before the pool exists (count stays 0). */
    PoolExtent *free=nullptr;unsigned count=0,live=0;
#else
    PoolExtent free[PoolExtentCapacity];unsigned count=0,live=0;
#endif
    bool write_combined=false;
} pool;
void poolShape()
{
    native_adreno_pool_shape[0]=pool.count;
    if(pool.count>native_adreno_pool_shape[1])native_adreno_pool_shape[1]=pool.count;
    native_adreno_pool_shape[2]=pool.live;
}
void poolRemove(unsigned at)
{
    memmove(&pool.free[at],&pool.free[at+1],(pool.count-at-1)*sizeof(PoolExtent));
    --pool.count;
}
bool poolTake(size_t bytes,size_t *offset) {
    if(pool.live>=PoolExtentCapacity-1) {native_adreno_pool_shape[3]=native_adreno_pool_shape[3]+1;return false;}
    const uint32_t pages=uint32_t(bytes>>12);
    int best=-1;
    for(unsigned i=0;i<pool.count;++i) {
        if(pool.free[i].pages<pages || (best>=0 && pool.free[i].pages>=pool.free[best].pages))continue;
        best=(int)i;
        if(pool.free[i].pages==pages)break;   /* an exact fit is the best fit */
    }
    if(best<0)return false;
    *offset=size_t(pool.free[best].page)<<12;
    pool.free[best].page+=pages;pool.free[best].pages-=pages;
    if(!pool.free[best].pages)poolRemove(unsigned(best));
    ++pool.live;
    pool.used+=bytes;if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    poolShape();
    return true;
}
#ifdef ADRENO830_MACOS
void poolGive(size_t offset,size_t bytes);
/* UniXNU-piano (2026-11-12, the first desktop sessions): a BO the pool holds in no single free extent is cut from several, the largest
 * first and the last one best fit, at most `max` of them (one span and one GPU range each). In run desk-dev the pool had 168 MiB free
 * in extents of at most 19 MiB when WindowServer's 49 MiB BoAlloc went to XNU's pages instead and XNU had none left to wire: the
 * next 10000 of its BoAllocs failed alike and WindowServer aborted. Only for a write-combined pool (a piece needs no attribute
 * change, and no kernel mapping is kept for the BO: its zero fill goes by span, its CPU views and GPU table take the spans). Each
 * piece counts in `live`, so the gaps bound (at most live + 1 free extents) holds. When they do not cover it, every piece goes back. */
bool poolTakeMany(size_t bytes,NativeAdrenoPhysicalSpan *pieces,size_t max,size_t *count)
{
    *count=0;
    if(!pool.write_combined || pool.bytes-pool.used<bytes)return false;
    size_t remaining=bytes,at=0;
    while(remaining && *count<max && pool.live<PoolExtentCapacity-1) {
        const uint32_t pages=uint32_t(remaining>>12);
        int fit=-1,largest=-1;
        for(unsigned i=0;i<pool.count;++i) {
            const uint32_t n=pool.free[i].pages;
            if(n>=pages && (fit<0 || n<pool.free[fit].pages))fit=(int)i;
            if(largest<0 || n>pool.free[largest].pages)largest=(int)i;
        }
        const int pick=fit>=0?fit:largest;
        if(pick<0)break;
        const uint32_t take=fit>=0?pages:pool.free[pick].pages;
        const size_t offset=size_t(pool.free[pick].page)<<12,piece=size_t(take)<<12;
        pool.free[pick].page+=take;pool.free[pick].pages-=take;
        if(!pool.free[pick].pages)poolRemove(unsigned(pick));
        ++pool.live;pool.used+=piece;
        pieces[(*count)++]={at,pool.physical+offset,piece};
        at+=piece;remaining-=piece;
    }
    if(remaining) {
        for(size_t i=0;i<*count;++i)poolGive(size_t(pieces[i].physical-pool.physical),size_t(pieces[i].bytes));
        *count=0;
        native_adreno_pool_split[2]=native_adreno_pool_split[2]+1;
        return false;
    }
    if(pool.used>pool.peak)pool.peak=pool.used;
    native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);native_adreno_pool_stats[4]=(uint32_t)(pool.peak>>20);
    native_adreno_pool_split[0]=native_adreno_pool_split[0]+1;native_adreno_pool_split[1]=native_adreno_pool_split[1]+uint32_t(*count);
    poolShape();
    return true;
}
#endif
void poolGive(size_t offset,size_t bytes) {
    const uint32_t page=uint32_t(offset>>12),pages=uint32_t(bytes>>12);
    /* The first extent at or after page (the extents are sorted by address and never overlap a live BO). */
    unsigned low=0,high=pool.count;
    while(low<high) {const unsigned mid=low+(high-low)/2;if(pool.free[mid].page<page)low=mid+1;else high=mid;}
    const unsigned at=low;
    const bool left=at && pool.free[at-1].page+pool.free[at-1].pages==page;
    const bool right=at<pool.count && page+pages==pool.free[at].page;
    if(left && right) {pool.free[at-1].pages+=pages+pool.free[at].pages;poolRemove(at);}
    else if(left)pool.free[at-1].pages+=pages;
    else if(right) {pool.free[at].page=page;pool.free[at].pages+=pages;}
    else if(pool.count<PoolExtentCapacity) {
        memmove(&pool.free[at+1],&pool.free[at],(pool.count-at)*sizeof(PoolExtent));
        pool.free[at]={page,pages};++pool.count;
    } else {
        /* Cannot happen (at most live + 1 gaps, and live stays below the capacity); counted, the extent stays out of use. */
        native_adreno_pool_shape[4]=native_adreno_pool_shape[4]+1;
        native_adreno_pool_shape[5]=native_adreno_pool_shape[5]+uint32_t((bytes+(1u<<20)-1)>>20);
    }
    if(pool.live)--pool.live;
    pool.used-=bytes;native_adreno_pool_stats[3]=(uint32_t)(pool.used>>20);
    poolShape();
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.bytes;}
size_t NativeAdrenoMemory::poolFreeBytes() {return pool.descriptor && pool.bytes>pool.used?pool.bytes-pool.used:0;}
#ifdef ADRENO830_MACOS
/* UniXNU-piano: a walk of the free extents (at most PoolExtentCapacity), only when a BoAlloc is refused. */
size_t NativeAdrenoMemory::poolLargestFree(uint32_t *extents)
{
    if(extents)*extents=pool.descriptor?pool.count:0;
    if(!pool.descriptor)return 0;
    uint32_t largest=0;
    for(unsigned i=0;i<pool.count;++i)if(pool.free[i].pages>largest)largest=pool.free[i].pages;
    return size_t(largest)<<12;
}
#endif
uint64_t NativeAdrenoMemory::poolPhysical() {return pool.descriptor?pool.physical:0;}
/* Imports alive (importTask and importDevice objects not yet released), for the MEM row. */
static uint32_t liveImports;
void NativeAdrenoMemory::usage(uint32_t *pool_mib,uint32_t *used_mib,uint32_t *imports)
{
    if(pool_mib)*pool_mib=pool.descriptor?uint32_t(pool.bytes>>20):0;
    if(used_mib)*used_mib=pool.descriptor?uint32_t(pool.used>>20):0;
    if(imports)*imports=__atomic_load_n(&liveImports,__ATOMIC_RELAXED);
}
/* UniXNU-piano (2026-11-22, GPU-PAGED P0b): machdep.piano_gpu_paged's rows and the tick's summary (NativeAdrenoPoolBypass.h). Read
 * without any lock: each figure is one atomic word, a row may mix a figure of this instant with one of the next. */
namespace {
uint64_t p0b(const volatile uint64_t *word) {return __atomic_load_n(word,__ATOMIC_RELAXED);}
unsigned long long u(uint64_t v) {return (unsigned long long)v;}
int p0bTimes(char *out,size_t size,const char *head,const volatile uint64_t *row)
{
    return snprintf(out,size,"%s n=%llu us=%llu max=%llu h=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",head,u(p0b(&row[0])),u(p0b(&row[1])),
        u(p0b(&row[2])),u(p0b(&row[3])),u(p0b(&row[4])),u(p0b(&row[5])),u(p0b(&row[6])),u(p0b(&row[7])),u(p0b(&row[8])),u(p0b(&row[9])),
        u(p0b(&row[10])));
}
/* Where the next text of a row goes and how much room it has (none once the row is full or failed). */
char *p0bAt(char *out,size_t size,int at,size_t *room)
{
    *room=at>0 && size_t(at)<size?size-size_t(at):0;
    return *room?out+at:nullptr;
}
int p0bMore(int at,int more) {return more>0?at+more:at;}
const char *const p0bPathNames[NativeAdrenoPaths]={"pool","contig","user","mapped"};
const char *const p0bMapNames[NativeAdrenoP0bMapKinds]={"pooled","outside","import"};
}
int NativeAdrenoPoolBypassLine(unsigned row,char *out,size_t size)
{
    if(!out || !size)return 0;
    out[0]=0;
    int n=0;
    if(row==0) {
        uint32_t pool_mib=0,used_mib=0;
        NativeAdrenoMemory::usage(&pool_mib,&used_mib,nullptr);
        const NativeAdrenoBackingTotals bt=NativeAdrenoMemory::backingTotals();
        n=snprintf(out,size,"p0b bypass=%u flips=%u clock=%u pool=%u/%uM owned-unmapped=%lluM owned-mapped=%lluM imports=%lluM",
            unsigned(__atomic_load_n(&native_adreno_pool_bypass,__ATOMIC_ACQUIRE)),
            unsigned(__atomic_load_n(&native_adreno_pool_bypass_flips,__ATOMIC_RELAXED)),native_adreno_p0b_now_us?1u:0u,used_mib,pool_mib,
            u(bt.unmapped>>20),u(bt.mapped>>20),u(bt.imports>>20));
    } else if(row<=NativeAdrenoPaths) {
        const unsigned path=row-1;
        char head[96];
        snprintf(head,sizeof head,"alloc %s kib=%llu fails=%llu fail-kib=%llu",p0bPathNames[path],u(p0b(&native_adreno_p0b_alloc[path][1])>>10),
            u(p0b(&native_adreno_p0b_alloc[path][2])),u(p0b(&native_adreno_p0b_alloc[path][3])>>10));
        n=p0bTimes(out,size,head,native_adreno_p0b_alloc_us[path]);
        /* The BOs backed (a failed attempt has no time row): normally equal to the time row's count, which needs the clock. */
        size_t room;char *at=p0bAt(out,size,n,&room);
        if(at)n=p0bMore(n,snprintf(at,room," backed=%llu",u(p0b(&native_adreno_p0b_alloc[path][0]))));
        at=p0bAt(out,size,n,&room);
        if(at && path==NativeAdrenoPathPool)n=p0bMore(n,snprintf(at,room," bypassed=%llu",u(p0b(&native_adreno_p0b_alloc[path][4]))));
    } else if(row<=NativeAdrenoPaths+2) {
        const unsigned kind=row-NativeAdrenoPaths-1;
        n=p0bTimes(out,size,kind?"wire kernel":"wire user",native_adreno_p0b_wire_us[kind]);
        size_t room;char *at=p0bAt(out,size,n,&room);
        if(at)n=p0bMore(n,snprintf(at,room," walk-us=%llu lookups=%llu spans=%llu",u(p0b(&native_adreno_p0b_lookup[kind][0])),
            u(p0b(&native_adreno_p0b_lookup[kind][1])),u(p0b(&native_adreno_p0b_lookup[kind][2]))));
    } else if(row<=NativeAdrenoPaths+2+NativeAdrenoP0bMapKinds) {
        const unsigned kind=row-NativeAdrenoPaths-3;
        char head[64];
        snprintf(head,sizeof head,"map %s pages=%llu",p0bMapNames[kind],u(p0b(&native_adreno_p0b_map_pages[kind])));
        n=p0bTimes(out,size,head,native_adreno_p0b_map_us[kind]);
    } else if(row==NativeAdrenoPaths+3+NativeAdrenoP0bMapKinds) {
        n=snprintf(out,size,"tlbi map-asid=%llu unmap-asid=%llu unmap-val=%llu val-pages=%llu",u(p0b(&native_adreno_p0b_tlbi[0])),
            u(p0b(&native_adreno_p0b_tlbi[1])),u(p0b(&native_adreno_p0b_tlbi[2])),u(p0b(&native_adreno_p0b_tlbi[3])));
    } else if(row==NativeAdrenoPaths+4+NativeAdrenoP0bMapKinds) {
        n=snprintf(out,size,"submit n=%llu maintained=%llu spans=%llu",u(p0b(&native_adreno_p0b_submit[0])),
            u(p0b(&native_adreno_p0b_submit[1])),u(p0b(&native_adreno_p0b_submit[2])));
    } else return 0;
    if(n<0)return 0;
    return size_t(n)<size?n:int(size-1);
}
int NativeAdrenoPoolBypassSummary(char *out,size_t size)
{
    static uint64_t printed=~UINT64_C(0);
    if(!out || !size)return 0;
    out[0]=0;
    uint64_t signature=__atomic_load_n(&native_adreno_pool_bypass,__ATOMIC_ACQUIRE);
    for(unsigned path=0;path<NativeAdrenoPaths;++path)signature=signature*1000003u+p0b(&native_adreno_p0b_alloc[path][0])+
        p0b(&native_adreno_p0b_alloc[path][2]);
    for(unsigned kind=0;kind<NativeAdrenoP0bMapKinds;++kind)signature=signature*1000003u+p0b(&native_adreno_p0b_map_us[kind][0]);
    if(signature==printed)return 0;
    printed=signature;
    const auto avg=[](const volatile uint64_t *row) {const uint64_t n=p0b(&row[0]);return u(n?p0b(&row[1])/n:0);};
    const uint64_t fails=p0b(&native_adreno_p0b_alloc[0][2])+p0b(&native_adreno_p0b_alloc[1][2])+p0b(&native_adreno_p0b_alloc[2][2])+
        p0b(&native_adreno_p0b_alloc[3][2]);
    const int n=snprintf(out,size,"p0b: bypass=%u pool %llu/%lluM %lluus user %llu/%lluM %lluus wire %lluus map %llu/%lluus "
        "tlbi %llu/%llu/%llu fails %llu",unsigned(__atomic_load_n(&native_adreno_pool_bypass,__ATOMIC_ACQUIRE)),
        u(p0b(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])),u(p0b(&native_adreno_p0b_alloc[NativeAdrenoPathPool][1])>>20),
        avg(native_adreno_p0b_alloc_us[NativeAdrenoPathPool]),u(p0b(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0])),
        u(p0b(&native_adreno_p0b_alloc[NativeAdrenoPathUser][1])>>20),avg(native_adreno_p0b_alloc_us[NativeAdrenoPathUser]),
        avg(native_adreno_p0b_wire_us[0]),avg(native_adreno_p0b_map_us[0]),avg(native_adreno_p0b_map_us[1]),
        u(p0b(&native_adreno_p0b_tlbi[0])),u(p0b(&native_adreno_p0b_tlbi[1])),u(p0b(&native_adreno_p0b_tlbi[2])),u(fails));
    if(n<0)return 0;
    return size_t(n)<size?n:int(size-1);
}
/* int-152: a user BO's pages changed attribute one pmap_set_cache_attributes call at a time (each walks the page's mappings, flushes the
 * TLB and cleans the page), on allocation and again on release, under the connection lock: 3 ms a BO on average and 90-130 ms for a
 * surface-sized one, while every other GPU call of every client waited. The pool is the BOs' own block and nothing else maps it, so
 * its pages take the attribute once here and keep it while the pool lives. */
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    if(!pool.descriptor || !pool.virt || !pool.bytes)return kIOReturnNotReady;
    if(pool.write_combined)return kIOReturnSuccess;
    IOReturn status=NativeAdrenoMaintainOwnedPages(pool.virt,pool.bytes,NativeAdrenoCleanInvalidate);
    if(status)return status;
    const NativeAdrenoPhysicalSpan span={0,pool.physical,pool.bytes};
    status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    pool.write_combined=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || (bytes>>12)>UINT32_MAX || !outputWidth(bits))return kIOReturnBadArgument;
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (2026-11-12): never a block the kernel map refuses after taking its pages (NativeAdrenoContiguousMaxBytes). */
    if(bytes>NativeAdrenoContiguousMaxBytes)return kIOReturnBadArgument;
#endif
#if defined(ADRENO830_MACOS) && !defined(ADRENO830_HOST_TEST)
    if(!pool.free)pool.free=IONewZeroData(PoolExtent,PoolExtentCapacity);
    if(!pool.free)return kIOReturnNoMemory;
#endif
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status!=kIOReturnSuccess) {descriptor->release();return status;}
    IOByteCount length=0,tail=0;
    const addr64_t first=descriptor->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    const addr64_t last=descriptor->getPhysicalSegment(bytes-4096,&tail,kIOMemoryMapperNone);
    unsigned char *virt=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    if(!first || (first&4095) || !virt || ((uintptr_t)virt&4095) || last!=first+bytes-4096 || first>=limit || bytes>limit-first ||
        descriptor->getLength()!=bytes) {
        (void)descriptor->complete();descriptor->release();return kIOReturnUnsupported;
    }
    pool.descriptor=descriptor;pool.virt=virt;pool.physical=first;pool.bytes=bytes;pool.used=pool.peak=0;
    pool.free[0]={0,uint32_t(bytes>>12)};pool.count=1;pool.live=0;
    poolShape();
    native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::reservePoolBestEffort(unsigned bits,uint32_t mib,uint32_t min_mib,bool write_combined,
    uint32_t *reserved_mib)
{
    if(!reserved_mib)return kIOReturnBadArgument;
    *reserved_mib=0;
    if(!outputWidth(bits))return kIOReturnBadArgument;
    if(!pool.descriptor) {
        if(!min_mib)min_mib=16;
        /* Halved until a reservation succeeds (a fragmented boot may not have 512 MiB contiguous); none is not an error: every
         * BO is then scattered and changes its own pages' attribute. */
        for(uint32_t size=mib;size && size>=min_mib;size>>=1) {
            const IOReturn status=reservePool(bits,(size_t)size<<20);
            native_adreno_pool_status=(uint32_t)status;
            if(status==kIOReturnSuccess)break;
        }
    }
    if(!pool.descriptor)return kIOReturnSuccess;
    *reserved_mib=uint32_t(pool.bytes>>20);
    native_adreno_pool_stats[0]=*reserved_mib;
    if(write_combined && !pool.write_combined)native_adreno_pool_wc=(uint32_t)writeCombinePool()|1u<<31;
    return kIOReturnSuccess;
}
namespace {
constexpr unsigned TierMaxSpans=64;
struct GpuTier {
    a830tier::Tier slabs;
    a830tier::Run runs[a830tier::kMaxRuns];
    unsigned mode=0,active=0,count=0;
    uint64_t refusals=0,allocations=0;
    const char *state="off";
};
GpuTier gpuTier;
#ifdef ADRENO830_HOST_TEST
std::mutex tierLock;
using TierLocked=std::lock_guard<std::mutex>;
#else
IOLock *tierLock;
struct TierLocked {
    IOLock *lock;
    explicit TierLocked(IOLock *l):lock(l) {IOLockLock(lock);}
    ~TierLocked() {IOLockUnlock(lock);}
};
#endif
}

a830tier::Parse NativeAdrenoMemory::configureTier(unsigned mode,const uint8_t *property,size_t length,
    uint64_t xnuBase,uint64_t xnuSize)
{
    using namespace a830tier;
    if (mode != 1 && mode != 2) return Parse::absent;
#ifndef ADRENO830_HOST_TEST
    IOLock *lock=IOLockAlloc();
    if (!lock) return Parse::absent;
    __atomic_store_n(&tierLock,lock,__ATOMIC_RELEASE);
#endif
    TierLocked locked(tierLock);
    __atomic_store_n(&gpuTier.active,0u,__ATOMIC_RELEASE);
    gpuTier.mode=mode;
    gpuTier.slabs=Tier();
    gpuTier.refusals=gpuTier.allocations=0;
    const Parse result=loadRuns(property,length,xnuBase,xnuSize,gpuTier.runs,gpuTier.count);
    gpuTier.state=result!=Parse::ok?parseName(result):gpuTier.count?"pending":"empty";
    return result;
}

void NativeAdrenoMemory::qualifyTier(unsigned bits,bool wc,bool cached_coherent,
    a830tier::Tier::ProbeFn probe,void *ctx,a830tier::Tier::LineFn line)
{
#ifndef ADRENO830_HOST_TEST
    if (!__atomic_load_n(&tierLock,__ATOMIC_ACQUIRE)) return;
#endif
    if (!gpuTier.count) return;
    if (!wc || cached_coherent || !outputWidth(bits)) {
        TierLocked locked(tierLock);
        gpuTier.state=cached_coherent?"cached-coherent":!wc?"write-combine-disabled":"output-width";
        return;
    }
    // Keep the tier closed throughout the probe, which can sleep waiting for the CP.
    a830tier::Tier checked;
    checked.init(gpuTier.runs,gpuTier.count,UINT64_C(1)<<bits,probe,ctx,line);
    TierLocked locked(tierLock);
    gpuTier.slabs=checked;
    gpuTier.state=checked.totalBytes()?"ready":"closed";
    __atomic_store_n(&gpuTier.active,gpuTier.mode,__ATOMIC_RELEASE);
}

NativeAdrenoTierStats NativeAdrenoMemory::tierStats()
{
#ifndef ADRENO830_HOST_TEST
    if (!__atomic_load_n(&tierLock,__ATOMIC_ACQUIRE)) return {"off",0,0,0,0,0,0,0,0,0};
#endif
    TierLocked locked(tierLock);
    unsigned passed=0,pass_mask=0;
    for (unsigned i=0;i<gpuTier.slabs.runs();++i) {
        if (gpuTier.slabs.runPassed(i)) {
            ++passed;
            pass_mask|=1u<<i;
        }
    }
    const uint64_t free=gpuTier.slabs.freeBytes();
    return {gpuTier.state,gpuTier.mode,gpuTier.count,gpuTier.slabs.runs(),passed,pass_mask,free,
        gpuTier.slabs.totalBytes()-free,gpuTier.refusals,gpuTier.allocations};
}

bool NativeAdrenoMemory::tierEligible(size_t bytes)
{
    return a830tier::eligible(__atomic_load_n(&gpuTier.active,__ATOMIC_ACQUIRE),bytes);
}

IOReturn NativeAdrenoMemory::allocateTier(size_t bytes,unsigned bits)
{
    using namespace a830tier;
    if (!tierEligible(bytes)) return kIOReturnNoResources;
    if (backed() || spans_ || quarantined_) return kIOReturnBusy;
    if (!bytes || (bytes & (kPage-1)) || bytes>NativeAdrenoMemoryMaxBytes || !outputWidth(bits))
        return kIOReturnBadArgument;
    a830tier::Span pieces[TierMaxSpans];
    TierLocked locked(tierLock);
    const unsigned count=gpuTier.slabs.alloc(bytes,pieces,TierMaxSpans,UINT64_C(1)<<bits);
    if (count) {
        spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,count);
        if (spans_) {
            span_count_=span_capacity_=count;
            asPieces(pieces,count,spans_);
            // The last slab belongs wholly to this BO. Expose only the requested bytes to both VMs.
            spans_[count-1].bytes=bytes-spans_[count-1].offset;
            bytes_=bytes;
            tier_=prepared_=unzeroed_=write_combined_=true;
            ++gpuTier.allocations;
            return kIOReturnSuccess;
        }
        gpuTier.slabs.release(pieces,count);
    }
    ++gpuTier.refusals;
    return kIOReturnNoResources;
}

/* A BO from the pool: kIOReturnNoResources when the pool cannot take it (none, too small, no room: counted as fell back when the pool
 * could have held it), else what the pooled allocation returned. The pool's free list and figures: under the Memory lock. */
IOReturn NativeAdrenoMemory::takePooled(size_t bytes,uint64_t limit,bool contiguous)
{
    (void)contiguous;   /* upstream builds: one extent, always */
    if(!pool.descriptor || bytes>pool.bytes)return kIOReturnNoResources;
    size_t offset=0;
    if(poolTake(bytes,&offset)) {
        const uint64_t physical=pool.physical+offset;
        if(physical>=limit || bytes>limit-physical) {poolGive(offset,bytes);}
        else {
            spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,1);
            if(!spans_) {poolGive(offset,bytes);return kIOReturnNoMemory;}
            span_capacity_=1;span_count_=1;spans_[0]={0,physical,bytes};
            mapping_=pool.virt+offset;bytes_=bytes;pooled_=true;pool_offset_=offset;prepared_=true;unzeroed_=true;
            native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
            return kIOReturnSuccess;
        }
    }
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: no single free extent holds it: several (poolTakeMany), when the BO may have more than one span. Such a BO has no
     * kernel mapping (mapping() is null: its zero fill goes by span, its views and the GPU's table take its spans). */
    else if(!contiguous && pool.physical<limit && pool.bytes<=limit-pool.physical) {
        NativeAdrenoPhysicalSpan *pieces=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,NativeAdrenoPoolMaxPieces);
        if(!pieces)return kIOReturnNoMemory;
        size_t count=0;
        if(poolTakeMany(bytes,pieces,NativeAdrenoPoolMaxPieces,&count)) {
            spans_=pieces;span_capacity_=NativeAdrenoPoolMaxPieces;span_count_=count;
            mapping_=nullptr;bytes_=bytes;pooled_=true;pool_offset_=0;prepared_=true;unzeroed_=true;
            native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
            return kIOReturnSuccess;
        }
        NativeAdrenoDeleteMeta(pieces,NativeAdrenoPhysicalSpan,NativeAdrenoPoolMaxPieces);
    }
#endif
    native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    return kIOReturnNoResources;
}
IOReturn NativeAdrenoMemory::finishPooled()
{
    if(!unzeroed_)return kIOReturnSuccess;
    if (tier_) {
        for (size_t i=0;i<span_count_;++i) {
            const auto &span=spans_[i];
            const size_t bytes=a830tier::slabBytes(span.bytes);
#ifdef ADRENO830_HOST_TEST
            if (fixture.zero_physical) fixture.zero_physical(span.physical,bytes);
#else
            bzero_phys(span.physical,bytes);
#endif
        }
#if defined(__aarch64__)
        __asm__ volatile("dsb sy":::"memory");
#endif
        unzeroed_=false;
        return kIOReturnSuccess;
    }
#ifdef ADRENO830_MACOS
    if(!mapping_) {   /* UniXNU-piano: cut from several extents (poolTakeMany, a write-combined pool only): each through the pool's mapping */
        for(size_t i=0;i<span_count_;++i)bzero(pool.virt+(spans_[i].physical-pool.physical),(size_t)spans_[i].bytes);
        __asm__ volatile("dsb sy":::"memory");
        unzeroed_=false;
        return kIOReturnSuccess;
    }
#endif
    bzero(mapping_,bytes_);
    /* Through a write-combined pool's own mapping the zeros go to DRAM; the barrier completes them. */
    if(pool.write_combined)__asm__ volatile("dsb sy":::"memory");
    else {
        const IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoClean);
        if(status)return status;
    }
    unzeroed_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::allocatePooled(size_t bytes,unsigned bits,bool contiguous)
{
    failure_={"pool-validate",kIOReturnBadArgument};
    if(descriptor_ || parts_ || spans_ || quarantined_) {failure_={"pool-busy",kIOReturnBusy};return kIOReturnBusy;}
    /* UniXNU-piano: here and below the bound is NativeAdrenoMemoryMaxBytes (NativeAdrenoMemory.h; 256 MiB upstream). */
    if(!bytes || (bytes&4095) || !outputWidth(bits) || bytes>NativeAdrenoMemoryMaxBytes)return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    const IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess) {failure_={"cpu-abi",status};return status;}
    const IOReturn result=takePooled(bytes,UINT64_C(1)<<bits,contiguous);
    failure_={result==kIOReturnNoMemory?"pool-span-metadata":"pool-capacity",result};
    return result;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled)
{return allocateBacking(bytes,bits,contiguous,budget,pooled,false,false);}
IOReturn NativeAdrenoMemory::allocateBacking(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled,bool vm,bool wc)
{
    failure_={"validate",kIOReturnBadArgument};
    if(descriptor_ || parts_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>NativeAdrenoMemoryMaxBytes)
        return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess) {failure_={"cpu-abi",status};return status;}
    uint64_t limit=UINT64_C(1)<<bits;
    if(pooled) {
        status=takePooled(bytes,limit,contiguous);
        if(status==kIOReturnSuccess) {
            status=finishPooled();
            if(status)(void)release();   /* its pages go back to the pool; the failure is the answer, as for every failed allocation */
            return status;
        }
        if(status!=kIOReturnNoResources)return status;
    }
    /* From here on nothing the pool or another BO shares: the caller needs no Memory lock (allocatePooled); the figures are atomic. */
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
#ifdef ADRENO830_MACOS
    /* UniXNU-piano (2026-11-12): a contiguous BO the kernel map would refuse after taking its pages is not asked for
     * (NativeAdrenoContiguousMaxBytes); as XNU's refusal it is no memory. */
    if(contiguous && bytes>NativeAdrenoContiguousMaxBytes) {failure_={"contiguous-limit",kIOReturnNoMemory};return kIOReturnNoMemory;}
#endif
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    failure_={vm?"vm-descriptor-create":"mapped-descriptor-create",kIOReturnNoMemory};
    /* UniXNU-piano (GPU-PAGED P0b): the wiring (descriptor + prepare) and the physical walk, timed (NativeAdrenoPoolBypass.h). */
    const unsigned p0bKind=vm?0:1;
    uint64_t p0bClock=NativeAdrenoP0bNow();
    /* UniXNU-piano (GPU-PAGED P1b): a large user BO as parts wired at the same time (P1-GATE.md section 7). */
    if(vm && !contiguous && bytes>=NativeAdrenoWireSplitBytes) {
        const unsigned workers=__atomic_load_n(&native_adreno_wire_workers,__ATOMIC_RELAXED);
        if(workers) {
            size_t parts=bytes/NativeAdrenoWirePartMin;
            if(parts>size_t(workers)+1)parts=size_t(workers)+1;
            if(parts>NativeAdrenoWireMaxParts)parts=NativeAdrenoWireMaxParts;
            if(parts>=2)return allocateParts(bytes,limit,unsigned(parts),wc);
        } else __atomic_fetch_add(&native_adreno_wire_stats[2],1u,__ATOMIC_RELAXED);
    }
    if(vm) {
        /* XNU's pageable/null-task factory creates a zero-fill named memory entry. prepare() faults and wires
         * those zeroed pages and applies the entry's cache mode; no cacheable aperture stores after publication. */
        options|=kIOMemoryPageable;
        if(wc)options|=kIOMapWriteCombineCache;
        descriptor_=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,options,bytes,4096);
    } else descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor_)return kIOReturnNoMemory;
    status=descriptor_->prepare();
    if(status!=kIOReturnSuccess) {failure_={"prepare",status};descriptor_->release();descriptor_=nullptr;return status;}
    if(p0bClock) {
        const uint64_t now=NativeAdrenoP0bNow();
        NativeAdrenoP0bTime(native_adreno_p0b_wire_us[p0bKind],now-p0bClock);
        p0bClock=now;
    }
    unmapped_=vm;write_combined_=vm && wc;
    accountBacking(bytes,vm);accounted_=true;
    prepared_=true;bytes_=bytes;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    failure_.stage="layout";
    if((vm?mapping_!=nullptr:(!mapping_ || ((uintptr_t)mapping_&4095) || bytes>UINTPTR_MAX-(uintptr_t)mapping_)) ||
        descriptor_->getLength()!=bytes)goto failed;
    failure_.stage="span-metadata";
    span_capacity_=bytes/4096;
    spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    failure_.stage="physical-spans";
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=descriptor_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
            {status=kIOReturnUnsupported;goto failed;}
        if(contiguous && offset && physical!=spans_[0].physical+offset)
            {status=kIOReturnUnsupported;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[p0bKind][1],bytes/4096);
    NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[p0bKind][2],span_count_);
    if(p0bClock)NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[p0bKind][0],NativeAdrenoP0bNow()-p0bClock);
    failure_.stage="zero-cache";
    if(vm) {
        /* VM provided the zeros, then set WCOMB if requested. Complete zero-fill publication before the GPU sees pages.
         * Cached mode still needs the physical cache path because mapping_ is deliberately null. */
        status=wc?kIOReturnSuccess:maintainImported(0,bytes,NativeAdrenoClean);
#if defined(__aarch64__)
        __asm__ volatile("dsb sy" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    } else {
        bzero(mapping_,bytes);
        status=NativeAdrenoMaintainOwnedPages(mapping_,bytes,NativeAdrenoClean);
    }
    if(status!=kIOReturnSuccess)goto failed;
    failure_={};
    __atomic_fetch_add(&native_adreno_pool_stats[5],1u,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_pool_stats[6],(uint32_t)span_count_,__ATOMIC_RELAXED);
    return kIOReturnSuccess;
failed:
    /* release() deliberately retains descriptor/metadata if complete fails.
     * Preserve the allocation's original failure instead of reporting cleanup
     * as success. No object has been exposed to a GPU or userspace yet. */
    failure_.status=status;
    (void)release();return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    failure_={"import-busy",kIOReturnBusy};
    if(descriptor_ || parts_ || imported_ || spans_ || quarantined_)return kIOReturnBusy;
    native_adreno_import_stage=11;
    failure_={"import-validate",kIOReturnBadArgument};
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>NativeAdrenoMemoryMaxBytes)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget) {failure_={"import-budget",kIOReturnNoResources};return kIOReturnNoResources;}
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);
    if(status!=kIOReturnSuccess) {failure_={"import-cpu-abi",status};return status;}
    const uint64_t limit=UINT64_C(1)<<bits;
    // Pinning a CPU view does not pin its slab. The owning BO's CPU lease controls that lifetime.
    const bool tierActive=__atomic_load_n(&gpuTier.active,__ATOMIC_ACQUIRE)!=0;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    failure_={"import-descriptor-create",kIOReturnNoMemory};
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    failure_.stage="import-prepare";
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_) {failure_={"import-readonly-descriptor",kIOReturnNoMemory};return kIOReturnNoMemory;}
        failure_.stage="import-readonly-prepare";
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {failure_.status=status;imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    failure_.stage="import-layout";
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    failure_.stage="import-span-metadata";
    span_capacity_=bytes/4096;
    spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;goto failed;}
    failure_.stage="import-physical-spans";
    for(size_t offset=0;offset<bytes;offset+=4096) {
        IOByteCount length=0;
        addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical ||
           (tierActive && physical>=a830tier::kRegionLo && physical<a830tier::kRegionHi && gpuTier.slabs.owns(physical)))
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnUnsupported;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={offset,physical,4096};
    }
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)span_count_;
    __atomic_fetch_add(&liveImports,1u,__ATOMIC_RELAXED);__atomic_fetch_add(&importBytes,bytes,__ATOMIC_RELAXED);live_import_=true;
    failure_={};return kIOReturnSuccess;
failed:
    failure_.status=status;(void)release();return status;
}
IOReturn NativeAdrenoMemory::importDevice(IOMemoryDescriptor *scanout,uint64_t offset,size_t bytes)
{
    if(descriptor_ || parts_ || imported_ || spans_ || device_ || pooled_ || quarantined_)return kIOReturnBusy;
    if(!scanout || (offset&4095) || !bytes || (bytes&4095) || offset>UINT64_MAX-bytes || bytes>NativeAdrenoMemoryMaxBytes ||
        offset+bytes>(uint64_t)scanout->getLength())return kIOReturnBadArgument;
    span_capacity_=bytes/4096;
    spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {span_capacity_=0;return kIOReturnNoMemory;}
    for(size_t at=0;at<bytes;at+=4096) {
        IOByteCount length=0;
        const addr64_t physical=scanout->getPhysicalSegment(offset+at,&length,kIOMemoryMapperNone);
        if(!physical || (physical&4095) || length<4096 || physical>UINT64_MAX-4096) {
            NativeAdrenoDeleteMeta(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
            spans_=nullptr;span_count_=span_capacity_=0;
            return kIOReturnUnsupported;
        }
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)spans_[span_count_-1].bytes+=4096;
        else spans_[span_count_++]={at,physical,4096};
    }
    /* Device memory: no wiring, no kernel mapping, no cache maintenance; the GPU writes it non-cacheable. */
    device_=true;prepared_=true;bytes_=bytes;mapping_=nullptr;read_only_=false;
    __atomic_fetch_add(&liveImports,1u,__ATOMIC_RELAXED);__atomic_fetch_add(&importBytes,bytes,__ATOMIC_RELAXED);live_import_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if (tier_) return prepared_ && !unzeroed_ && !quarantined_ && !device_references_?
        kIOReturnSuccess:kIOReturnNotReady;
    /* A VM object owns its cache attribute, including its user mappings. It was chosen before prepare(). */
    if(unmapped_)return prepared_ && !quarantined_ && write_combined_?kIOReturnSuccess:kIOReturnNotReady;
    /* A write-combined pool's BO only takes the flag (a BO cut from several of its extents has no kernel mapping). */
    if(pooled_ && pool.write_combined && prepared_ && !quarantined_ && !device_references_ && !write_combined_ && span_count_)
        {write_combined_=true;return kIOReturnSuccess;}
    if(!(descriptor_ || pooled_) || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,true);
    if(status)return status;
    write_combined_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    if(write_combined_ && pooled_ && pool.write_combined)write_combined_=false;   /* the pool's pages keep the attribute */
    if(write_combined_ && !unmapped_ && !tier_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(imported_) {
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    /* The presenter's descriptor of a device import was never retained or wired: only the spans go. */
    if(device_) {device_=false;prepared_=false;}
    if(live_import_) {live_import_=false;__atomic_fetch_sub(&liveImports,1u,__ATOMIC_RELAXED);__atomic_fetch_sub(&importBytes,bytes_,__ATOMIC_RELAXED);}
    if(parts_) {   /* UniXNU-piano (GPU-PAGED P1b): every part completed, the view and the parts released */
        if(prepared_) {
            native_adreno_trace_stage=31;
            for(uint32_t i=0;i<part_count_;++i)if(parts_[i]) {
                const IOReturn status=parts_[i]->complete();
                if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            }
            prepared_=false;
        }
        releaseParts();
    }
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    if(pooled_) {
        /* Every extent it holds goes back (one, or several when poolTakeMany cut it). */
        for(size_t i=0;i<span_count_;++i)poolGive(size_t(spans_[i].physical-pool.physical),size_t(spans_[i].bytes));
        pooled_=false;prepared_=false;pool_offset_=0;unzeroed_=false;
    }
    if (tier_) {
        a830tier::Span pieces[TierMaxSpans];
        for (size_t i=0;i<span_count_;++i)
            pieces[i]={spans_[i].physical,a830tier::slabBytes(spans_[i].bytes)};
        TierLocked locked(tierLock);
        if (!gpuTier.slabs.release(pieces,unsigned(span_count_))) {
            quarantined_=true;
            return kIOReturnIOError;
        }
        tier_=prepared_=unzeroed_=false;
    }
    if(accounted_) {
        __atomic_fetch_sub(unmapped_?&ownedUnmapped:&ownedMapped,bytes_,__ATOMIC_RELAXED);
        __atomic_fetch_add(&backingFrees,1ull,__ATOMIC_RELAXED);accounted_=false;
    }
    unmapped_=false;write_combined_=false;
    if(spans_)NativeAdrenoDeleteMeta(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    mapping_=nullptr;spans_=nullptr;bytes_=span_count_=span_capacity_=0;read_only_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_ || unzeroed_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(device_ || tier_)return kIOReturnSuccess;   /* only uncached CPU views */
    if(imported_ || (unmapped_ && !write_combined_))return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    if(write_combined_)   /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        return include_write_combined?maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean):kIOReturnSuccess;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(device_)return kIOReturnSuccess;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_)return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);
    if(unmapped_ && !write_combined_)return maintainImported(offset,bytes,NativeAdrenoInvalidate);
    if(write_combined_)return kIOReturnSuccess;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    for(size_t i=0;i<span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=spans_[i];
        const size_t begin=offset>span.offset?offset:span.offset;
        const size_t end=offset+bytes<span.offset+span.bytes?offset+bytes:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;
    }
    return kIOReturnSuccess;
}

/* ---- UniXNU-piano (2026-11-23, GPU-PAGED P1b): a split user BO ----------------------------------------------------------------- */
#ifdef ADRENO830_HOST_TEST
static IOMultiMemoryDescriptor *makePartsView(IOBufferMemoryDescriptor **parts,unsigned count)
{return IOMultiMemoryDescriptor::withBuffers(parts,count);}
#else
static IOMultiMemoryDescriptor *makePartsView(IOBufferMemoryDescriptor **parts,unsigned count)
{
    IOMemoryDescriptor *list[NativeAdrenoWireMaxParts];
    for(unsigned i=0;i<count;++i)list[i]=parts[i];
    return IOMultiMemoryDescriptor::withDescriptors(list,count,kIODirectionInOut,false);
}
#endif
void NativeAdrenoMemory::releaseParts()
{
    if(multi_) {multi_->release();multi_=nullptr;}
    for(uint32_t i=0;i<part_count_;++i)if(parts_[i])parts_[i]->release();
    IODelete(parts_,IOBufferMemoryDescriptor *,NativeAdrenoWireMaxParts);
    parts_=nullptr;part_count_=0;
}
/* allocateBacking's named-entry path for a large BO: `count` parts on 64 KiB boundaries (the last takes the rest), created here, wired
 * at once (wireParts), walked into spans in order; the CPU view their IOMultiMemoryDescriptor. Any failure gives everything back. */
IOReturn NativeAdrenoMemory::allocateParts(size_t bytes,uint64_t limit,unsigned count,bool wc)
{
    const uint64_t p0bClock=NativeAdrenoP0bNow();
    failure_={"vm-parts",kIOReturnNoMemory};
    size_t each=(bytes/count)&~(NativeAdrenoWirePartAlign-1);
    if(!each)return kIOReturnBadArgument;
    parts_=IONew(IOBufferMemoryDescriptor *,NativeAdrenoWireMaxParts);
    if(!parts_)return kIOReturnNoMemory;
    for(unsigned i=0;i<NativeAdrenoWireMaxParts;++i)parts_[i]=nullptr;
    part_count_=count;
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPageable;
    if(wc)options|=kIOMapWriteCombineCache;
    IOReturn results[NativeAdrenoWireMaxParts];
    for(unsigned i=0;i<count;++i) {
        const size_t size=i+1<count?each:bytes-each*(count-1);
        parts_[i]=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,options,size,4096);
        if(!parts_[i]) {releaseParts();return kIOReturnNoMemory;}
        results[i]=kIOReturnSuccess;
    }
    wireParts(parts_,results,count,__atomic_load_n(&native_adreno_wire_workers,__ATOMIC_RELAXED));
    IOReturn status=kIOReturnSuccess;
    for(unsigned i=0;i<count;++i)if(results[i]!=kIOReturnSuccess && !status)status=results[i];
    if(status) {
        /* the parts that were wired are completed before they go */
        failure_={"prepare",status};
        for(unsigned i=0;i<count;++i)if(results[i]==kIOReturnSuccess && parts_[i]->complete()!=kIOReturnSuccess) {quarantined_=true;}
        __atomic_fetch_add(&native_adreno_wire_stats[3],1u,__ATOMIC_RELAXED);
        if(quarantined_)return status;   /* the parts stay retained (release() refuses a quarantined object) */
        releaseParts();return status;
    }
    if(p0bClock)NativeAdrenoP0bTime(native_adreno_p0b_wire_us[0],NativeAdrenoP0bNow()-p0bClock);
    const uint64_t walkClock=NativeAdrenoP0bNow();
    unmapped_=true;write_combined_=wc;
    accountBacking(bytes,true);accounted_=true;
    prepared_=true;bytes_=bytes;mapping_=nullptr;
    failure_.stage="span-metadata";
    span_capacity_=bytes/4096;
    spans_=NativeAdrenoNewMeta(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_) {status=kIOReturnNoMemory;failure_.status=status;(void)release();return status;}
    failure_.stage="physical-spans";
    size_t base=0;
    for(unsigned i=0;i<count;++i) {
        const size_t size=parts_[i]->getLength();
        for(size_t offset=0;offset<size;offset+=4096) {
            IOByteCount length=0;
            const addr64_t physical=parts_[i]->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!physical || (physical&4095) || length<4096 || physical>=limit || 4096>limit-physical)
                {status=kIOReturnUnsupported;failure_.status=status;(void)release();return status;}
            if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)spans_[span_count_-1].bytes+=4096;
            else spans_[span_count_++]={base+offset,physical,4096};
        }
        base+=size;
    }
    if(base!=bytes) {status=kIOReturnUnsupported;failure_.status=status;(void)release();return status;}
    NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[0][1],bytes/4096);
    NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[0][2],span_count_);
    if(walkClock)NativeAdrenoP0bAdd(&native_adreno_p0b_lookup[0][0],NativeAdrenoP0bNow()-walkClock);
    failure_.stage="cpu-view";
    multi_=makePartsView(parts_,count);
    if(!multi_) {status=kIOReturnNoMemory;failure_.status=status;(void)release();return status;}
    failure_.stage="zero-cache";
    status=wc?kIOReturnSuccess:maintainImported(0,bytes,NativeAdrenoClean);
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    if(status) {failure_.status=status;(void)release();return status;}
    failure_={};
    __atomic_fetch_add(&native_adreno_wire_stats[0],1u,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_wire_stats[1],count,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_pool_stats[5],1u,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_pool_stats[6],(uint32_t)span_count_,__ATOMIC_RELAXED);
    return kIOReturnSuccess;
}
