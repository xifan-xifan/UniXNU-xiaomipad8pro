/* SPDX-License-Identifier: MIT */
/* UniXNU-piano (2026-11-23, GPU-PAGED P1, private host-checks/20261122-gpu-paged/P1-GATE.md): the GPU VM's paged mode
 * (piano-gpu-mem=paged) over the IOKit doubles and the A830 SMMU model (its TLB I/O: single 64-bit TLBIVAL, 32-bit TLBIASID and
 * TLBSYNC). What it shows: the paged root encodes the same context image as the live table pool but for TTBR0; maps write exactly the
 * kit's leaves from each BO's spans, read no SMMU register and keep one record per mapping; reserve-then-commit refusals (table
 * pages, the mapped-bytes cap, the render server's reserve) write nothing and never poison; unmaps never unlink a table and invalidate
 * by page up to 512 pages, else by ASID; the tick's trim unlinks emptied tables, issues one TLBIASID + TLBSYNC and frees the pages only
 * after a successful sync; findAddress, largestClientHole, handleAt and locate over the records; the table audit; poisoning keeps every
 * page. Then the replay of the gpu-paged0c H-on peak (console.out:48282: 54 clients, 7625 objects, 4003 MiB by client, all scattered
 * one page per span, as paged mode backs them) plus the Metal image check's 96 + 187 MiB: refusals, table pages, chunks, records, the
 * TLB plans of unmapping it all, the trim. `test_p1vm --replay` runs the peak at full size (about 4.3 GiB of host memory); the suite
 * runs it at 1/16 of the bytes with every object kept. No hardware is touched. */
#include "XnuMemoryFixture.h"
#include "A830SmmuModel.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoSmmuContext.h"
#include "NativeAdrenoPagedLine.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
extern "C" {extern uint64_t (*native_adreno_vm_now_us)(void);}
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
static unsigned tableCleans;
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    ++fixture.cache_calls;if(bytes==4096 && op==NativeAdrenoClean)++tableCleans;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
static uint64_t nowUs() {return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}

static A830SmmuModel model;
struct Programmer {IOReturn verify=kIOReturnSuccess;unsigned verifies=0,faults=0,tlb_reads=0,tlb_limit=0;};
static Programmer prog;
static int tableRead32(void *cookie,size_t at,uint32_t *v)
{
    if((at&0xFFF)==0x7F4 && prog.tlb_limit && ++prog.tlb_reads>prog.tlb_limit)return 1;   /* the sync's budget ran out */
    return A830SmmuModel::read32(cookie,at,v);
}
static ArmSmmu4KIo tableIo;
static IOReturn verify(void *,unsigned bank,const ArmSmmu4KStream *s,size_t n)
{CHECK(bank==0 && s && n==2 && s[0].sid==0 && s[1].sid==1);++prog.verifies;return prog.verify;}
static IOReturn faultStreams(void *,unsigned bank,const ArmSmmu4KStream *,size_t n)
{CHECK(bank==0 && n==2);++prog.faults;return kIOReturnSuccess;}
static IOReturn unused(void *,unsigned,const ArmSmmu4KStream *) {CHECK(0);return kIOReturnUnsupported;}
static IOReturn unusedProgram(void *,unsigned,const ArmSmmu4KContextImage *,uint64_t,NativeAdrenoSmmuProgramReport *) {CHECK(0);return 1;}
static IOReturn unusedDisable(void *,unsigned) {CHECK(0);return kIOReturnUnsupported;}
static NativeAdrenoSmmuProgrammer programmer;
static const ArmSmmu4KStream routes[2]={{0,0},{3,1}};
static unsigned checks;
#define OK(x) do {CHECK(x);++checks;} while(0)
/* The refusal reason is recorded in macOS builds only (NativeAdrenoVmRefusal). */
#ifdef ADRENO830_MACOS
#define WHY(got,want) ((got)==NativeAdrenoVmWhy##want)
#else
#define WHY(got,want) ((void)(got),true)
#endif

static void smmuReset(bool sync_after_map)
{
    model.boot(91);prog=Programmer{};
    tableIo=model.io;tableIo.read32=tableRead32;
    programmer={&prog,&tableIo,0x4C017E18,0x40000015,0x5511,unused,unusedProgram,unused,verify,faultStreams,unusedDisable,sync_after_map};
}
/* The board's bring-up order for paged mode: preparePaged (root), the context's image on it, then attachPaged. */
struct Paged {
    NativeAdrenoVm vm;
    NativeAdrenoSmmuContext smmu;
    uint64_t root=0;
    std::vector<uint32_t> scratch=std::vector<uint32_t>(size_t(1)<<16);
};
static NativeAdrenoVmPagedConfig config(uint64_t aperture=UINT64_C(64)<<30,uint64_t cap=0,uint64_t reserve=0,size_t table_cap=0,
    Paged *p=nullptr)
{
    NativeAdrenoVmPagedConfig c;
    c.input_bits=36;c.output_bits=36;   /* SM8750: IDR2 0x5511, OAS 36 */c.aperture=aperture;
    if(cap)c.cap_bytes=cap;
    if(reserve)c.reserve_bytes=reserve;
    if(table_cap)c.table_cap_pages=table_cap;
    if(p) {c.alias_scratch=p->scratch.data();c.alias_scratch_entries=p->scratch.size();}
    return c;
}
static IOReturn up(Paged &p,const NativeAdrenoVmPagedConfig &c,bool sync_after_map=false)
{
    smmuReset(sync_after_map);
    IOReturn status=p.vm.preparePaged(c,&p.root);
    if(status)return status;
    status=p.smmu.preparePaged(p.root,c.aperture,size_t(c.aperture/4096),0,1,4000,c.output_bits,c.input_bits);
    if(status)return status;
    const NativeAdrenoSmmuBinding binding{&programmer,p.smmu.configHandle(),0,A830SmmuModel::cb(0),routes,2};
    status=p.vm.attachPaged(binding,1000);
    if(!status)status=p.vm.reserveWindow(0,0x100000);
    if(!status)status=p.vm.reserveWindow(0x4000000,0xC00000);
    return status;
}
static void down(Paged &p)
{
    CHECK(p.vm.objectCount()==0 && !p.vm.detach() && !p.vm.releasePaged());
    CHECK(fixture.live.empty() && fixture.metadata.empty());
}
static IOReturn clientMap(NativeAdrenoVm &vm,NativeAdrenoMemory *m,uint64_t iova,unsigned perms,uint64_t *handle,bool render,
    uint8_t *why=nullptr)
{
#ifdef ADRENO830_MACOS
    NativeAdrenoVmRefusal r{};
    const IOReturn status=vm.mapClient(m,iova,perms,0,2,handle,render?0:NativeAdrenoVmRenderReserveObjects,
        render?0:NativeAdrenoVmRenderReserveSpans,&r);
    if(why)*why=r.why;
    return status;
#else
    if(why)*why=0;
    return vm.mapClient(m,iova,perms,0,2,handle,render?0:NativeAdrenoVmRenderReserveObjects,render?0:NativeAdrenoVmRenderReserveSpans);
#endif
}
static NativeAdrenoVmPagedStats stats(const NativeAdrenoVm &vm) {NativeAdrenoVmPagedStats s;vm.pagedStats(&s);return s;}
/* A poisoned VM keeps every page and reference by design; the host frees the doubles' storage by hand. */
static void scrap()
{
    for(auto *d:fixture.live) {std::free(d->raw);delete d;}
    fixture.live.clear();
    for(void *m:fixture.metadata)std::free(m);
    fixture.metadata.clear();
}

/* The paged root's image: the live pool's TCR/MAIR/CBAR at the same input size, TTBR0 = the root and the ASID. */
static void image()
{
    Paged p;
    OK(up(p,config())==kIOReturnSuccess && p.root && !(p.root&4095) && prog.verifies==1 && p.vm.attached() && p.vm.paged());
    ArmSmmu4KContextConfig paged=*p.smmu.configuration();paged.id0=0x4C017E18;paged.id2=0x5511;
    ArmSmmu4KContextImage a{},b{};
    OK(ArmSmmu4KEncodeContext(&paged,&a)==ArmSmmu4KSuccess && (a.ttbr0&UINT64_C(0x0000fffffffff000))==p.root && a.ttbr0>>48==1);
    NativeAdrenoMemory tables;
    NativeAdrenoSmmuContext live;
    CHECK(!tables.allocate(64*4096,36,true,64*4096));
    OK(!live.prepare(&tables,0,0,UINT64_C(64)<<20,16384,0,1,4000,36,36));
    ArmSmmu4KContextConfig pool=*live.configuration();pool.id0=0x4C017E18;pool.id2=0x5511;
    OK(ArmSmmu4KEncodeContext(&pool,&b)==ArmSmmu4KSuccess);
    OK(a.tcr==b.tcr && a.tcr2==b.tcr2 && a.mair0==b.mair0 && a.mair1==b.mair1 && a.cbar==b.cbar && a.cba2r==b.cba2r &&
       a.sctlr==b.sctlr && a.ttbr1==b.ttbr1 && (a.ttbr0>>48)==(b.ttbr0>>48) && a.ttbr0!=b.ttbr0);
    /* the root page is the first of the first chunk; a second preparePaged is refused */
    uint64_t again=0;
    OK(p.vm.preparePaged(config(),&again)==kIOReturnBusy && !again);
    const NativeAdrenoVmPagedStats s=stats(p.vm);
    OK(s.paged && s.table_pages==1 && s.chunks==1 && s.chunk_pages==512 && s.free_pages==511 && !s.records);
    (void)tables.retireAfterQuiescence();(void)tables.release();
    down(p);
}
/* Maps: the kit's leaves from the spans, no SMMU register, one record each; TLB plans of unmaps; the kernel buffer path. */
static void transactions()
{
    Paged p;
    fixture.mode=ScatterUnlessContiguous;
    CHECK(up(p,config(UINT64_C(64)<<30,0,0,0,&p))==kIOReturnSuccess);
    /* a kernel buffer as the GPU context maps them: privileged read/write at 0x300000, contiguous */
    NativeAdrenoMemory memptrs;
    CHECK(!memptrs.allocate(4096,40,true,4096));
    uint64_t kh=0;
    OK(p.vm.map(&memptrs,0,0x300000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,0,2,&kh)==kIOReturnSuccess && kh);
    uint64_t refused=1;
    OK(p.vm.map(&memptrs,1,0x400000,ArmSmmu4KRead,0,2,&refused)==kIOReturnBadArgument && !refused);   /* no upper region in paged mode */
    /* a scattered own BO of 300 pages (one span per page) */
    NativeAdrenoMemory bo;
    CHECK(!bo.allocateUser(300*4096,40,300*4096,true) && bo.spanCount()==300);
    uint64_t iova=0,h=0;
    OK(!p.vm.findAddress(0,300*4096,4096,&iova) && iova==0x100000);   /* above the null guard, below the memptrs page */
    OK(!p.vm.findAddress(0,600*4096,4096,&iova) && iova==0x301000);   /* past the memptrs page: 2 MiB below it are too few */
    iova=UINT64_C(0x100000000);
    const unsigned reads=model.reads,writes=model.writes;
    OK(clientMap(p.vm,&bo,iova,ArmSmmu4KRead|ArmSmmu4KWrite,&h,true)==kIOReturnSuccess && h);
    OK(model.reads==reads && model.writes==writes);   /* no register touched by a map (no sync_after_map here) */
    OK(p.vm.objectCount()==2 && p.vm.spanCount()==2 && bo.deviceReferences()==1);
    const ArmSmmu4KPagedAttrs attrs={ArmSmmu4KRead|ArmSmmu4KWrite,0,2};
    bool leaves=true;
    for(size_t i=0;i<300;++i)leaves=leaves && p.vm.pagedLeaf(iova+i*4096)==ArmSmmu4KPagedLeaf(&attrs,bo.spans()[i].physical);
    OK(leaves && !p.vm.pagedLeaf(iova+300*4096) && !p.vm.pagedLeaf(iova-4096));
    OK(p.vm.handleAt(iova+4096*17)==h && !p.vm.handleAt(iova+300*4096) && p.vm.handleAt(0x300000)==kh);
#ifdef ADRENO830_MACOS
    NativeAdrenoVmPlace over{},below{};
    OK(!p.vm.locate(iova+4096*5,&over,&below) && over.handle==h && over.bytes==300*4096 && over.kind==NativeAdrenoVmKindOwn &&
       below.handle==kh);
    OK(p.vm.largestClientHole(iova,UINT64_C(1)<<30)==(UINT64_C(1)<<30)-300*4096);
#endif
    /* overlap with a record, the null guard, the GMEM window, out of the aperture */
    NativeAdrenoMemory two;
    CHECK(!two.allocateUser(2*4096,40,2*4096,true));
    uint64_t h2=0;
    OK(clientMap(p.vm,&two,iova+299*4096,3,&h2,true)==kIOReturnBusy && !h2);
    OK(clientMap(p.vm,&two,0x80000,3,&h2,true)==kIOReturnBadArgument && clientMap(p.vm,&two,0x4000000,3,&h2,true)==kIOReturnBadArgument);
    OK(clientMap(p.vm,&two,(UINT64_C(64)<<30)-4096,3,&h2,true)==kIOReturnBadArgument && two.deviceReferences()==0 && !p.vm.poisoned());
    /* unmap: 300 pages by TLBIVAL with ASID 1, one TLBSYNC; the L3 it emptied waits for the tick */
    model.tlbival.clear();
    const unsigned syncs=model.tlbsync,asids=model.tlbiasid;
    OK(p.vm.unmap(h)==kIOReturnSuccess && model.tlbival.size()==300 && model.tlbival[0]==(uint64_t(1)<<48|(iova>>12)) &&
       model.tlbsync==syncs+1 && model.tlbiasid==asids && model.violations==0 && model.aborts==0);
    OK(!p.vm.pagedLeaf(iova) && p.vm.objectCount()==1 && p.vm.spanCount()==1 && bo.deviceReferences()==0);
    NativeAdrenoVmPagedStats s=stats(p.vm);
    OK(s.unmaps==1 && s.unmap_val==1 && s.unmap_val_pages==300 && s.kept_empty==1 && s.trim_pending==1 && s.table_pages==5);
    /* the tick: the L3 and the L2 above it unlinked, one TLBIASID + TLBSYNC, then the pages are free again */
    const size_t freeBefore=s.free_pages;
    size_t retired=0;
    OK(!p.vm.trimTables(&retired) && retired==2 && model.tlbiasid==asids+1 && model.tlbsync==syncs+2);
    s=stats(p.vm);
    OK(s.table_pages==3 && s.free_pages==freeBefore+2 && s.trim_retired==2 && !s.trim_pending && s.trims==1);
    OK(!p.vm.trimTables(&retired) && !retired && model.tlbiasid==asids+1);   /* nothing pending: no TLB operation */
    /* 600 pages: the ASID */
    NativeAdrenoMemory big;
    CHECK(!big.allocateUser(600*4096,40,600*4096,true));
    OK(clientMap(p.vm,&big,iova,3,&h,true)==kIOReturnSuccess);
    model.tlbival.clear();
    OK(p.vm.unmap(h)==kIOReturnSuccess && model.tlbival.empty() && model.tlbiasid==asids+2);
    OK(!p.vm.trimTables(&retired) && retired==3);   /* two L3s and their L2 */
    /* the audit over a mixed set: a kernel buffer, a scattered BO, an import */
    CHECK(clientMap(p.vm,&bo,iova,3,&h,true)==kIOReturnSuccess);
    NativeAdrenoMemory imp;
    CHECK(!imp.importTask(reinterpret_cast<void *>(2),0x20000000,64*4096,40,64*4096));
    uint64_t hi=0;
    OK(clientMap(p.vm,&imp,iova+UINT64_C(0x10000000),ArmSmmu4KRead,&hi,true)==kIOReturnSuccess);
    ArmSmmu4KPagedAuditResult audit{};
    OK(!p.vm.auditTables(&audit) && audit.why==ArmSmmu4KPagedAuditOk && audit.leaves==1+300+64);
    s=stats(p.vm);
    OK(s.audits==1 && !s.audit_failures && s.records==3);
    char line[200];
    OK(NativeAdrenoPagedLine(0,s,line,sizeof line)>0 && !strcmp(line,"pt mode=paged"));
    OK(NativeAdrenoPagedLine(1,s,line,150)>0 && strstr(line,"pt: tables ") && strlen(line)<150);
    OK(NativeAdrenoPagedLine(2,s,line,150)>0 && strstr(line,"pt2: maps ") && NativeAdrenoPagedLine(4,s,line,sizeof line)==0);
    CHECK(!p.vm.unmap(h) && !p.vm.unmap(hi) && !p.vm.unmap(kh));
    CHECK(!p.vm.trimTables(&retired));
    CHECK(!bo.release() && !two.release() && !big.release() && !imp.release() && !memptrs.release());
    down(p);
    fixture.mode=Contiguous;
}
/* The annibale programmer's sync_after_map: one TLBIASID + TLBSYNC after each map, none for a refused one. */
static void syncAfterMap()
{
    Paged p;
    CHECK(up(p,config(),true)==kIOReturnSuccess);
    NativeAdrenoMemory bo;
    CHECK(!bo.allocateUser(4*4096,40,4*4096,true));
    uint64_t h=0;
    const unsigned asids=model.tlbiasid,syncs=model.tlbsync;
    OK(clientMap(p.vm,&bo,UINT64_C(0x100000000),3,&h,true)==kIOReturnSuccess && model.tlbiasid==asids+1 && model.tlbsync==syncs+1);
    NativeAdrenoMemory other;
    CHECK(!other.allocateUser(4096,40,4096,true));
    uint64_t ho=0;
    OK(clientMap(p.vm,&other,UINT64_C(0x100000000),3,&ho,true)==kIOReturnBusy && model.tlbiasid==asids+1 && !ho);   /* an overlap */
    CHECK(!p.vm.unmap(h) && !bo.release() && !other.release());
    down(p);
}
/* Reserve, then commit: the table cap and an empty free list whose growth fails are clean NoResources: no leaf, no reference, no
 * poison; the map succeeds once the list can grow. */
static void reserveCommit()
{
    {
        Paged p;
        CHECK(up(p,config(UINT64_C(64)<<30,0,0,4))==kIOReturnSuccess);   /* 4 table pages: the root + 3 */
        NativeAdrenoMemory a,b;
        CHECK(!a.allocateUser(4096,40,4096,true) && !b.allocateUser(4096,40,4096,true));
        uint64_t ha=0,hb=0;uint8_t why=0;
        OK(clientMap(p.vm,&a,UINT64_C(0x100000000),3,&ha,true)==kIOReturnSuccess);   /* L2 + L3 */
        OK(clientMap(p.vm,&b,UINT64_C(0x140000000),3,&hb,true,&why)==kIOReturnNoResources && !hb && !b.deviceReferences() &&
           !p.vm.pagedLeaf(UINT64_C(0x140000000)) && !p.vm.poisoned());
        OK(WHY(why,Tables));
        OK(stats(p.vm).refused_tables>=1 && p.vm.objectCount()==1);
        CHECK(!p.vm.unmap(ha) && !p.vm.trimTables());
        OK(clientMap(p.vm,&b,UINT64_C(0x140000000),3,&hb,true)==kIOReturnSuccess);   /* the trimmed pages serve it */
        CHECK(!p.vm.unmap(hb) && !a.release() && !b.release());
        down(p);
    }
    {
        Paged p;
        fixture.contiguous_limit=128*1024;   /* only 128 KiB chunks: 32 pages each */
        CHECK(up(p,config())==kIOReturnSuccess);
        OK(stats(p.vm).chunk_pages==64 && stats(p.vm).chunks==2);   /* preparePaged grew to the spare (64), then took the root */
        const size_t n=240;
        std::vector<NativeAdrenoMemory> bos(n);
        std::vector<uint64_t> handles(n,0);
        for(auto &b:bos)CHECK(!b.allocateUser(4096,40,4096,true));
        size_t mapped=0;IOReturn status=kIOReturnSuccess;
        for(;mapped<n;++mapped) {
            if(mapped==40)fixture.allocation_fail=true;   /* from here no chunk can be had */
            status=clientMap(p.vm,&bos[mapped],UINT64_C(0x100000000)+mapped*(UINT64_C(2)<<20),3,&handles[mapped],true);
            if(status)break;
        }
        fixture.allocation_fail=false;
        const NativeAdrenoVmPagedStats s=stats(p.vm);
        OK(status==kIOReturnNoResources && mapped>40 && mapped<n && !handles[mapped] && !bos[mapped].deviceReferences() &&
           !p.vm.pagedLeaf(UINT64_C(0x100000000)+mapped*(UINT64_C(2)<<20)) && !p.vm.poisoned() && s.grow_failures>=1 &&
           s.grow_retries>=1 && !s.free_pages);
        OK(clientMap(p.vm,&bos[mapped],UINT64_C(0x100000000)+mapped*(UINT64_C(2)<<20),3,&handles[mapped],true)==kIOReturnSuccess);
        for(size_t i=0;i<=mapped;++i)CHECK(!p.vm.unmap(handles[i]));
        for(auto &b:bos)CHECK(!b.release());
        size_t retired=0;
        while(!p.vm.trimTables(&retired) && (retired || stats(p.vm).trim_pending)) {}
        OK(stats(p.vm).table_pages==1 && !stats(p.vm).trim_full);   /* the root alone: 241 neighbouring 2 MiB blocks merged into one range */
        fixture.contiguous_limit=0;
        down(p);
    }
}
/* Bytes, not ranges: every client under the cap, every one but the render server leaves the reserve. */
static void capReserve()
{
    Paged p;
    CHECK(up(p,config(UINT64_C(64)<<30,UINT64_C(64)<<20,UINT64_C(16)<<20))==kIOReturnSuccess);
    std::vector<NativeAdrenoMemory> bos(10);
    std::vector<uint64_t> h(10,0);
    for(auto &b:bos)CHECK(!b.allocateUser(UINT64_C(8)<<20,40,UINT64_C(8)<<20,true));
    uint8_t why=0;
    size_t i=0;
    for(;i<10;++i)if(clientMap(p.vm,&bos[i],UINT64_C(0x100000000)+i*(UINT64_C(16)<<20),3,&h[i],false,&why))break;
    OK(i==6 && WHY(why,ReserveBytes) && !bos[6].deviceReferences() && stats(p.vm).refused_reserve==1);   /* 48 MiB mapped + 8 > 64 - 16 */
    for(;i<10;++i)if(clientMap(p.vm,&bos[i],UINT64_C(0x100000000)+i*(UINT64_C(16)<<20),3,&h[i],true,&why))break;
    OK(i==8 && WHY(why,Cap) && stats(p.vm).refused_cap==1 && stats(p.vm).mapped==UINT64_C(64)<<20 && !p.vm.poisoned());
    for(size_t k=0;k<8;++k)CHECK(!p.vm.unmap(h[k]));
    for(auto &b:bos)CHECK(!b.release());
    CHECK(!p.vm.trimTables());
    down(p);
}
/* The tick's trim releases only after its TLBSYNC: a sync that fails poisons the VM and keeps every unlinked page; the streams fault. */
static void trimOrder()
{
    Paged p;
    CHECK(up(p,config())==kIOReturnSuccess);
    NativeAdrenoMemory bo;
    CHECK(!bo.allocateUser(4096,40,4096,true));
    uint64_t h=0;
    CHECK(clientMap(p.vm,&bo,UINT64_C(0x100000000),3,&h,true)==kIOReturnSuccess && !p.vm.unmap(h));
    const NativeAdrenoVmPagedStats before=stats(p.vm);
    prog.tlb_reads=0;prog.tlb_limit=1;   /* the trim's TLBSYNC never completes */
    OK(p.vm.trimTables()==kIOReturnIOError && p.vm.poisoned() && prog.faults==1);
    const NativeAdrenoVmPagedStats after=stats(p.vm);
    OK(after.free_pages==before.free_pages && !after.trim_retired && after.poisoned);   /* unlinked, never given back */
    OK(clientMap(p.vm,&bo,UINT64_C(0x100000000),3,&h,true)==kIOReturnNotReady);
    prog.tlb_limit=0;
    OK(!bo.release() && p.vm.detach()==kIOReturnBusy && p.vm.releasePaged()==kIOReturnBusy);   /* nothing is given back */
    scrap();
}
/* P1b: one tick's trim stops once its budget is spent and the next tick goes on; neighbouring unmaps share a 2 MiB-block range;
 * more pending ranges than slots start a sweep that also resumes tick by tick. */
static uint64_t fakeUs;
static uint64_t fakeClock() {return fakeUs+=100;}   /* every reading 100 us later */
static void trimBudget()
{
    Paged p;
    CHECK(up(p,config())==kIOReturnSuccess);
    /* 100 one-page BOs in one 2 MiB block, unmapped: one pending range */
    std::vector<NativeAdrenoMemory> small(100);
    std::vector<uint64_t> hs(100,0);
    for(size_t i=0;i<100;++i) {CHECK(!small[i].allocateUser(4096,40,4096,true));CHECK(!clientMap(p.vm,&small[i],UINT64_C(0x100000000)+i*4096,3,&hs[i],true));}
    for(size_t i=0;i<100;++i)CHECK(!p.vm.unmap(hs[i]) && !small[i].release());
    OK(stats(p.vm).trim_pending==1);
    size_t r=0;
    OK(!p.vm.trimTables(&r) && r==2 && !stats(p.vm).trim_pending);
    /* 600 BOs in 600 separate 2 MiB blocks spread over 75 GiB-slots... the slots overflow: a sweep */
    const size_t n=600;
    std::vector<NativeAdrenoMemory> bos(n);
    std::vector<uint64_t> h(n,0);
    for(size_t i=0;i<n;++i) {
        CHECK(!bos[i].allocateUser(4096,40,4096,true));
        CHECK(!clientMap(p.vm,&bos[i],UINT64_C(0x100000000)+uint64_t(i)*(UINT64_C(64)<<20),3,&h[i],true));
    }
    for(size_t i=0;i<n;i+=2)CHECK(!p.vm.unmap(h[i]));   /* every other one: no two neighbours */
    for(size_t i=1;i<n;i+=2)CHECK(!p.vm.unmap(h[i]));
    for(auto &b:bos)CHECK(!b.release());
    NativeAdrenoVmPagedStats s=stats(p.vm);
    OK(s.trim_pending==UINT64_MAX && s.trim_full==1);   /* sweeping */
    native_adreno_vm_now_us=fakeClock;fakeUs=0;
    size_t ticks=0,total=0;
    while(!p.vm.trimTables(&r) && (r || stats(p.vm).trim_pending)) {++ticks;total+=r;CHECK(ticks<10000);}
    native_adreno_vm_now_us=nullptr;
    s=stats(p.vm);
    /* 64 GiB in 128 MiB slices, at most 5 slices a tick (100 us a reading, 500 us budget) */
    OK(ticks>=512/5 && total>=n && s.table_pages==1 && s.trim_us_max<=600 && !s.trim_pending);
    down(p);
}
/* A table page whose publication fails poisons the VM (no later map, every reference kept). */
static void publishFailure()
{
    Paged p;
    CHECK(up(p,config())==kIOReturnSuccess);
    NativeAdrenoMemory bo;
    CHECK(!bo.allocateUser(4096,40,4096,true));
    uint64_t h=0;
    fixture.cache_fail_call=fixture.cache_calls+1;
    OK(clientMap(p.vm,&bo,UINT64_C(0x100000000),3,&h,true)==kIOReturnIOError && p.vm.poisoned() && bo.deviceReferences()==1 &&
       bo.quarantined());
    fixture.cache_fail_call=0;
    OK(p.vm.detach()==kIOReturnBusy && p.vm.releasePaged()==kIOReturnBusy);
    scrap();
}

/* ---- the gpu-paged0c H-on peak (console.out:48282-48300) ------------------------------------------------------------------------- */
static const char *const kPeak=
    " 0:WindowServer 1463/3584M pk2500 4731o 1:iconservicesag 21/2048M pk21 39o 2:iconservicesag 119/1024M pk1024 1398o"
    " 4:NotificationCe 1/1024M pk1 3o 5:Finder 1/1024M pk1 3o 6:loginwindow 1/1024M pk1 3o 7:CursorUIViewSe 1/1024M pk1 3o"
    " 8:avconferenced 1/1024M pk1 3o 9:KeyboardSetupA 25/1024M pk25 27o 10:TextInputMenuA 1/1024M pk1 3o"
    " 11:MenuBarAgent 1/1024M pk1 3o 12:ControlCenter 22/1024M pk26 61o 13:Siri_AI 89/1024M pk1013 83o"
    " 14:WallpaperAeria 1/1024M pk1 3o 15:VTDecoderXPCSe 94/1024M pk192 22o 16:TextInputSwitc 1/1024M pk1 3o"
    " 17:com.apple.dock 53/1024M pk63 244o 18:metal-hold 29/1024M 14o 19:metal-hold 29/1024M 14o 20:metal-hold 29/1024M 14o"
    " 21:metal-hold 29/1024M 14o 22:metal-hold 29/1024M 14o 23:metal-hold 29/1024M 14o 24:metal-hold 29/1024M 14o"
    " 25:metal-hold 29/1024M 14o 26:metal-hold 29/1024M 14o 27:metal-hold 29/1024M 14o 28:metal-hold 29/1024M 14o"
    " 29:metal-hold 29/1024M 14o 30:metal-hold 29/1024M 14o 31:metal-hold 29/1024M 14o 32:metal-hold 29/1024M 14o"
    " 33:metal-hold 29/1024M 14o 34:metal-hold 29/1024M 14o 35:metal-hold 29/1024M 14o 36:metal-hold 29/1024M 14o"
    " 37:metal-hold 29/1024M 14o 38:metal-hold 29/1024M 14o 39:metal-hold 29/1024M 14o 40:metal-hold 29/1024M 14o"
    " 41:metal-hold 29/1024M 14o 42:metal-hold 29/1024M 14o 43:metal-hold 29/1024M 14o 44:metal-hold 29/1024M 14o"
    " 45:metal-hold 29/1024M 14o 46:metal-hold 29/1024M 14o 47:metal-hold 29/1024M 14o 48:metal-hold 709/1024M 34o"
    " 49:Preview 155/1024M 52o 50:com.apple.appk 1/1024M 3o 51:Photos 1/1024M 3o 52:com.apple.appk 1/1024M 3o"
    " 53:Safari 1/1024M 3o 54:Maps 370/1024M 475o";
/* gpu va/sizes (the same window): per class count/MiB: <1M, 1-16M, 16-64M, 64M+, imports */
struct Sizes {unsigned slot;unsigned n[5],mib[5];};
static const Sizes kSizes[4]={{0,{4437,12,31,0,251},{234,83,864,0,284}},{48,{10,2,22,0,0},{1,8,700,0,0}},
                              {49,{37,5,0,0,10},{1,24,0,0,131}},{54,{445,6,13,0,11},{11,32,286,0,42}}};
struct Bo {NativeAdrenoMemory memory;uint64_t handle=0;unsigned slot=0;};
static unsigned replay(bool full)
{
    struct Client {unsigned slot,mib,objects;};
    std::vector<Client> clients;
    for(const char *c=kPeak;(c=strchr(c,':'));) {
        const char *s=c;while(s>kPeak && s[-1]>='0' && s[-1]<='9')--s;
        Client k{};k.slot=unsigned(atoi(s));
        const char *m=strchr(c,' ');k.mib=unsigned(atoi(m+1));
        const char *o=strchr(m+1,'o');const char *os=o;while(os[-1]>='0' && os[-1]<='9')--os;k.objects=unsigned(atoi(os));
        clients.push_back(k);c=o;
    }
    CHECK(clients.size()==54);
    unsigned objects=0,mib=0;for(const auto &k:clients) {objects+=k.objects;mib+=k.mib;}
    CHECK(objects==7625 && mib==4003);
    const unsigned scale=full?1:16;
    /* every object's size: the class mean of its size line, else the client's mean; rounded up to 4 KiB; imports apart */
    struct Plan {unsigned slot;uint64_t bytes;bool import;};
    std::vector<Plan> plan;
    for(const auto &k:clients) {
        const Sizes *z=nullptr;for(const auto &q:kSizes)if(q.slot==k.slot)z=&q;
        auto add=[&](unsigned n,uint64_t bytes,bool imp) {
            bytes=(bytes/scale+4095)&~uint64_t(4095);if(!bytes)bytes=4096;
            for(unsigned i=0;i<n;++i)plan.push_back({k.slot,bytes,imp});
        };
        if(z)for(unsigned c=0;c<5;++c) {if(z->n[c])add(z->n[c],(uint64_t(z->mib[c])<<20)/z->n[c],c==4);}
        else add(k.objects,(uint64_t(k.mib)<<20)/k.objects,false);
    }
    CHECK(plan.size()==7625);
    /* interleave the clients as they allocate (round robin over the plan in slot order) */
    std::vector<Plan> order;order.reserve(plan.size());
    {
        std::vector<std::vector<Plan>> by(64);for(const auto &q:plan)by[q.slot].push_back(q);
        for(size_t round=0;order.size()<plan.size();++round)for(auto &v:by)if(round<v.size())order.push_back(v[round]);
    }
    Paged p;
    fixture.mode=ScatterUnlessContiguous;
    CHECK(up(p,config(UINT64_C(64)<<30,0,0,0,&p),true)==kIOReturnSuccess);   /* the annibale programmer: sync_after_map */
    /* the six kernel buffers (contiguous) */
    static const uint64_t kIova[6]={0x100000,0x180000,0x200000,0x300000,0x301000,0x302000};
    static const size_t kBytes[6]={0x1C000,0x7000,0x8000,0x1000,0x1000,0x1000};
    static const unsigned kPerm[6]={5,5,9,11,11,11};
    std::vector<NativeAdrenoMemory> kernel(6);
    std::vector<uint64_t> kh(6);
    for(unsigned i=0;i<6;++i) {
        fixture.mode=Contiguous;
        CHECK(!kernel[i].allocate(kBytes[i],40,true,kBytes[i]));
        CHECK(!p.vm.map(&kernel[i],0,kIova[i],kPerm[i],0,2,&kh[i]));
    }
    fixture.mode=ScatterUnlessContiguous;
    /* windows: WindowServer 3.5 GiB at 0x20000000, every other slot 1 GiB from 4 GiB; a 16 KiB-aligned bump per window */
    auto base=[](unsigned slot) {return slot?UINT64_C(0x100000000)+uint64_t(slot-1)*(UINT64_C(1)<<30):UINT64_C(0x20000000);};
    std::vector<uint64_t> next(64);for(unsigned s=0;s<64;++s)next[s]=base(s);
    std::vector<Bo> bos(order.size()+2+256);
    size_t used=0,refusals=0;
    uint64_t mapUs=0,pages=0;
    const uint64_t t0=nowUs();
    auto mapOne=[&](unsigned slot,uint64_t bytes,bool import,uint8_t *why)->IOReturn {
        Bo &b=bos[used];
        IOReturn status=import?b.memory.importTask(reinterpret_cast<void *>(uintptr_t(2+slot)),UINT64_C(0x100000000)+uint64_t(used)*0x1000000,
                                                   size_t(bytes),40,size_t(bytes)):
                               b.memory.allocateUser(size_t(bytes),40,size_t(bytes),true);
        CHECK(!status);
        const uint64_t at=next[slot];
        const uint64_t began=nowUs();
        status=clientMap(p.vm,&b.memory,at,import?ArmSmmu4KRead:3,&b.handle,slot==0,why);
        mapUs+=nowUs()-began;
        if(!status) {next[slot]=(at+bytes+0x3FFF)&~uint64_t(0x3FFF);b.slot=slot;pages+=bytes/4096;++used;}
        else CHECK(!b.memory.release());
        return status;
    };
    for(const auto &q:order)if(mapOne(q.slot,q.bytes,q.import,nullptr))++refusals;
    const uint64_t t1=nowUs();
    NativeAdrenoVmPagedStats peak=stats(p.vm);
    const size_t peakRecords=p.vm.spanCount();
    /* the Metal image check (pid 6356, slot 55): its 96 MiB scattered BO, then the 187 MiB retry */
    const uint64_t big1=(UINT64_C(96)<<20)/scale,big2=(UINT64_C(187)<<20)/scale;
    size_t micRefused=0;
    if(mapOne(55,big1,false,nullptr))++micRefused;
    if(mapOne(55,big2,false,nullptr))++micRefused;
    NativeAdrenoVmPagedStats top=stats(p.vm);
    ArmSmmu4KPagedAuditResult audit{};
    const uint64_t a0=nowUs();
    const IOReturn audited=p.vm.auditTables(&audit);
    const uint64_t auditUs=nowUs()-a0;
    /* past the peak: other clients' 64 MiB BOs until the render server's reserve refuses, then the render server's into it */
    size_t extra=0,reserveRefused=0;
    uint64_t atRefusal=0;
    uint8_t why=0;
    if(full) {
        for(unsigned s=1;s<=47 && !reserveRefused;) {
            if(next[s]+(UINT64_C(64)<<20)>base(s)+(UINT64_C(1)<<30)) {++s;continue;}
            const IOReturn status=mapOne(s,UINT64_C(64)<<20,false,&why);
            if(status==kIOReturnNoResources) {++reserveRefused;atRefusal=stats(p.vm).mapped;}
            else {CHECK(!status);++extra;}
        }
        CHECK(!mapOne(0,UINT64_C(64)<<20,false,&why));   /* the render server, inside its reserve */
    }
    const NativeAdrenoVmPagedStats past=stats(p.vm);
    /* everything unmapped (the TLB plans), then the tick's trims */
    model.tlbival.clear();
    const unsigned asid0=model.tlbiasid;
    const uint64_t u0=nowUs();
    for(size_t i=0;i<used;++i)CHECK(!p.vm.unmap(bos[i].handle));
    const uint64_t unmapUs=nowUs()-u0;
    const NativeAdrenoVmPagedStats unmapped=stats(p.vm);
    size_t trims=0,retired=0,r=0;
    const uint64_t r0=nowUs();
    while(!p.vm.trimTables(&r) && (r || stats(p.vm).trim_pending)) {++trims;retired+=r;}
    const uint64_t trimUs=nowUs()-r0;
    const unsigned trimAsids=unsigned(model.tlbiasid-asid0-unsigned(stats(p.vm).unmap_asid-top.unmap_asid));
    const NativeAdrenoVmPagedStats end=stats(p.vm);
    for(size_t i=0;i<used;++i)CHECK(!bos[i].memory.release());
    for(unsigned i=0;i<6;++i)CHECK(!p.vm.unmap(kh[i]) && !kernel[i].release());
    while(!p.vm.trimTables(&r) && (r || stats(p.vm).trim_pending)) {}
    std::printf("REPLAY %s (bytes 1/%u): %zu objects mapped of %zu, %zu refused; %zu records; %.0f MiB mapped (peak %.0f MiB); "
        "table pages %llu (peak %llu, %.1f MiB) in %llu chunks (%.1f MiB, %llu free); leaves %llu; map %.3f us/page (%zu us total, "
        "%.1f us/map); Metal image check 96 + 187 MiB: %zu refused; audit %s (%zu tables, %zu leaves, %llu us)\n",
        full?"full":"scaled",scale,size_t(order.size())-refusals,order.size(),refusals,peakRecords,double(peak.mapped)/1048576.0,
        double(top.mapped_peak)/1048576.0,(unsigned long long)top.table_pages,(unsigned long long)top.table_peak,top.table_peak*4096.0/1048576.0,
        (unsigned long long)top.chunks,top.chunk_pages*4096.0/1048576.0,(unsigned long long)top.free_pages,(unsigned long long)top.leaves,
        pages?double(mapUs)/double(pages):0.0,size_t(t1-t0),used?double(mapUs)/double(used):0.0,micRefused,audited?"FAILED":"ok",
        size_t(audit.tables),size_t(audit.leaves),(unsigned long long)auditUs);
    if(full)std::printf("REPLAY past the peak: %zu more 64 MiB BOs of other clients until the reserve refused (%zu refusal at %.0f MiB mapped, "
        "the limit %u MiB), then the render server's 64 MiB inside the reserve: mapped; refusals tables %llu cap %llu reserve %llu\n",
        extra,reserveRefused,double(atRefusal)/1048576.0,unsigned((NativeAdrenoVmPagedCapDefault-NativeAdrenoVmPagedReserveDefault)>>20),
        (unsigned long long)past.refused_tables,(unsigned long long)past.refused_cap,(unsigned long long)past.refused_reserve);
    std::printf("REPLAY unmap all: %zu unmaps in %llu us; TLB plans: TLBIVAL %llu (%llu pages), TLBIASID %llu (+%u with the trims); "
        "%llu tables left empty, %zu trims retired %zu pages in %llu us; table pages %llu -> %llu\n",used,(unsigned long long)unmapUs,
        (unsigned long long)(unmapped.unmap_val-top.unmap_val),(unsigned long long)(unmapped.unmap_val_pages-top.unmap_val_pages),
        (unsigned long long)(unmapped.unmap_asid-top.unmap_asid),trimAsids,
        (unsigned long long)unmapped.kept_empty,trims,retired,(unsigned long long)trimUs,(unsigned long long)unmapped.table_pages,
        (unsigned long long)end.table_pages);
    CHECK(!refusals && !micRefused && !audited && peakRecords==7631 && !top.refused_tables && !top.refused_cap && !top.refused_reserve);
    CHECK(!p.vm.poisoned() && end.table_pages<=8 && model.violations==0 && model.aborts==0);
    if(full)CHECK(reserveRefused==1 && past.refused_reserve==1 && !past.refused_cap &&
                  atRefusal<=NativeAdrenoVmPagedCapDefault-NativeAdrenoVmPagedReserveDefault &&
                  atRefusal+(UINT64_C(64)<<20)>NativeAdrenoVmPagedCapDefault-NativeAdrenoVmPagedReserveDefault);
    down(p);
    fixture.mode=Contiguous;
    return unsigned(order.size()+2);
}

int main(int argc,char **argv)
{
    const bool full=argc>1 && !strcmp(argv[1],"--replay");
    if(full) {replay(true);return 0;}
    image();
    transactions();
    syncAfterMap();
    reserveCommit();
    capReserve();
    trimBudget();
    trimOrder();
    publishFailure();
    const unsigned replayed=replay(false);
    std::printf("PASS: paged GPU VM (GPU-PAGED P1): %u checks: the paged root encodes the live pool's image but TTBR0; leaves from each BO's "
        "spans, no SMMU register per map, one record per mapping (findAddress, handleAt, locate, holes); unmaps never unlink a table "
        "(TLBIVAL <= 512 pages, else TLBIASID); the tick's trim: one TLBIASID + TLBSYNC, pages freed only after it; reserve-then-"
        "commit refusals (table cap, a list that cannot grow), the cap and the render server's reserve in bytes, none poisons; a failed "
        "sync or publish poisons and keeps every page; replay of the gpu-paged0c H-on peak at 1/16 bytes: %u objects, 0 refused\n",
        checks,replayed);
    return 0;
}
