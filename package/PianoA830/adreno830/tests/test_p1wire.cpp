/* SPDX-License-Identifier: MIT */
/* UniXNU-piano (2026-11-23, GPU-PAGED P1b, private host-checks/20261122-gpu-paged/P1-GATE.md section 7): a large user BO (the pageable
 * named entry) made of parts wired at the same time. What it shows over the IOKit doubles: with no workers nothing changes (one named
 * entry); with workers a BO of >= 1 MiB is up to workers+1 parts on 64 KiB boundaries, its spans the parts' pages in order covering every
 * byte, its CPU view one IOMultiMemoryDescriptor holding every part, released whole; a part whose wiring fails fails the BO and leaves
 * nothing behind; four threads allocating at once share the workers. Then the micro-benchmark of the allocation path: the doubles spend
 * a fixed time per 4 KiB page in prepare() (the tablet's wiring is 0.5-3 us per page), BOs of 1-64 MiB serial vs split. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoMemory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out) {CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation)
{CHECK(mapping && bytes);return kIOReturnSuccess;}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();return kIOReturnSuccess;}
static unsigned checks;
#define OK(x) do {CHECK(x);++checks;} while(0)
static double nowMs() {return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}

/* The spans of a BO: in order, from offset 0, covering every byte, each page-aligned. */
static bool spansCover(const NativeAdrenoMemory &m)
{
    uint64_t at=0;
    for(size_t i=0;i<m.spanCount();++i) {
        const auto &s=m.spans()[i];
        if(s.offset!=at || !s.bytes || (s.bytes&4095) || !s.physical || (s.physical&4095))return false;
        at+=s.bytes;
    }
    return at==m.bytes();
}
static void clean()
{
    CHECK(fixture.live.empty() && fixture.metadata.empty() && !fixture.multi_live);
    fixture=MemoryFixture{};
}
static void layouts()
{
    fixture.mode=ScatterUnlessContiguous;
    /* no workers: one named entry, as before */
    native_adreno_wire_workers=0;
    const uint64_t serialBefore=native_adreno_wire_stats[2];
    {
        NativeAdrenoMemory m;
        OK(!m.allocateUser(4u<<20,40,4u<<20,true) && m.partCount()==1 && !m.partsView());
        IOBufferMemoryDescriptor *view=m.retainVmCpuDescriptor();
        OK(view && view->vm_object);view->release();
        OK(m.spanCount()==1024 && spansCover(m) && native_adreno_wire_stats[2]==serialBefore+1 && fixture.prepared==1);
        OK(!m.release());
    }
    clean();fixture.mode=ScatterUnlessContiguous;
    /* three workers: 4 MiB in 4 parts of 1 MiB, 1 MiB in 4 of 256 KiB, an odd size, below the split size whole */
    native_adreno_wire_workers=3;
    struct Case {size_t bytes;unsigned parts;} cases[]={{4u<<20,4},{1u<<20,4},{(1u<<20)+12288,4},{(768u<<10),1},{(64u<<20),4},{(1u<<20)-4096,1}};
    for(const auto &c:cases) {
        NativeAdrenoMemory m;
        const uint64_t split=native_adreno_wire_stats[0];
        OK(!m.allocateUser(c.bytes,40,c.bytes,true) && m.partCount()==c.parts && m.bytes()==c.bytes && m.unmapped() && m.writeCombined());
        OK(spansCover(m) && m.spanCount()==c.bytes/4096);
        if(c.parts>1) {
            const IOMultiMemoryDescriptor *view=m.partsView();
            OK(view && view->parts.size()==c.parts && view->getLength()==c.bytes && !m.retainVmCpuDescriptor());
            bool aligned=true;
            for(size_t i=0;i+1<view->parts.size();++i)aligned=aligned && !(view->parts[i]->bytes&(NativeAdrenoWirePartAlign-1));
            OK(aligned && native_adreno_wire_stats[0]==split+1 && fixture.multi_live==1 && fixture.prepared==c.parts);
        } else OK(!m.partsView() && fixture.prepared==1);
        OK(m.holdForDevice()==kIOReturnSuccess && m.release()==kIOReturnBusy && !m.retireAfterQuiescence());
        const unsigned completed=fixture.completed;
        OK(!m.release() && fixture.completed==completed+c.parts && !fixture.multi_live && fixture.live.empty());
        clean();fixture.mode=ScatterUnlessContiguous;
    }
    /* seven workers: 64 MiB in 8 parts (the most) */
    native_adreno_wire_priority=81;   /* P1d: a change of the workers' priority between BOs (applied before their next part) */
    native_adreno_wire_workers=7;
    {
        NativeAdrenoMemory m;
        OK(!m.allocateUser(64u<<20,40,64u<<20,true) && m.partCount()==8 && spansCover(m));
        OK(!m.release());
    }
    native_adreno_wire_priority=NativeAdrenoWirePriorityDefault;
    clean();fixture.mode=ScatterUnlessContiguous;
    /* a part's wiring fails: the BO fails, the wired parts are completed, nothing stays */
    native_adreno_wire_workers=3;
    {
        NativeAdrenoMemory m;
        fixture.prepare_fail_at=2;
        const uint64_t failed=native_adreno_wire_stats[3];
        OK(m.allocateUser(8u<<20,40,8u<<20,true)==kIOReturnIOError && !m.backed() && !m.quarantined() && !m.spans());
        OK(native_adreno_wire_stats[3]==failed+1 && fixture.completed==3 && fixture.live.empty() && !fixture.multi_live);
        fixture.prepare_fail_at=0;
        OK(!m.allocateUser(8u<<20,40,8u<<20,true) && m.partCount()==4 && !m.release());
    }
    clean();fixture.mode=ScatterUnlessContiguous;
    /* a split BO keeps the pageable rules: no kernel mapping, a CPU view only while prepared */
    {
        NativeAdrenoMemory m;
        OK(!m.allocateUser(2u<<20,40,2u<<20,false) && m.partCount()==4 && !m.mapping() && !m.writeCombined() && !m.release());
    }
    clean();
}
/* Four threads allocating split BOs at once: the batches share the workers, every BO comes back whole. */
static unsigned together()
{
    fixture.mode=ScatterUnlessContiguous;fixture.prepare_ns_per_page=200;
    native_adreno_wire_workers=3;
    std::atomic<unsigned> good{0};
    std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t)threads.emplace_back([&,t] {
        for(unsigned i=0;i<6;++i) {
            NativeAdrenoMemory m;
            const size_t bytes=size_t(4+((t+i)%4)*4)<<20;
            if(m.allocateUser(bytes,40,bytes,true) || !spansCover(m) || m.partCount()!=4 || m.release())std::abort();
            ++good;
        }
    });
    for(auto &t:threads)t.join();
    OK(good==24);
    fixture.prepare_ns_per_page=0;
    clean();
    return good;
}
/* The micro-benchmark: BoAlloc's allocation path (descriptor, wiring, physical walk, view) at 1 us of wiring per 4 KiB page. */
static void bench(unsigned ns)
{
    fixture.mode=ScatterUnlessContiguous;fixture.prepare_ns_per_page=ns;
    const size_t sizes[]={size_t(1)<<20,size_t(4)<<20,size_t(16)<<20,size_t(64)<<20};
    for(size_t bytes:sizes) {
        double ms[2]={0,0};
        for(unsigned mode=0;mode<2;++mode) {
            native_adreno_wire_workers=mode?3:0;
            const unsigned rounds=bytes>=(size_t(64)<<20)?3:8;
            double best=1e30;
            for(unsigned r=0;r<rounds;++r) {
                NativeAdrenoMemory m;
                const double t0=nowMs();
                CHECK(!m.allocateUser(bytes,40,bytes,true));
                const double t1=nowMs();
                CHECK(!m.release());
                if(t1-t0<best)best=t1-t0;
            }
            ms[mode]=best;
        }
        const double pages=double(bytes/4096);
        std::printf("BENCH wiring %u ns/page, %4zu MiB: serial %.2f ms (%.3f us/page), 3 workers + caller %.2f ms (%.3f us/page), %.2fx\n",ns,
            bytes>>20,ms[0],1000*ms[0]/pages,ms[1],1000*ms[1]/pages,ms[0]/ms[1]);
        if(ns && bytes>=(size_t(16)<<20))CHECK(ms[0]/ms[1]>=1.8);   /* the parts are wired at the same time */
    }
    fixture.prepare_ns_per_page=0;
    clean();
}

int main(int argc,char **argv)
{
    layouts();
    const unsigned both=together();
    const bool full=argc>1 && !strcmp(argv[1],"--bench");
    bench(0);      /* our own path alone: descriptors, the physical walk, the view */
    bench(1000);   /* with the tablet's wiring cost */
    if(full)bench(3000);
    std::printf("PASS: split user BOs (GPU-PAGED P1b): %u checks: no workers = one named entry; >= 1 MiB in up to workers+1 parts on 64 KiB "
        "boundaries, spans in order over every byte, one IOMultiMemoryDescriptor view holding every part, released whole; a failed part "
        "fails the BO and leaves nothing; %u BOs from 4 threads at once; at 1 us per page 16 and 64 MiB wire >= 1.8x faster split\n",checks,both);
    return 0;
}
