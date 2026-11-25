/* SPDX-License-Identifier: MIT */
/* UniXNU-piano (2026-11-22, GPU-PAGED P0b): the pool bypass (NativeAdrenoPoolBypass.h) through the real user client over software SMMU/CP
 * peers (test_userclient's fixture): off = pooled BOs as before; on = the path outside the pool, without the Memory lock; back off =
 * pooled again; the figures by path (BOs, bytes, failures, times), the wiring and physical walk, the map times by backing, the unmap TLB
 * rule, the submission walk, the machdep.piano_gpu_paged rows and the tick's summary line. No IOKit.
 * host-test: NativeAdrenoMemory.cpp NativeAdrenoSmmuMemory.cpp NativeAdrenoVm.cpp NativeAdrenoRing.cpp NativeAdrenoSubmit.cpp NativeAdrenoUserClient.cpp NativeAdrenoTimer.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoPoolBypass.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define SMMU_BYTES 0x40000
#define CB 0x22000
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean || op==NativeAdrenoCleanInvalidate || op==NativeAdrenoInvalidate);
    ++fixture.cache_calls;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
static uint32_t mmio[SMMU_BYTES/4];
static struct {unsigned calls,fail_call,after;} mf;
static ArmSmmu4KIo smmuio;
static ArmSmmu4KStreamState streams;
static ArmSmmu4KStream routes[2]={{2,0},{4,1}};
static ArmSmmu4KStreamResult route_result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KContextImage context_image;
static int mevent(void){return ++mf.calls==mf.fail_call;}
static int mread32(void *c,size_t at,uint32_t *v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);if(mevent())return 1;*v=mmio[at/4];return 0;}
static int mread64(void *c,size_t at,uint64_t *v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);if(mevent())return 1;std::memcpy(v,mmio+at/4,8);return 0;}
static int mwrite32(void *c,size_t at,uint32_t v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);int f=mevent();if(f && !mf.after)return 1;mmio[at/4]=v;return f;}
static int mwrite64(void *c,size_t at,uint64_t v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);int f=mevent();if(f && !mf.after)return 1;std::memcpy(mmio+at/4,&v,8);return f;}
static int mbarrier(void *c){CHECK(c==&mf);return mevent();}
struct CpPeer {
    std::vector<uint32_t> regs,writes;
    unsigned publish=0,acquire=0,barriers=0,fail_publish=0;
    CpPeer():regs(0x10000,0){}
};
static CpPeer cp;
static uint64_t clock_us=0;
static int cwrite(void *c,uint32_t reg,uint32_t v)
{CHECK(c==&cp && reg<cp.regs.size());cp.regs[reg]=v;cp.writes.push_back(reg);return 0;}
static int cread(void *c,uint32_t reg,uint32_t *v)
{CHECK(c==&cp && v && reg<cp.regs.size());*v=cp.regs[reg];return 0;}
static int cbarrier(void *c){CHECK(c==&cp);++cp.barriers;return 0;}
static int cpublish(void *c,const volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes && bytes<=32768);++cp.publish;return cp.publish==cp.fail_publish;}
static int cacquire(void *c,volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes>=8 && bytes<=4096);++cp.acquire;return 0;}
static uint64_t cnow(void *c){CHECK(c==&cp);return clock_us;}
static int cidle(void *c){CHECK(c==&cp);clock_us+=1000;return 0;}
struct Path {
    NativeAdrenoMemory tables;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit execbuf;
    NativeAdrenoUserClient client;
};
static uint32_t ringmem[8192],fencemem[1024];
static NativeAdrenoRingIo ringio{&cp,cwrite,cread,cbarrier,nullptr,nullptr,nullptr};
static NativeAdrenoRingCache ringcache{&cp,cpublish,cacquire};
static NativeAdrenoRingClock ringclock{&cp,cnow,cidle};
static void smmuReset(void)
{
    std::memset(mmio,0,sizeof(mmio));mf={};streams=(ArmSmmu4KStreamState){};
    smmuio=(ArmSmmu4KIo){&mf,SMMU_BYTES,mread32,mread64,mwrite32,mwrite64,mbarrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
}
/* The GPU address space of these tests: 0x800000 + 64 KiB (the arena: 16 pages), for windows small enough to map into. */
enum : uint64_t {Base=0x800000,ArenaBytes=0x10000};
static int failed(int step,int status)
{
    std::fprintf(stderr,"exposed: step %d returned %d\n",step,status);
    return status?status:kIOReturnBadArgument;
}
static int exposed(Path &p,uint32_t output_bits=36)
{
    smmuReset();cp=CpPeer{};clock_us=0;
    std::memset(ringmem,0xef,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
    int status=p.tables.allocate(32*4096,40,true,32*4096);
    if(status)return failed(1,status);
    status=NativeAdrenoPrepareSmmuMemory(&p.tables,32,40,Base,ArenaBytes,0,16,&p.arena,&p.state,&p.visibility);
    if(status)return failed(2,status);
    context=(ArmSmmu4KContextConfig){.lower=&p.arena.config,.lower_tables=&p.state.tables,
        .id0=mmio[0x20/4],.id2=mmio[0x28/4],.asid=1,.coherent_walk=1,
        .stall_on_fault=0,.continue_after_fault=1};
    ArmSmmu4KContextConfig actual=context;
    CHECK(ArmSmmu4KEncodeContext(&actual,&context_image)==ArmSmmu4KSuccess);
    mmio[0x1008/4]=context_image.cbar;mmio[0x1808/4]=context_image.cba2r;
    mmio[CB/4]=context_image.sctlr;mmio[(CB+0x10)/4]=context_image.tcr2;mmio[(CB+0x30)/4]=context_image.tcr;
    std::memcpy(mmio+(CB+0x20)/4,&context_image.ttbr0,8);
    std::memcpy(mmio+(CB+0x28)/4,&context_image.ttbr1,8);
    mmio[(CB+0x38)/4]=context_image.mair0;
    status=ArmSmmu4KAttachStreams(&smmuio,&streams,routes,2,2,&context,&route_result);
    if(status)return failed(3,status);
    NativeAdrenoVmRegion lower={&p.tables,&p.arena,&p.state};
    status=p.vm.attach(&smmuio,&context,&streams,&lower,nullptr,1024,1000);
    if(status)return failed(4,status);
    p.vm.setFenceSource(&fencemem[1]);
    status=p.ring.attach(&ringio,&ringcache,ringmem,sizeof(ringmem),0x100000,fencemem,sizeof(fencemem),0x108000);
    if(status)return failed(5,status);
    status=p.execbuf.attach(&p.vm,&p.ring);
    if(status)return failed(6,status);
    status=p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,output_bits);
    return status?failed(7,status):kIOReturnSuccess;
}

/* The P0b clock: 7 us further at each read. */
static uint64_t p0bUs;
static uint64_t p0bClock(void) {return p0bUs+=7;}
static uint64_t w(const volatile uint64_t *word) {return *word;}
static int memoryTaken,memoryDepth;
static std::string rowText(unsigned row)
{
    char line[NativeAdrenoP0bLineBytes];
    const int n=NativeAdrenoPoolBypassLine(row,line,sizeof line);
    CHECK(n>=0 && size_t(n)<sizeof line && std::strlen(line)==size_t(n));
    return line;
}
static bool has(const std::string &text,const char *part) {return text.find(part)!=std::string::npos;}
int main()
{
    unsigned checks=0;
    /* the latency buckets: 16 us times powers of 4, the last one open */
    CHECK(NativeAdrenoP0bBucket(0)==0 && NativeAdrenoP0bBucket(15)==0 && NativeAdrenoP0bBucket(16)==1 && NativeAdrenoP0bBucket(63)==1 &&
          NativeAdrenoP0bBucket(64)==2 && NativeAdrenoP0bBucket(1023)==3 && NativeAdrenoP0bBucket(1024)==4 &&
          NativeAdrenoP0bBucket(65535)==6 && NativeAdrenoP0bBucket(65536)==7 && NativeAdrenoP0bBucket(UINT64_MAX)==7);
    checks+=10;
    {
        volatile uint64_t row[NativeAdrenoP0bTimeWords]={};
        NativeAdrenoP0bTime(row,20);NativeAdrenoP0bTime(row,5);NativeAdrenoP0bTime(row,70000);
        CHECK(w(&row[0])==3 && w(&row[1])==70025 && w(&row[2])==70000 && w(&row[3])==1 && w(&row[4])==1 && w(&row[10])==1);
        ++checks;
    }
    /* the default is today's behaviour; nothing is timed before the publish shell sets the clock */
    CHECK(!NativeAdrenoPoolBypassed() && !native_adreno_p0b_now_us);
    ++checks;
    NativeAdrenoMemory::setUnmappedUser(true);   /* piano-gpu-user-vm=1, the daily line */
    fixture.mode=ScatterUnlessContiguous;        /* the tablet: one physical page per span outside the pool */
    Path p;CHECK(exposed(p)==kIOReturnSuccess);
    uint32_t mib=0;
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,16,16,true,&mib)==kIOReturnSuccess && mib==16);
    const NativeAdrenoSubmitLocks locks={nullptr,
        [](void *,NativeAdrenoSubmitLock which) {if(which==NativeAdrenoSubmitLockMemory) {CHECK(!memoryDepth);++memoryDepth;++memoryTaken;}},
        [](void *,NativeAdrenoSubmitLock which) {if(which==NativeAdrenoSubmitLockMemory) {CHECK(memoryDepth==1);--memoryDepth;}}};
    CHECK(p.execbuf.attachLocks(&locks)==kIOReturnSuccess);
    native_adreno_p0b_now_us=p0bClock;
    const size_t free0=NativeAdrenoMemory::poolFreeBytes();
    NativeAdrenoAbiBoAlloc alloc{16384,12,0};
    /* off: the pool, under the Memory lock */
    NativeAdrenoAbiBoAllocReply pooled{};
    CHECK(p.client.allocBo(&alloc,&pooled)==kIOReturnSuccess && p.client.memoryOf(pooled.handle)->pooled());
    CHECK(memoryTaken==1 && NativeAdrenoMemory::poolFreeBytes()==free0-16384);
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])==1 && w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][1])==16384 &&
          !w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][4]) && w(&native_adreno_p0b_alloc_us[NativeAdrenoPathPool][0])==1 &&
          w(&native_adreno_p0b_alloc_us[NativeAdrenoPathPool][1])>0 && !w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0]));
    CHECK(!w(&native_adreno_p0b_wire_us[0][0]));   /* a pooled BO is not wired by prepare() */
    checks+=4;
    /* on: past the pool to the named entry, no Memory lock, one span per page, the wiring and the walk timed */
    native_adreno_pool_bypass=1;
    NativeAdrenoAbiBoAllocReply scattered{};
    CHECK(p.client.allocBo(&alloc,&scattered)==kIOReturnSuccess);
    NativeAdrenoMemory *own=p.client.memoryOf(scattered.handle);
    CHECK(own && !own->pooled() && own->unmapped() && own->spanCount()==4);
    CHECK(memoryTaken==1 && NativeAdrenoMemory::poolFreeBytes()==free0-16384);
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][4])==1 && w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])==1 &&
          w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0])==1 && w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][1])==16384 &&
          w(&native_adreno_p0b_alloc_us[NativeAdrenoPathUser][0])==1);
    CHECK(w(&native_adreno_p0b_wire_us[0][0])==1 && w(&native_adreno_p0b_wire_us[0][1])>0 && w(&native_adreno_p0b_lookup[0][1])==4 &&
          w(&native_adreno_p0b_lookup[0][2])==4 && w(&native_adreno_p0b_lookup[0][0])>0 && !w(&native_adreno_p0b_wire_us[1][0]));
    checks+=5;
    /* a failure on the path that answered: counted with its bytes, nothing backed */
    fixture.prepare_fail=true;
    NativeAdrenoAbiBoAllocReply refused{};
    CHECK(p.client.allocBo(&alloc,&refused)!=kIOReturnSuccess && !refused.handle);
    fixture.prepare_fail=false;
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][2])==1 && w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][3])==16384 &&
          w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0])==1 && w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][4])==2);
    checks+=2;
    /* (a macOS refusal record reads the pool's figures under the Memory lock: only on the failure) */
    const int takenBefore=memoryTaken;
    /* off again: the pool again */
    native_adreno_pool_bypass=0;
    NativeAdrenoAbiBoAllocReply again{};
    CHECK(p.client.allocBo(&alloc,&again)==kIOReturnSuccess && p.client.memoryOf(again.handle)->pooled() && memoryTaken==takenBefore+1);
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])==2 && w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][4])==2);
    checks+=2;
    /* GPU maps by backing: 4 leaves each, timed */
    NativeAdrenoAbiBoMapReply mapped{};
    NativeAdrenoAbiBoMap mapPooled{pooled.handle,Base,3,0,2,0},mapOwn{scattered.handle,Base+0x4000,3,0,2,0};
    CHECK(p.client.mapBo(&mapPooled,&mapped)==kIOReturnSuccess && p.client.mapBo(&mapOwn,&mapped)==kIOReturnSuccess);
    CHECK(w(&native_adreno_p0b_map_us[0][0])==1 && w(&native_adreno_p0b_map_us[1][0])==1 && w(&native_adreno_p0b_map_pages[0])==4 &&
          w(&native_adreno_p0b_map_pages[1])==4 && !w(&native_adreno_p0b_map_us[2][0]) && w(&native_adreno_p0b_map_us[1][1])>0);
    const uint64_t mapAsid=w(&native_adreno_p0b_tlbi[0]);
    CHECK(mapAsid==0 || mapAsid==2);   /* one per map with the programmer's sync_after_map, none without it */
    checks+=3;
    /* a submission's maintenance walk: write-combined own BOs need none */
    CHECK(p.vm.cleanForSubmission()==kIOReturnSuccess && p.vm.cleanForSubmission()==kIOReturnSuccess);
    CHECK(w(&native_adreno_p0b_submit[0])==2 && !w(&native_adreno_p0b_submit[1]) && !w(&native_adreno_p0b_submit[2]));
    checks+=2;
    /* the rows and the summary */
    const std::string r0=rowText(0),pool=rowText(1),user=rowText(3),wire=rowText(5),outside=rowText(8),tlbi=rowText(10),submit=rowText(11);
    CHECK(has(r0,"p0b bypass=0 flips=0 clock=1 pool=0/16M") && has(r0,"owned-unmapped=0M"));
    CHECK(has(pool,"alloc pool kib=32 fails=0 fail-kib=0 n=2 ") && has(pool," backed=2 bypassed=2"));
    CHECK(has(rowText(2),"alloc contig kib=0 fails=0") && has(rowText(4),"alloc mapped kib=0 fails=0"));
    CHECK(has(user,"alloc user kib=16 fails=1 fail-kib=16 n=1 ") && has(user," backed=1") && !has(user,"bypassed"));
    CHECK(has(wire,"wire user n=1 ") && has(wire," lookups=4 spans=4") && has(rowText(6),"wire kernel n=0 "));
    CHECK(has(rowText(7),"map pooled pages=4 n=1 ") && has(outside,"map outside pages=4 n=1 ") && has(rowText(9),"map import pages=0 n=0 "));
    CHECK(has(tlbi,"tlbi map-asid=") && has(submit,"submit n=2 maintained=0 spans=0"));
    CHECK(NativeAdrenoPoolBypassLine(12,nullptr,0)==0);
    {
        char line[NativeAdrenoP0bLineBytes]="x";
        CHECK(NativeAdrenoPoolBypassLine(12,line,sizeof line)==0 && !line[0]);
        char small[20];
        const int n=NativeAdrenoPoolBypassLine(3,small,sizeof small);
        CHECK(n==int(sizeof small)-1 && std::strlen(small)==sizeof small-1);
        char summary[150];
        const int s=NativeAdrenoPoolBypassSummary(summary,sizeof summary);
        CHECK(s>0 && size_t(s)<sizeof summary && has(summary,"p0b: bypass=0 pool 2/0M ") && has(summary," user 1/0M ") && has(summary," fails 1"));
        CHECK(NativeAdrenoPoolBypassSummary(summary,sizeof summary)==0);   /* nothing moved */
        native_adreno_pool_bypass=1;
        CHECK(NativeAdrenoPoolBypassSummary(summary,sizeof summary)>0 && has(summary,"p0b: bypass=1 "));
        native_adreno_pool_bypass=0;
    }
    checks+=12;
    /* unmaps by the VM's TLB rule: <= 512 pages by page (TLBIVAL), counted with their pages */
    NativeAdrenoAbiBoFree f1{pooled.handle},f2{scattered.handle},f3{again.handle};
    CHECK(p.client.freeBo(&f1)==kIOReturnSuccess && p.client.freeBo(&f2)==kIOReturnSuccess && p.client.freeBo(&f3)==kIOReturnSuccess);
    CHECK(w(&native_adreno_p0b_tlbi[2])==2 && w(&native_adreno_p0b_tlbi[3])==8 && !w(&native_adreno_p0b_tlbi[1]));
    CHECK(NativeAdrenoMemory::poolFreeBytes()==free0 && p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && !p.client.liveObjects());
    CHECK(p.execbuf.attachLocks(nullptr)==kIOReturnSuccess && !memoryDepth);
    checks+=4;
    native_adreno_p0b_now_us=nullptr;
    NativeAdrenoMemory::setUnmappedUser(false);
    std::printf("PASS: GPU-PAGED P0b pool bypass: %u checks: off pooled under the Memory lock, on the named entry outside the pool without "
        "it (one span per page, wiring and walk timed), a failure on its path, off pooled again; map times by backing, the unmap TLB rule, "
        "the submission walk, 12 sysctl rows and the summary line\n",checks);
    return 0;
}
/* Host doubles (test_userclient's): the page attribute change, the reassert and physical maintenance touch no host page. */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
