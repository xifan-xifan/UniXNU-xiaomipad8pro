/* SPDX-License-Identifier: MIT */
/* Deliberately small authored host doubles, not original XNU headers or ABI. */
#ifndef NATIVE_ADRENO_XNU_MEMORY_FIXTURE_H
#define NATIVE_ADRENO_XNU_MEMORY_FIXTURE_H
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
using IOReturn=int;
using IOOptionBits=unsigned;
using IOByteCount=size_t;
using addr64_t=uint64_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnBusy=2,
 kIOReturnNoResources=3,kIOReturnNoMemory=4,kIOReturnUnsupported=5,
 kIOReturnNotReady=6,kIOReturnIOError=7,kIOReturnTimeout=8;
using IODirection=unsigned;
constexpr unsigned kIODirectionOut=2,kIODirectionInOut=3,kIOMemoryMapperNone=0x800,kIOMemoryPhysicallyContiguous=0x10,kIOMemoryPersistent=0x10000;
constexpr unsigned kIOMemoryPageable=0x100000,kIOMapWriteCombineCache=0x200000;
inline void *kernel_task=reinterpret_cast<void *>(1);
using task_t=void *;
/* ScatterUnlessContiguous: what the tablet does, one physical page per span unless the caller asked for contiguous memory. */
enum SegmentMode {Contiguous,Scattered,ZeroPhysical,UnalignedPhysical,ShortSegment,HighPhysical,Paired,ScatterUnlessContiguous};
class IOBufferMemoryDescriptor;
struct MemoryFixture {
    unsigned created=0,prepared=0,completed=0,released=0,cache_calls=0,segment_calls=0;
    unsigned allocation_fail_count=0;
    bool allocation_fail=false,prepare_fail=false,complete_fail=false,metadata_fail=false;
    bool write_protected_imports=false;   /* user ranges that can only be pinned for reading: prepare() of a writable one reports KERN_PROTECTION_FAILURE */
    bool null_mapping=false,unaligned_mapping=false,wrong_length=false;
    unsigned cache_fail_call=0;
    uint64_t next_physical=0x30000000;
    size_t contiguous_limit=0;   /* nonzero: physically contiguous requests above this many bytes fail (a fragmented tablet) */
    unsigned imported=0,imports_live=0;
    unsigned physical_calls=0;size_t physical_bytes=0;   /* NativeAdrenoMaintainPhysicalPages doubles */
    void (*zero_physical)(uint64_t,size_t)=nullptr;
    void (*allocation_hook)(unsigned options,size_t bytes)=nullptr;   /* sees every IOBufferMemoryDescriptor request, refused ones too */
    /* UniXNU-piano (GPU-PAGED P1b): prepare() of a pageable object spends this long per 4 KiB page (the tablet's wiring is 0.5-3 us per
     * page); prepare call number prepare_fail_at (1-based, all objects) fails. prepare() may run on several threads at once. */
    unsigned prepare_ns_per_page=0,prepare_fail_at=0,prepare_calls=0;
    unsigned multi_created=0,multi_live=0;
    IOReturn cpu_status=kIOReturnSuccess;
    SegmentMode mode=Contiguous;
    std::vector<IOBufferMemoryDescriptor *> live;
    std::vector<void *> metadata;
    std::vector<unsigned> cache_operations;
};
inline MemoryFixture fixture;
/* UniXNU-piano (GPU-PAGED P1b): the doubles' shared lists under one lock (BOs are allocated and released on several threads). */
inline std::recursive_mutex &fixtureLock() {static std::recursive_mutex m;return m;}
inline void *fixtureMetadata(size_t bytes) {
    std::lock_guard<std::recursive_mutex> guard(fixtureLock());
    if(fixture.metadata_fail)return nullptr;
    void *p=std::calloc(1,bytes);if(p)fixture.metadata.push_back(p);return p;
}
inline void fixtureMetadataFree(void *p) {
    std::lock_guard<std::recursive_mutex> guard(fixtureLock());
    auto at=std::find(fixture.metadata.begin(),fixture.metadata.end(),p);
    if(at==fixture.metadata.end())std::abort();fixture.metadata.erase(at);std::free(p);
}
#define IONewData(type,count) static_cast<type *>(fixtureMetadata(sizeof(type)*(count)))
#define IODeleteData(ptr,type,count) do {fixtureMetadataFree(ptr);ptr=nullptr;}while(0)
#ifndef IONew
#define IONew(type,count) static_cast<type *>(fixtureMetadata(sizeof(type)*(count)))
#define IODelete(ptr,type,count) do {fixtureMetadataFree(ptr);ptr=nullptr;}while(0)
#endif
class IOBufferMemoryDescriptor {
public:
    void *raw=nullptr;
    size_t bytes=0;
    uint64_t mask=0;
    uint64_t physical_base=0;
    bool contiguous=false,vm_object=false,write_combined=false;
    unsigned references=1;
    void retain() {std::lock_guard<std::recursive_mutex> guard(fixtureLock());++references;}
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(void *task,unsigned options,size_t bytes,uint64_t mask) {
        if(task!=kernel_task || !(options&kIOMemoryMapperNone))std::abort();
        std::lock_guard<std::recursive_mutex> guard(fixtureLock());
        if(fixture.allocation_hook)fixture.allocation_hook(options,bytes);
        if(fixture.allocation_fail)return nullptr;
        if(fixture.allocation_fail_count) {--fixture.allocation_fail_count;return nullptr;}
        if(fixture.contiguous_limit && (options&kIOMemoryPhysicallyContiguous) && bytes>fixture.contiguous_limit)return nullptr;
        auto *d=new IOBufferMemoryDescriptor;
        d->bytes=bytes;d->mask=mask;d->physical_base=fixture.next_physical;d->contiguous=options&kIOMemoryPhysicallyContiguous;
        fixture.next_physical+=2*bytes+0x10000;
        if(posix_memalign(&d->raw,4096,bytes+4096))std::abort();
        std::memset(d->raw,0xa5,bytes+4096);++fixture.created;fixture.live.push_back(d);return d;
    }
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *task,unsigned options,size_t bytes,size_t alignment) {
        if(task || !(options&kIOMemoryPageable) || alignment!=4096)std::abort();
        auto *d=inTaskWithPhysicalMask(kernel_task,options,bytes,UINT64_C(0xFFFFFFFFFFFFF000));
        if(d) {d->vm_object=true;d->write_combined=options&kIOMapWriteCombineCache;std::memset(d->raw,0,bytes);}
        return d;
    }
    static std::recursive_mutex &lock() {return fixtureLock();}
    IOReturn prepare() {
        bool fail;
        {
            std::lock_guard<std::recursive_mutex> guard(lock());
            ++fixture.prepared;fail=fixture.prepare_fail || (fixture.prepare_fail_at && ++fixture.prepare_calls==fixture.prepare_fail_at);
        }
        if(fixture.prepare_ns_per_page && vm_object) {   /* the wiring's time, busy (it is CPU work on the tablet) */
            const auto until=std::chrono::steady_clock::now()+std::chrono::nanoseconds(uint64_t(fixture.prepare_ns_per_page)*(bytes/4096));
            while(std::chrono::steady_clock::now()<until) {}
        }
        return fail?kIOReturnIOError:kIOReturnSuccess;
    }
    IOReturn complete() {std::lock_guard<std::recursive_mutex> guard(lock());++fixture.completed;return fixture.complete_fail?kIOReturnIOError:kIOReturnSuccess;}
    void release() {
        std::lock_guard<std::recursive_mutex> guard(lock());
        if(--references)return;
        ++fixture.released;auto at=std::find(fixture.live.begin(),fixture.live.end(),this);
        if(at==fixture.live.end())std::abort();fixture.live.erase(at);std::free(raw);delete this;
    }
    void *getBytesNoCopy() {return vm_object || fixture.null_mapping?nullptr:static_cast<char *>(raw)+(fixture.unaligned_mapping?8:0);}
    size_t getLength() {return bytes+(fixture.wrong_length?4096:0);}
    uint64_t getPhysicalSegment(size_t offset,size_t *length,unsigned options) {
        if((offset&4095) || offset>=bytes || options!=kIOMemoryMapperNone)std::abort();
        {std::lock_guard<std::recursive_mutex> guard(fixtureLock());++fixture.segment_calls;}
        *length=bytes-offset;
        uint64_t base=physical_base,physical=base+offset;
        if(fixture.mode==Scattered || (fixture.mode==ScatterUnlessContiguous && !contiguous)) {
            physical=base+2*offset;
            if(fixture.mode==ScatterUnlessContiguous)*length=4096;
        }
        if(fixture.mode==Paired)physical=base+offset+((offset/8192)*4096);
        if(offset==4096) {
            if(fixture.mode==ZeroPhysical)physical=0;
            if(fixture.mode==UnalignedPhysical)++physical;
            if(fixture.mode==ShortSegment)*length=4095;
            if(fixture.mode==HighPhysical)physical=(mask|4095)+1;
        }
        return physical;
    }
};
/* UniXNU-piano (GPU-PAGED P1b): the CPU view of a BO made of several named entries: it holds a reference on each part, its length is
 * theirs end to end. */
class IOMultiMemoryDescriptor {
public:
    std::vector<IOBufferMemoryDescriptor *> parts;
    unsigned references=1;
    static IOMultiMemoryDescriptor *withBuffers(IOBufferMemoryDescriptor **list,unsigned count) {
        std::lock_guard<std::recursive_mutex> guard(fixtureLock());
        if(fixture.allocation_fail || !list || !count)return nullptr;
        auto *m=new IOMultiMemoryDescriptor;
        for(unsigned i=0;i<count;++i) {list[i]->retain();m->parts.push_back(list[i]);}
        ++fixture.multi_created;++fixture.multi_live;return m;
    }
    void retain() {++references;}
    size_t getLength() const {size_t n=0;for(auto *d:parts)n+=d->bytes;return n;}
    void release() {
        std::lock_guard<std::recursive_mutex> guard(fixtureLock());
        if(--references)return;
        for(auto *d:parts)d->release();
        --fixture.multi_live;delete this;
    }
};
/* A user range pinned with withAddressRange/prepare: pages at a fixed physical stride (every other page), so imports are scattered. */
class IOMemoryDescriptor {
public:
    uint64_t address=0,bytes=0,physical_base=0;unsigned options=0;
    static IOMemoryDescriptor *withAddressRange(uint64_t address,uint64_t bytes,unsigned options,task_t task) {
        if(!task || task==kernel_task || !(options&kIOMemoryMapperNone))std::abort();
        if(fixture.allocation_fail)return nullptr;
        auto *d=new IOMemoryDescriptor;d->options=options;d->address=address;d->bytes=bytes;d->physical_base=fixture.next_physical;
        fixture.next_physical+=2*bytes+0x10000;++fixture.imported;++fixture.imports_live;return d;
    }
    IOReturn prepare() {++fixture.prepared;if(fixture.write_protected_imports && (options&1))return 2;return fixture.prepare_fail?kIOReturnIOError:kIOReturnSuccess;}
    IOReturn complete() {++fixture.completed;return fixture.complete_fail?kIOReturnIOError:kIOReturnSuccess;}
    void release() {--fixture.imports_live;delete this;}
    uint64_t getLength() {return bytes;}
    uint64_t getPhysicalSegment(size_t offset,size_t *length,unsigned options) {
        if((offset&4095) || offset>=bytes || options!=kIOMemoryMapperNone)std::abort();
        ++fixture.segment_calls;*length=4096;return physical_base+2*offset;
    }
};
#endif
