/* SPDX-License-Identifier: MIT */
/* GPU-PAGED P0a (host only): the on-demand native4K kit ArmSmmu4KPaged against today's builders and its own contracts.
 *  1 PTE equality: the same mappings through ArmSmmu4KBuild and through Need/Reserve/Map give the same table image byte for byte
 *    (same allocation order) and the same leaves in any order, at 32/36/39-bit inputs, for every class the driver maps; the live
 *    kit's whole audit accepts the kit's leaves in its fixed topology.
 *  2 reserve-then-commit: an allocator failure at every table page of a map writes nothing and leaks nothing; a short reservation
 *    refuses before any write; a malformed run rolls back (leaves cleared, plan ASID); a publish failure poisons, nothing leaks.
 *  3 two roots share the kernel subtree: kernel buffers in both, a client page only in its own, an unmap in one root never changes
 *    the other's pages; the owner's later maps reach every root; its pinned table is never retired.
 *  4 unmap order and TLB plans: TLBIVAL up to 512 pages, else the ASID; ASID + walk when a table is unlinked; unlinked pages hold
 *    only invalid entries and stay allocated until Release; refused unmaps and overlapping maps write nothing.
 *  5 the audit finds planted stray, missing and wrong leaves, block and foreign table descriptors and an extra linked table.
 *  6 churn: after everything is unmapped and released every table page is back; teardown returns the root; no write after release.
 *  7 replay of the gpu-daily3 peak (its console 47536-47552: 5328 records, 3048 MiB mapped): table pages per layout.
 * `test_pagedvm --replay` prints the replay tables with times (meaningful only in a build without sanitizers). */
#include "ArmSmmu4KPaged.h"
#include "ArmSmmu4KLive.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}} while(0)

enum : unsigned {R=ArmSmmu4KRead,W=ArmSmmu4KWrite,X=ArmSmmu4KExecute,P=ArmSmmu4KPrivileged};
static constexpr uint64_t KiB=1024,MiB=1024*KiB,GiB=1024*MiB,PG=4096,PA_MASK=0x0000fffffffff000ull;
static_assert(int(ArmSmmu4KPagedTlbPageLimit)==int(ArmSmmu4KTlbPageLimit),"one TLBIVAL limit");
using Clock=std::chrono::steady_clock;
static uint64_t nsSince(Clock::time_point t)
{return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count();}
static uint64_t rngState=0x9e3779b97f4a7c15ull;
static uint64_t rng() {rngState^=rngState<<13;rngState^=rngState>>7;rngState^=rngState<<17;return rngState;}

/* The table-page allocator double: one arena at physical Base, pages handed out dirty (the kit must zero them), poisoned when given
 * back (a later write by the kit shows when the page is reused), cpu() answering only for pages currently out. strict (the small
 * tests): the publication order is checked at every publish and release: a new table's first publish happens while nothing links it
 * yet, every table a published page links was published before, and nothing links a page when it goes back. */
struct HostPages {
    static constexpr uint64_t Base=0xF00000000ull;   /* 60 GiB: above every BO page of the tests */
    static constexpr uint64_t Dirty=0xA5A5A5A5A5A5A5A5ull,Poison=0x5A5A5A5A5A5A5A5Aull;
    size_t capacity,bump=0,outstanding=0,peak=0;
    uint64_t *arena;
    std::vector<uint32_t> free_list;
    std::vector<uint8_t> state,published;   /* state: 0 never out, 1 out, 2 back; published since it went out */
    long calls=0,fail_at=-1,publishes=0,publish_fail_at=-1;
    unsigned violations=0,order_violations=0;
    bool strict=true;
    explicit HostPages(size_t pages):capacity(pages),arena(static_cast<uint64_t *>(std::aligned_alloc(4096,pages*4096))),state(pages,0),
        published(pages,0)
    {CHECK(arena);}
    bool linked(uint64_t pa) const
    {
        for(size_t i=0;i<bump;++i)if(state[i]==1)for(size_t k=0;k<512;++k)if(arena[i*512+k]==(pa|3))return true;
        return false;
    }
    ~HostPages() {std::free(arena);}
    HostPages(const HostPages &)=delete;
    HostPages &operator=(const HostPages &)=delete;
    bool inArena(uint64_t pa) const {return pa>=Base && pa<Base+capacity*PG && !(pa&(PG-1));}
    size_t index(uint64_t pa) const {return (size_t)((pa-Base)/PG);}
    const uint64_t *page(uint64_t pa) const {return arena+index(pa)*512;}
    std::vector<uint64_t> snapshot() const {return std::vector<uint64_t>(arena,arena+bump*512);}
    bool same(const std::vector<uint64_t> &snap) const {return !std::memcmp(snap.data(),arena,snap.size()*8);}
};
static int hostAllocate(void *cookie,uint64_t *pa,volatile uint64_t **cpu)
{
    auto *h=static_cast<HostPages *>(cookie);
    ++h->calls;
    if(h->fail_at>=0 && h->calls==h->fail_at)return 1;
    size_t i;
    if(!h->free_list.empty()) {
        i=h->free_list.back();h->free_list.pop_back();
        for(size_t k=0;k<512;++k)if(h->arena[i*512+k]!=HostPages::Poison) {++h->violations;break;}
    } else if(h->bump<h->capacity)i=h->bump++;
    else return 1;
    h->state[i]=1;h->published[i]=0;
    if(++h->outstanding>h->peak)h->peak=h->outstanding;
    for(size_t k=0;k<512;++k)h->arena[i*512+k]=HostPages::Dirty;
    *pa=HostPages::Base+i*PG;*cpu=h->arena+i*512;
    return 0;
}
static void hostRelease(void *cookie,uint64_t pa,volatile uint64_t *cpu)
{
    auto *h=static_cast<HostPages *>(cookie);
    if(!h->inArena(pa) || h->state[h->index(pa)]!=1 || cpu!=h->arena+h->index(pa)*512) {++h->violations;return;}
    const size_t i=h->index(pa);
    if(h->strict && h->linked(pa)) {++h->violations;++h->order_violations;}
    h->state[i]=2;--h->outstanding;
    for(size_t k=0;k<512;++k)h->arena[i*512+k]=HostPages::Poison;
    h->free_list.push_back((uint32_t)i);
}
static volatile uint64_t *hostCpu(void *cookie,uint64_t pa)
{
    auto *h=static_cast<HostPages *>(cookie);
    if(!h->inArena(pa) || h->state[h->index(pa)]!=1)return nullptr;
    return h->arena+h->index(pa)*512;
}
static int hostPublish(void *cookie,volatile uint64_t *cpu,size_t bytes)
{
    auto *h=static_cast<HostPages *>(cookie);
    ++h->publishes;
    if(h->publish_fail_at>=0 && h->publishes==h->publish_fail_at)return 1;
    const uintptr_t at=(uintptr_t)cpu,base=(uintptr_t)h->arena;
    if(bytes!=PG || at<base || (at-base)%PG || (at-base)/PG>=h->capacity || h->state[(at-base)/PG]!=1) {++h->violations;return 0;}
    const size_t i=(at-base)/PG;
    if(h->strict) {
        const uint64_t pa=HostPages::Base+i*PG;
        bool bad=!h->published[i] && h->linked(pa);
        for(size_t k=0;k<512;++k) {
            const uint64_t e=h->arena[i*512+k],child=e&PA_MASK;
            if((e&3)==3 && h->inArena(child) && (e!=(child|3) || h->state[h->index(child)]!=1 || !h->published[h->index(child)]))bad=true;
        }
        if(bad) {++h->violations;++h->order_violations;}
    }
    h->published[i]=1;
    return 0;
}
static int hostOwns(void *cookie,uint64_t pa,uint64_t bytes)
{
    auto *h=static_cast<HostPages *>(cookie);
    return pa<HostPages::Base+h->capacity*PG && HostPages::Base<pa+bytes;
}
static ArmSmmu4KPagedPages hooks(HostPages &h) {return {&h,hostAllocate,hostRelease,hostCpu,hostPublish,hostOwns};}

/* A BO's pages: scattered (one run per page, a bijection over 16 GiB above 4 GiB) or contiguous above 20 GiB (one run, or runs of
 * run_pages pages). bad_run: the run starting at that page is malformed (unaligned, or names bad_physical). */
static uint64_t scatter(uint64_t n)
{
    const uint64_t mask=(UINT64_C(1)<<22)-1;
    n=(n*0x9E3779B1u)&mask;
    n^=n>>11;
    return 0x100000000ull+n*PG;
}
struct Bo {
    uint64_t iova=0,bytes=0,first=0;
    unsigned run_pages=1;
    ArmSmmu4KPagedAttrs attrs{R|W,0,2};
    long bad_run=-1;
    uint64_t bad_physical=0;
    ArmSmmu4KPagedRuns runs{};
    uint64_t pages() const {return bytes/PG;}
    uint64_t physical(uint64_t page) const
    {return run_pages==1?scatter(first+page):0x500000000ull+(((first+page)&((UINT64_C(1)<<22)-1))*PG);}
    uint64_t leaf(uint64_t page) const {return ArmSmmu4KPagedLeaf(&attrs,physical(page));}
};
static int boRun(void *cookie,uint64_t offset,uint64_t *physical,uint64_t *bytes)
{
    const Bo *b=static_cast<const Bo *>(cookie);
    if((offset&(PG-1)) || offset>=b->bytes)return 1;
    const uint64_t page=offset/PG;
    if(b->bad_run>=0 && page==(uint64_t)b->bad_run) {*physical=b->bad_physical?b->bad_physical:b->physical(page)+8;*bytes=PG;return 0;}
    *physical=b->physical(page);
    *bytes=b->run_pages==1?PG:!b->run_pages?b->bytes-offset:(b->run_pages-page%b->run_pages)*PG;   /* may reach past the end */
    return 0;
}
static uint64_t physCounter=0;
static void place(Bo &b)
{
    const uint64_t span=UINT64_C(1)<<22;
    if(b.run_pages!=1 && (physCounter%span)+b.pages()>span)physCounter=(physCounter/span+1)*span;
    b.first=physCounter;physCounter+=b.pages();
}
static Bo makeBo(uint64_t iova,uint64_t bytes,ArmSmmu4KPagedAttrs attrs,unsigned run_pages)
{Bo b;b.iova=iova;b.bytes=bytes;b.attrs=attrs;b.run_pages=run_pages;place(b);return b;}
static ArmSmmu4KStatus mapBo(ArmSmmu4KPagedSpace *s,ArmSmmu4KPagedReservation *r,Bo &b,ArmSmmu4KPagedPlan *plan)
{
    size_t need=0;
    ArmSmmu4KStatus status=ArmSmmu4KPagedNeed(s,b.iova,b.bytes,&need);
    if(status)return status;
    if(need>r->pages && (status=ArmSmmu4KPagedReserve(s,need-r->pages,r)))return status;
    b.runs={&b,boRun};
    return ArmSmmu4KPagedMap(s,r,b.iova,b.bytes,&b.runs,&b.attrs,plan);
}
/* Unmap with up to capacity tables unlinked; release them at once (the caller's TLBSYNC is instantaneous here). */
static ArmSmmu4KStatus unmapBo(ArmSmmu4KPagedSpace *s,const Bo &b,size_t capacity,ArmSmmu4KPagedPlan *plan,bool release=true)
{
    static std::vector<uint64_t> retired(8192);
    const ArmSmmu4KStatus status=ArmSmmu4KPagedUnmap(s,b.iova,b.bytes,&b.attrs,retired.data(),capacity,plan);
    if(!status && release)ArmSmmu4KPagedRelease(s,retired.data(),plan->retired);
    return status;
}
static std::vector<ArmSmmu4KPagedRecord> records(std::vector<Bo *> bos)
{
    std::sort(bos.begin(),bos.end(),[](const Bo *a,const Bo *b) {return a->iova<b->iova;});
    std::vector<ArmSmmu4KPagedRecord> out;
    for(Bo *b:bos) {b->runs={b,boRun};out.push_back({b->iova,b->bytes,b->attrs,&b->runs});}
    return out;
}
static bool auditOk(const ArmSmmu4KPagedSpace &s,std::vector<Bo *> bos,ArmSmmu4KPagedAuditResult *result=nullptr)
{
    ArmSmmu4KPagedAuditResult local;
    ArmSmmu4KPagedAuditResult *r=result?result:&local;
    const auto recs=records(bos);
    return ArmSmmu4KPagedAudit(&s,recs.data(),recs.size(),r)==ArmSmmu4KSuccess && r->why==ArmSmmu4KPagedAuditOk;
}
static uint64_t lookup(const ArmSmmu4KPagedSpace &s,uint64_t va)
{
    uint64_t d=~0ull;
    CHECK(ArmSmmu4KPagedLookup(&s,va,&d)==ArmSmmu4KSuccess);
    return d;
}
static bool mapped(const ArmSmmu4KPagedSpace &s,const Bo &b)
{
    for(uint64_t i=0;i<b.pages();++i)if(lookup(s,b.iova+i*PG)!=b.leaf(i))return false;
    return true;
}
static bool unmapped(const ArmSmmu4KPagedSpace &s,const Bo &b)
{
    for(uint64_t i=0;i<b.pages();++i)if(lookup(s,b.iova+i*PG))return false;
    return true;
}
/* Every leaf of a table image, by address, through cpu(physical). */
template<class Cpu> static void imageLeaves(Cpu cpu,uint64_t root,unsigned input_bits,std::map<uint64_t,uint64_t> &out)
{
    const unsigned start=4-(input_bits-12+8)/9;
    auto walk=[&](auto &&self,uint64_t table,unsigned level,uint64_t base)->void {
        const uint64_t *t=cpu(table);
        CHECK(t);
        const unsigned shift=39-9*level,width=level==start?input_bits-shift:9;
        for(unsigned i=0;i<(1u<<width);++i) {
            const uint64_t e=t[i],va=base+((uint64_t)i<<shift);
            if(!e)continue;
            if(level==3)out[va]=e;
            else {CHECK(e==((e&PA_MASK)|3));self(self,e&PA_MASK,level+1,va);}
        }
    };
    walk(walk,root,start,0);
}
static void appendRanges(const Bo &b,std::vector<ArmSmmu4KRange> &out)
{
    for(uint64_t off=0;off<b.bytes;) {
        uint64_t pa=0,len=0;
        CHECK(!boRun(const_cast<Bo *>(&b),off,&pa,&len));
        len=std::min(len,b.bytes-off);
        out.push_back({b.iova+off,pa,len,b.attrs.permissions,b.attrs.writeback,b.attrs.shareability});
        off+=len;
    }
}
static uint64_t *liveSlot(uint64_t *mapping,uint64_t table_physical,unsigned input_bits,uint64_t va)
{
    const unsigned start=4-(input_bits-12+8)/9;
    size_t page=0;
    for(unsigned level=start;level<3;++level) {
        const unsigned shift=39-9*level,width=level==start?input_bits-shift:9;
        const uint64_t e=mapping[page*512+((va>>shift)&((1ull<<width)-1))];
        CHECK((e&3)==3);
        page=(size_t)(((e&PA_MASK)-table_physical)/PG);
    }
    return &mapping[page*512+((va>>12)&511)];
}
static ArmSmmu4KPagedSpace newSpace(HostPages &h,unsigned input_bits,uint64_t start=0,uint64_t end=0)
{
    ArmSmmu4KPagedSpace s{};
    const ArmSmmu4KPagedConfig config{input_bits,40,start,end};
    const ArmSmmu4KPagedPages pages=hooks(h);
    CHECK(ArmSmmu4KPagedInit(&s,&config,&pages)==ArmSmmu4KSuccess);
    return s;
}

/* 1: every class NativeAdrenoGpuContext/NativeAdrenoVm/the GMU map, across 2 MiB, 1 GiB and 4 GiB lines and at the input's top. */
static std::deque<Bo> classLayout(unsigned bits)
{
    std::deque<Bo> bos;
    auto add=[&](uint64_t iova,uint64_t bytes,ArmSmmu4KPagedAttrs a,unsigned run_pages) {bos.push_back(makeBo(iova,bytes,a,run_pages));};
    add(0x100000,0x1C000,{R|X,0,2},0);          /* SQE: read + execute, unprivileged readable */
    add(0x180000,0x7000,{R|X,0,2},0);           /* AQE */
    add(0x200000,0x8000,{R|P,0,2},0);           /* ring: privileged read-only */
    add(0x300000,0x1000,{R|W|P,0,2},0);         /* memptrs, pwrup, scratch: privileged read/write */
    add(0x301000,0x1000,{R|W|P,0,2},0);
    add(0x302000,0x1000,{R|W|P,0,2},0);
    add(0x201FD000,0x6000,{R|W,0,2},1);         /* client NC read/write, scattered, across a 2 MiB line */
    add(0x3FFFE000,0x5000,{R,0,2},1);           /* client NC read-only, across the 1 GiB line */
    add(0x60000000,0x76000,{R|W|X|P,0,2},3);    /* privileged RWX (the GMU context's CACHE class), 3-page runs */
    add(0x7FE00000,0x200000,{R,1,3},3);         /* import write-back read-only: one whole level-3 table */
    add(0xA0000000,0x11000,{R|W,1,3},0);        /* write-back read/write (coherence 2) */
    if(bits==32)add(0xFFFFE000,0x2000,{R|W,0,2},1);   /* the top of 4 GiB */
    else {
        add(0xFFFFF000,0x3000,{R|W,0,2},1);     /* across 4 GiB */
        add(0x123401000,64*MiB,{R|W,0,2},1);    /* a WindowServer-size scattered BO */
        if(bits==36)add(0xFFFFF0000,0x10000,{R,0,2},1);   /* the top of 64 GiB */
        else {add(0x4000000000,0x2000,{R|W,0,2},1);add(0x7FFFFFE000,0x2000,{R|W,0,2},1);}
    }
    return bos;
}
static unsigned testEquality(size_t *leaves_checked)
{
    unsigned profiles=0;
    for(unsigned bits:{32u,36u,39u}) {
        physCounter=0;
        auto bos=classLayout(bits);
        std::vector<ArmSmmu4KRange> ranges;
        for(const Bo &b:bos)appendRanges(b,ranges);
        const size_t capacity=4096;
        std::vector<uint64_t> image(capacity*512,0x1111);
        const ArmSmmu4KConfig config{bits,40,HostPages::Base,size_t(1)<<24};
        ArmSmmu4KResult built{};
        CHECK(ArmSmmu4KBuild(&config,ranges.data(),ranges.size(),image.data(),capacity,&built)==ArmSmmu4KSuccess);
        /* The same mappings in address order: the same pages in the same order, so the same image byte for byte. */
        HostPages h(8192);
        ArmSmmu4KPagedSpace s=newSpace(h,bits);
        ArmSmmu4KPagedReservation r{};
        for(Bo &b:bos) {ArmSmmu4KPagedPlan plan;CHECK(mapBo(&s,&r,b,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbNone);}
        CHECK(!r.pages && s.counters.table_pages==built.table_pages && s.root_physical==built.root_physical &&
              built.mair==0xff44 && built.start_level==s.start_level && s.counters.leaves==built.leaf_pages);
        CHECK(!std::memcmp(image.data(),h.arena,built.table_pages*PG));
        std::map<uint64_t,uint64_t> want,got,reversed;
        imageLeaves([&](uint64_t pa)->const uint64_t * {return pa>=HostPages::Base && pa<HostPages::Base+capacity*PG?
            image.data()+(pa-HostPages::Base)/PG*512:nullptr;},built.root_physical,bits,want);
        imageLeaves([&](uint64_t pa)->const uint64_t * {return h.inArena(pa)?h.page(pa):nullptr;},s.root_physical,bits,got);
        CHECK(want==got && want.size()==built.leaf_pages);
        std::vector<Bo *> all;
        for(Bo &b:bos) {CHECK(mapped(s,b));all.push_back(&b);}
        CHECK(auditOk(s,all) && !h.violations);
        /* In reverse order: other pages, the same leaves. */
        HostPages h2(8192);
        ArmSmmu4KPagedSpace s2=newSpace(h2,bits);
        ArmSmmu4KPagedReservation r2{};
        for(auto it=bos.rbegin();it!=bos.rend();++it) {ArmSmmu4KPagedPlan plan;CHECK(mapBo(&s2,&r2,*it,&plan)==ArmSmmu4KSuccess);}
        imageLeaves([&](uint64_t pa)->const uint64_t * {return h2.inArena(pa)?h2.page(pa):nullptr;},s2.root_physical,bits,reversed);
        CHECK(reversed==want && s2.counters.table_pages==built.table_pages && auditOk(s2,all) && !h2.violations);
        *leaves_checked+=want.size();
        ++profiles;
        /* The live kit (today's VM tables): its whole audit compares every descriptor of its fixed topology with its own encoder. */
        if(bits==32)continue;
        const uint64_t aperture=8*GiB;
        const size_t live_pages=1+8+4096;
        auto *mapping=static_cast<uint64_t *>(std::aligned_alloc(4096,live_pages*PG));
        CHECK(mapping);
        std::vector<ArmSmmu4KRange> inside;
        for(const auto &range:ranges)if(range.iova+range.bytes<=aperture)inside.push_back(range);
        std::vector<uint32_t> scratch(inside.size()+16);
        ArmSmmu4KLiveArena arena{};
        arena.config={bits,40,0xE00000000ull,(size_t)(aperture/PG)};
        arena.iova=0;arena.bytes=aperture;arena.mapping=mapping;arena.capacity_pages=live_pages;
        arena.alias_scratch=scratch.data();arena.alias_scratch_entries=scratch.size();
        ArmSmmu4KLiveState state{};
        CHECK(ArmSmmu4KLivePrepare(&arena,&state)==ArmSmmu4KSuccess && state.tables.table_pages==live_pages);
        size_t leaves=0;
        for(const auto &range:inside) {
            const ArmSmmu4KPagedAttrs a{range.permissions,range.writeback,range.shareability};
            for(uint64_t off=0;off<range.bytes;off+=PG,++leaves)
                *liveSlot(mapping,arena.config.table_physical,bits,range.iova+off)=ArmSmmu4KPagedLeaf(&a,range.physical+off);
        }
        state.tables.leaf_pages=leaves;
        ArmSmmu4KLiveResult audit{};
        CHECK(ArmSmmu4KLiveAudit(&arena,&state,inside.data(),inside.size(),&audit)==ArmSmmu4KSuccess && audit.complete);
        uint64_t *probe=liveSlot(mapping,arena.config.table_physical,bits,0x201FE000);
        *probe^=1u<<6;   /* a known-positive: one AP bit off and the audit refuses */
        CHECK(ArmSmmu4KLiveAudit(&arena,&state,inside.data(),inside.size(),&audit)==ArmSmmu4KStateMismatch);
        *probe^=1u<<6;
        CHECK(ArmSmmu4KLiveAudit(&arena,&state,inside.data(),inside.size(),&audit)==ArmSmmu4KSuccess);
        std::free(mapping);
        ++profiles;
    }
    return profiles;
}

/* 2: reserve-then-commit. T needs 6 new tables beside one existing level-3 table (A's): blocks 510 (A's) and 511 of GiB 0 and
 * blocks 0-3 of GiB 1. */
static unsigned testFailures()
{
    unsigned cases=0;
    physCounter=0x10000;
    HostPages h(4096);
    ArmSmmu4KPagedSpace s=newSpace(h,39);
    ArmSmmu4KPagedReservation r{};
    Bo a=makeBo(0x3FC00000,PG,{R|W,0,2},1);
    ArmSmmu4KPagedPlan plan;
    CHECK(mapBo(&s,&r,a,&plan)==ArmSmmu4KSuccess);
    Bo t=makeBo(0x3FDFF000,0x40601000-0x3FDFF000,{R|W,0,2},1);
    size_t need=0;
    CHECK(ArmSmmu4KPagedNeed(&s,t.iova,t.bytes,&need)==ArmSmmu4KSuccess && need==6 && ArmSmmu4KPagedWorstCase(&s,t.iova,t.bytes)==8);
    const uint64_t tables=s.counters.table_pages,leaves=s.counters.leaves;
    /* The allocator refuses the k-th table page: nothing written, nothing kept. */
    for(long k=1;k<=6;++k,++cases) {
        const auto snap=h.snapshot();
        const size_t out=h.outstanding;
        h.fail_at=h.calls+k;
        CHECK(ArmSmmu4KPagedReserve(&s,need,&r)==ArmSmmu4KCapacity);
        h.fail_at=-1;
        CHECK(!r.pages && h.outstanding==out && h.same(snap) && s.counters.table_pages==tables && s.counters.leaves==leaves);
        CHECK(s.counters.allocation_failures==(uint64_t)k);
    }
    /* A reservation one page short: refused before any write. */
    CHECK(ArmSmmu4KPagedReserve(&s,need-1,&r)==ArmSmmu4KSuccess && r.pages==need-1);
    {
        const auto snap=h.snapshot();
        t.runs={&t,boRun};
        CHECK(ArmSmmu4KPagedMap(&s,&r,t.iova,t.bytes,&t.runs,&t.attrs,&plan)==ArmSmmu4KCapacity && h.same(snap) && r.pages==need-1);
        ArmSmmu4KPagedCancel(&s,&r);
        CHECK(!r.pages && h.outstanding==tables);
        ++cases;
    }
    /* A malformed run at the first page, inside A's table, at a new table's first page, inside it, in the second GiB and at the end,
     * then a run naming a table page: the leaves written are cleared, the empty new tables stay linked until a Trim. */
    const long positions[]={0,1,2,300,513,(long)t.pages()-1};
    std::vector<uint64_t> retired(64);
    for(int named=0;named<2;++named)for(long at:positions) {
        if(named && at!=300)continue;
        t.bad_run=at;t.bad_physical=named?s.root_physical:0;
        CHECK(mapBo(&s,&r,t,&plan)==ArmSmmu4KBadRange);
        CHECK(plan.rolled_back && plan.kind==ArmSmmu4KPagedTlbAsid && unmapped(s,t) && mapped(s,a) && s.counters.leaves==leaves);
        CHECK(h.outstanding==s.counters.table_pages+r.pages+s.counters.retired_pending);
        CHECK(auditOk(s,{&a}));
        ArmSmmu4KPagedPlan trim;
        CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&trim)==ArmSmmu4KSuccess && trim.retired==plan.tables_new);
        CHECK(trim.retired?trim.kind==ArmSmmu4KPagedTlbAsid && trim.walk:trim.kind==ArmSmmu4KPagedTlbNone);
        ArmSmmu4KPagedRelease(&s,retired.data(),trim.retired);
        CHECK(s.counters.table_pages==tables && auditOk(s,{&a}));
        ++cases;
    }
    t.bad_run=-1;t.bad_physical=0;
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(mapBo(&s,&r,t,&plan)==ArmSmmu4KSuccess && plan.tables_new==6 && !r.pages && mapped(s,t) && auditOk(s,{&a,&t}));
    /* A publish failure poisons the space: every later change refuses; teardown and cancel still return every page. */
    Bo u=makeBo(0x80000000,0x3000,{R|W,0,2},1);
    h.publish_fail_at=h.publishes+1;
    CHECK(mapBo(&s,&r,u,&plan)==ArmSmmu4KIoError && s.poisoned);
    h.publish_fail_at=-1;
    CHECK(ArmSmmu4KPagedMap(&s,&r,u.iova,u.bytes,&u.runs,&u.attrs,&plan)==ArmSmmu4KArgument);
    CHECK(ArmSmmu4KPagedUnmap(&s,a.iova,a.bytes,nullptr,nullptr,0,&plan)==ArmSmmu4KArgument);
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    ++cases;
    /* A space whose root cannot be had. */
    HostPages none(1);
    ArmSmmu4KPagedSpace first=newSpace(none,36),second{};
    const ArmSmmu4KPagedConfig config{36,40,0,0};
    const ArmSmmu4KPagedPages pages=hooks(none);
    CHECK(ArmSmmu4KPagedInit(&second,&config,&pages)==ArmSmmu4KCapacity && !second.prepared);
    CHECK(ArmSmmu4KPagedTeardown(&first)==ArmSmmu4KSuccess && !none.outstanding);
    return cases+1;
}

/* 3: the kernel space K owns root slot 0 (0..1 GiB, the kernel buffers); clients A and B link it and map at 4 GiB. */
static unsigned testSharedRoots()
{
    physCounter=0x20000;
    HostPages h(4096);
    ArmSmmu4KPagedSpace k=newSpace(h,39,0,GiB),a=newSpace(h,39,4*GiB,8*GiB),b=newSpace(h,39,4*GiB,8*GiB),c=newSpace(h,39);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    std::deque<Bo> kernel;
    for(const auto &[iova,bytes,attrs]:{std::tuple{0x100000ull,0x1C000ull,ArmSmmu4KPagedAttrs{R|X,0,2}},
            std::tuple{0x200000ull,0x8000ull,ArmSmmu4KPagedAttrs{R|P,0,2}},std::tuple{0x300000ull,0x1000ull,ArmSmmu4KPagedAttrs{R|W|P,0,2}}})
        kernel.push_back(makeBo(iova,bytes,attrs,0));
    for(Bo &bo:kernel)CHECK(mapBo(&k,&r,bo,&plan)==ArmSmmu4KSuccess);
    uint64_t shared=0;
    CHECK(ArmSmmu4KPagedShare(&k,0,&shared)==ArmSmmu4KSuccess && shared && ArmSmmu4KPagedShare(&k,1,&shared)==ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedShare(&k,0,&shared)==ArmSmmu4KSuccess);   /* again: the same table */
    for(ArmSmmu4KPagedSpace *s:{&a,&b,&c})CHECK(ArmSmmu4KPagedLinkShared(s,0,shared)==ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedLinkShared(&a,0,shared)==ArmSmmu4KArgument && ArmSmmu4KPagedLinkShared(&k,0,shared)==ArmSmmu4KArgument);
    for(const Bo &bo:kernel)CHECK(mapped(k,bo) && mapped(a,bo) && mapped(b,bo) && mapped(c,bo));
    /* The same GPU address, other pages in each client. */
    Bo ba=makeBo(4*GiB+0x10000,33*PG,{R|W,0,2},1),bb=makeBo(4*GiB+0x10000,33*PG,{R|W,0,2},1);
    CHECK(mapBo(&a,&r,ba,&plan)==ArmSmmu4KSuccess && mapBo(&b,&r,bb,&plan)==ArmSmmu4KSuccess);
    CHECK(mapped(a,ba) && mapped(b,bb) && !mapped(a,bb) && unmapped(k,ba) && unmapped(c,ba));
    /* Nothing below a linked slot: outside A's range, and inside C's whole range but in the shared slot. */
    Bo stray=makeBo(0x400000,PG,{R|W,0,2},1);
    CHECK(mapBo(&a,&r,stray,&plan)==ArmSmmu4KBadRange && mapBo(&c,&r,stray,&plan)==ArmSmmu4KOverlap);
    CHECK(ArmSmmu4KPagedUnmap(&c,0x100000,PG,nullptr,nullptr,0,&plan)==ArmSmmu4KOverlap);
    /* An unmap with retirement in A leaves every page of B and of the shared subtree as it was. */
    std::vector<uint64_t> others;
    auto collect=[&](const ArmSmmu4KPagedSpace &s) {
        std::map<uint64_t,uint64_t> leaves;
        imageLeaves([&](uint64_t pa)->const uint64_t * {others.push_back(pa);return h.page(pa);},s.root_physical,39,leaves);
    };
    collect(b);collect(k);
    std::vector<std::vector<uint64_t>> before;
    for(uint64_t pa:others)before.emplace_back(h.page(pa),h.page(pa)+512);
    CHECK(unmapBo(&a,ba,16,&plan)==ArmSmmu4KSuccess && plan.retired==2 && plan.walk && plan.kind==ArmSmmu4KPagedTlbAsid);
    for(size_t i=0;i<others.size();++i)CHECK(!std::memcmp(before[i].data(),h.page(others[i]),PG));
    CHECK(unmapped(a,ba) && mapped(b,bb) && lookup(a,0x100000)==kernel[0].leaf(0));
    /* The owner maps later: every root sees it; it unmaps all its buffers: their tables go, the pinned one stays linked. */
    Bo late=makeBo(0x303000,PG,{R|W|P,0,2},0);
    CHECK(mapBo(&k,&r,late,&plan)==ArmSmmu4KSuccess && mapped(a,late) && mapped(b,late));
    kernel.push_back(late);
    size_t retired=0;
    for(const Bo &bo:kernel) {CHECK(unmapBo(&k,bo,16,&plan)==ArmSmmu4KSuccess);retired+=plan.retired;}
    CHECK(retired==2 && k.root[0]==(shared|3) && a.root[0]==(shared|3) && b.root[0]==(shared|3) && unmapped(a,kernel[0]));
    CHECK(mapBo(&k,&r,kernel[0],&plan)==ArmSmmu4KSuccess && mapped(b,kernel[0]) && mapped(c,kernel[0]));
    std::vector<Bo *> kernel_live{&kernel[0]};
    CHECK(auditOk(k,kernel_live) && auditOk(a,{}) && auditOk(b,{&bb}) && auditOk(c,{}));
    for(ArmSmmu4KPagedSpace *s:{&a,&b,&c})CHECK(ArmSmmu4KPagedTeardown(s)==ArmSmmu4KSuccess);
    CHECK(mapped(k,kernel[0]));
    ArmSmmu4KPagedCancel(&k,&r);
    CHECK(ArmSmmu4KPagedTeardown(&k)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    return 9;
}

/* 4: TLB plans and the order of an unmap. */
static unsigned testPlans()
{
    unsigned cases=0;
    physCounter=0x30000;
    HostPages h(4096);
    ArmSmmu4KPagedSpace s=newSpace(h,36);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    const uint64_t base=8*GiB;
    Bo one=makeBo(base,PG,{R|W,0,2},1),full=makeBo(base+2*MiB,512*PG,{R|W,0,2},1),big=makeBo(base+4*MiB,513*PG,{R,0,2},0);
    for(Bo *b:{&one,&full,&big})CHECK(mapBo(&s,&r,*b,&plan)==ArmSmmu4KSuccess);
    const uint64_t tables=s.counters.table_pages;   /* root, L2 of GiB 8, 4 level-3 tables */
    CHECK(tables==6);
    /* Mapping over a valid leaf and unmapping with other attributes or across a hole write nothing. */
    {
        const auto snap=h.snapshot();
        Bo over=makeBo(base+2*MiB-PG,2*PG,{R|W,0,2},1);
        CHECK(mapBo(&s,&r,over,&plan)==ArmSmmu4KOverlap && h.same(snap));
        const ArmSmmu4KPagedAttrs ro{R,0,2};
        CHECK(ArmSmmu4KPagedUnmap(&s,one.iova,one.bytes,&ro,nullptr,0,&plan)==ArmSmmu4KStateMismatch && h.same(snap));
        CHECK(ArmSmmu4KPagedUnmap(&s,base,3*MiB,nullptr,nullptr,0,&plan)==ArmSmmu4KStateMismatch && h.same(snap));
        cases+=3;
    }
    /* Without an array: TLBIVAL plans up to 512 pages, the ASID above; the emptied tables stay linked. */
    CHECK(unmapBo(&s,one,0,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbPages && plan.iova==base && plan.pages==1 &&
          !plan.walk && plan.kept_empty==1);
    CHECK(unmapBo(&s,full,0,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbPages && plan.pages==512 && plan.kept_empty==1);
    CHECK(unmapBo(&s,big,0,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbAsid && !plan.walk && plan.leaves==513 &&
          plan.kept_empty==2);
    CHECK(s.counters.table_pages==tables && !s.counters.leaves);
    cases+=3;
    /* Trim unlinks them (ASID + walk); they hold only invalid entries and stay allocated until Release. */
    std::vector<uint64_t> retired(16);
    const size_t out=h.outstanding;
    CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&plan)==ArmSmmu4KSuccess && plan.retired==5 && plan.walk &&
          plan.kind==ArmSmmu4KPagedTlbAsid && s.counters.table_pages==1 && s.counters.retired_pending==5);
    std::vector<std::vector<uint64_t>> held;
    for(size_t i=0;i<plan.retired;++i) {
        const uint64_t *page=h.page(retired[i]);
        CHECK(h.state[h.index(retired[i])]==1 && std::all_of(page,page+512,[](uint64_t e) {return !e;}));
        held.emplace_back(page,page+512);
    }
    /* Other work before the TLBSYNC (another map and unmap) never takes or writes an unlinked page. */
    Bo other=makeBo(base+64*MiB,3*PG,{R|W,0,2},1);
    CHECK(mapBo(&s,&r,other,&plan)==ArmSmmu4KSuccess && unmapBo(&s,other,0,&plan)==ArmSmmu4KSuccess);
    for(size_t i=0;i<held.size();++i)CHECK(!std::memcmp(held[i].data(),h.page(retired[i]),PG));
    CHECK(h.outstanding==out+2);   /* the new map's L3 and L2 */
    ArmSmmu4KPagedRelease(&s,retired.data(),5);
    CHECK(h.outstanding==out-3 && !s.counters.retired_pending && !hostCpu(&h,retired[0]));
    cases+=3;
    /* With an array: an unmap that empties a table plans ASID + walk; one that does not stays TLBIVAL. */
    ArmSmmu4KPagedPlan trim;
    CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&trim)==ArmSmmu4KSuccess && trim.retired==2);
    ArmSmmu4KPagedRelease(&s,retired.data(),trim.retired);
    Bo first=makeBo(base,PG,{R|W,0,2},1),second=makeBo(base+PG,PG,{R|W,0,2},1);
    CHECK(mapBo(&s,&r,first,&plan)==ArmSmmu4KSuccess && mapBo(&s,&r,second,&plan)==ArmSmmu4KSuccess);
    CHECK(unmapBo(&s,first,16,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbPages && !plan.retired);
    CHECK(unmapBo(&s,second,16,&plan)==ArmSmmu4KSuccess && plan.kind==ArmSmmu4KPagedTlbAsid && plan.walk && plan.retired==2);
    CHECK(s.counters.table_pages==1 && h.outstanding==1+r.pages);
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    return cases+2;
}

/* 5: the audit against planted faults (each a known-positive), then clean again. */
static unsigned testAudit()
{
    physCounter=0x40000;
    HostPages h(4096);
    ArmSmmu4KPagedSpace s=newSpace(h,36);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    Bo b1=makeBo(0x200000000,4*PG,{R|W,0,2},1),b2=makeBo(0x200100000,16*PG,{R,0,2},3),b3=makeBo(0x240000000,2*PG,{R,1,3},0);
    for(Bo *b:{&b1,&b2,&b3})CHECK(mapBo(&s,&r,*b,&plan)==ArmSmmu4KSuccess);
    ArmSmmu4KPagedAuditResult audit;
    CHECK(auditOk(s,{&b1,&b2,&b3},&audit) && audit.tables==s.counters.table_pages && audit.leaves==22 && !audit.empty_tables);
    auto slot=[&](uint64_t va,unsigned level)->uint64_t * {   /* the entry for va at level, through existing tables */
        const uint64_t *t=h.page(s.root_physical);
        for(unsigned l=s.start_level;l<level;++l)t=h.page(t[(va>>(39-9*l))&(l==s.start_level?(1u<<(36-30))-1:511)]&PA_MASK);
        return const_cast<uint64_t *>(&t[(va>>(39-9*level))&511]);
    };
    auto expect=[&](ArmSmmu4KPagedAuditWhy why,uint64_t iova) {
        const auto recs=records({&b1,&b2,&b3});
        ArmSmmu4KPagedAuditResult result;
        CHECK(ArmSmmu4KPagedAudit(&s,recs.data(),recs.size(),&result)==ArmSmmu4KStateMismatch && result.why==why && result.iova==iova);
    };
    uint64_t *e=slot(b1.iova+4*PG,3);
    *e=b1.leaf(0);expect(ArmSmmu4KPagedAuditStrayLeaf,b1.iova+4*PG);*e=0;
    e=slot(b2.iova+5*PG,3);
    const uint64_t saved=*e;
    *e=0;expect(ArmSmmu4KPagedAuditMissingLeaf,b2.iova+5*PG);
    *e=saved^(1u<<7);expect(ArmSmmu4KPagedAuditWrongLeaf,b2.iova+5*PG);*e=saved;
    e=slot(0x200400000,2);
    *e=0x80000000|1;expect(ArmSmmu4KPagedAuditBadTable,0x200400000);   /* a block descriptor */
    *e=0x123000|3;expect(ArmSmmu4KPagedAuditBadTable,0x200400000);      /* a page no allocator gave */
    CHECK(ArmSmmu4KPagedReserve(&s,1,&r)==ArmSmmu4KSuccess);
    std::memset(const_cast<uint64_t *>(h.page(r.head)),0,PG);
    *e=r.head|3;expect(ArmSmmu4KPagedAuditTableCount,0);                /* a table nobody counted */
    *e=0;
    {
        const auto recs=records({&b1,&b3});   /* a record missing: its leaves are strays */
        ArmSmmu4KPagedAuditResult result;
        CHECK(ArmSmmu4KPagedAudit(&s,recs.data(),recs.size(),&result)==ArmSmmu4KStateMismatch &&
              result.why==ArmSmmu4KPagedAuditStrayLeaf && result.iova==b2.iova);
    }
    CHECK(auditOk(s,{&b1,&b2,&b3}));
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    return 8;
}

/* A best-fit GPU address heap as Turnip's darwin_va_alloc: the smallest hole that fits, its low end, 4 KiB alignment. */
struct VaHeap {
    std::map<uint64_t,uint64_t> by_start;
    std::multimap<uint64_t,uint64_t> by_size;
    VaHeap(uint64_t start,uint64_t bytes) {add(start,bytes);}
    void add(uint64_t start,uint64_t bytes) {if(bytes) {by_start[start]=bytes;by_size.emplace(bytes,start);}}
    void del(uint64_t start,uint64_t bytes)
    {
        by_start.erase(start);
        for(auto [it,end]=by_size.equal_range(bytes);it!=end;++it)if(it->second==start) {by_size.erase(it);return;}
        CHECK(false);
    }
    uint64_t take(uint64_t bytes)
    {
        const auto it=by_size.lower_bound(bytes);
        if(it==by_size.end())return 0;
        const uint64_t start=it->second,hole=it->first;
        del(start,hole);add(start+bytes,hole-bytes);
        return start;
    }
    void give(uint64_t start,uint64_t bytes)
    {
        uint64_t begin=start,end=start+bytes;
        auto next=by_start.lower_bound(start);
        if(next!=by_start.begin()) {
            const auto prev=std::prev(next);
            if(prev->first+prev->second==start) {begin=prev->first;del(prev->first,prev->second);}
        }
        const auto after=by_start.find(end);
        if(after!=by_start.end()) {const uint64_t extra=after->second;del(end,extra);end+=extra;}
        add(begin,end-begin);
    }
};
static uint64_t pickSize(uint64_t pages_max)
{
    const uint64_t roll=rng()%100;
    const uint64_t pages=roll<70?1+rng()%16:roll<95?17+rng()%1008:1025+rng()%7168;
    return std::min(pages,pages_max)*PG;
}
/* 6: random maps and unmaps in a best-fit heap; audited every 500 operations; everything back at the end, in both retire modes. */
static unsigned testChurn(uint64_t *ops_done)
{
    for(int keep=0;keep<2;++keep) {
        rngState=0x1234567ull+keep;
        physCounter=0x50000;
        HostPages h(16384);
        ArmSmmu4KPagedSpace s=newSpace(h,36,8*GiB,12*GiB);
        VaHeap heap(8*GiB,4*GiB);
        ArmSmmu4KPagedReservation r{};
        ArmSmmu4KPagedPlan plan;
        std::deque<Bo> pool;
        std::vector<Bo *> live;
        for(unsigned op=0;op<4000;++op,++*ops_done) {
            if(live.size()<64 || rng()%100<55) {
                const unsigned modes[]={1,1,1,0,5};
                Bo b=makeBo(0,pickSize(8192),{R|W,0,2},modes[rng()%5]);
                if(!(b.iova=heap.take(b.bytes)))continue;
                pool.push_back(b);
                CHECK(mapBo(&s,&r,pool.back(),&plan)==ArmSmmu4KSuccess);
                live.push_back(&pool.back());
            } else {
                const size_t at=rng()%live.size();
                Bo *b=live[at];
                CHECK(unmapBo(&s,*b,keep?0:8192,&plan)==ArmSmmu4KSuccess);
                CHECK(plan.leaves==b->pages() && (plan.walk || plan.kind==(b->pages()<=512?ArmSmmu4KPagedTlbPages:ArmSmmu4KPagedTlbAsid)));
                heap.give(b->iova,b->bytes);
                live[at]=live.back();live.pop_back();
            }
            if(op%500==499)CHECK(auditOk(s,live));
        }
        for(Bo *b:live)CHECK(unmapBo(&s,*b,keep?0:8192,&plan)==ArmSmmu4KSuccess);
        if(keep) {
            static std::vector<uint64_t> retired(16384);
            CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&plan)==ArmSmmu4KSuccess);
            ArmSmmu4KPagedRelease(&s,retired.data(),plan.retired);
        }
        ArmSmmu4KPagedAuditResult audit;
        CHECK(auditOk(s,{},&audit) && !audit.empty_tables && s.counters.table_pages==1 && !s.counters.leaves);
        CHECK(h.outstanding==1+r.pages);
        ArmSmmu4KPagedCancel(&s,&r);
        CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    }
    return 2;
}

/* 7: the gpu-daily3 peak (run 20261122-runs/gpu-daily3, console 47536-47552, "gpu va/map" and "gpu va/sizes"): per client its
 * objects and MiB, by size class where the console gives them (<1M, 1-16M, 16-64M, imports), else objects of their mean size. */
struct PeakBucket {unsigned count;uint64_t mib;bool import;};
struct PeakClient {const char *name;uint64_t window_start,window_bytes;std::vector<PeakBucket> buckets;};
static std::vector<PeakClient> peakClients()
{
    /* Windows of today's 64 GiB table (NativeAdrenoPublish.h NativeAdrenoMakeClientWindows64G): 0 the render server, 1-3 large,
     * 4.. ordinary 1 GiB windows from 8 GiB, 58 the decoder. */
    auto ordinary=[](unsigned slot)->uint64_t {return 0x200000000ull+uint64_t(slot-4)*GiB;};
    return {
        {"WindowServer",0x20000000ull,0xE0000000ull,{{3069,127,false},{11,79,false},{67,1600,false},{132,142,true}}},
        {"iconservicesagent",0x100000000ull,0x80000000ull,{{52,21,false}}},
        {"iconservicesagent",0x180000000ull,0x40000000ull,{{1347,31,false},{5,24,false},{24,384,false},{52,4,true}}},
        {"avconferenced",ordinary(4),GiB,{{3,1,false}}},
        {"NotificationCenter",ordinary(5),GiB,{{3,1,false}}},
        {"loginwindow",ordinary(6),GiB,{{52,50,false}}},
        {"MenuBarAgent",ordinary(7),GiB,{{3,1,false}}},
        {"ControlCenter",ordinary(8),GiB,{{79,26,false}}},
        {"Finder",ordinary(9),GiB,{{3,1,false}}},
        {"WallpaperAerials",ordinary(10),GiB,{{3,1,false}}},
        {"Siri AI",ordinary(11),GiB,{{8,1,false},{5,28,false},{17,272,false}}},
        {"CursorUIViewService",ordinary(12),GiB,{{3,1,false}}},
        {"KeyboardSetupAssistant",ordinary(13),GiB,{{27,25,false}}},
        {"TextInputMenuAgent",ordinary(14),GiB,{{3,1,false}}},
        {"Dock",ordinary(15),GiB,{{319,71,false}}},
        {"TextInputSwitcher",ordinary(16),GiB,{{3,1,false}}},
        {"VTDecoderXPCService",0xFA0000000ull,0x5FFFE000ull,{{24,1,false},{4,20,false},{2,58,false},{2,90,true}}},
    };
}
struct Workload {std::deque<Bo> bos;std::vector<size_t> client_of;size_t kernel=0;};
/* Every BO of the peak in allocation order (round robin over each client's classes), addresses from a best-fit heap per window
 * (per_process: every client's own window [4 GiB, 8 GiB)), pages scattered (run_pages 1) or contiguous (0). */
static Workload peakWorkload(unsigned run_pages,bool per_process)
{
    Workload w;
    physCounter=0x100000;
    const ArmSmmu4KPagedAttrs kernel_attrs[]={{R|X,0,2},{R|X,0,2},{R|P,0,2},{R|W|P,0,2},{R|W|P,0,2},{R|W|P,0,2}};
    const uint64_t kernel_iova[]={0x100000,0x180000,0x200000,0x300000,0x301000,0x302000},kernel_bytes[]={0x1C000,0x7000,0x8000,PG,PG,PG};
    for(unsigned i=0;i<6;++i) {w.bos.push_back(makeBo(kernel_iova[i],kernel_bytes[i],kernel_attrs[i],0));w.client_of.push_back(SIZE_MAX);}
    w.kernel=6;
    const auto clients=peakClients();
    for(size_t c=0;c<clients.size();++c) {
        const auto &client=clients[c];
        VaHeap heap(per_process?4*GiB:client.window_start,per_process?4*GiB:client.window_bytes);
        std::vector<unsigned> left;
        for(const auto &bucket:client.buckets)left.push_back(bucket.count);
        for(bool any=true;any;) {
            any=false;
            for(size_t k=0;k<client.buckets.size();++k)if(left[k]) {
                const auto &bucket=client.buckets[k];
                --left[k];any=true;
                const uint64_t bytes=std::max<uint64_t>(PG,(bucket.mib*MiB/bucket.count+PG-1)&~(PG-1));
                Bo b=makeBo(0,bytes,bucket.import?ArmSmmu4KPagedAttrs{R,0,2}:ArmSmmu4KPagedAttrs{R|W,0,2},run_pages);
                CHECK((b.iova=heap.take(bytes)));
                w.bos.push_back(b);w.client_of.push_back(c);
            }
        }
    }
    return w;
}
struct Measure {
    uint64_t objects=0,pages=0,tables=0,level[4]{},map_ns=0,unmap_ns=0,audit_ns=0,small_maps=0,small_ns=0,spaces=0;
    uint64_t plans_pages=0,plans_asid=0,plans_walk=0,retired=0,descriptor_reads=0,descriptor_writes=0;
};
static void count(Measure &m,const ArmSmmu4KPagedSpace &s)
{
    m.tables+=s.counters.table_pages;
    for(unsigned l=0;l<4;++l)m.level[l]+=s.counters.level_pages[l];
}
/* P1: one global 36-bit space (today's 64 GiB aperture and windows). eager: tables unlinked at each unmap (released at once). */
static Measure replayGlobal(unsigned run_pages,bool eager,bool check)
{
    Measure m;
    Workload w=peakWorkload(run_pages,false);
    HostPages h(65536);
    h.strict=false;   /* the order checks scan the arena: the small tests carry them */
    ArmSmmu4KPagedSpace s=newSpace(h,36);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    const auto t0=Clock::now();
    for(Bo &b:w.bos)CHECK(mapBo(&s,&r,b,&plan)==ArmSmmu4KSuccess);
    m.map_ns=nsSince(t0);
    for(Bo &b:w.bos) {
        ++m.objects;m.pages+=b.pages();
        if(b.bytes<MiB) {const auto t=Clock::now();CHECK(unmapBo(&s,b,4096,&plan)==ArmSmmu4KSuccess);
            CHECK(mapBo(&s,&r,b,&plan)==ArmSmmu4KSuccess);m.small_ns+=nsSince(t);++m.small_maps;}
    }
    count(m,s);
    if(check) {
        std::vector<Bo *> all;
        for(Bo &b:w.bos)all.push_back(&b);
        const auto t1=Clock::now();
        CHECK(auditOk(s,all));
        m.audit_ns=nsSince(t1);
    }
    std::vector<size_t> order(w.bos.size());
    for(size_t i=0;i<order.size();++i)order[i]=i;
    rngState=0xfeedull;
    for(size_t i=order.size();i>1;--i)std::swap(order[i-1],order[rng()%i]);
    const auto t2=Clock::now();
    for(size_t i:order) {
        CHECK(unmapBo(&s,w.bos[i],eager?4096:0,&plan)==ArmSmmu4KSuccess);
        m.plans_pages+=plan.kind==ArmSmmu4KPagedTlbPages;m.plans_asid+=plan.kind==ArmSmmu4KPagedTlbAsid;m.plans_walk+=plan.walk;
        m.retired+=plan.retired;
    }
    m.unmap_ns=nsSince(t2);
    if(!eager) {
        static std::vector<uint64_t> retired(65536);
        CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&plan)==ArmSmmu4KSuccess);
        ArmSmmu4KPagedRelease(&s,retired.data(),plan.retired);
        m.retired+=plan.retired;
    }
    m.descriptor_reads=s.counters.descriptor_reads;m.descriptor_writes=s.counters.descriptor_writes;
    CHECK(s.counters.table_pages==1 && !s.counters.leaves);
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    m.spaces=1;
    return m;
}
/* P2b: the kernel space (its shared 0..1 GiB slot) and one 39-bit space per client process, each window [4 GiB, 8 GiB). */
static Measure replayPerProcess(bool check)
{
    Measure m;
    Workload w=peakWorkload(1,true);
    const size_t clients=peakClients().size();
    HostPages h(65536);
    h.strict=false;   /* the order checks scan the arena: the small tests carry them */
    ArmSmmu4KPagedSpace kernel=newSpace(h,39,0,GiB);
    std::vector<ArmSmmu4KPagedSpace> spaces;
    for(size_t c=0;c<clients;++c)spaces.push_back(newSpace(h,39,4*GiB,8*GiB));
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    const auto t0=Clock::now();
    for(size_t i=0;i<w.kernel;++i)CHECK(mapBo(&kernel,&r,w.bos[i],&plan)==ArmSmmu4KSuccess);
    uint64_t shared=0;
    CHECK(ArmSmmu4KPagedShare(&kernel,0,&shared)==ArmSmmu4KSuccess);
    for(auto &s:spaces)CHECK(ArmSmmu4KPagedLinkShared(&s,0,shared)==ArmSmmu4KSuccess);
    for(size_t i=w.kernel;i<w.bos.size();++i)CHECK(mapBo(&spaces[w.client_of[i]],&r,w.bos[i],&plan)==ArmSmmu4KSuccess);
    m.map_ns=nsSince(t0);
    for(const Bo &b:w.bos) {++m.objects;m.pages+=b.pages();}
    count(m,kernel);
    for(const auto &s:spaces)count(m,s);
    if(check) {
        std::vector<std::vector<Bo *>> per(clients);
        std::vector<Bo *> kernel_bos;
        for(size_t i=0;i<w.bos.size();++i)(i<w.kernel?kernel_bos:per[w.client_of[i]]).push_back(&w.bos[i]);
        CHECK(auditOk(kernel,kernel_bos));
        for(size_t c=0;c<clients;++c)CHECK(auditOk(spaces[c],per[c]) && lookup(spaces[c],0x100000)==w.bos[0].leaf(0));
    }
    for(auto &s:spaces)CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess);
    ArmSmmu4KPagedCancel(&kernel,&r);
    CHECK(ArmSmmu4KPagedTeardown(&kernel)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    m.spaces=1+clients;
    return m;
}
/* The worst case of 2.6: one tiny BO per 2 MiB region, 32768 of them (39-bit space). */
static Measure replayWorst()
{
    Measure m;
    physCounter=0x200000;
    HostPages h(40000);
    h.strict=false;   /* the order checks scan the arena: the small tests carry them */
    ArmSmmu4KPagedSpace s=newSpace(h,39);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    std::deque<Bo> bos;
    for(unsigned i=0;i<32768;++i) {bos.push_back(makeBo(0x20000000ull+uint64_t(i)*2*MiB,PG,{R|W,0,2},1));CHECK(mapBo(&s,&r,bos.back(),&plan)==ArmSmmu4KSuccess);}
    m.objects=bos.size();m.pages=bos.size();
    count(m,s);
    for(const Bo &b:bos)CHECK(unmapBo(&s,b,16,&plan)==ArmSmmu4KSuccess);
    CHECK(s.counters.table_pages==1);
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    return m;
}
/* The render server's working set over time (GPU-DAILY.md: 261 k -> 79 k ranges idle, back to 270 k): its peak mix, then 70 % of it
 * freed at random, cycles times, in its 3.5 GiB window with Turnip's best fit. policy 0 eager, 1 keep, 2 keep + a trim per shrink. */
struct Churn {uint64_t peak_tables=0,end_tables=0,maps=0,unmaps=0,map_pages=0,unmap_pages=0,map_ns=0,unmap_ns=0,pages_plans=0,asid_plans=0,walk_plans=0,trim_retired=0,peak_live_pages=0;};
static Churn replayChurn(int policy,unsigned cycles)
{
    Churn c;
    rngState=0xabcdefull;
    physCounter=0x300000;
    const auto ws=peakClients()[0];
    HostPages h(65536);
    h.strict=false;   /* the order checks scan the arena: the small tests carry them */
    ArmSmmu4KPagedSpace s=newSpace(h,36);
    VaHeap heap(ws.window_start,ws.window_bytes);
    ArmSmmu4KPagedReservation r{};
    ArmSmmu4KPagedPlan plan;
    std::deque<Bo> pool;
    std::vector<Bo *> live;
    static std::vector<uint64_t> retired(65536);
    std::vector<uint64_t> sizes;
    for(const auto &bucket:ws.buckets)for(unsigned i=0;i<bucket.count;++i)
        sizes.push_back(std::max<uint64_t>(PG,(bucket.mib*MiB/bucket.count+PG-1)&~(PG-1)));
    uint64_t live_pages=0;
    for(unsigned cycle=0;cycle<cycles;++cycle) {
        while(live.size()<sizes.size()) {
            Bo b=makeBo(0,sizes[rng()%sizes.size()],{R|W,0,2},1);
            if(!(b.iova=heap.take(b.bytes)))break;
            pool.push_back(b);
            const auto t=Clock::now();
            CHECK(mapBo(&s,&r,pool.back(),&plan)==ArmSmmu4KSuccess);
            c.map_ns+=nsSince(t);++c.maps;c.map_pages+=b.pages();
            live.push_back(&pool.back());live_pages+=b.pages();
        }
        c.peak_tables=std::max<uint64_t>(c.peak_tables,s.counters.table_pages);
        c.peak_live_pages=std::max(c.peak_live_pages,live_pages);
        for(size_t n=live.size()*7/10;n;--n) {
            const size_t at=rng()%live.size();
            Bo *b=live[at];
            const auto t=Clock::now();
            CHECK(unmapBo(&s,*b,policy==0?65536:0,&plan)==ArmSmmu4KSuccess);
            c.unmap_ns+=nsSince(t);++c.unmaps;c.unmap_pages+=b->pages();
            c.pages_plans+=plan.kind==ArmSmmu4KPagedTlbPages;c.asid_plans+=plan.kind==ArmSmmu4KPagedTlbAsid;c.walk_plans+=plan.walk;
            heap.give(b->iova,b->bytes);live_pages-=b->pages();
            live[at]=live.back();live.pop_back();
        }
        if(policy==2) {
            CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&plan)==ArmSmmu4KSuccess);
            ArmSmmu4KPagedRelease(&s,retired.data(),plan.retired);c.trim_retired+=plan.retired;
        }
    }
    c.end_tables=s.counters.table_pages;
    CHECK(auditOk(s,live));
    for(Bo *b:live)CHECK(unmapBo(&s,*b,0,&plan)==ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedTrim(&s,retired.data(),retired.size(),&plan)==ArmSmmu4KSuccess);
    ArmSmmu4KPagedRelease(&s,retired.data(),plan.retired);
    CHECK(s.counters.table_pages==1);
    ArmSmmu4KPagedCancel(&s,&r);
    CHECK(ArmSmmu4KPagedTeardown(&s)==ArmSmmu4KSuccess && !h.outstanding && !h.violations);
    return c;
}
static double mib(uint64_t pages) {return double(pages)*PG/double(MiB);}
static double slabMib(uint64_t pages) {return double((pages+511)/512)*2.0;}
static double perPage(uint64_t ns,uint64_t pages) {return pages?double(ns)/1000.0/double(pages):0.0;}
static void printReplay(unsigned cycles)
{
    std::printf("## P1: one global space (36-bit, today's 64 GiB windows), the gpu-daily3 peak\n\n");
    std::printf("| layout | objects | pages mapped | MiB mapped | table pages (L1/L2/L3) | table KiB | 2 MiB slabs | map us/page | "
                "small BO remap us/BO | unmap us/page | audit ms | unmap plans pages/ASID/walk | retired |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for(unsigned run_pages:{1u,0u})for(int eager=1;eager>=0;--eager) {
        const Measure m=replayGlobal(run_pages,eager,true);
        std::printf("| %s, %s | %llu | %llu | %.0f | %llu (%llu/%llu/%llu) | %.0f | %.0f MiB | %.4f | %.2f | %.4f | %.1f | %llu/%llu/%llu | %llu |\n",
            run_pages==1?"scattered 4 KiB runs":"contiguous (pool-like)",eager?"eager retire":"keep + trim",
            (unsigned long long)m.objects,(unsigned long long)m.pages,mib(m.pages),(unsigned long long)m.tables,
            (unsigned long long)m.level[1],(unsigned long long)m.level[2],(unsigned long long)m.level[3],mib(m.tables)*1024,slabMib(m.tables),
            perPage(m.map_ns,m.pages),m.small_maps?double(m.small_ns)/1000.0/double(m.small_maps):0.0,perPage(m.unmap_ns,m.pages),
            double(m.audit_ns)/1e6,(unsigned long long)m.plans_pages,(unsigned long long)m.plans_asid,(unsigned long long)m.plans_walk,
            (unsigned long long)m.retired);
    }
    const Measure p=replayPerProcess(true);
    std::printf("\n## P2b: per-process roots (39-bit, every client [4 GiB, 8 GiB), the kernel slot shared)\n\n| spaces | objects | pages | "
                "table pages (L1/L2/L3) | table KiB | 2 MiB slabs | map us/page |\n|---|---|---|---|---|---|---|\n");
    std::printf("| %llu | %llu | %llu | %llu (%llu/%llu/%llu) | %.0f | %.0f MiB | %.4f |\n",(unsigned long long)p.spaces,
        (unsigned long long)p.objects,(unsigned long long)p.pages,(unsigned long long)p.tables,(unsigned long long)p.level[1],
        (unsigned long long)p.level[2],(unsigned long long)p.level[3],mib(p.tables)*1024,slabMib(p.tables),perPage(p.map_ns,p.pages));
    const Measure worst=replayWorst();
    std::printf("\n## Worst case: 32768 BOs of 4 KiB, one per 2 MiB region (39-bit)\n\n| table pages (L1/L2/L3) | MiB |\n|---|---|\n"
                "| %llu (%llu/%llu/%llu) | %.1f |\n",(unsigned long long)worst.tables,(unsigned long long)worst.level[1],
                (unsigned long long)worst.level[2],(unsigned long long)worst.level[3],mib(worst.tables));
    std::printf("\n## Render-server churn: %u cycles of its peak mix (3279 BOs), 70 %% freed per cycle, best fit in its 3.5 GiB window\n\n"
                "| policy | peak table pages | end table pages | maps | unmaps | map us/page | unmap us/page | unmap plans pages/ASID/walk | "
                "trim retired | peak live MiB |\n|---|---|---|---|---|---|---|---|---|---|\n",cycles);
    const char *names[]={"eager retire","keep (no retire)","keep + trim per shrink"};
    for(int policy=0;policy<3;++policy) {
        const Churn c=replayChurn(policy,cycles);
        std::printf("| %s | %llu | %llu | %llu | %llu | %.4f | %.4f | %llu/%llu/%llu | %llu | %.0f |\n",names[policy],
            (unsigned long long)c.peak_tables,(unsigned long long)c.end_tables,(unsigned long long)c.maps,(unsigned long long)c.unmaps,
            perPage(c.map_ns,c.map_pages),perPage(c.unmap_ns,c.unmap_pages),(unsigned long long)c.pages_plans,(unsigned long long)c.asid_plans,
            (unsigned long long)c.walk_plans,(unsigned long long)c.trim_retired,mib(c.peak_live_pages));
    }
}

int main(int argc,char **argv)
{
    if(argc>1 && !std::strcmp(argv[1],"--replay")) {printReplay(argc>2?(unsigned)std::atoi(argv[2]):30);return 0;}
    size_t leaves=0;
    uint64_t ops=0;
    const unsigned profiles=testEquality(&leaves);
    const unsigned failures=testFailures();
    const unsigned shared=testSharedRoots();
    const unsigned plans=testPlans();
    const unsigned audits=testAudit();
    const unsigned churns=testChurn(&ops);
    const Measure global=replayGlobal(1,true,true),pool=replayGlobal(0,false,true),per=replayPerProcess(true),worst=replayWorst();
    CHECK(global.tables==pool.tables && global.objects==5328);
    const Churn churn=replayChurn(0,4);
    CHECK(churn.end_tables<=churn.peak_tables);
    std::printf("PASS: paged native4K kit: %u PTE profiles byte-equal to ArmSmmu4KBuild at 32/36/39 bits and accepted by the live audit "
                "(%zu leaves, 7 classes); %u reserve/commit failure cases; %u shared-root checks; %u TLB plan checks; %u audit findings; "
                "%u churn runs (%llu operations, every table page back); gpu-daily3 peak replay %llu objects %llu MiB: %llu table pages "
                "global, %llu per process (%llu spaces), worst case %llu\n",
        profiles,leaves,failures,shared,plans,audits,churns,(unsigned long long)ops,(unsigned long long)global.objects,
        (unsigned long long)(global.pages*PG/MiB),(unsigned long long)global.tables,(unsigned long long)per.tables,
        (unsigned long long)per.spaces,(unsigned long long)worst.tables);
    return 0;
}
