/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoVm.h"
#include "NativeAdrenoVmAlias.h"
#include "NativeAdrenoMeter.h"
#include "NativeAdrenoPoolBypass.h"   /* UniXNU-piano: GPU-PAGED P0b */
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {int (*native_adreno_dram_read32)(uint64_t,uint32_t *);int (*native_adreno_cached_read32)(uint64_t,uint32_t *);
    int (*native_adreno_line_audit)(uint64_t,size_t,uint32_t *,uint32_t *);}
#include <string.h>
#include <IOKit/IOLib.h>
#ifdef ADRENO830_HOST_TEST
#include <mutex>
#else
#include <IOKit/IOLocks.h>
#endif
/* The effective coherence level of ARCHITECTURE 4.14.1 (NativeAdrenoVm::setCoherence): 0 none, 1 GPU-read-only imports
 * write-back inner-shareable, 2 also GPU-written imports and own BOs mapped write-back. Counters: maintenance passes skipped for
 * coherent mappings at map, unmap, submission and completion. */
extern "C" {volatile uint32_t native_adreno_io_coherent;volatile uint32_t native_adreno_coherent_skips[4];}
static bool coherentMapping(unsigned writeback) {return writeback && native_adreno_io_coherent;}
/* UniXNU-piano: a map refusal records why (NativeAdrenoVmWhy, NativeAdrenoVm::lastRefusal) in macOS builds; elsewhere the status
 * alone, exactly as before. */
#ifdef ADRENO830_MACOS
#define VM_REFUSE(why,status,...) refuse(NativeAdrenoVmWhy##why,status,__VA_ARGS__)
#else
#define VM_REFUSE(why,status,...) (status)
#endif
static bool overlap(uint64_t a,uint64_t ab,uint64_t b,uint64_t bb)
{return a<b+bb && b<a+ab;}
/* Mappings of one cache/shareability/privilege class may share pages; only pages shared across classes refuse. */
static unsigned aliasClass(unsigned permissions,unsigned writeback,unsigned shareability)
{return writeback|(shareability<<1)|((permissions&ArmSmmu4KPrivileged)?8u:0u);}
static void reverse(ArmSmmu4KRange *first,ArmSmmu4KRange *last)
{while(first<last && first<--last) {const ArmSmmu4KRange t=*first;*first++=*last;*last=t;}}
static bool validBinding(const NativeAdrenoVmRegion &r,unsigned upper)
{
    return r.tables && r.arena && r.state && !r.tables->quarantined() &&
        r.tables->deviceReferences() && r.tables->spanCount()==1 &&
        r.tables->spans()[0].bytes==r.tables->bytes() && !r.tables->spans()[0].offset &&
        r.arena->mapping==r.tables->mapping() && r.arena->upper_region==upper &&
        r.arena->config.table_physical==r.tables->spans()[0].physical &&
        r.arena->capacity_pages==r.tables->bytes()/4096 && r.state->prepared &&
        !r.state->poisoned && !r.state->tables.leaf_pages;
}
/* The VM lock (ARCHITECTURE 5.4, M10): page-table updates and their TLB synchronization. A wait for it goes to the call meter on the
 * VM's own clock: a submission holding the Ring lock waits here for another process's table transaction. */
#ifdef ADRENO830_HOST_TEST
static void *lockAlloc() {return new std::mutex;}
static void lockFree(void *lock) {delete static_cast<std::mutex *>(lock);}
static void lockTake(void *lock)
{
    std::mutex *mutex=static_cast<std::mutex *>(lock);
    NativeAdrenoMeterLock(Adreno830WaitVm,native_adreno_vm_now_us,[&] {return mutex->try_lock();},[&] {mutex->lock();});
}
static void lockDrop(void *lock) {static_cast<std::mutex *>(lock)->unlock();}
#else
static void *lockAlloc() {return IOLockAlloc();}
static void lockFree(void *lock) {IOLockFree(static_cast<IOLock *>(lock));}
static void lockTake(void *lock)
{
    IOLock *held=static_cast<IOLock *>(lock);
    NativeAdrenoMeterLock(Adreno830WaitVm,native_adreno_vm_now_us,[&] {return IOLockTryLock(held)!=0;},[&] {IOLockLock(held);});
}
static void lockDrop(void *lock) {IOLockUnlock(static_cast<IOLock *>(lock));}
#endif
void NativeAdrenoVm::lock() const {if(lock_)lockTake(lock_);}
void NativeAdrenoVm::unlock() const {if(lock_)lockDrop(lock_);}
namespace {
struct Locked {
    const NativeAdrenoVm *vm;void (NativeAdrenoVm::*drop)() const;
    ~Locked() {(vm->*drop)();}
};
}
#define VM_LOCKED Locked locked_scope{this,&NativeAdrenoVm::unlock};lock();(void)locked_scope

ArmSmmu4KLiveRegion NativeAdrenoVm::region(unsigned upper) const
{
    return {regions_[upper].arena,regions_[upper].state,
        ranges_+(upper?lower_count_:0),upper?upper_count_:lower_count_};
}
/* The clock (the publish shell sets it, pagani-gpu-kopt bit 5; null: no time in any figure below) and per-phase time of the two table
 * transactions: [0] map, [1] unmap; each row is count, the phases in order, the slowest call and the bytes mapped (int-134: BoMap
 * averaged 135 ms and the desktop crawled). */
extern "C" {uint64_t (*native_adreno_vm_now_us)(void)=nullptr;volatile uint64_t native_adreno_vm_phase_us[2][8];}
/* {count, us, spans} per kind: 0 map of driver memory, 1 import map without the alias pass, 2 import map with it, 3 unmap dropping its
 * pages from the TLB one by one, 4 unmap invalidating the ASID. */
extern "C" {volatile uint64_t native_adreno_vm_kind_us[NativeAdrenoVmKinds][3];}
extern "C" {volatile uint32_t native_adreno_vm_reserve_refused[2];}
namespace {
struct PhaseClock {
    uint64_t last=native_adreno_vm_now_us?native_adreno_vm_now_us():0,began=last;
    void lap(unsigned row,unsigned slot) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t now=native_adreno_vm_now_us();native_adreno_vm_phase_us[row][slot]+=now-last;last=now;
    }
    uint64_t elapsed() const {return native_adreno_vm_now_us?native_adreno_vm_now_us()-began:0;}
    void done(unsigned row,uint64_t bytes) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t total=native_adreno_vm_now_us()-began;
        native_adreno_vm_phase_us[row][0]+=1;native_adreno_vm_phase_us[row][7]+=bytes;
        if(total>native_adreno_vm_phase_us[row][6])native_adreno_vm_phase_us[row][6]=total;
    }
};
}
/* A transaction that cannot be proved: the VM refuses everything from now on, every BO and table stays referenced, and the
 * owned streams FAULT (their SMRs stay valid, so the SIDs stay blocked). Only this path poisons; no fault status does. */
IOReturn NativeAdrenoVm::fail(ArmSmmu4KStatus status)
{
    poisoned_=true;result_.operation=status;result_.complete=false;
    for(size_t i=0;mappings_ && i<high_;++i)if(mappings_[i].memory)mappings_[i].memory->quarantine();
    for(auto &r:regions_)if(r.arena) {r.tables->quarantine();r.state->poisoned=1;}
    if(paged_) {for(size_t i=0;i<pg_->chunk_count;++i)pg_->chunks[i].quarantine();pg_->space.poisoned=1;}   /* GPU-PAGED P1: every table page kept */
    if(has_programmer_)
        result_.quarantine=programmer_.fault(programmer_.cookie,bank_,streams_,stream_count_)==kIOReturnSuccess?
            ArmSmmu4KSuccess:ArmSmmu4KIoError;
    else if(kit_streams_) {
        ArmSmmu4KStreamResult routes;
        result_.quarantine=ArmSmmu4KFaultStreams(&io_,kit_streams_,&routes);
    }
    return kIOReturnIOError;
}
IOReturn NativeAdrenoVm::attachCommon(const ArmSmmu4KIo *io,ArmSmmu4KContextConfig *context,const NativeAdrenoVmRegion *lower,
    const NativeAdrenoVmRegion *upper,size_t capacity,unsigned budget)
{
    if(attached_ || ranges_ || poisoned_)return kIOReturnBusy;
    if(!io || !io->read32 || !io->write32 || !io->write64 || !io->barrier || !context || (!lower && !upper) || !capacity ||
#ifdef ADRENO830_MACOS
        capacity>NativeAdrenoVmMaxSpans ||   /* UniXNU-piano: 1048576 (gpu/table1) */
#else
        capacity>262144 ||
#endif
        !budget || budget>1048576 || (lower && !validBinding(*lower,0)) || (upper && !validBinding(*upper,1)))
        return kIOReturnBadArgument;
    if(lower && (context->lower!=&lower->arena->config || context->lower_tables!=&lower->state->tables))return kIOReturnBadArgument;
    if(upper && (context->upper!=&upper->arena->config || context->upper_tables!=&upper->state->tables))return kIOReturnBadArgument;
    if(!lock_ && !(lock_=lockAlloc()))return kIOReturnNoMemory;
    ranges_=NativeAdrenoNewMeta(ArmSmmu4KRange,capacity);
    mappings_=NativeAdrenoNewMeta(Mapping,NativeAdrenoVmMaxObjects);
    if(!ranges_ || !mappings_ || !index_.allocate()) {
        if(ranges_)NativeAdrenoDeleteMeta(ranges_,ArmSmmu4KRange,capacity);
        if(mappings_)NativeAdrenoDeleteMeta(mappings_,Mapping,NativeAdrenoVmMaxObjects);
        index_.release();lockFree(lock_);lock_=nullptr;ranges_=nullptr;mappings_=nullptr;
        return kIOReturnNoMemory;
    }
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i)mappings_[i]=Mapping{};
    free_hint_=0;high_=0;memset(class_spans_,0,sizeof(class_spans_));memset(import_spans_,0,sizeof(import_spans_));
    maintained_head_=UINT32_MAX;maintained_count_=0;window_count_=0;
    capacity_=capacity;io_=*io;context_=context;budget_=budget;
    if(lower)regions_[0]=*lower;
    if(upper)regions_[1]=*upper;
    for(unsigned i=0;i<2;++i)if(regions_[i].arena) {
        if(NativeAdrenoBindSmmuVisibility(regions_[i].tables,&visibility_[i])!=kIOReturnSuccess) {
            NativeAdrenoDeleteMeta(ranges_,ArmSmmu4KRange,capacity_);NativeAdrenoDeleteMeta(mappings_,Mapping,NativeAdrenoVmMaxObjects);index_.release();
            regions_[0]={};regions_[1]={};ranges_=nullptr;mappings_=nullptr;capacity_=0;lockFree(lock_);lock_=nullptr;
            return kIOReturnBadArgument;
        }
    }
    attached_=true;coherent_walk_=false;
    return kIOReturnSuccess;
}
/* The whole-region proof the kit's first transaction of a region would make, done once here: every descriptor of both regions
 * (ArmSmmu4KLiveAuditContext, no SMMU access) and the routes and context through the programmer, under the board's own rules
 * (A830Smmu: TTBR1 guard table, CB2 TTBR1 0, FSR record only). The regions are then recorded as proved at the bank's register
 * offset, so the kit's transactions read no SMMU register: a map only writes and publishes leaves, an unmap invalidates its
 * pages (ARCHITECTURE 4.6.4). */
IOReturn NativeAdrenoVm::verifyAndSeed()
{
    ArmSmmu4KLiveRegion lower=region(0),upper=region(1);
    ArmSmmu4KLiveResult audit;
    ArmSmmu4KStatus status=ArmSmmu4KLiveAuditContext(context_,lower.arena?&lower:nullptr,upper.arena?&upper:nullptr,&audit);
    if(status)return fail(status);
    if(has_programmer_) {
        if(programmer_.verify(programmer_.cookie,bank_,streams_,stream_count_)!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    } else {
        ArmSmmu4KStreamResult routes;
        status=ArmSmmu4KObserveStreams(&io_,kit_streams_,context_,&routes);
        if(status)return fail(status);
        context_offset_=routes.context.context_offset;
    }
    if(!context_offset_ || context_offset_>io_.register_bytes || io_.register_bytes-context_offset_<0x7f8)
        return fail(ArmSmmu4KBadRange);
    for(auto &r:regions_)if(r.state) {
        if(r.state->generation==UINT64_MAX)return fail(ArmSmmu4KStateMismatch);
        if(!r.state->generation)r.state->generation=1;
        r.state->context_offset=context_offset_;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::attach(const NativeAdrenoSmmuBinding &smmu,const NativeAdrenoVmRegion *lower,
    const NativeAdrenoVmRegion *upper,size_t capacity,unsigned budget)
{
    const NativeAdrenoSmmuProgrammer *p=smmu.programmer;
    if(attached_ || ranges_ || poisoned_)return kIOReturnBusy;
    if(!p || !p->io || !p->verify || !p->fault || !smmu.streams || !smmu.stream_count ||
        smmu.stream_count>NativeAdrenoSmmuContext::MaxStreams || !smmu.context_offset || smmu.bank>255)return kIOReturnBadArgument;
    IOReturn status=attachCommon(p->io,smmu.context,lower,upper,capacity,budget);
    if(status!=kIOReturnSuccess)return status;
    VM_LOCKED;
    programmer_=*p;has_programmer_=true;kit_streams_=nullptr;sync_after_map_=p->sync_after_map;
    for(size_t i=0;i<smmu.stream_count;++i)streams_[i]=smmu.streams[i];
    stream_count_=smmu.stream_count;bank_=smmu.bank;context_offset_=smmu.context_offset;
    return verifyAndSeed();
}
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *io,ArmSmmu4KContextConfig *context,
    ArmSmmu4KStreamState *streams,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
    size_t capacity,unsigned budget)
{
    if(attached_ || ranges_ || poisoned_)return kIOReturnBusy;
    if(!streams || !streams->attached || streams->poisoned || !streams->count || streams->count>8)return kIOReturnBadArgument;
    IOReturn status=attachCommon(io,context,lower,upper,capacity,budget);
    if(status!=kIOReturnSuccess)return status;
    VM_LOCKED;
    has_programmer_=false;sync_after_map_=false;kit_streams_=streams;bank_=streams->bank;stream_count_=streams->count;
    for(size_t i=0;i<stream_count_;++i)streams_[i]=streams->streams[i];
    return verifyAndSeed();
}
IOReturn NativeAdrenoVm::detach()
{
    if(!attached_)return kIOReturnNotReady;
    {
        VM_LOCKED;
        if(poisoned_ || objects_ || spanCount())return kIOReturnBusy;
        /* GPU-PAGED P1: the records go; the table pages stay owned until releasePaged (after the bank is disabled) */
        NativeAdrenoDeleteMeta(ranges_,ArmSmmu4KRange,capacity_);NativeAdrenoDeleteMeta(mappings_,Mapping,NativeAdrenoVmMaxObjects);index_.release();
        attached_=false;regions_[0]={};regions_[1]={};context_=nullptr;kit_streams_=nullptr;has_programmer_=false;
        sync_after_map_=false;
        capacity_=0;ranges_=nullptr;mappings_=nullptr;stream_count_=0;window_count_=0;fence_=nullptr;
    }
    lockFree(lock_);lock_=nullptr;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::reserveWindow(uint64_t iova,uint64_t bytes)
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(!bytes || ((iova|bytes)&4095) || iova>UINT64_MAX-bytes+1)return kIOReturnBadArgument;
    if(window_count_>=NativeAdrenoVmMaxWindows)return kIOReturnNoResources;
    if(paged_) {   /* GPU-PAGED P1: one lower space [0, aperture) */
        if(iova>=pg_->config.aperture || bytes>pg_->config.aperture-iova)return kIOReturnBadArgument;
        const size_t i=rangeAt(iova);
        if(i<spanCount() && ranges_[i].iova<=iova+(bytes-1))return kIOReturnBusy;
        windows_[window_count_++]={iova,bytes,0};
        return kIOReturnSuccess;
    }
    for(unsigned u=0;u<2;++u) {
        const ArmSmmu4KLiveArena *a=regions_[u].arena;
        if(!a || iova<a->iova || iova-a->iova>=a->bytes)continue;
        /* Only a window nothing is mapped in yet. */
        const size_t i=rangeAt(iova);
        if(i<spanCount() && ranges_[i].iova<=iova+(bytes-1))return kIOReturnBusy;
        windows_[window_count_++]={iova,bytes,u};
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
bool NativeAdrenoVm::forbidden(uint64_t iova,uint64_t bytes) const
{
    for(size_t i=0;i<window_count_;++i)
        if(iova<=windows_[i].iova+(windows_[i].bytes-1) && windows_[i].iova<=iova+(bytes-1))return true;
    return false;
}
IOReturn NativeAdrenoVm::findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *out) const
{
    if(!out)return kIOReturnBadArgument;*out=0;
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(upper>1 || (!paged_ && !regions_[upper].arena) || (paged_ && upper) || !bytes || (bytes&4095) || alignment<4096 ||
        (alignment&(alignment-1)))return kIOReturnBadArgument;
    /* GPU-PAGED P1: the paged space is one lower region [0, aperture) */
    struct Aperture {uint64_t iova,bytes;};
    const Aperture aperture=paged_?Aperture{0,pg_->config.aperture}:Aperture{regions_[upper].arena->iova,regions_[upper].arena->bytes};
    const Aperture *a=&aperture;
    const size_t end=upper?spanCount():lower_count_;
    size_t i=upper?lower_count_:0;
    uint64_t offset=0;   /* relative to the aperture: native addresses of the upper region may end at 2^64 */
    for(;;) {
        const uint64_t rem=(a->iova+offset)&(alignment-1),padding=rem?alignment-rem:0;
        if(padding>a->bytes-offset)return kIOReturnNoResources;
        offset+=padding;
        if(bytes>a->bytes-offset)return kIOReturnNoResources;
        bool moved=false;
        for(size_t w=0;w<window_count_;++w) {
            if(windows_[w].upper!=upper)continue;
            const uint64_t ws=windows_[w].iova-a->iova,we=ws+windows_[w].bytes;
            if(offset<we && ws<offset+bytes) {offset=we;moved=true;}
        }
        if(moved)continue;
        while(i<end && ranges_[i].iova-a->iova+ranges_[i].bytes<=offset)++i;
        if(i==end || ranges_[i].iova-a->iova>=offset+bytes) {*out=a->iova+offset;return kIOReturnSuccess;}
        offset=ranges_[i].iova-a->iova+ranges_[i].bytes;
    }
}
IOReturn NativeAdrenoVm::preflight(const NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,bool exclusive) const
{
    if(!memory || upper>1 || !regions_[upper].arena || !memory->backed() || memory->quarantined() ||
        !memory->bytes() || !memory->spanCount() || !(permissions&ArmSmmu4KRead) || (permissions&~15u) ||
        writeback>1 || (shareability!=2 && shareability!=3))return VM_REFUSE(Request,kIOReturnBadArgument,permissions,writeback,shareability,memory);
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    if((iova&4095) || iova<a->iova || iova-a->iova>=a->bytes || memory->bytes()>a->bytes-(iova-a->iova))
        return VM_REFUSE(Aperture,kIOReturnBadArgument,iova,memory->bytes(),a->iova+a->bytes,memory);
    /* The null guard and the GMEM window are never mapped (ARCHITECTURE 4.6.2). */
    if(forbidden(iova,memory->bytes()))return VM_REFUSE(Reserved,kIOReturnBadArgument,iova,memory->bytes(),0,memory);
    /* Memory the GPU may only read is never mapped writable; the scanout is never mapped write-back. */
    if((memory->readOnly() && (permissions&ArmSmmu4KWrite)) || (memory->scanout() && writeback))
        return VM_REFUSE(ReadOnly,kIOReturnBadArgument,permissions,writeback,0,memory);
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: the two table limits told apart. */
    if(objects_>=NativeAdrenoVmMaxObjects)return refuse(NativeAdrenoVmWhyObjects,kIOReturnNoResources,objects_,NativeAdrenoVmMaxObjects,0,memory);
    if(memory->spanCount()>capacity_-spanCount())
        return refuse(NativeAdrenoVmWhySpans,kIOReturnNoResources,memory->spanCount(),capacity_-spanCount(),capacity_,memory);
#else
    if(objects_>=NativeAdrenoVmMaxObjects || memory->spanCount()>capacity_-spanCount())return kIOReturnNoResources;
#endif
    {
        /* ranges_ is sorted by address: only the first range not wholly below iova can overlap (native addresses may end at 2^64,
         * so ends are compared as last bytes). A walk over every range cost each map a pass over 52609 of them (i567). */
        const size_t i=rangeAt(iova);
        if(i<spanCount() && ranges_[i].iova<=iova+(memory->bytes()-1))return VM_REFUSE(Overlap,kIOReturnBusy,iova,ranges_[i].iova,0,memory);
    }
    if(a->config.output_bits<32 || a->config.output_bits>48)return VM_REFUSE(Pages,kIOReturnBadArgument,UINT64_MAX,0,0,memory);
    uint64_t offset=0,limit=UINT64_C(1)<<a->config.output_bits;
    for(size_t j=0;j<memory->spanCount();++j) {
        const auto &span=memory->spans()[j];
        if(span.offset!=offset || !span.physical || !span.bytes || ((span.physical|span.bytes)&4095) ||
            span.bytes>memory->bytes()-offset || span.physical>=limit || span.bytes>limit-span.physical)
            return VM_REFUSE(Pages,kIOReturnBadArgument,j,span.physical,span.bytes,memory);
        for(const auto &r:regions_)if(r.arena && overlap(span.physical,span.bytes,
            r.arena->config.table_physical,r.arena->capacity_pages*UINT64_C(4096)))return VM_REFUSE(Pages,kIOReturnBadArgument,j,span.physical,span.bytes,memory);
        offset+=span.bytes;
    }
    if(offset!=memory->bytes())return VM_REFUSE(Pages,kIOReturnBadArgument,memory->spanCount(),offset,memory->bytes(),memory);
    /* Only another cache/shareability/privilege class can conflict. Reuse the
     * arena's caller-owned temporary alias scratch before the table transaction
     * uses it. Index the smaller side and query the other in O(log n); neither
     * the resource spans nor the actual page tables are modified here. */
    /* exclusive (exclusivePages): no mapped range can alias these pages in conflict, so the pass over every other class's ranges is
     * not needed. i568: this pass and the table transaction's walked every one of about 50000 ranges on each map, 0.6 ms of its 0.66. */
    if(exclusive)return kIOReturnSuccess;
    const unsigned mine=aliasClass(permissions,writeback,shareability);
    const size_t other=spanCount()-class_spans_[mine&15];
    if(!other)return kIOReturnSuccess;
    const bool indexNew=memory->spanCount()<=other;
    const size_t smaller=indexNew?memory->spanCount():other;
    uint32_t *order=a->alias_scratch;
    if(order && smaller<=a->alias_scratch_entries) {
        if(indexNew) {
            for(size_t i=0;i<smaller;++i)order[i]=(uint32_t)i;
            NativeAdrenoPhysicalIndex index(memory->spans(),order,smaller);
            for(size_t i=0;i<spanCount();++i) {
                const auto &r=ranges_[i];
                if(aliasClass(r.permissions,r.writeback,r.shareability)!=mine && index.overlaps(r.physical,r.bytes))
                    return VM_REFUSE(Alias,kIOReturnBadArgument,r.physical,r.iova,mine,memory);
            }
        } else {
            size_t n=0;
            for(size_t i=0;i<spanCount();++i)
                if(aliasClass(ranges_[i].permissions,ranges_[i].writeback,ranges_[i].shareability)!=mine)order[n++]=(uint32_t)i;
            NativeAdrenoPhysicalIndex index(ranges_,order,n);
            for(size_t j=0;j<memory->spanCount();++j)
                if(index.overlaps(memory->spans()[j].physical,memory->spans()[j].bytes))
                    return VM_REFUSE(Alias,kIOReturnBadArgument,memory->spans()[j].physical,0,mine,memory);
        }
        return kIOReturnSuccess;
    }
    /* Small/unconfigured callers retain the conservative allocation-free path. */
    for(size_t i=0;i<spanCount();++i) {
        const ArmSmmu4KRange &r=ranges_[i];
        if(aliasClass(r.permissions,r.writeback,r.shareability)==mine)continue;
        for(size_t j=0;j<memory->spanCount();++j)
            if(overlap(memory->spans()[j].physical,memory->spans()[j].bytes,r.physical,r.bytes))
                return VM_REFUSE(Alias,kIOReturnBadArgument,r.physical,r.iova,mine,memory);
    }
    return kIOReturnSuccess;
}
/* Whether no mapped range can alias memory's pages with another cache/shareability/privilege class. A buffer this driver allocated
 * owns its pages until its unmap has completed (the free waits for it). An import (a user range, usually an IOSurface, which any
 * process may import again) can alias only another import or a range of driver memory it covers: it is exclusive when every mapped
 * import has its class and none of its pages lies in the BO pool or in a mapped scattered buffer. i568: about 100 surface imports of
 * 5000-6000 spans each, every one checked against every other range. */
bool NativeAdrenoVm::exclusivePages(const NativeAdrenoMemory *memory,unsigned cls) const
{
    if(!memory->imported() && !memory->scanout())return true;
    size_t imports=0;for(size_t k=0;k<16;++k)imports+=import_spans_[k];
    if(imports!=import_spans_[cls&15])return false;
    const uint64_t pool=NativeAdrenoMemory::poolPhysical();const size_t poolBytes=NativeAdrenoMemory::poolBytes();
    const auto *spans=memory->spans();const size_t n=memory->spanCount();
    if(pool)for(size_t j=0;j<n;++j)if(overlap(spans[j].physical,spans[j].bytes,pool,poolBytes))return false;
    size_t pairs=0;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || m.memory->imported() || m.memory->scanout() || m.memory->pooled())continue;
        const size_t mn=m.memory->spanCount();
        if(mn>(size_t)65536/(n?n:1) || (pairs+=mn*n)>(size_t)1<<20)return false;   /* too many to compare: the full check */
        for(size_t a=0;a<mn;++a)for(size_t b=0;b<n;++b)
            if(overlap(m.memory->spans()[a].physical,m.memory->spans()[a].bytes,spans[b].physical,spans[b].bytes))return false;
    }
    return true;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle)
{return const_cast<Mapping *>(static_cast<const NativeAdrenoVm *>(this)->find(handle));}
const NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle) const
{
    const uint32_t slot=mappings_?index_.find(handle):UINT32_MAX;
    if(slot>=NativeAdrenoVmMaxObjects)return nullptr;
    const Mapping *m=&mappings_[slot];
    return m->handle==handle && m->memory?m:nullptr;
}
/* The first range whose last byte is at or past iova (spanCount() when none). */
size_t NativeAdrenoVm::rangeAt(uint64_t iova) const
{
    size_t low=0,high=spanCount();
    while(low<high) {
        const size_t mid=low+(high-low)/2;
        if(ranges_[mid].iova+(ranges_[mid].bytes-1)<iova)low=mid+1;else high=mid;
    }
    return low;
}
#ifdef ADRENO830_MACOS
uint64_t NativeAdrenoVm::largestClientHole(uint64_t start,uint64_t bytes) const
{
    const uint64_t end=start+bytes;
    if(!bytes || end<=start)return 0;
    VM_LOCKED;
    if(!attached_ || poisoned_)return 0;
    uint64_t cursor=start,largest=0;
    const size_t spans=spanCount();
    for(size_t i=rangeAt(start);i<spans && ranges_[i].iova<end;++i) {
        const auto &range=ranges_[i];
        if(range.iova>cursor && range.iova-cursor>largest)largest=range.iova-cursor;
        cursor=range.iova+range.bytes;
    }
    return cursor<end && end-cursor>largest?end-cursor:largest;
}
#endif
/* The n ranges just past the table's end (one mapping's spans, consecutive in IOVA and all in one gap) take their sorted place. */
void NativeAdrenoVm::insert(size_t n,unsigned upper)
{
    const size_t count=spanCount();
    for(size_t i=0;i<n;++i)++class_spans_[aliasClass(ranges_[count+i].permissions,ranges_[count+i].writeback,ranges_[count+i].shareability)&15];
    const size_t at=rangeAt(ranges_[count].iova);
    /* With room for a copy of the new ranges past them, one memmove opens the gap (the three reversals moved every later range twice). */
    if(capacity_-count-n>=n) {
        memcpy(ranges_+count+n,ranges_+count,n*sizeof(*ranges_));
        memmove(ranges_+at+n,ranges_+at,(count-at)*sizeof(*ranges_));
        memcpy(ranges_+at,ranges_+count+n,n*sizeof(*ranges_));
    } else {
        reverse(ranges_+at,ranges_+count);reverse(ranges_+count,ranges_+count+n);reverse(ranges_+at,ranges_+count+n);
    }
    if(upper)upper_count_+=n;else lower_count_+=n;
}
void NativeAdrenoVm::erase(size_t at,size_t n,unsigned upper)
{
    const size_t count=spanCount();
    for(size_t i=0;i<n;++i)--class_spans_[aliasClass(ranges_[at+i].permissions,ranges_[at+i].writeback,ranges_[at+i].shareability)&15];
    memmove(ranges_+at,ranges_+at+n,(count-at-n)*sizeof(*ranges_));
    if(upper)upper_count_-=n;else lower_count_-=n;
}
void NativeAdrenoVm::linkMaintained(Mapping &m)
{
    const uint32_t slot=uint32_t(&m-mappings_);
    m.prev=UINT32_MAX;m.next=maintained_head_;
    if(maintained_head_!=UINT32_MAX)mappings_[maintained_head_].prev=slot;
    maintained_head_=slot;m.maintained=true;++maintained_count_;
}
void NativeAdrenoVm::unlinkMaintained(Mapping &m)
{
    if(!m.maintained)return;
    if(m.prev!=UINT32_MAX)mappings_[m.prev].next=m.next;else maintained_head_=m.next;
    if(m.next!=UINT32_MAX)mappings_[m.next].prev=m.prev;
    m.prev=m.next=UINT32_MAX;m.maintained=false;--maintained_count_;
}
/* Per-submission maintenance (ARCHITECTURE 4.14.1): a GPU-read-only import mapped non-cacheable is cleaned before each submission
 * (empty from coherence level 1); so is an own BO whose CPU view is cacheable (not write-combined: never with the default
 * write-combined user BOs). */
static bool needsSubmitMaintenance(const NativeAdrenoMemory *memory,unsigned permissions,unsigned writeback)
{
    if(coherentMapping(writeback) || memory->scanout())return false;
    if(memory->imported())return !(permissions&ArmSmmu4KWrite);
    return !memory->writeCombined() && memory->mapping();
}
IOReturn NativeAdrenoVm::mapLocked(NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;result_={};
#ifdef ADRENO830_MACOS
    refusal_={};   /* UniXNU-piano: this call's refusal only */
#endif
    if(!attached_ || poisoned_)return VM_REFUSE(NotReady,kIOReturnNotReady,attached_,poisoned_,0,memory);
    PhaseClock clock;
    const uint64_t p0b_began=NativeAdrenoP0bNow();   /* UniXNU-piano: GPU-PAGED P0b's map time by backing */
    if(!memory)return VM_REFUSE(Request,kIOReturnBadArgument,permissions,writeback,shareability,memory);
    const unsigned cls=aliasClass(permissions,writeback,shareability)&15;
    const bool exclusive=exclusivePages(memory,cls);
    IOReturn checked=preflight(memory,upper,iova,permissions,writeback,shareability,exclusive);if(checked)return checked;
    if(next_handle_==UINT64_MAX)return kIOReturnNoResources;
    clock.lap(0,5);
    checked=memory->holdForDevice();if(checked)return VM_REFUSE(Hold,checked,uint32_t(checked),0,0,memory);
    clock.lap(0,1);
    Mapping *record=nullptr;
    for(size_t i=free_hint_;i<NativeAdrenoVmMaxObjects;++i)if(!mappings_[i].memory) {
        record=&mappings_[i];free_hint_=i+1;if(high_<i+1)high_=i+1;break;
    }
    if(!record) {memory->quarantine();return fail(ArmSmmu4KStateMismatch);}
    *record=Mapping{};
    record->memory=memory;record->iova=iova;record->spans=memory->spanCount();record->permissions=permissions;
    record->writeback=writeback;record->shareability=shareability;
    ++objects_;
    __atomic_store_n(&bytes_,bytes_+memory->bytes(),__ATOMIC_RELAXED);
    /* Once, before the GPU can see the pages: an import is cleaned (and invalidated when the GPU writes it) through its physical
     * pages; a write-combined own BO and the scanout need nothing; a coherent mapping needs nothing. */
    if(coherentMapping(writeback))native_adreno_coherent_skips[0]=native_adreno_coherent_skips[0]+1;
    else if(!memory->scanout()) {
        checked=memory->publishForDevice(0,memory->bytes(),permissions&ArmSmmu4KWrite,distrust_write_combined_);
        if(checked)return fail(ArmSmmu4KIoError);
    }
    clock.lap(0,2);
    /* Every span in one table transaction, staged in the table's free tail (preflight checked the room). */
    const size_t spans=record->spans;
    ArmSmmu4KRange *changes=ranges_+spanCount();
    for(size_t i=0;i<spans;++i) {
        const auto &span=memory->spans()[i];
        changes[i]={iova+span.offset,span.physical,span.bytes,permissions,writeback,shareability};
    }
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    /* preflight checked a map that is not exclusive against every other class's range of both regions, the alias rule of the table
     * transaction's own pass (same classes, same overlap), so that pass would sort them all again for the same answer, under this
     * lock, which every submission waits for (upstream Adreno740 a087fca: 3.56 ms a map, 79% of all map time). pagani-gpu-kopt bit 2. */
    const ArmSmmu4KStatus status=exclusive || NativeAdrenoKoptOn(NativeAdrenoKoptAliasOnce)?
        ArmSmmu4KLiveMapContextBatchExclusive(&io_,bank_,context_,lower.arena?&lower:nullptr,
            high.arena?&high:nullptr,upper,&visibility_[upper],changes,spans,budget_,&update):
        ArmSmmu4KLiveUpdateContextBatch(&io_,bank_,context_,lower.arena?&lower:nullptr,
            high.arena?&high:nullptr,upper,&visibility_[upper],changes,spans,1,budget_,&update);
    if(status)return fail(status);
    /* The programmer's sync_after_map (A830Smmu, annibale profile): once the leaves are published, one invalidation of the
     * bank's ASID and its TLBSYNC, which A830Smmu's I/O sends as TLBIASID + TLBSYNC = 1. A sync that does not complete within
     * the budget fails the transaction like any other: the VM is poisoned and keeps every reference. */
    if(sync_after_map_) {
        ArmSmmu4KProgramResult synced;
        const ArmSmmu4KStatus sync=ArmSmmu4KSyncContextTlb(&io_,context_offset_,context_->asid,0,budget_,&synced);
        if(sync)return fail(sync);
        NativeAdrenoP0bAdd(&native_adreno_p0b_tlbi[0],1);
    }
    clock.lap(0,3);
    insert(spans,upper);record->mapped_spans=spans;result_.completed_spans=spans;
    if(memory->imported() || memory->scanout())import_spans_[cls]+=spans;
    if(needsSubmitMaintenance(memory,permissions,writeback))linkMaintained(*record);
    clock.lap(0,4);clock.done(0,memory->bytes());
    {
        const unsigned kind=!memory->imported()?0:exclusive?1:2;
        native_adreno_vm_kind_us[kind][0]=native_adreno_vm_kind_us[kind][0]+1;
        native_adreno_vm_kind_us[kind][1]=native_adreno_vm_kind_us[kind][1]+clock.elapsed();
        native_adreno_vm_kind_us[kind][2]=native_adreno_vm_kind_us[kind][2]+spans;
    }
    {   /* GPU-PAGED P0b (NativeAdrenoPoolBypass.h): [0] pooled, [1] an own BO outside the pool, [2] an import or the scanout */
        const unsigned p0b_kind=memory->imported() || memory->scanout()?2:memory->pooled()?0:1;
        NativeAdrenoP0bAdd(&native_adreno_p0b_map_pages[p0b_kind],memory->bytes()/4096);
        if(p0b_began)NativeAdrenoP0bTime(native_adreno_p0b_map_us[p0b_kind],NativeAdrenoP0bNow()-p0b_began);
    }
    record->handle=next_handle_++;*handle=record->handle;index_.insert(record->handle,(uint32_t)(record-mappings_));
    result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;
    *handle=0;
    if(!attached_)return kIOReturnNotReady;
    /* GPU-PAGED P1: kernel buffers and the self-test's pages: no cap, no reserve, the lower space only */
    if(paged_)return upper?kIOReturnBadArgument:mapPaged(memory,iova,permissions,writeback,shareability,handle,false,0,false,nullptr);
    VM_LOCKED;
    return mapLocked(memory,upper,iova,permissions,writeback,shareability,handle);
}
void NativeAdrenoVm::setCoherence(unsigned level)
{
    __atomic_store_n(&native_adreno_io_coherent,level>NativeAdrenoCoherenceCached?uint32_t(NativeAdrenoCoherenceCached):level,
                     __ATOMIC_RELEASE);
}
unsigned NativeAdrenoVm::coherence() {return __atomic_load_n(&native_adreno_io_coherent,__ATOMIC_ACQUIRE);}
void NativeAdrenoVm::attributes(const NativeAdrenoMemory &memory,unsigned permissions,unsigned requested_writeback,
    unsigned requested_shareability,unsigned *writeback,unsigned *shareability)
{
    (void)requested_shareability;
    const unsigned level=coherence();
    bool cached=false;
    if(memory.scanout())cached=false;                                       /* the scanout: non-cacheable, the GPU writes it */
    else if(memory.imported())
        cached=(permissions&ArmSmmu4KWrite)?level>=NativeAdrenoCoherenceCached:level>=NativeAdrenoCoherenceReadImports;
    else cached=level>=NativeAdrenoCoherenceCached && requested_writeback;   /* own BOs: write-combined for the CPU below 2 */
    if(writeback)*writeback=cached?1u:0u;
    if(shareability)*shareability=cached?3u:2u;
}
IOReturn NativeAdrenoVm::mapClient(NativeAdrenoMemory *memory,uint64_t iova,unsigned permissions,unsigned writeback,
    unsigned shareability,uint64_t *handle,size_t keep_objects,size_t keep_spans
#ifdef ADRENO830_MACOS
    ,NativeAdrenoVmRefusal *refusal
#endif
    )
{
#ifdef ADRENO830_MACOS
    if(refusal)*refusal={};
    /* Validation before the lock uses only call-owned output, never the shared last-refusal slot. */
    const auto early=[&](uint8_t why,IOReturn status,uint64_t a=0,uint64_t b=0,uint64_t c=0) {
        if(refusal)*refusal={why,a,b,c,memory};return status;
    };
#endif
    if(!handle)return kIOReturnBadArgument;
    *handle=0;
    if(!memory || writeback>1 || (shareability!=2 && shareability!=3) || (permissions&ArmSmmu4KPrivileged))
#ifdef ADRENO830_MACOS
        return early(NativeAdrenoVmWhyRequest,kIOReturnBadArgument,permissions,writeback,shareability);
#else
        return kIOReturnBadArgument;
#endif
    /* Memory the owner maps read-only could only be pinned for reading: the GPU gets the read half of what was asked. */
    if(memory->readOnly())permissions&=~unsigned(ArmSmmu4KWrite);
    unsigned wb=0,sh=2;
    attributes(*memory,permissions,writeback,shareability,&wb,&sh);
    if(!attached_)
#ifdef ADRENO830_MACOS
        return early(NativeAdrenoVmWhyNotReady,kIOReturnNotReady);
#else
        return kIOReturnNotReady;
#endif
    /* GPU-PAGED P1: the mapped-bytes cap for every client; a client with keep_spans (every one but the render server) also leaves the
     * render server's reserve in bytes; the records' reserve as before. */
    if(paged_)
        return mapPaged(memory,iova,permissions,wb,sh,handle,true,keep_objects,keep_spans!=0,
#ifdef ADRENO830_MACOS
            refusal
#else
            nullptr
#endif
            );
    VM_LOCKED;
#ifdef ADRENO830_MACOS
    refusal_={};
    const auto finish=[&](IOReturn status) {
        if(refusal) {
            *refusal=status?refusal_:NativeAdrenoVmRefusal{};
            refusal->objects=objects_;refusal->spans=spanCount();refusal->capacity=capacity_;refusal->bytes=bytes_;
            refusal->keep_objects=keep_objects;refusal->keep_spans=keep_spans;
        }
        return status;
    };
#else
    const auto finish=[](IOReturn status) {return status;};
#endif
    /* The render server's reserve (NativeAdrenoVmRenderReserve*): another client's mapping must leave this much of both tables. */
    if(keep_objects && objects_+1+keep_objects>NativeAdrenoVmMaxObjects) {
        native_adreno_vm_reserve_refused[0]=native_adreno_vm_reserve_refused[0]+1;
        return finish(VM_REFUSE(ReserveObjects,kIOReturnNoResources,objects_,keep_objects,0,memory));
    }
    if(keep_spans && memory->spanCount()+keep_spans>capacity_-spanCount()) {
        native_adreno_vm_reserve_refused[1]=native_adreno_vm_reserve_refused[1]+1;
        return finish(VM_REFUSE(ReserveSpans,kIOReturnNoResources,memory->spanCount(),capacity_-spanCount(),keep_spans,memory));
    }
    return finish(mapLocked(memory,0,iova,permissions,wb,sh,handle));
}

bool NativeAdrenoVm::completed(uint64_t fence) const
{
    if(!fence)return true;
    if(!fence_)return false;
    const uint32_t done=__atomic_load_n(fence_,__ATOMIC_ACQUIRE);
    return int32_t(done-uint32_t(fence))>=0;
}
IOReturn NativeAdrenoVm::unmap(uint64_t handle)
{
    native_adreno_trace_stage=10;
    result_={};
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    /* Only this buffer's own last use decides (ARCHITECTURE 4.6.4): the GPU is never idled globally for an unmap. */
    if(record->jobs || !completed(record->last_use))return kIOReturnBusy;
    PhaseClock clock;
    native_adreno_trace_stage=11;
    clock.lap(1,1);
    unsigned upper=regions_[1].arena && record->iova>=regions_[1].arena->iova;
    /* Imports were cleaned and invalidated once when mapped and the scanout and write-combined BOs never need it; only an own BO
     * the CPU sees cacheable (not write-combined) drops the lines the GPU's writes made stale. */
    if(coherentMapping(record->writeback))native_adreno_coherent_skips[1]=native_adreno_coherent_skips[1]+1;
    else if((record->permissions&ArmSmmu4KWrite) && !record->memory->imported() && !record->memory->scanout()) {
        if(record->memory->acquireDeviceWrites(0,record->memory->bytes())!=kIOReturnSuccess)
            return fail(ArmSmmu4KIoError);
    }
    clock.lap(1,2);
    /* The mapping's spans are consecutive in the table; unmap them in one transaction. */
    const size_t spans=paged_?record->memory->spanCount():record->spans;
    const size_t at=rangeAt(record->iova);
    if(paged_) {   /* GPU-PAGED P1: the leaves of the record's range, its TLB plan, the record */
        native_adreno_trace_stage=12;
        const IOReturn paged=unmapPagedTables(*record,at);
        native_adreno_trace_stage=13;
        if(paged)return paged;
        clock.lap(1,3);
        erase(at,1,0);record->mapped_spans=0;result_.completed_spans=spans;
    } else {
    if(at>=spanCount() || ranges_[at].iova!=record->iova || spans>spanCount()-at)return fail(ArmSmmu4KStateMismatch);
    for(size_t i=0;i<spans;++i) {
        const auto &span=record->memory->spans()[i];
        const ArmSmmu4KRange &r=ranges_[at+i];
        if(r.iova!=record->iova+span.offset || r.physical!=span.physical || r.bytes!=span.bytes ||
            r.permissions!=record->permissions || r.writeback!=record->writeback || r.shareability!=record->shareability)
            return fail(ArmSmmu4KStateMismatch);
    }
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    native_adreno_trace_stage=12;
    /* Leaves cleared and published, then <= 512 pages by TLBIVAL (one 64-bit store each) or TLBIASID, one TLBSYNC: the pages may
     * be freed only once that sync completed (the kit returns after it). */
    const ArmSmmu4KStatus status=ArmSmmu4KLiveUpdateContextBatch(&io_,bank_,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],ranges_+at,spans,0,budget_,&update);
    native_adreno_trace_stage=13;
    if(status)return fail(status);
    clock.lap(1,3);
    erase(at,spans,upper);record->mapped_spans=0;result_.completed_spans=spans;
    }
    native_adreno_trace_stage=14;
    clock.lap(1,4);
    if(record->memory->retireAfterQuiescence()!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    clock.lap(1,5);clock.done(1,record->memory->bytes());
    if(!paged_ && (record->memory->imported() || record->memory->scanout()))
        import_spans_[aliasClass(record->permissions,record->writeback,record->shareability)&15]-=spans;
    {
        const unsigned kind=record->memory->bytes()<=(size_t)ArmSmmu4KTlbPageLimit*4096?3:4;   /* page TLBIs, else the ASID */
        native_adreno_vm_kind_us[kind][0]=native_adreno_vm_kind_us[kind][0]+1;
        native_adreno_vm_kind_us[kind][1]=native_adreno_vm_kind_us[kind][1]+clock.elapsed();
        native_adreno_vm_kind_us[kind][2]=native_adreno_vm_kind_us[kind][2]+spans;
        /* GPU-PAGED P0b: the unmap's TLB invalidation by the same rule */
        if(kind==3) {NativeAdrenoP0bAdd(&native_adreno_p0b_tlbi[2],1);NativeAdrenoP0bAdd(&native_adreno_p0b_tlbi[3],record->memory->bytes()/4096);}
        else NativeAdrenoP0bAdd(&native_adreno_p0b_tlbi[1],1);
    }
    unlinkMaintained(*record);
    index_.erase(record->handle);
    const size_t slot=(size_t)(record-mappings_);if(slot<free_hint_)free_hint_=slot;
    __atomic_store_n(&bytes_,bytes_-record->memory->bytes(),__ATOMIC_RELAXED);
    *record=Mapping{};--objects_;
    while(high_ && !mappings_[high_-1].memory)--high_;result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::describe(uint64_t handle,uint64_t *iova,size_t *bytes,
    unsigned *permissions) const
{
    if(!iova || !bytes || !permissions)return kIOReturnBadArgument;
    *iova=0;*bytes=0;*permissions=0;
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    const Mapping *mapping=find(handle);if(!mapping)return kIOReturnBadArgument;
    *iova=mapping->iova;*bytes=mapping->memory->bytes();*permissions=mapping->permissions;
    return mapping->mapped_spans==mapping->spans?kIOReturnSuccess:kIOReturnNotReady;
}
/* TLBIALL of the bank and TLBSYNC at the proved register offset: no image or fault observation. */
IOReturn NativeAdrenoVm::invalidateTlb()
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    ArmSmmu4KProgramResult r;
    const ArmSmmu4KStatus status=ArmSmmu4KSyncContextTlb(&io_,context_offset_,context_->asid,1,budget_,&r);
    return status?fail(status):kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::setTableWalkCoherent(bool coherent)
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(paged_) {coherent_walk_=coherent;return kIOReturnSuccess;}   /* GPU-PAGED P1: tablePublish reads it */
    ArmSmmu4KLiveVisibility next[2]={};
    for(unsigned i=0;i<2;++i)if(regions_[i].arena) {
        const IOReturn status=coherent?NativeAdrenoBindSmmuVisibilityCoherent(regions_[i].tables,&next[i]):
            NativeAdrenoBindSmmuVisibility(regions_[i].tables,&next[i]);
        if(status!=kIOReturnSuccess)return status;
    }
    for(unsigned i=0;i<2;++i)if(regions_[i].arena)visibility_[i]=next[i];
    coherent_walk_=coherent;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::cleanForSubmission()
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    uint64_t p0b_spans=0;   /* GPU-PAGED P0b: the submission's walk of physical pages */
    for(uint32_t slot=maintained_head_;slot!=UINT32_MAX;slot=mappings_[slot].next) {
        const Mapping &m=mappings_[slot];
        if(m.memory->publishForDevice(0,m.memory->bytes(),false,distrust_write_combined_))return fail(ArmSmmu4KIoError);
        p0b_spans+=m.memory->spanCount();
    }
    NativeAdrenoP0bAdd(&native_adreno_p0b_submit[0],1);
    NativeAdrenoP0bAdd(&native_adreno_p0b_submit[1],maintained_count_);
    NativeAdrenoP0bAdd(&native_adreno_p0b_submit[2],p0b_spans);
    native_adreno_coherent_skips[2]=native_adreno_coherent_skips[2]+(maintained_count_?0:1);
    return kIOReturnSuccess;
}
size_t NativeAdrenoVm::maintainedCount() const
{
    if(!attached_)return 0;
    VM_LOCKED;
    return maintained_count_;
}
IOReturn NativeAdrenoVm::publishMapped() {return cleanForSubmission();}
/* After a completion: nothing for imports (cleaned and invalidated once when mapped, 4.14.1) and write-combined BOs; an own BO
 * the CPU sees cacheable (never with the default write-combined user BOs) drops the lines the GPU's writes made stale. */
IOReturn NativeAdrenoVm::acquireImported()
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    for(uint32_t slot=maintained_head_;slot!=UINT32_MAX;slot=mappings_[slot].next) {
        const Mapping &m=mappings_[slot];
        if(m.memory->imported() || !(m.permissions&ArmSmmu4KWrite))continue;
        if(m.memory->acquireDeviceWrites(0,m.memory->bytes()))return fail(ArmSmmu4KIoError);
    }
    native_adreno_coherent_skips[3]=native_adreno_coherent_skips[3]+1;
    return kIOReturnSuccess;
}
void NativeAdrenoVm::setFenceSource(const volatile uint32_t *completed)
{
    lock();fence_=completed;unlock();
}
IOReturn NativeAdrenoVm::noteUse(uint64_t handle,uint64_t fence)
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record || !fence)return kIOReturnBadArgument;
    if(!record->last_use || int32_t(uint32_t(fence)-uint32_t(record->last_use))>0)record->last_use=fence;
    return kIOReturnSuccess;
}
uint64_t NativeAdrenoVm::lastUse(uint64_t handle) const
{
    if(!attached_)return 0;
    VM_LOCKED;
    const Mapping *record=find(handle);
    return record?record->last_use:0;
}
bool NativeAdrenoVm::idle(uint64_t handle) const
{
    if(!attached_)return false;
    VM_LOCKED;
    const Mapping *record=find(handle);
    return record && !record->jobs && completed(record->last_use);
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const
{
    if(!cached || !dram)return kIOReturnBadArgument;
    *cached=*dram=~0u;
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(iova&3)return kIOReturnBadArgument;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        const size_t offset=(size_t)(iova-m.iova);
        if(m.memory->mapping())*cached=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4];
        for(size_t j=0;j<m.memory->spanCount();++j) {
            const auto &span=m.memory->spans()[j];
            if(offset<span.offset || offset-span.offset>=span.bytes)continue;
            uint32_t value=0;
            /* DRAM first: the cached reader writes its line back, so read after it DRAM would always agree (int-099). */
            if(native_adreno_dram_read32 && !native_adreno_dram_read32(span.physical+(offset-span.offset),&value))*dram=value;
            /* A write-combined BO's own mapping is non-cacheable: read the cacheable alias instead, so a dirty line is still seen. */
            if(native_adreno_cached_read32 && !native_adreno_cached_read32(span.physical+(offset-span.offset),&value))*cached=value;
            break;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::reassertWriteCombined(uint32_t *reset) const
{
    if(!reset)return kIOReturnBadArgument;
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory)continue;
        const IOReturn status=m.memory->reassertWriteCombined(reset);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const
{
    if(!lines || !dirty || !stale)return kIOReturnBadArgument;
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(!native_adreno_line_audit)return kIOReturnUnsupported;
    if(!bytes || iova>UINT64_MAX-bytes)return kIOReturnBadArgument;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        if(!m.memory->writeCombined())return kIOReturnSuccess;   /* maintained before every submission anyway */
        /* Whole lines; the range is clipped to its BO. */
        size_t at=(size_t)(iova-m.iova)&~(size_t)63;
        const size_t end=(size_t)(iova-m.iova)+bytes>m.memory->bytes()?m.memory->bytes():(size_t)(iova-m.iova)+bytes;
        while(at<end) {
            const NativeAdrenoPhysicalSpan *span=nullptr;
            for(size_t j=0;j<m.memory->spanCount();++j) {
                const auto &s=m.memory->spans()[j];
                if(at>=s.offset && at-s.offset<s.bytes) {span=&s;break;}
            }
            if(!span)return kIOReturnBadArgument;
            const uint64_t physical=span->physical+(at-span->offset);
            size_t chunk=4096-(size_t)(physical&4095);
            if(chunk>span->offset+span->bytes-at)chunk=span->offset+span->bytes-at;
            if(chunk>end-at)chunk=(end-at+63)&~(size_t)63;
            if(native_adreno_line_audit(physical,chunk,dirty,stale))return kIOReturnIOError;
            *lines+=(uint32_t)(chunk/64);at+=chunk;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
static IOReturn peekMapped(const NativeAdrenoMemory *memory,size_t offset,uint32_t *out,size_t count)
{
    for(size_t k=0;k<count && offset+4*k+4<=memory->bytes();++k)out[k]=static_cast<const volatile uint32_t *>(memory->mapping())[offset/4+k];
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t iova,uint32_t *out,size_t count) const
{
    if(!out || !attached_)return kIOReturnBadArgument;
    VM_LOCKED;
    if(poisoned_)return kIOReturnBadArgument;
    for(size_t k=0;k<count;++k)out[k]=0;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || !m.memory->mapping() || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        return peekMapped(m.memory,(size_t)(iova-m.iova),out,count);
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::peekWordsUncached(uint64_t iova,uint32_t *out,size_t count) const
{
    if(!out || !attached_ || !count)return kIOReturnBadArgument;
    VM_LOCKED;
    if(poisoned_)return kIOReturnBadArgument;
    for(size_t k=0;k<count;++k)out[k]=0;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || !m.memory->mapping() || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        const size_t offset=(size_t)(iova-m.iova),first=offset&~(size_t)4095;
        const size_t end=offset+4*count<m.memory->bytes()?offset+4*count:m.memory->bytes();
        const size_t bytes=((end+4095)&~(size_t)4095)-first;
        IOReturn status=NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(m.memory->mapping())+first,bytes,NativeAdrenoCleanInvalidate);
        if(status)return status;
        return peekMapped(m.memory,offset,out,count);
    }
    return kIOReturnBadArgument;
}
uint64_t NativeAdrenoVm::handleAt(uint64_t iova) const
{
    if(!attached_ || !mappings_)return 0;
    VM_LOCKED;
    if(!mappings_)return 0;
    /* ranges_ is sorted by address: the range over iova names the mapping (its spans are consecutive). */
    const size_t i=rangeAt(iova);
    if(i>=spanCount() || ranges_[i].iova>iova)return 0;
    if(paged_)return ranges_[i].physical<high_ && mappings_[ranges_[i].physical].memory?mappings_[ranges_[i].physical].handle:0;
    for(size_t j=0;j<high_;++j) {
        const Mapping &m=mappings_[j];
        if(m.memory && iova>=m.iova && iova-m.iova<m.memory->bytes())return m.handle;
    }
    return 0;
}
#ifdef ADRENO830_MACOS
IOReturn NativeAdrenoVm::locate(uint64_t iova,NativeAdrenoVmPlace *over,NativeAdrenoVmPlace *below) const
{
    if(over)*over={};
    if(below)*below={};
    if(!attached_ || !mappings_)return kIOReturnNotReady;
    VM_LOCKED;
    if(!mappings_)return kIOReturnNotReady;
    const auto place=[](const Mapping &m)->NativeAdrenoVmPlace {
        const NativeAdrenoMemory &o=*m.memory;
        const uint8_t kind=o.scanout()?NativeAdrenoVmKindScanout:o.imported()?NativeAdrenoVmKindImport:
                           o.pooled()?NativeAdrenoVmKindPool:NativeAdrenoVmKindOwn;
        return {m.handle,m.iova,o.bytes(),kind};
    };
    uint64_t highest=0;
    for(size_t i=0;i<high_;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory)continue;
        const uint64_t bytes=m.memory->bytes();
        if(iova>=m.iova && iova-m.iova<bytes) {if(over)*over=place(m);continue;}
        if(m.iova<iova && m.iova+bytes>highest) {highest=m.iova+bytes;if(below)*below=place(m);}
    }
    return kIOReturnSuccess;
}
#endif
IOReturn NativeAdrenoVm::faultState(uint32_t out[4]) const
{
    if(!out || !attached_)return kIOReturnBadArgument;
    VM_LOCKED;
    if(poisoned_)return kIOReturnBadArgument;
    if(!context_offset_)return kIOReturnNotReady;
    static const uint32_t regs[4]={0x58,0x60,0x64,0x68};
    for(unsigned i=0;i<4;++i)if(io_.read32(io_.cookie,(size_t)context_offset_+regs[i],&out[i]))return kIOReturnIOError;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::holdJob(uint64_t handle)
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs==UINT32_MAX)return kIOReturnNoResources;
    IOReturn status=record->memory->holdForDevice();if(status)return status;
    ++record->jobs;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t handle)
{
    if(!attached_)return kIOReturnNotReady;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record || !record->jobs)return kIOReturnBadArgument;
    IOReturn status=record->memory->retireAfterQuiescence();if(status)return status;
    --record->jobs;return kIOReturnSuccess;
}

/* ---------------------------------------------------------------------------------------------------------------------------------
 * UniXNU-piano (2026-11-23, GPU-PAGED P1, private host-checks/20261122-gpu-paged/P1-GATE.md): the paged VM. The tables are the only
 * per-page record (ArmSmmu4KPaged); ranges_ holds one record per mapping whose `physical` field is the mapping's slot in mappings_.
 * Table pages: pg_->chunks (contiguous, zeroed, kernel-mapped, cacheable, held for the device) cut into a free list of 4 KiB pages linked
 * through their first word; the list, the chunk index and every table change live under the VM lock. A chunk is allocated only by
 * growTables, outside the VM lock (its own grow lock): no allocation and no wait for memory while every submission waits here. */
#ifdef ADRENO830_HOST_TEST
static void *growLockAlloc() {return new std::mutex;}
static void growLockFree(void *lock) {delete static_cast<std::mutex *>(lock);}
static void growLockTake(void *lock) {static_cast<std::mutex *>(lock)->lock();}
static void growLockDrop(void *lock) {static_cast<std::mutex *>(lock)->unlock();}
#else
static void *growLockAlloc() {return IOLockAlloc();}
static void growLockFree(void *lock) {IOLockFree(static_cast<IOLock *>(lock));}
static void growLockTake(void *lock) {IOLockLock(static_cast<IOLock *>(lock));}
static void growLockDrop(void *lock) {IOLockUnlock(static_cast<IOLock *>(lock));}
#endif
namespace {
/* A BO's spans as the kit's run iterator (ascending offsets, from 0 for each walk). */
struct PagedSpans {const NativeAdrenoPhysicalSpan *spans;size_t count,at;};
int pagedRun(void *cookie,uint64_t offset,uint64_t *physical,uint64_t *bytes)
{
    PagedSpans *c=static_cast<PagedSpans *>(cookie);
    while(c->at<c->count && c->spans[c->at].offset+c->spans[c->at].bytes<=offset)++c->at;
    if(c->at>=c->count || offset<c->spans[c->at].offset)return 1;
    const uint64_t into=offset-c->spans[c->at].offset;
    *physical=c->spans[c->at].physical+into;*bytes=c->spans[c->at].bytes-into;
    return 0;
}
uint64_t pagedNow() {return native_adreno_vm_now_us?native_adreno_vm_now_us():0;}
}
int NativeAdrenoVm::chunkAt(uint64_t physical) const
{
    size_t low=0,high=pg_->chunk_count;
    while(low<high) {
        const size_t mid=low+(high-low)/2;
        if(pg_->chunks[pg_->order[mid]].spans()[0].physical<=physical)low=mid+1;else high=mid;
    }
    if(!low)return -1;
    const unsigned c=pg_->order[low-1];
    return physical-pg_->chunks[c].spans()[0].physical<pg_->chunks[c].bytes()?int(c):-1;
}
/* Whether [physical, physical + bytes) touches a table chunk (the chunks are disjoint and sorted: only the last one starting below the
 * end can, as it starts inside the range or reaches into it). */
bool NativeAdrenoVm::onTables(uint64_t physical,uint64_t bytes) const
{
    const uint64_t end=physical+bytes;
    size_t low=0,high=pg_->chunk_count;
    while(low<high) {
        const size_t mid=low+(high-low)/2;
        if(pg_->chunks[pg_->order[mid]].spans()[0].physical<end)low=mid+1;else high=mid;
    }
    if(!low)return false;
    const NativeAdrenoMemory &c=pg_->chunks[pg_->order[low-1]];
    return c.spans()[0].physical>=physical || c.spans()[0].physical+c.bytes()>physical;
}
volatile uint64_t *NativeAdrenoVm::tableCpu(void *cookie,uint64_t physical)
{
    const NativeAdrenoVm *vm=static_cast<const NativeAdrenoVm *>(cookie);
    if(physical&4095)return nullptr;
    const int c=vm->chunkAt(physical);
    if(c<0)return nullptr;
    return reinterpret_cast<volatile uint64_t *>(static_cast<unsigned char *>(vm->pg_->chunks[c].mapping())+
        (physical-vm->pg_->chunks[c].spans()[0].physical));
}
int NativeAdrenoVm::tableAllocate(void *cookie,uint64_t *physical,volatile uint64_t **cpu)
{
    NativeAdrenoVm *vm=static_cast<NativeAdrenoVm *>(cookie);
    if(!vm->pg_->free_pages)return 1;
    volatile uint64_t *page=tableCpu(cookie,vm->pg_->free_head);
    if(!page)return 1;
    *physical=vm->pg_->free_head;*cpu=page;
    vm->pg_->free_head=page[0];
    __atomic_store_n(&vm->pg_->free_pages,vm->pg_->free_pages-1,__ATOMIC_RELAXED);
    return 0;
}
void NativeAdrenoVm::tableRelease(void *cookie,uint64_t physical,volatile uint64_t *cpu)
{
    NativeAdrenoVm *vm=static_cast<NativeAdrenoVm *>(cookie);
    if(!cpu)cpu=tableCpu(cookie,physical);
    if(!cpu)return;
    cpu[0]=vm->pg_->free_pages?vm->pg_->free_head:0;
    vm->pg_->free_head=physical;
    __atomic_store_n(&vm->pg_->free_pages,vm->pg_->free_pages+1,__ATOMIC_RELAXED);
}
/* A changed table page for the walker: cleaned to PoC until V4 proved the walk coherent (M10), then dsb ishst only. */
int NativeAdrenoVm::tablePublish(void *cookie,volatile uint64_t *cpu,size_t bytes)
{
    const NativeAdrenoVm *vm=static_cast<const NativeAdrenoVm *>(cookie);
    if(!cpu || bytes!=4096 || (reinterpret_cast<uintptr_t>(cpu)&4095))return 1;
    if(vm->coherent_walk_) {NativeAdrenoStoreBarrier();return 0;}
    return NativeAdrenoMaintainOwnedPages(const_cast<uint64_t *>(cpu),4096,NativeAdrenoClean)==kIOReturnSuccess?0:1;
}
/* One more chunk (the grow lock held, the VM lock not): allocated, held for the device, then indexed and cut into the free list
 * under the VM lock, lowest address handed out first. */
IOReturn NativeAdrenoVm::addChunk(size_t bytes)
{
    const size_t slot=pg_->chunk_count;   /* only growers change it, and they hold the grow lock */
    if(slot>=NativeAdrenoVmPagedMaxChunks)return kIOReturnNoResources;
    NativeAdrenoMemory &chunk=pg_->chunks[slot];
    IOReturn status=chunk.allocate(bytes,pg_->config.output_bits,true,bytes);
    if(status) {if(!chunk.quarantined() && chunk.backed())(void)chunk.release();return status;}
    if(!chunk.mapping() || chunk.spanCount()!=1 || chunk.spans()[0].bytes!=bytes || chunk.spans()[0].offset ||
        (chunk.spans()[0].physical&4095)) {(void)chunk.release();return kIOReturnUnsupported;}
    status=chunk.holdForDevice();
    if(status) {(void)chunk.release();return status;}
    VM_LOCKED;
    const uint64_t base=chunk.spans()[0].physical;
    size_t at=0;
    while(at<pg_->chunk_count && pg_->chunks[pg_->order[at]].spans()[0].physical<base)++at;
    memmove(pg_->order+at+1,pg_->order+at,(pg_->chunk_count-at)*sizeof pg_->order[0]);
    pg_->order[at]=uint16_t(slot);
    __atomic_store_n(&pg_->chunk_count,pg_->chunk_count+1,__ATOMIC_RELEASE);
    const size_t pages=bytes/4096;
    pg_->chunk_pages+=pages;
    for(size_t i=pages;i--;)tableRelease(this,base+uint64_t(i)*4096,nullptr);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::growTables(size_t pages)
{
    if(!paged_ || !grow_lock_)return kIOReturnSuccess;
    if(__atomic_load_n(&pg_->free_pages,__ATOMIC_RELAXED)>=pages)return kIOReturnSuccess;
    growLockTake(grow_lock_);
    IOReturn status=kIOReturnSuccess;
    static const size_t sizes[3]={size_t(2)<<20,size_t(512)<<10,size_t(128)<<10};
    while(__atomic_load_n(&pg_->free_pages,__ATOMIC_RELAXED)<pages) {
        const size_t room=pg_->config.table_cap_pages>pg_->chunk_pages?pg_->config.table_cap_pages-pg_->chunk_pages:0;
        if(!room || pg_->chunk_count>=NativeAdrenoVmPagedMaxChunks) {status=kIOReturnNoResources;break;}
        status=kIOReturnNoMemory;
        for(size_t s:sizes) {
            status=addChunk(s/4096>room?room*4096:s);
            if(!status)break;
        }
        if(status) {__atomic_fetch_add(&pg_->stats.grow_failures,1,__ATOMIC_RELAXED);break;}
    }
    growLockDrop(grow_lock_);
    return status;
}
static void releaseChunk(NativeAdrenoMemory &chunk)
{
    if(!chunk.backed())return;
    if(chunk.deviceReferences())(void)chunk.retireAfterQuiescence();
    (void)chunk.release();
}
IOReturn NativeAdrenoVm::preparePaged(const NativeAdrenoVmPagedConfig &config,uint64_t *root)
{
    if(!root)return kIOReturnBadArgument;
    *root=0;
    if(attached_ || ranges_ || poisoned_ || paged_)return kIOReturnBusy;
    if(config.input_bits<32 || config.input_bits>48 || config.output_bits<32 || config.output_bits>48 || !config.aperture ||
        (config.aperture&4095) || config.aperture>(UINT64_C(1)<<config.input_bits) || !config.table_cap_pages ||
        config.table_cap_pages>size_t(NativeAdrenoVmPagedMaxChunks)*512 || !config.cap_bytes || config.reserve_bytes>=config.cap_bytes ||
        (config.alias_scratch && !config.alias_scratch_entries))
        return kIOReturnBadArgument;
    if(!lock_ && !(lock_=lockAlloc()))return kIOReturnNoMemory;
    if(!grow_lock_ && !(grow_lock_=growLockAlloc()))return kIOReturnNoMemory;
    if(!pg_ && !(pg_=new PagedState))return kIOReturnNoMemory;   /* ~33 KiB of kalloc, not the collection's __DATA */
    ranges_=NativeAdrenoNewMeta(ArmSmmu4KRange,NativeAdrenoVmMaxObjects);
    mappings_=NativeAdrenoNewMeta(Mapping,NativeAdrenoVmMaxObjects);
    const auto undo=[&](IOReturn status) {
        for(size_t i=0;i<pg_->chunk_count;++i)releaseChunk(pg_->chunks[i]);
        delete pg_;pg_=nullptr;paged_=false;
        if(ranges_)NativeAdrenoDeleteMeta(ranges_,ArmSmmu4KRange,NativeAdrenoVmMaxObjects);
        if(mappings_)NativeAdrenoDeleteMeta(mappings_,Mapping,NativeAdrenoVmMaxObjects);
        index_.release();ranges_=nullptr;mappings_=nullptr;capacity_=0;
        return status;
    };
    if(!ranges_ || !mappings_ || !index_.allocate())return undo(kIOReturnNoMemory);
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i)mappings_[i]=Mapping{};
    free_hint_=0;high_=0;memset(class_spans_,0,sizeof(class_spans_));memset(import_spans_,0,sizeof(import_spans_));
    maintained_head_=UINT32_MAX;maintained_count_=0;window_count_=0;lower_count_=upper_count_=0;objects_=0;bytes_=0;
    capacity_=NativeAdrenoVmMaxObjects;
    pg_->config=config;paged_=true;
    const size_t first=config.table_cap_pages<NativeAdrenoVmPagedSpare?config.table_cap_pages:NativeAdrenoVmPagedSpare;
    const IOReturn grown=growTables(first);
    if(!pg_->free_pages)return undo(grown?grown:kIOReturnNoMemory);   /* a short first grow is fine as long as the root has its page */
    const ArmSmmu4KPagedConfig sc={config.input_bits,config.output_bits,0,config.aperture};
    const ArmSmmu4KPagedPages pages={this,tableAllocate,tableRelease,tableCpu,tablePublish,nullptr};
    ArmSmmu4KStatus init;
    {
        VM_LOCKED;
        init=ArmSmmu4KPagedInit(&pg_->space,&sc,&pages);
    }
    if(init)return undo(init==ArmSmmu4KCapacity?kIOReturnNoMemory:kIOReturnBadArgument);
    *root=pg_->space.root_physical;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::attachPaged(const NativeAdrenoSmmuBinding &smmu,unsigned budget)
{
    const NativeAdrenoSmmuProgrammer *p=smmu.programmer;
    if(!paged_ || attached_ || poisoned_ || !pg_->space.prepared)return kIOReturnBusy;
    if(!p || !p->io || !p->verify || !p->fault || !smmu.streams || !smmu.stream_count || !smmu.context ||
        smmu.stream_count>NativeAdrenoSmmuContext::MaxStreams || !smmu.context_offset || smmu.bank>255 || !budget || budget>1048576)
        return kIOReturnBadArgument;
    const ArmSmmu4KIo *io=p->io;
    if(!io->read32 || !io->write32 || !io->write64 || !io->barrier)return kIOReturnBadArgument;
    /* The bank must have been programmed on this space's root with its input size. */
    const ArmSmmu4KContextConfig *c=smmu.context;
    if(!c->lower || c->upper || !c->lower_tables || c->lower->table_physical!=pg_->space.root_physical ||
        c->lower_tables->root_physical!=pg_->space.root_physical || c->lower->input_bits!=pg_->config.input_bits)
        return kIOReturnBadArgument;
    VM_LOCKED;
    io_=*io;context_=smmu.context;budget_=budget;
    programmer_=*p;has_programmer_=true;kit_streams_=nullptr;sync_after_map_=p->sync_after_map;
    for(size_t i=0;i<smmu.stream_count;++i)streams_[i]=smmu.streams[i];
    stream_count_=smmu.stream_count;bank_=smmu.bank;context_offset_=smmu.context_offset;coherent_walk_=false;
    if(programmer_.verify(programmer_.cookie,bank_,streams_,stream_count_)!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    if(context_offset_>io_.register_bytes || io_.register_bytes-context_offset_<0x7f8)return fail(ArmSmmu4KBadRange);
    ArmSmmu4KPagedAuditResult audit;
    if(ArmSmmu4KPagedAudit(&pg_->space,nullptr,0,&audit))return fail(ArmSmmu4KStateMismatch);
    attached_=true;
    return kIOReturnSuccess;
}
/* The paged preflight: request, aperture, reserved windows, read-only rules, records, overlap with a record, every span inside the
 * output size and on no table page, and the cross-class alias rule (DESIGN 2.5): own BOs and kernel buffers own their pages; an import
 * or the scanout is compared only with the mappings of another unprivileged class (none at coherence 0). */
IOReturn NativeAdrenoVm::preflightPaged(const NativeAdrenoMemory *memory,uint64_t iova,unsigned permissions,unsigned writeback,
    unsigned shareability) const
{
    if(!memory || !memory->backed() || memory->quarantined() || !memory->bytes() || !memory->spanCount() ||
        !(permissions&ArmSmmu4KRead) || (permissions&~15u) || writeback>1 || (shareability!=2 && shareability!=3))
        return VM_REFUSE(Request,kIOReturnBadArgument,permissions,writeback,shareability,memory);
    const uint64_t bytes=memory->bytes(),aperture=pg_->config.aperture;
    if((iova&4095) || iova>=aperture || bytes>aperture-iova)return VM_REFUSE(Aperture,kIOReturnBadArgument,iova,bytes,aperture,memory);
    if(forbidden(iova,bytes))return VM_REFUSE(Reserved,kIOReturnBadArgument,iova,bytes,0,memory);
    if((memory->readOnly() && (permissions&ArmSmmu4KWrite)) || (memory->scanout() && writeback))
        return VM_REFUSE(ReadOnly,kIOReturnBadArgument,permissions,writeback,0,memory);
    if(objects_>=NativeAdrenoVmMaxObjects || spanCount()>=capacity_)
        return VM_REFUSE(Objects,kIOReturnNoResources,objects_,NativeAdrenoVmMaxObjects,0,memory);
    {
        const size_t i=rangeAt(iova);
        if(i<spanCount() && ranges_[i].iova<=iova+(bytes-1))return VM_REFUSE(Overlap,kIOReturnBusy,iova,ranges_[i].iova,0,memory);
    }
    const uint64_t limit=UINT64_C(1)<<pg_->config.output_bits;
    uint64_t offset=0;
    for(size_t j=0;j<memory->spanCount();++j) {
        const auto &span=memory->spans()[j];
        if(span.offset!=offset || !span.physical || !span.bytes || ((span.physical|span.bytes)&4095) ||
            span.bytes>bytes-offset || span.physical>=limit || span.bytes>limit-span.physical || onTables(span.physical,span.bytes))
            return VM_REFUSE(Pages,kIOReturnBadArgument,j,span.physical,span.bytes,memory);
        offset+=span.bytes;
    }
    if(offset!=bytes)return VM_REFUSE(Pages,kIOReturnBadArgument,memory->spanCount(),offset,bytes,memory);
    if(!memory->imported() && !memory->scanout())return kIOReturnSuccess;
    const unsigned mine=aliasClass(permissions,writeback,shareability)&15;
    size_t other=0;
    for(unsigned k=0;k<16;++k)if(k!=mine && !(k&8))other+=class_spans_[k];
    if(!other)return kIOReturnSuccess;
    const size_t n=memory->spanCount();
    uint32_t *order=pg_->config.alias_scratch;
    const bool indexed=order && n<=pg_->config.alias_scratch_entries;
    if(indexed)for(size_t i=0;i<n;++i)order[i]=uint32_t(i);
    NativeAdrenoPhysicalIndex<NativeAdrenoPhysicalSpan> index(memory->spans(),order,indexed?n:0);
    for(size_t s=0;s<high_;++s) {
        const Mapping &m=mappings_[s];
        if(!m.memory)continue;
        const unsigned cls=aliasClass(m.permissions,m.writeback,m.shareability)&15;
        if(cls==mine || (cls&8))continue;
        for(size_t a=0;a<m.memory->spanCount();++a) {
            const auto &theirs=m.memory->spans()[a];
            bool hit=false;
            if(indexed)hit=index.overlaps(theirs.physical,theirs.bytes);
            else for(size_t b=0;b<n && !hit;++b)hit=overlap(theirs.physical,theirs.bytes,memory->spans()[b].physical,memory->spans()[b].bytes);
            if(hit)return VM_REFUSE(Alias,kIOReturnBadArgument,theirs.physical,m.iova,mine,memory);
        }
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::mapPagedLocked(NativeAdrenoMemory *memory,uint64_t iova,unsigned permissions,unsigned writeback,
    unsigned shareability,uint64_t *handle,bool client,bool keep_reserve)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;result_={};pg_->short_pages=0;
#ifdef ADRENO830_MACOS
    refusal_={};
#endif
    if(!attached_ || poisoned_)return VM_REFUSE(NotReady,kIOReturnNotReady,attached_,poisoned_,0,memory);
    PhaseClock clock;
    const uint64_t p0b_began=NativeAdrenoP0bNow();
    if(!memory)return VM_REFUSE(Request,kIOReturnBadArgument,permissions,writeback,shareability,memory);
    const uint64_t bytes=memory->bytes();
    /* Bytes, not ranges (P1-GATE.md 2.3): every client stays under the cap; every client but the render server leaves its reserve. */
    if(client && bytes_+bytes>pg_->config.cap_bytes) {
        ++pg_->stats.refused_cap;
        return VM_REFUSE(Cap,kIOReturnNoResources,bytes_,bytes,pg_->config.cap_bytes,memory);
    }
    if(client && keep_reserve && bytes_+bytes+pg_->config.reserve_bytes>pg_->config.cap_bytes) {
        ++pg_->stats.refused_reserve;native_adreno_vm_reserve_refused[1]=native_adreno_vm_reserve_refused[1]+1;
        return VM_REFUSE(ReserveBytes,kIOReturnNoResources,bytes_,bytes,pg_->config.reserve_bytes,memory);
    }
    IOReturn checked=preflightPaged(memory,iova,permissions,writeback,shareability);if(checked)return checked;
    if(next_handle_==UINT64_MAX)return kIOReturnNoResources;
    /* Reserve, then commit: the table pages this map needs, from the free list, before the device reference and the record. */
    size_t need=0;
    ArmSmmu4KStatus status=ArmSmmu4KPagedNeed(&pg_->space,iova,bytes,&need);
    if(status==ArmSmmu4KOverlap)return fail(ArmSmmu4KStateMismatch);   /* a valid leaf where no record is: tables and records disagree */
    if(status)return VM_REFUSE(Aperture,kIOReturnBadArgument,iova,bytes,pg_->config.aperture,memory);
    if(pg_->space.counters.table_pages+need>pg_->config.table_cap_pages) {
        ++pg_->stats.refused_tables;
        return VM_REFUSE(Tables,kIOReturnNoResources,need,pg_->free_pages,pg_->config.table_cap_pages,memory);
    }
    ArmSmmu4KPagedReservation reservation{};
    if(need>pg_->free_pages || (need && ArmSmmu4KPagedReserve(&pg_->space,need,&reservation))) {
        ArmSmmu4KPagedCancel(&pg_->space,&reservation);
        pg_->short_pages=need;   /* mapPaged grows the list outside the lock and tries once more */
        return VM_REFUSE(Tables,kIOReturnNoResources,need,pg_->free_pages,pg_->config.table_cap_pages,memory);
    }
    clock.lap(0,5);
    checked=memory->holdForDevice();
    if(checked) {ArmSmmu4KPagedCancel(&pg_->space,&reservation);return VM_REFUSE(Hold,checked,uint32_t(checked),0,0,memory);}
    clock.lap(0,1);
    Mapping *record=nullptr;
    for(size_t i=free_hint_;i<NativeAdrenoVmMaxObjects;++i)if(!mappings_[i].memory) {
        record=&mappings_[i];free_hint_=i+1;if(high_<i+1)high_=i+1;break;
    }
    if(!record) {ArmSmmu4KPagedCancel(&pg_->space,&reservation);memory->quarantine();return fail(ArmSmmu4KStateMismatch);}
    *record=Mapping{};
    record->memory=memory;record->iova=iova;record->spans=1;record->permissions=permissions;
    record->writeback=writeback;record->shareability=shareability;
    ++objects_;
    __atomic_store_n(&bytes_,bytes_+bytes,__ATOMIC_RELAXED);
    if(coherentMapping(writeback))native_adreno_coherent_skips[0]=native_adreno_coherent_skips[0]+1;
    else if(!memory->scanout()) {
        checked=memory->publishForDevice(0,bytes,permissions&ArmSmmu4KWrite,distrust_write_combined_);
        if(checked) {ArmSmmu4KPagedCancel(&pg_->space,&reservation);return fail(ArmSmmu4KIoError);}
    }
    clock.lap(0,2);
    PagedSpans cursor={memory->spans(),memory->spanCount(),0};
    const ArmSmmu4KPagedRuns runs={&cursor,pagedRun};
    const ArmSmmu4KPagedAttrs attrs={permissions,writeback,shareability};
    ArmSmmu4KPagedPlan plan;
    status=ArmSmmu4KPagedMap(&pg_->space,&reservation,iova,bytes,&runs,&attrs,&plan);
    ArmSmmu4KPagedCancel(&pg_->space,&reservation);   /* what the map did not take (nothing, as Need counted it) */
    if(status)return fail(status);
    if(sync_after_map_) {
        ArmSmmu4KProgramResult synced;
        const ArmSmmu4KStatus sync=ArmSmmu4KSyncContextTlb(&io_,context_offset_,context_->asid,0,budget_,&synced);
        if(sync)return fail(sync);
        NativeAdrenoP0bAdd(&native_adreno_p0b_tlbi[0],1);
    }
    clock.lap(0,3);
    const size_t slot=size_t(record-mappings_);
    ranges_[spanCount()]={iova,uint64_t(slot),bytes,permissions,writeback,shareability};
    insert(1,0);record->mapped_spans=1;result_.completed_spans=memory->spanCount();
    if(needsSubmitMaintenance(memory,permissions,writeback))linkMaintained(*record);
    clock.lap(0,4);clock.done(0,bytes);
    {
        const unsigned kind=!memory->imported()?0:1;
        native_adreno_vm_kind_us[kind][0]=native_adreno_vm_kind_us[kind][0]+1;
        native_adreno_vm_kind_us[kind][1]=native_adreno_vm_kind_us[kind][1]+clock.elapsed();
        native_adreno_vm_kind_us[kind][2]=native_adreno_vm_kind_us[kind][2]+memory->spanCount();
    }
    {
        const unsigned p0b_kind=memory->imported() || memory->scanout()?2:memory->pooled()?0:1;
        NativeAdrenoP0bAdd(&native_adreno_p0b_map_pages[p0b_kind],bytes/4096);
        if(p0b_began)NativeAdrenoP0bTime(native_adreno_p0b_map_us[p0b_kind],NativeAdrenoP0bNow()-p0b_began);
    }
    ++pg_->stats.maps;
    if(bytes_>pg_->stats.mapped_peak)pg_->stats.mapped_peak=bytes_;
    record->handle=next_handle_++;*handle=record->handle;index_.insert(record->handle,uint32_t(slot));
    result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::mapPaged(NativeAdrenoMemory *memory,uint64_t iova,unsigned permissions,unsigned writeback,unsigned shareability,
    uint64_t *handle,bool client,size_t keep_objects,bool keep_reserve,NativeAdrenoVmRefusal *refusal)
{
    (void)refusal;
    size_t want=memory && memory->bytes()?ArmSmmu4KPagedWorstCase(&pg_->space,iova,memory->bytes()):0;
    for(unsigned attempt=0;;++attempt) {
        if(want+NativeAdrenoVmPagedSpare>__atomic_load_n(&pg_->free_pages,__ATOMIC_RELAXED))(void)growTables(want+NativeAdrenoVmPagedSpare);
        VM_LOCKED;
#ifdef ADRENO830_MACOS
        refusal_={};
#endif
        IOReturn status;
        if(keep_objects && objects_+1+keep_objects>NativeAdrenoVmMaxObjects) {
            native_adreno_vm_reserve_refused[0]=native_adreno_vm_reserve_refused[0]+1;pg_->short_pages=0;
            status=VM_REFUSE(ReserveObjects,kIOReturnNoResources,objects_,keep_objects,0,memory);
        } else status=mapPagedLocked(memory,iova,permissions,writeback,shareability,handle,client,keep_reserve);
        if(status==kIOReturnNoResources && pg_->short_pages && !attempt) {
            ++pg_->stats.grow_retries;want=pg_->short_pages;continue;   /* another map took the pages: grow outside the lock, once more */
        }
        if(status==kIOReturnNoResources && pg_->short_pages)++pg_->stats.refused_tables;
#ifdef ADRENO830_MACOS
        if(refusal) {
            *refusal=status?refusal_:NativeAdrenoVmRefusal{};
            refusal->objects=objects_;refusal->spans=spanCount();refusal->capacity=capacity_;refusal->bytes=bytes_;
            refusal->keep_objects=keep_objects;refusal->keep_spans=keep_reserve?pg_->config.reserve_bytes>>20:0;
        }
#endif
        return status;
    }
}
/* UniXNU-piano (GPU-PAGED P1b): the unmapped ranges are kept by 2 MiB block (the span of one level-3 table), neighbours merged; with
 * every slot taken the tick sweeps the whole space instead, in slices from a cursor (no whole-tree walk under the lock). */
void NativeAdrenoVm::noteTrim(uint64_t iova,uint64_t bytes)
{
    if(pg_->sweep)return;
    const uint64_t block=UINT64_C(2)<<20;
    const uint64_t lo=iova&~(block-1),hi=(iova+bytes+block-1)&~(block-1);
    for(size_t i=pg_->trim_count;i-- && i+4>=pg_->trim_count;) {   /* the newest few: unmaps of one free burst lie together */
        TrimRange &r=pg_->trim[i];
        if(lo<=r.iova+r.bytes && r.iova<=hi) {
            const uint64_t a=lo<r.iova?lo:r.iova,b=hi>r.iova+r.bytes?hi:r.iova+r.bytes;
            r={a,b-a};return;
        }
    }
    if(pg_->trim_count>=NativeAdrenoVmPagedTrimRanges) {pg_->sweep=true;pg_->sweep_cursor=0;pg_->trim_count=0;++pg_->stats.trim_full;return;}
    pg_->trim[pg_->trim_count++]={lo,hi-lo};
}
/* The unmap's table half: the leaves of the record's range cleared (no table unlinked, so the plan never depends on the walk caches:
 * the TLB rule of REPLAY.md), TLBIVAL of <= 512 pages or TLBIASID, one TLBSYNC; a range that left tables empty is noted for the tick. */
IOReturn NativeAdrenoVm::unmapPagedTables(const Mapping &record,size_t at)
{
    const uint64_t bytes=record.memory->bytes();
    const size_t slot=size_t(&record-mappings_);
    if(at>=spanCount() || ranges_[at].iova!=record.iova || ranges_[at].bytes!=bytes || ranges_[at].physical!=slot ||
        ranges_[at].permissions!=record.permissions || ranges_[at].writeback!=record.writeback || ranges_[at].shareability!=record.shareability)
        return fail(ArmSmmu4KStateMismatch);
    const ArmSmmu4KPagedAttrs expect={record.permissions,record.writeback,record.shareability};
    ArmSmmu4KPagedPlan plan;
    ArmSmmu4KStatus status=ArmSmmu4KPagedUnmap(&pg_->space,record.iova,bytes,&expect,nullptr,0,&plan);
    if(status)return fail(status);
    if(plan.walk || plan.retired)return fail(ArmSmmu4KStateMismatch);
    ArmSmmu4KProgramResult synced;
    if(plan.kind==ArmSmmu4KPagedTlbPages) {
        const ArmSmmu4KRange r={plan.iova,0,plan.pages*UINT64_C(4096),record.permissions,record.writeback,record.shareability};
        status=ArmSmmu4KSyncContextTlbPages(&io_,context_offset_,context_->asid,&r,1,budget_,&synced);
        ++pg_->stats.unmap_val;pg_->stats.unmap_val_pages+=plan.pages;
    } else if(plan.kind==ArmSmmu4KPagedTlbAsid) {
        status=ArmSmmu4KSyncContextTlb(&io_,context_offset_,context_->asid,0,budget_,&synced);
        ++pg_->stats.unmap_asid;
    }
    if(status)return fail(status);
    if(plan.kept_empty) {pg_->stats.kept_empty+=plan.kept_empty;noteTrim(record.iova,bytes);}
    ++pg_->stats.unmaps;
    return kIOReturnSuccess;
}
/* The tick: pending ranges, then sweep slices, each step a ArmSmmu4KPagedTrimRange of at most NativeAdrenoVmPagedTrimSlice bytes, until
 * NativeAdrenoVmPagedTrimBudgetUs passed (the publish shell's clock; without one NativeAdrenoVmPagedTrimSteps steps) or the retire batch
 * is full; the rest waits for the next tick. One TLBIASID + TLBSYNC for what was unlinked, then the pages go back. */
IOReturn NativeAdrenoVm::trimTables(size_t *out)
{
    if(out)*out=0;
    if(!paged_ || !attached_)return kIOReturnSuccess;
    VM_LOCKED;
    if(poisoned_)return kIOReturnNotReady;
    if(!pg_->sweep && !pg_->trim_count)return kIOReturnSuccess;
    const uint64_t began=pagedNow();
    size_t n=0,steps=0;
    ArmSmmu4KStatus status=ArmSmmu4KSuccess;
    const auto spent=[&] {
        if(n>=NativeAdrenoVmPagedTrimBatch)return true;
        if(began)return pagedNow()-began>=NativeAdrenoVmPagedTrimBudgetUs;
        return steps>=NativeAdrenoVmPagedTrimSteps;
    };
    /* one step over [iova, iova + bytes): false when it could not finish (the batch filled) */
    const auto step=[&](uint64_t iova,uint64_t bytes)->bool {
        ArmSmmu4KPagedPlan plan;
        status=ArmSmmu4KPagedTrimRange(&pg_->space,iova,bytes,pg_->retired+n,NativeAdrenoVmPagedTrimBatch-n,&plan);
        ++steps;
        if(status)return false;
        n+=plan.retired;
        return !plan.kept_empty;
    };
    while(!status && pg_->trim_count && !spent()) {
        TrimRange &r=pg_->trim[pg_->trim_count-1];   /* newest first: its tables are the warmest */
        const uint64_t bytes=r.bytes>NativeAdrenoVmPagedTrimSlice?NativeAdrenoVmPagedTrimSlice:r.bytes;
        if(!step(r.iova,bytes))break;
        r.iova+=bytes;r.bytes-=bytes;
        if(!r.bytes)--pg_->trim_count;
    }
    while(!status && pg_->sweep && !spent()) {
        const uint64_t end=pg_->config.aperture;
        const uint64_t bytes=end-pg_->sweep_cursor>NativeAdrenoVmPagedTrimSlice?NativeAdrenoVmPagedTrimSlice:end-pg_->sweep_cursor;
        if(!step(pg_->sweep_cursor,bytes))break;
        pg_->sweep_cursor+=bytes;
        if(pg_->sweep_cursor>=end) {pg_->sweep=false;pg_->sweep_cursor=0;}
    }
    if(status)return fail(status);   /* the unlinked pages stay owned */
    if(n) {
        /* TLBIVAL is leaf-only: an unlinked table needs the walk caches cleared, so one TLBIASID + TLBSYNC for the whole batch, and
         * only after it the pages go back to the free list. */
        ArmSmmu4KProgramResult synced;
        const ArmSmmu4KStatus sync=ArmSmmu4KSyncContextTlb(&io_,context_offset_,context_->asid,0,budget_,&synced);
        if(sync)return fail(sync);
        ArmSmmu4KPagedRelease(&pg_->space,pg_->retired,n);
        pg_->stats.trim_retired+=n;
    }
    ++pg_->stats.trims;
    if(began) {const uint64_t us=pagedNow()-began;if(us>pg_->stats.trim_us_max)pg_->stats.trim_us_max=us;}
    if(out)*out=n;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::auditTables(ArmSmmu4KPagedAuditResult *out)
{
    if(out)*out={};
    if(!paged_ || !attached_)return kIOReturnNotReady;
    const size_t room=__atomic_load_n(&objects_,__ATOMIC_RELAXED)+64;
    ArmSmmu4KPagedRecord *records=NativeAdrenoNewMeta(ArmSmmu4KPagedRecord,room);
    ArmSmmu4KPagedRuns *runs=NativeAdrenoNewMeta(ArmSmmu4KPagedRuns,room);
    PagedSpans *cursors=NativeAdrenoNewMeta(PagedSpans,room);
    IOReturn result=kIOReturnSuccess;
    if(!records || !runs || !cursors)result=kIOReturnNoMemory;
    else {
        VM_LOCKED;
        if(poisoned_)result=kIOReturnNotReady;
        else if(spanCount()>room)result=kIOReturnBusy;
        else {
            const uint64_t began=pagedNow();
            const size_t count=spanCount();
            for(size_t i=0;i<count;++i) {
                const Mapping &m=mappings_[ranges_[i].physical];
                cursors[i]={m.memory->spans(),m.memory->spanCount(),0};
                runs[i]={&cursors[i],pagedRun};
                records[i]={ranges_[i].iova,ranges_[i].bytes,{ranges_[i].permissions,ranges_[i].writeback,ranges_[i].shareability},&runs[i]};
            }
            ArmSmmu4KPagedAuditResult r;
            const ArmSmmu4KStatus status=ArmSmmu4KPagedAudit(&pg_->space,records,count,&r);
            ++pg_->stats.audits;
            pg_->stats.audit_why=r.why;pg_->stats.audit_iova=r.iova;pg_->stats.audit_tables=r.tables;pg_->stats.audit_leaves=r.leaves;
            pg_->stats.audit_us=began?pagedNow()-began:0;
            if(status) {++pg_->stats.audit_failures;result=kIOReturnIOError;}
            if(out)*out=r;
        }
    }
    if(records)NativeAdrenoDeleteMeta(records,ArmSmmu4KPagedRecord,room);
    if(runs)NativeAdrenoDeleteMeta(runs,ArmSmmu4KPagedRuns,room);
    if(cursors)NativeAdrenoDeleteMeta(cursors,PagedSpans,room);
    return result;
}
void NativeAdrenoVm::pagedStats(NativeAdrenoVmPagedStats *out) const
{
    if(!out)return;
    *out={};
    if(!paged_ || !lock_)return;
    VM_LOCKED;
    *out=pg_->stats;
    out->paged=true;out->poisoned=poisoned_;
    out->table_pages=pg_->space.counters.table_pages;out->table_peak=pg_->space.counters.table_pages_peak;
    out->table_cap=pg_->config.table_cap_pages;out->chunks=pg_->chunk_count;out->chunk_pages=pg_->chunk_pages;out->free_pages=pg_->free_pages;
    out->leaves=pg_->space.counters.leaves;out->trim_pending=pg_->sweep?UINT64_MAX:pg_->trim_count;
    out->mapped=bytes_;out->cap=pg_->config.cap_bytes;out->reserve=pg_->config.reserve_bytes;out->records=spanCount();
    out->grow_failures=__atomic_load_n(&pg_->stats.grow_failures,__ATOMIC_RELAXED);
}
uint64_t NativeAdrenoVm::pagedLeaf(uint64_t iova) const
{
    if(!paged_ || !lock_)return 0;
    VM_LOCKED;
    uint64_t descriptor=0;
    return ArmSmmu4KPagedLookup(&pg_->space,iova,&descriptor)?0:descriptor;
}
IOReturn NativeAdrenoVm::releasePaged()
{
    if(!paged_ || attached_ || objects_)return kIOReturnBusy;
    if(pg_->space.prepared)(void)ArmSmmu4KPagedTeardown(&pg_->space);
    if(ranges_) {   /* prepared but never attached (detach frees the records of an attached VM) */
        NativeAdrenoDeleteMeta(ranges_,ArmSmmu4KRange,NativeAdrenoVmMaxObjects);NativeAdrenoDeleteMeta(mappings_,Mapping,NativeAdrenoVmMaxObjects);
        index_.release();ranges_=nullptr;mappings_=nullptr;capacity_=0;
    }
    if(lock_) {lockFree(lock_);lock_=nullptr;}
    for(size_t i=0;i<pg_->chunk_count;++i)releaseChunk(pg_->chunks[i]);
    delete pg_;pg_=nullptr;paged_=false;
    if(grow_lock_) {growLockFree(grow_lock_);grow_lock_=nullptr;}
    return kIOReturnSuccess;
}
