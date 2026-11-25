/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MAP_LINES_H
#define NATIVE_ADRENO_MAP_LINES_H
/* UniXNU-piano (macOS 27, compiled with ADRENO830_MACOS, 2026-11-12): the console's GPU address-space lines, as pure functions the host
 * tests check (tests/test_vamap.cpp). The publish shell (NativeAdrenoPublish.cpp) prints them through boot_progress_gpu_event, whose
 * ring keeps 159 characters with its "gpu ": every line here stays under NativeAdrenoMapLineBytes.
 *  - NativeAdrenoMapRefusalLine: after a refused BoMap's "call failed" line, why (NativeAdrenoUserClient::lastMapRefusal): which
 *    argument or which limit, with its figures. The first macOS desktop (run attend4) said only "BoMap -> NoResources" and
 *    "BoMap -> BadArgument".
 *  - NativeAdrenoAllocRefusalLine (2026-11-12): after a refused BoAlloc's or BoImport's "call failed" line, why
 *    (NativeAdrenoUserClient::lastAllocRefusal): the per-object limit and the window behind it, the pool's room and XNU's answer.
 *  - NativeAdrenoVaTotalsLine / NativeAdrenoVaClientEntry: the shared VM's records and table ranges, the BO pool and what lies outside
 *    it, then per connection its mappings, ranges, mapped MiB against its window and the highest end it reached, so a run shows the
 *    trend before a limit is hit. */
#ifdef ADRENO830_MACOS
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoPublish.h"   /* NativeAdrenoReturnName */
#ifdef ADRENO830_HOST_TEST
#include <stdio.h>
#else
#include <libkern/libkern.h>   /* snprintf */
#endif
#include <string.h>

enum : unsigned {NativeAdrenoMapLineBytes=150};

struct NativeAdrenoResidencyRecord {
    char name[20]{};
    unsigned connection=0;
    int pid=0;
    NativeAdrenoResidencyUsage usage{};

    int line(char *text,size_t size,unsigned slot,const char *state,uint64_t window,uint64_t hole) const
    {
        if(!connection)return 0;
        using U64=unsigned long long;
        return snprintf(text,size,
            "residency slot=%u connection=%u pid=%d name=%s state=%s window=%llu resident=%llu peak-resident=%llu "
            "imports=%llu peak-imports=%llu import-bytes=%llu peak-import-bytes=%llu largest-vm-hole=%llu",
            slot,connection,pid,name,state,U64(window),U64(usage.resident_bytes),U64(usage.peak_resident_bytes),
            U64(usage.imports),U64(usage.peak_imports),U64(usage.import_bytes),U64(usage.peak_import_bytes),U64(hole));
    }
};

/* Captured by the wrapper under the connection lock. VM fields came from mapClient's locked output;
 * global backing counters are independent relaxed samples, not a whole-device atomic census. */
struct NativeAdrenoFailureSnapshot {
    bool captured=false;uint64_t sequence=0;
    NativeAdrenoAllocRefusal alloc{};NativeAdrenoMapRefusal map{};NativeAdrenoMapUsage usage{};
    uint64_t objects=0,capacity=0,pool=0,other=0,imports=0;
    NativeAdrenoBackingTotals backing{};
};
static inline void NativeAdrenoCaptureFailure(NativeAdrenoFailureSnapshot *out,const NativeAdrenoUserClient &core,uint64_t sequence)
{
    if(!out)return;
    *out={};out->captured=true;out->sequence=sequence;
    out->alloc=core.lastAllocRefusal();out->map=core.lastMapRefusal();core.mapUsage(&out->usage);
    out->objects=core.liveObjects();out->capacity=core.capacity();core.usage(&out->pool,&out->other,&out->imports);
    out->backing=NativeAdrenoMemory::backingTotals();
}
/* Each row carries its own sequence: different threads can interleave ring writes. No user addresses or PA are printed here. */
enum : unsigned {NativeAdrenoFailureLines=10};
static inline int NativeAdrenoFailureLine(char *line,size_t size,unsigned row,const NativeAdrenoFailureSnapshot &s,
    unsigned slot,int pid,uint32_t method,IOReturn status)
{
    if(!line || !size)return 0;
    typedef unsigned long long u64;
    const u64 n=s.sequence;
    switch(row) {
    case 0:return snprintf(line,size,"refusal n=%llu pid=%d slot=%u method=%u status=%#x",n,pid,slot,method,unsigned(status));
    case 1:
        if(method==NativeAdrenoMethodBoMap)
            return snprintf(line,size,"refusal n=%llu map-why=%u vm-why=%u own=%u map-b=%llu map-c=%llu",n,s.map.why,s.map.vm.why,
                unsigned(s.map.vm_captured),u64(s.map.b),u64(s.map.c));
        return snprintf(line,size,"refusal n=%llu alloc-why=%u stage=%s alloc-a=%llu driver-status=%#x",n,s.alloc.why,
            s.alloc.failure.stage,u64(s.alloc.a),unsigned(s.alloc.failure.status));
    case 2:
        if(method==NativeAdrenoMethodBoMap)
            return snprintf(line,size,"refusal n=%llu vm-a=%llu vm-b=%llu vm-c=%llu",n,u64(s.map.vm.a),u64(s.map.vm.b),u64(s.map.vm.c));
        if(method==NativeAdrenoMethodBoImport)
            return snprintf(line,size,"refusal n=%llu import-stage=%llu",n,u64(s.alloc.c));
        return snprintf(line,size,"refusal n=%llu alloc-c=%llu alloc-d=%llu alloc-e=%llu",n,u64(s.alloc.c),u64(s.alloc.d),u64(s.alloc.e));
    case 3:return snprintf(line,size,"refusal n=%llu objects=%llu/%llu pool=%llu own=%llu",n,u64(s.objects),u64(s.capacity),u64(s.pool),u64(s.other));
    case 4:return snprintf(line,size,"refusal n=%llu imports=%llu map-objects=%llu map-spans=%llu",n,u64(s.imports),u64(s.usage.objects),u64(s.usage.spans));
    case 5:return snprintf(line,size,"refusal n=%llu mapped=%llu window-bytes=%llu",n,u64(s.usage.bytes),u64(s.usage.window_bytes));
    case 6:
        if(method!=NativeAdrenoMethodBoMap || s.map.why!=NativeAdrenoMapWhyVm)
            return snprintf(line,size,"refusal n=%llu vm=not-called",n);
        return snprintf(line,size,"refusal n=%llu vm-objects=%llu/%u vm-spans=%llu/%llu",n,u64(s.map.vm.objects),unsigned(NativeAdrenoVmMaxObjects),
        u64(s.map.vm.spans),u64(s.map.vm.capacity));
    case 7:
        if(method!=NativeAdrenoMethodBoMap || s.map.why!=NativeAdrenoMapWhyVm)
            return snprintf(line,size,"refusal n=%llu vm-reserve=not-called",n);
        return snprintf(line,size,"refusal n=%llu vm-bytes=%llu keep-objects=%llu keep-spans=%llu",n,u64(s.map.vm.bytes),
        u64(s.map.vm.keep_objects),u64(s.map.vm.keep_spans));
    case 8:return snprintf(line,size,"refusal n=%llu sample-owned-mapped=%llu sample-unmapped=%llu",n,u64(s.backing.mapped),u64(s.backing.unmapped));
    case 9:return snprintf(line,size,"refusal n=%llu sample-imports=%llu sample-allocs=%llu sample-frees=%llu",n,u64(s.backing.imports),
        u64(s.backing.allocs),u64(s.backing.frees));
    default:line[0]=0;return 0;
    }
}

/* Single publication only: a reader observing state=2 sees immutable bytes, even after a connection closes or its slot is reused.
 * Publish under the connection lock. Never wait for another writer, allocate, call the VM, or run a callback here. */
struct NativeAdrenoFailureRecord {
    uint32_t state=0;unsigned slot=0;int pid=0;uint32_t method=0;IOReturn status=0;NativeAdrenoFailureSnapshot snapshot{};
    bool remember(const NativeAdrenoFailureSnapshot &s,unsigned owner,int process,uint32_t op,IOReturn result)
    {
        if(!s.captured || (result!=kIOReturnNoMemory && result!=kIOReturnNoResources))return false;
        uint32_t empty=0;
        if(!__atomic_compare_exchange_n(&state,&empty,1u,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return false;
        snapshot=s;slot=owner;pid=process;method=op;status=result;
        __atomic_store_n(&state,2u,__ATOMIC_RELEASE);return true;
    }
    int line(char *text,size_t size,unsigned row) const
    {
        if(__atomic_load_n(&state,__ATOMIC_ACQUIRE)!=2u) {if(text && size)text[0]=0;return 0;}
        return NativeAdrenoFailureLine(text,size,row,snapshot,slot,pid,method,status);
    }
};
static_assert(unsigned(NativeAdrenoFailureLines)==unsigned(NativeAdrenoFailureRows),"refusal export rows");

static inline const char *NativeAdrenoVmWhyText(uint8_t why)
{
    switch(why) {
    case NativeAdrenoVmWhyRequest: return "the attributes or an unusable object";
    case NativeAdrenoVmWhyAperture: return "outside the GPU aperture";
    case NativeAdrenoVmWhyReserved: return "a reserved window (null guard, GMEM)";
    case NativeAdrenoVmWhyReadOnly: return "writable read-only memory or a write-back scanout";
    case NativeAdrenoVmWhyObjects: return "mapping records full";
    case NativeAdrenoVmWhySpans: return "table ranges full";
    case NativeAdrenoVmWhyOverlap: return "overlaps a mapping";
    case NativeAdrenoVmWhyPages: return "the object's pages";
    case NativeAdrenoVmWhyAlias: return "a page mapped with another cache class";
    case NativeAdrenoVmWhyReserveObjects: return "the render server's reserve of records";
    case NativeAdrenoVmWhyReserveSpans: return "the render server's reserve of ranges";
    case NativeAdrenoVmWhyHold: return "the object refused a device reference";
    case NativeAdrenoVmWhyNotReady: return "the VM is not attached or poisoned";
    default: return "no reason recorded";
    }
}

/* The detail of a refused BoMap (line gets at most NativeAdrenoMapLineBytes, NUL included). */
static inline void NativeAdrenoMapRefusalLine(char *line,size_t size,const NativeAdrenoMapRefusal &r)
{
    if(!line || !size)return;
    typedef unsigned long long u64;
    const u64 a=r.a,b=r.b,c=r.c,d=r.d;
    switch(r.why) {
    case NativeAdrenoMapWhyNotOpen: snprintf(line,size,"BoMap refused: the connection is not open");break;
    case NativeAdrenoMapWhyRequest:
        snprintf(line,size,"BoMap refused: the request: permissions %#llx writeback %llu shareability %llu, reserved/iova low bits %#llx",
                 a,b,c,d);
        break;
    case NativeAdrenoMapWhyHandle:
        snprintf(line,size,"BoMap refused: handle %#llx %s",a,b?"is not ready (being made, or its release failed)":"is no live object");
        break;
    case NativeAdrenoMapWhyMapped: snprintf(line,size,"BoMap refused: handle %#llx is mapped already, at %#llx",a,b);break;
    case NativeAdrenoMapWhyWindow:
        snprintf(line,size,"BoMap refused: iova %#llx + %llu KiB ends past this connection's window %#llx-%#llx",a,b>>10,c,d);
        break;
    case NativeAdrenoMapWhyScanout: snprintf(line,size,"BoMap refused: the scanout asked without write (permissions %#llx)",a);break;
    case NativeAdrenoMapWhyVm: {
        const u64 x=r.vm.a,y=r.vm.b,z=r.vm.c;
        char why[96];
        switch(r.vm.why) {
        case NativeAdrenoVmWhyObjects: snprintf(why,sizeof why,"mapping records full, %llu of %llu",x,y);break;
        case NativeAdrenoVmWhySpans: snprintf(why,sizeof why,"table ranges full, %llu needed, %llu free of %llu",x,y,z);break;
        case NativeAdrenoVmWhyReserveObjects: snprintf(why,sizeof why,"the render server's reserve, %llu records used, %llu kept",x,y);break;
        case NativeAdrenoVmWhyReserveSpans:
            snprintf(why,sizeof why,"the render server's reserve, %llu ranges needed, %llu free, %llu kept",x,y,z);break;
        case NativeAdrenoVmWhyOverlap: snprintf(why,sizeof why,"overlaps the mapping at %#llx",y);break;
        case NativeAdrenoVmWhyAperture: snprintf(why,sizeof why,"outside the GPU aperture (it ends at %#llx)",z);break;
        case NativeAdrenoVmWhyPages: snprintf(why,sizeof why,"the object's pages (span %llu, %#llx + %llu KiB)",x,y,z>>10);break;
        case NativeAdrenoVmWhyAlias: snprintf(why,sizeof why,"physical %#llx mapped with another cache class",x);break;
        case NativeAdrenoVmWhyHold: snprintf(why,sizeof why,"the object refused a device reference (%#llx)",x);break;
        default: snprintf(why,sizeof why,"%s",NativeAdrenoVmWhyText(r.vm.why));break;
        }
        snprintf(line,size,"BoMap refused by the VM: %s%s (iova %#llx, %llu KiB in %llu ranges)",why,
                 r.vm_captured?"":", another call's",a,b>>10,c);
        break;
    }
    default: snprintf(line,size,"BoMap refused: no reason recorded");break;
    }
}

/* The detail of a refused BoAlloc or BoImport (2026-11-12; NativeAdrenoUserClient::lastAllocRefusal): the per-object limit with the
 * window behind it, the pool's room and XNU's answer, the object slots, the import's stage. Sizes in MiB, rounded up. */
static inline void NativeAdrenoAllocRefusalLine(char *line,size_t size,const NativeAdrenoAllocRefusal &r)
{
    if(!line || !size)return;
    typedef unsigned long long u64;
    const char *what=r.method==NativeAdrenoMethodBoImport?"BoImport":"BoAlloc";
    const auto mib=[](uint64_t bytes) {return u64((bytes+(UINT64_C(1)<<20)-1)>>20);};
    switch(r.why) {
    case NativeAdrenoAllocWhyNotOpen: snprintf(line,size,"%s refused: the connection is not open",what);break;
    case NativeAdrenoAllocWhyRequest:
        if(r.method==NativeAdrenoMethodBoImport)
            snprintf(line,size,"%s refused: the request: %llu bytes at %#llx (both whole 4 KiB pages, nonzero)",what,u64(r.a),u64(r.b));
        else snprintf(line,size,"%s refused: the request: %llu bytes (whole 4 KiB pages), alignment 2^%llu (12..24), contiguous %llu",what,
                      u64(r.a),u64(r.b),u64(r.c));
        break;
    case NativeAdrenoAllocWhyLimit:
        if(r.c)snprintf(line,size,"%s refused: %llu MiB is past this connection's per-object limit of %llu MiB (its GPU window)",what,
                        mib(r.a),u64(r.b>>20));
        else snprintf(line,size,"%s refused: %llu MiB is past the per-object limit of %llu MiB",what,mib(r.a),u64(r.b>>20));
        break;
    case NativeAdrenoAllocWhyObjects: snprintf(line,size,"%s refused: no object slot free, %llu live of %llu",what,u64(r.a),u64(r.b));break;
    case NativeAdrenoAllocWhyMemory: {
        const char *code=NativeAdrenoReturnName(IOReturn(r.b));
        char kept[40]="";
        if(r.e)snprintf(kept,sizeof kept,", %llu kept for the render server",mib(r.e));
        snprintf(line,size,"%s refused: %llu MiB: pool %llu MiB free, largest %llu%s; then no pages from XNU (%#llx%s%s)",what,mib(r.a),
                 u64(r.c>>20),u64(r.d>>20),kept,u64(r.b),code?" ":"",code?code:"");
        break;
    }
    case NativeAdrenoAllocWhyWriteCombine:
        snprintf(line,size,"%s refused: %llu MiB: its pages' write-combined attribute was refused (%#llx)",what,mib(r.a),u64(r.b));
        break;
    case NativeAdrenoAllocWhyImport: {
        const char *code=NativeAdrenoReturnName(IOReturn(r.b));
        snprintf(line,size,"%s refused: %llu MiB at %#llx: the task's pages were not pinned (%#llx%s%s, stage %llu)",what,mib(r.a),u64(r.d),
                 u64(r.b),code?" ":"",code?code:"",u64(r.c));
        break;
    }
    default: snprintf(line,size,"%s refused: no reason recorded",what);break;
    }
}

/* The detail of a refused Submit (2026-11-21, 20261121-ws-crash: "WindowServer[102] slot 0 Submit -> BadArgument" named no check;
 * NativeAdrenoUserClient::lastSubmitRefusal): the check, and for an entry "entry <i>/<n> handle <BO handle> iova <iova> bytes <bytes>
 * (mapped <iova>+<bytes> | not mapped)" with what that check adds. The boot_progress_gpu_event prefix makes it "gpu submit refused: ...". */
static inline const char *NativeAdrenoSubmitWhyText(uint8_t why)
{
    switch(why) {
    case NativeAdrenoSubmitWhyNotOpen: return "not open";
    case NativeAdrenoSubmitWhyCount: return "count";
    case NativeAdrenoSubmitWhyQueue: return "queue";
    case NativeAdrenoSubmitWhyWaitFence: return "wait fence";
    case NativeAdrenoSubmitWhyBytes: return "bytes";
    case NativeAdrenoSubmitWhyHandle: return "handle unknown";
    case NativeAdrenoSubmitWhyNotMapped: return "not mapped";
    case NativeAdrenoSubmitWhyRange: return "range";
    case NativeAdrenoSubmitWhyQueueFull: return "queue full";
    case NativeAdrenoSubmitWhyOther: return "other";
    default: return "no reason recorded";
    }
}
static inline int NativeAdrenoSubmitRefusalLine(char *line,size_t size,const NativeAdrenoSubmitRefusal &r)
{
    if(!line || !size)return 0;
    typedef unsigned long long u64;
    const char *what=NativeAdrenoSubmitWhyText(r.why),*code=NativeAdrenoReturnName(r.status);
    char status[40];
    snprintf(status,sizeof status,"%#x%s%s",unsigned(r.status),code?" ":"",code?code:"");
    if(r.index==UINT32_MAX) {
        switch(r.why) {
        case NativeAdrenoSubmitWhyCount:
            return snprintf(line,size,"submit refused: count: %u buffers (1..%u), reserved %#llx",r.count,unsigned(NativeAdrenoAbiMaxBuffers),
                            u64(r.a));
        case NativeAdrenoSubmitWhyQueue:
            return snprintf(line,size,"submit refused: queue: %llu is not an open queue of this connection (%u buffers)",u64(r.a),r.count);
        case NativeAdrenoSubmitWhyWaitFence:
            return snprintf(line,size,"submit refused: wait fence: %llu was never issued (%s, %u buffers)",u64(r.a),status,r.count);
        default:
            return snprintf(line,size,"submit refused: %s: %s (%u buffers)",what,status,r.count);
        }
    }
    char mapping[48],tail[56]="";
    if(r.why==NativeAdrenoSubmitWhyRange || (r.mapped && r.mapped_bytes))
        snprintf(mapping,sizeof mapping,"mapped %#llx+%#llx",u64(r.mapped),u64(r.mapped_bytes));
    else snprintf(mapping,sizeof mapping,"%s",r.why==NativeAdrenoSubmitWhyBytes?"mapping not looked up":"not mapped");
    switch(r.why) {
    case NativeAdrenoSubmitWhyHandle: snprintf(tail,sizeof tail,", no live object, next handle %#llx",u64(r.a));break;
    case NativeAdrenoSubmitWhyNotMapped:
        if(r.b)snprintf(tail,sizeof tail,", VM mapping %#llx: %s",u64(r.b),status);
        else snprintf(tail,sizeof tail,", object of %llu KiB",u64(r.mapped_bytes>>10));
        break;
    case NativeAdrenoSubmitWhyRange: snprintf(tail,sizeof tail,", permissions %#llx",u64(r.a));break;
    case NativeAdrenoSubmitWhyBytes: break;
    default: snprintf(tail,sizeof tail,", %s",status);break;
    }
    return snprintf(line,size,"submit refused: %s: entry %u/%u handle %#llx iova %#llx bytes %llu (%s)%s",what,r.index,r.count,u64(r.handle),
                    u64(r.iova),u64(r.bytes),mapping,tail);
}

/* The VM's totals: "va/map: VM <records>/<max> records, <ranges>/<capacity> ranges (<permille/10>%), <MiB> MiB mapped; pool <used>/<size>
 * MiB, <MiB> MiB outside it". */
static inline void NativeAdrenoVaTotalsLine(char *line,size_t size,uint64_t objects,uint64_t max_objects,uint64_t spans,uint64_t capacity,
    uint64_t mapped_mib,uint32_t pool_used_mib,uint32_t pool_mib,uint32_t outside_mib)
{
    if(!line || !size)return;
    const uint64_t permille=capacity?spans*1000/capacity:0;
    snprintf(line,size,"va/map: VM %llu/%llu records, %llu/%llu ranges (%llu.%llu%%), %llu MiB mapped; pool %u/%u MiB, %u MiB outside it",
             (unsigned long long)objects,(unsigned long long)max_objects,(unsigned long long)spans,(unsigned long long)capacity,
             (unsigned long long)(permille/10),(unsigned long long)(permille%10),(unsigned long long)mapped_mib,pool_used_mib,pool_mib,
             outside_mib);
}

/* One connection: " <slot>:<name> <mapped MiB>/<window MiB>M pk<MiB of the window's highest end> <records>o <ranges>r", ranges in
 * thousands ("k") from 10000. The name is cut to 14 characters. */
static inline int NativeAdrenoVaClientEntry(char *entry,size_t size,unsigned slot,const char *name,const NativeAdrenoMapUsage &u)
{
    if(!entry || !size)return 0;
    char cut[15]={};
    if(name)strlcpy(cut,name,sizeof cut);
    const unsigned long long mib=(u.bytes+(1u<<20)-1)>>20,window=u.window_bytes>>20,
                             peak=u.window_peak>u.window_start?(u.window_peak-u.window_start+(1u<<20)-1)>>20:0;
    if(u.spans>=10000)
        return snprintf(entry,size," %u:%s %llu/%lluM pk%llu %lluo %llukr",slot,cut,mib,window,peak,(unsigned long long)u.objects,
                        (unsigned long long)(u.spans/1000));
    return snprintf(entry,size," %u:%s %llu/%lluM pk%llu %lluo %llur",slot,cut,mib,window,peak,(unsigned long long)u.objects,
                    (unsigned long long)u.spans);
}

/* 2026-11-12 (desk-dev: WindowServer's 1.2 GiB of own BOs, nothing said what they were): one connection's live objects by size,
 * "va/sizes: <slot>:<name> <1M <n>/<MiB>M 1-16M <n>/<MiB>M 16-64M <n>/<MiB>M 64M+ <n>/<MiB>M imp <n>/<MiB>M" (MiB rounded up): the
 * runtime's blocks (16-64 MiB), large textures and conversions (64 MiB and more) and imports told apart. */
static inline int NativeAdrenoVaSizesLine(char *line,size_t size,unsigned slot,const char *name,const NativeAdrenoSizeUsage &u)
{
    if(!line || !size)return 0;
    char cut[15]={};
    if(name)strlcpy(cut,name,sizeof cut);
    typedef unsigned long long u64;
    const auto m=[](uint64_t bytes) {return u64((bytes+(UINT64_C(1)<<20)-1)>>20);};
    return snprintf(line,size,"va/sizes: %u:%s <1M %llu/%lluM 1-16M %llu/%lluM 16-64M %llu/%lluM 64M+ %llu/%lluM imp %llu/%lluM",slot,cut,
                    u64(u.count[0]),m(u.bytes[0]),u64(u.count[1]),m(u.bytes[1]),u64(u.count[2]),m(u.bytes[2]),u64(u.count[3]),
                    m(u.bytes[3]),u64(u.count[4]),m(u.bytes[4]));
}
#endif
#endif
