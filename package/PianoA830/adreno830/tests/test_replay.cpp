/* SPDX-License-Identifier: MIT */
/* host-test: A830PowerState.cpp A830Recovery.cpp A830Dcvs.cpp platform/sm8750/Sm8750Tsens.c NativeAdrenoMemory.cpp NativeAdrenoSmmuMemory.cpp NativeAdrenoVm.cpp NativeAdrenoRing.cpp NativeAdrenoSubmit.cpp
 * UniXNU-piano (2026-11-22, private 20261122-render/ANALYSIS.md "ONE change"; Linux msm recover_worker as the behaviour reference): the
 * replaying GPU recovery end to end on the host. The real A830Recovery orchestration (over A830PowerFakes' register model and
 * operation log) drives the real NativeAdrenoRing, NativeAdrenoVm and NativeAdrenoSubmit (over test_submit's software SMMU and CP
 * peers); the publish shell's begin / plan / replay / end and its per-connection fault counters (NativeAdrenoStatus) and per-name hang
 * rows (NativeAdrenoClientRecord, NativeAdrenoOpenRefusal) are modelled as NativeAdrenoPublish.cpp does them. Checked: the operation
 * order (no fault at the gate, the plan after the GMU is forced off, S6 at the restart fence, the replay after the interrupt restore),
 * the ring positions and words of every replayed job (its own fence in the framed fence, the context entry waiting for the fence
 * replayed before it), the fences (the culprit's passes at the restart, the others only as the CP completes them, the numbering goes
 * on), the per-client fault state (the culprit's connection alone; WindowServer's never), the culprit (the job at the final completed
 * fence + 1, after the CP moved on past the halt; none for a driver-own cause or with nothing in flight), the hold packet, repeated
 * hangs from the same culprit (its name refused after NativeAdrenoClientHangLimit, the third recovery in 60 s final), a hang inside
 * a replayed job, the fallbacks to failing every job (poisoned, a kernel fence among them, no ring room), a replay the ring refuses
 * (final at S6.0x18), and the A/B switch (adreno830_recovery_set_replay(0): the old fail-all, WindowServer faulted). */
#include "A830PowerFakes.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoPublish.h"
#include <cstring>
#include <memory>

IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean || op==NativeAdrenoCleanInvalidate || op==NativeAdrenoInvalidate);
    ++fixture.cache_calls;
    return kIOReturnSuccess;
}
/* test_submit's doubles: the page attribute change and physical maintenance have no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}

/* ---- test_submit's software SMMU and CP peers ------------------------------------------------------------------------------ */
#define SMMU_BYTES 0x40000
#define CB 0x22000
static uint32_t mmio[SMMU_BYTES/4];
static ArmSmmu4KIo smmuio;
static ArmSmmu4KStreamState streams;
static ArmSmmu4KStream routes[2]={{2,0},{4,1}};
static ArmSmmu4KStreamResult route_result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KContextImage context_image;
static int smmuCookie;
static int mread32(void *c,size_t at,uint32_t *v) {CHECK(c==&smmuCookie && at%4==0 && at<=SMMU_BYTES-4);*v=mmio[at/4];return 0;}
static int mread64(void *c,size_t at,uint64_t *v) {CHECK(c==&smmuCookie && at%8==0 && at<=SMMU_BYTES-8);std::memcpy(v,mmio+at/4,8);return 0;}
static int mwrite32(void *c,size_t at,uint32_t v) {CHECK(c==&smmuCookie && at%4==0 && at<=SMMU_BYTES-4);mmio[at/4]=v;return 0;}
static int mwrite64(void *c,size_t at,uint64_t v) {CHECK(c==&smmuCookie && at%8==0 && at<=SMMU_BYTES-8);std::memcpy(mmio+at/4,&v,8);return 0;}
static int mbarrier(void *c) {CHECK(c==&smmuCookie);return 0;}
struct CpPeer {
    std::vector<uint32_t> regs;
    unsigned publish=0,fail_publish=0;
    CpPeer():regs(0x10000,0){}
};
static CpPeer cp;
static uint64_t clockUs=0;
static int cwrite(void *c,uint32_t reg,uint32_t v) {CHECK(c==&cp && reg<cp.regs.size());cp.regs[reg]=v;return 0;}
static int cread(void *c,uint32_t reg,uint32_t *v) {CHECK(c==&cp && v && reg<cp.regs.size());*v=cp.regs[reg];return 0;}
static int cbarrier(void *c) {CHECK(c==&cp);return 0;}
static int cpublish(void *c,const volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes && bytes<=32768 && !(bytes&3));++cp.publish;return cp.publish==cp.fail_publish;}
static int cacquire(void *c,volatile uint32_t *w,size_t bytes) {CHECK(c==&cp && w && bytes>=8 && bytes<=4096);return 0;}
static uint64_t cnow(void *c) {CHECK(c==&cp);return clockUs;}
static int cidle(void *c) {CHECK(c==&cp);clockUs+=1000;return 0;}
static uint32_t ringmem[8192],fencemem[1024];
static uint32_t replayStore[NativeAdrenoSubmitReplayStoreWords];   /* the publish shell's heap store (native_adreno_publish) */
static NativeAdrenoRingIo ringio{&cp,cwrite,cread,cbarrier,nullptr,nullptr,nullptr};
static NativeAdrenoRingCache ringcache{&cp,cpublish,cacquire};
static NativeAdrenoRingClock ringclock{&cp,cnow,cidle};
enum : uint64_t {RingIova=0x100000,MemptrsIova=0x108000,BufferIova=0x800000};
enum : uint32_t {Wptr=0x0807};

struct World {
    NativeAdrenoMemory tables,object;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit submit;
    uint64_t handle=0;
};
static World *W;
static IOReturn expose(World &p)
{
    std::memset(mmio,0,sizeof(mmio));streams=ArmSmmu4KStreamState{};
    smmuio=ArmSmmu4KIo{&smmuCookie,SMMU_BYTES,mread32,mread64,mwrite32,mwrite64,mbarrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
    cp=CpPeer{};clockUs=0;
    std::memset(ringmem,0xef,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
    if(p.tables.allocate(32*4096,40,true,32*4096))return kIOReturnNoMemory;
    if(NativeAdrenoPrepareSmmuMemory(&p.tables,32,40,0x800000,65536,0,16,&p.arena,&p.state,&p.visibility))return kIOReturnBadArgument;
    context=ArmSmmu4KContextConfig{};
    context.lower=&p.arena.config;context.lower_tables=&p.state.tables;context.id0=mmio[0x20/4];context.id2=mmio[0x28/4];
    context.asid=1;context.coherent_walk=1;context.stall_on_fault=0;context.continue_after_fault=1;
    ArmSmmu4KContextConfig actual=context;
    CHECK(ArmSmmu4KEncodeContext(&actual,&context_image)==ArmSmmu4KSuccess);
    mmio[0x1008/4]=context_image.cbar;mmio[0x1808/4]=context_image.cba2r;
    mmio[CB/4]=context_image.sctlr;mmio[(CB+0x10)/4]=context_image.tcr2;mmio[(CB+0x30)/4]=context_image.tcr;
    std::memcpy(mmio+(CB+0x20)/4,&context_image.ttbr0,8);
    std::memcpy(mmio+(CB+0x28)/4,&context_image.ttbr1,8);
    mmio[(CB+0x38)/4]=context_image.mair0;
    if(ArmSmmu4KAttachStreams(&smmuio,&streams,routes,2,2,&context,&route_result))return kIOReturnBadArgument;
    NativeAdrenoVmRegion lower={&p.tables,&p.arena,&p.state};
    if(p.vm.attach(&smmuio,&context,&streams,&lower,nullptr,32,1000))return kIOReturnBadArgument;
    p.vm.setFenceSource(&fencemem[1]);
    if(p.object.allocate(8192,40,false,8192))return kIOReturnNoMemory;
    if(p.object.makeWriteCombined())return kIOReturnNoMemory;
    if(p.vm.map(&p.object,0,BufferIova,3,0,2,&p.handle))return kIOReturnBadArgument;
    if(p.ring.attach(&ringio,&ringcache,ringmem,sizeof(ringmem),RingIova,fencemem,sizeof(fencemem),MemptrsIova))return kIOReturnBadArgument;
    if(const IOReturn r=p.submit.attach(&p.vm,&p.ring))return r;
    std::memset(replayStore,0xa5,sizeof(replayStore));
    return p.submit.attachReplayStore(replayStore,NativeAdrenoSubmitReplayStoreWords);
}
/* The CP finished everything up to fence (BR and BV) and consumed the ring up to rptr. */
static void complete(uint32_t fence,uint32_t rptr) {fencemem[1]=fence;fencemem[3]=fence;fencemem[0]=rptr;}

/* ---- the publish shell, as NativeAdrenoPublish.cpp does it ------------------------------------------------------------------- */
static A830PowerState gPower;
static A830Recovery gRecovery;
static const char *const kRenderServer="WindowServer";
struct Shell {
    NativeAdrenoStatus status[NativeAdrenoSubmitRanges];
    const char *names[NativeAdrenoSubmitRanges]{};
    NativeAdrenoClientRecord records[8]{};
    NativeAdrenoSubmitPlan plan{};
    bool replaying=false,lost=false;
    unsigned begins=0,beginsReplay=0,plans=0,replays=0;
    NativeAdrenoClientRecord *record(const char *name)
    {
        for(auto &r:records) {if(r.name[0] && NativeAdrenoClientRecordIs(r,name))return &r;}
        for(auto &r:records)if(!r.name[0]) {std::strncpy(r.name,name,sizeof r.name-1);return &r;}
        return nullptr;
    }
    /* NativeAdrenoPublish.cpp chargeCulpritLocked: a client other than the render server gets one hang against its name. */
    void charge(bool chargeCulprit,uint32_t culprit)
    {
        if(!chargeCulprit || culprit>=NativeAdrenoSubmitRanges || !names[culprit] || !std::strcmp(names[culprit],kRenderServer))return;
        if(NativeAdrenoClientRecord *r=record(names[culprit]))++r->hangs;
    }
    void fault(NativeAdrenoRangeMask ranges)
    {
        for(uint32_t s=0;s<NativeAdrenoSubmitRanges;++s)if(ranges&NativeAdrenoRangeBit(s))status[s].noteFault();
    }
    /* native_adreno_new_user_client's policy for a name (no cap, no loss here unless lost). */
    IOReturn open(const char *name)
    {
        const NativeAdrenoClientRecord *r=record(name);
        return NativeAdrenoOpenRefusal(lost,0,0,r && std::strcmp(name,kRenderServer)?r->hangs:0);
    }
    uint64_t faults(uint32_t range) const {return status[range].faults();}
} gShell;
static uint32_t opFailInflightW(void *) {gOps.hit("failInflight");return W->ring.outstanding();}
static void opGateW(void *,uint8_t gate,uint8_t cause)
{
    gOps.gates.push_back(gate);gOps.gateCauses.push_back(cause);gOps.hit("gate:"+std::to_string(gate));
    if(gate==A830GateRecovering) {
        NativeAdrenoRangeMask ranges=0;uint32_t oldest=UINT32_MAX;
        CHECK(W->submit.halt(&ranges,&oldest)==kIOReturnSuccess);
        gShell.replaying=gRecovery.replaying();
        if(gShell.replaying)++gShell.beginsReplay;
        else {
            /* native_adreno_recovery_begin: whoever had a job in flight is faulted, whatever the cause. */
            ++gShell.begins;
            gShell.charge(A830FaultBlamesClient(cause),oldest);
            gShell.fault(ranges);
        }
    } else if(gate==A830GateOpen) {
        if(!gShell.replaying) {NativeAdrenoRangeMask abandoned=0;CHECK(W->submit.recover(&abandoned)==kIOReturnSuccess);}
        for(auto &s:gShell.status)s.noteSuspend();
        W->submit.resume();
        gShell.replaying=false;
    } else {
        for(auto &s:gShell.status)s.noteFault();   /* native_adreno_device_lost */
        gShell.lost=true;gShell.replaying=false;
    }
}
static uint32_t opPlanW(void *,uint8_t cause,uint32_t *restart)
{
    gOps.hit("plan");
    *restart=W->ring.lastSubmitted();
    NativeAdrenoSubmitPlan p{};
    CHECK(W->submit.plan(A830FaultBlamesClient(cause),&p)==kIOReturnSuccess);
    *restart=p.restart;
    gShell.plan=p;++gShell.plans;
    gShell.charge(A830FaultBlamesClient(cause),p.culprit_fence?p.culprit_range:UINT32_MAX);
    gShell.fault(p.fault_ranges);
    return p.failed;
}
static IOReturn opReplayW(void *,uint32_t *replayed)
{
    gOps.hit("replay");++gShell.replays;
    return W->submit.replay(replayed);
}
/* opCpStart's work on the ring: the queue meter aside, ring.recover at the fence, memptrs set to it (startRing cleared them). */
static IOReturn opCpStartW(void *,uint32_t fence,uint16_t *step)
{
    gOps.fence=fence;
    const IOReturn r=gOps.hit("cpStart");
    if(r) {*step=gOps.failStep;return r;}
    uint32_t last=0;
    CHECK(W->ring.recover(&last,&fence)==kIOReturnSuccess && last==W->ring.lastSubmitted());
    complete(fence,0);
    return kIOReturnSuccess;
}
static uint32_t opLastSubmittedW(void *) {return W->ring.lastSubmitted();}
static uint32_t opRetiredW(void *) {return fencemem[1];}
static A830PowerOps powerOps()
{
    A830PowerOps ops=gPowerOps;
    ops.cpStart=opCpStartW;ops.lastSubmitted=opLastSubmittedW;ops.retired=opRetiredW;
    return ops;
}
static const A830RecoveryOps kReplayOps={nullptr,opFailInflightW,opGateW,gRecoveryOps.fenceSignal,gRecoveryOps.irqRestore,
                                         gRecoveryOps.readPipe,gRecoveryOps.snapshot,opPlanW,opReplayW};

/* A fresh world: WindowServer (range 0, context 5), the hog (range 3, context 8), the innocent bench (range 7, context 9). */
enum : uint32_t {WS=0,HOG=3,BENCH=7};
static uint32_t queueOf(uint32_t range) {return range*NativeAdrenoSubmitRangeQueues;}
static uint32_t contextOf(uint32_t range) {return range==WS?5:range==HOG?8:range==BENCH?9:20+range;}
static void boot(bool replay=true)
{
    resetAll();
    delete W;W=new World;
    CHECK(expose(*W)==kIOReturnSuccess);
    gShell=Shell{};
    gShell.names[WS]=kRenderServer;gShell.names[HOG]="PianoGpuHog";gShell.names[BENCH]="PianoGpuBench";
    const A830PowerOps ops=powerOps();
    CHECK(gPower.init(A830PowerConfig{},gIo,gPowerEnv,ops)==kIOReturnSuccess);
    A830RecoveryConfig config;config.replay=replay;
    CHECK(gRecovery.init(config,gIo,gPowerEnv,ops,kReplayOps,&gPower)==kIOReturnSuccess);
    gPower.start(4,3);
    gHw.at(A830PowerWinGpu,G(A8XX_RBBM_STATUS))=0x00800004u;
    adreno830_recovery_set_replay(1);
}
/* One job of range r: n IBs of 256 bytes at BufferIova + 256 * k. */
static uint32_t submitJob(uint32_t range,uint32_t n=1,IOReturn want=kIOReturnSuccess)
{
    NativeAdrenoSubmitBuffer buffers[NativeAdrenoSubmitMaxBuffers];
    for(uint32_t i=0;i<n;++i)buffers[i]={W->handle,BufferIova+256u*(i%32),256};
    uint32_t fence=0;
    CHECK(W->submit.submit(buffers,n,queueOf(range),&fence,nullptr,contextOf(range),
                           range==WS?NativeAdrenoRingSourceRender:NativeAdrenoRingSourceExternal)==want);
    return fence;
}
static uint32_t wptr() {return cp.regs[Wptr];}
static void later() {gEnv.now+=31ull*1000000000;}   /* outside the "3rd within 60 s" window of the previous recovery */
/* The framed submission at ring index at: n IBs (the hold first when held), its fence in the scratch write and the BR timestamp, the
 * context entry (reset) waiting for waitFor; returns the index after it. */
static uint32_t checkFramed(uint32_t at,bool reset,uint32_t n,uint32_t seq,uint32_t waitFor,bool held=false)
{
    if(reset) {
        CHECK((ringmem[at+2]>>16&0x7fu)==0x14 && (ringmem[at+2]&0x3fffu)==4);   /* CP_WAIT_TIMESTAMP on the memptrs fence */
        CHECK(ringmem[at+3]==0 && ringmem[at+4]==uint32_t(MemptrsIova+4) && ringmem[at+6]==waitFor);
    }
    uint32_t user=at+NativeAdrenoRingPrologueWords+(reset?NativeAdrenoRingResetWords:0);
    if(held) {
        CHECK((ringmem[user]>>16&0x7fu)==0x14 && (ringmem[user]&0x3fffu)==4);   /* the hold: CP_WAIT_TIMESTAMP ... */
        CHECK(ringmem[user+1]==0 && ringmem[user+2]==uint32_t(uint64_t(MemptrsIova)+NativeAdrenoSubmitHoldOffset) && ringmem[user+3]==0 &&
              ringmem[user+4]==1);   /* ... for >= 1 on a word nothing writes */
        CHECK(fencemem[NativeAdrenoSubmitHoldOffset/4]==0);
        user+=NativeAdrenoSubmitHoldWords;
    }
    for(uint32_t i=0;i<n;++i) {
        CHECK((ringmem[user+4*i]>>16&0x7fu)==0x3f && ringmem[user+4*i+1]==uint32_t(BufferIova+256u*(i%32)) && ringmem[user+4*i+3]==64);
    }
    const uint32_t fence=user+4*n;
    CHECK(ringmem[fence+1]==seq && ringmem[fence+26]==seq);
    return fence+NativeAdrenoRingFramedFenceWords;
}

/* The mic-ns3b case, fixed: a hang in the hog's job while WindowServer and the bench have work behind it. */
static void testReplay()
{
    boot();
    const uint32_t f1=submitJob(WS);                                  /* 1: done before the hang */
    CHECK(W->submit.armHold(NativeAdrenoRangeBit(HOG))==kIOReturnSuccess && W->submit.holdArmed()==NativeAdrenoRangeBit(HOG));
    const uint32_t hogAt=wptr();
    const uint32_t f2=submitJob(HOG,2);                               /* 2: the CP stops in it */
    CHECK(W->submit.holdArmed()==0 && W->submit.heldFence()==f2 && W->submit.stats().holds==1);
    checkFramed(hogAt,true,2,f2,f1,true);
    const uint32_t f3=submitJob(WS,3),f4=submitJob(BENCH),f5=submitJob(WS);   /* 3, 4, 5 behind it */
    CHECK(f1==1 && f2==2 && f3==3 && f4==4 && f5==5 && W->object.deviceReferences()==6);
    complete(f1,0);
    const uint64_t wsBefore=gShell.faults(WS);
    CHECK(gRecovery.recover(A830FaultWatchdog,0x7E57)==kIOReturnSuccess);
    /* Order: nobody faulted at the gate, no failInflight; the plan after the GMU is off; S6 at the culprit's fence; the replay after
     * the fence signal and the interrupt restore, before the gate opens. */
    std::vector<std::string> ops;
    for(const auto &c:gOps.calls)if(c!="readPipe")ops.push_back(c);
    const std::vector<std::string> expected={"gate:1","snapshot","gmuForceOff","plan","gmuStart","cpStart","secureResume","irq:3f:on",
                                             "fenceSignal","irqRestore","replay","gate:0"};
    CHECK(ops==expected && gShell.begins==0 && gShell.beginsReplay==1);
    CHECK(gOps.fence==f2 && fencemem[1]==f2 && fencemem[3]==f2);
    /* Plan: the culprit is the hog's job (done + 1), its connection alone is faulted, the others are replayed. */
    const NativeAdrenoSubmitPlan &p=gShell.plan;
    CHECK(p.mode==NativeAdrenoSubmitPlanReplay && p.done==f1 && p.issued==f5 && p.restart==f2 && p.culprit_range==HOG &&
          p.culprit_fence==f2 && p.fault_ranges==NativeAdrenoRangeBit(HOG) && p.failed==1);
    CHECK(p.replay_jobs==3 && p.replay_ranges==(NativeAdrenoRangeBit(WS)|NativeAdrenoRangeBit(BENCH)) && p.replay_count[WS]==2 &&
          p.replay_count[BENCH]==1 && p.first_replay==f3 && p.last_replay==f5);
    CHECK(gShell.faults(HOG)==1 && gShell.faults(WS)==wsBefore && gShell.faults(BENCH)==0);
    CHECK(gShell.records[0].hangs==1 && NativeAdrenoClientRecordIs(gShell.records[0],"PianoGpuHog"));
    /* Ring: the numbering did not move; each replayed job at its place with its own fence, a context entry for the first and at each
     * change of client, waiting for the fence before it (the restart, then the jobs replayed). */
    CHECK(W->ring.lastSubmitted()==f5 && W->ring.replays()==3);
    uint32_t at=checkFramed(0,true,3,f3,f2);
    at=checkFramed(at,true,1,f4,f3);
    at=checkFramed(at,true,1,f5,f4);
    CHECK(wptr()==at);
    /* Fences: the culprit's passed at the restart (its waiter wakes to its fault, its references went); the others wait for the CP. */
    CHECK(W->submit.completed(f2) && !W->submit.completed(f3) && W->object.deviceReferences()==4);
    CHECK(W->submit.wait(f3,&ringclock,0)==kIOReturnTimeout && W->submit.outstanding()==3);
    const NativeAdrenoSubmitStats s=W->submit.stats();
    CHECK(s.replayed==3 && s.skipped==1 && s.abandoned==0 && s.fallbacks==0);
    CHECK(gRecovery.stats().replayRecoveries==1 && gRecovery.stats().jobsReplayed==3 && gRecovery.stats().jobsFailed==1 &&
          gRecovery.stats().recoveries==1 && !gRecovery.lost());
    /* The CP runs the replayed jobs: WindowServer's frames complete with their own fences. */
    complete(f3,0);
    CHECK(W->submit.wait(f3,&ringclock,0)==kIOReturnSuccess && W->object.deviceReferences()==3);
    complete(f5,at);
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess && W->submit.outstanding()==0 && W->object.deviceReferences()==1);
    /* Submissions go on, the numbering continues, and the next job from WindowServer's context has no context entry. */
    const uint32_t f6=submitJob(WS);
    CHECK(f6==f5+1 && wptr()==checkFramed(at,false,1,f6,0));
    complete(f6,wptr());
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess);
    CHECK(gShell.faults(WS)==wsBefore && gShell.status[WS].suspends()==1);
}

/* The culprit is decided on the fence once the GPU stopped: the CP finished WindowServer's job after the halt (before the GMU was
 * forced off), so the hang is in the job after it. */
static void testCulpritAfterHalt()
{
    boot();
    const uint32_t f1=submitJob(WS),f2=submitJob(BENCH),f3=submitJob(WS);
    gOps.during=[&](const std::string &name) {if(name=="gmuForceOff")complete(f1,0);};
    CHECK(gRecovery.recover(A830FaultHang,1)==kIOReturnSuccess);
    gOps.during=nullptr;
    const NativeAdrenoSubmitPlan &p=gShell.plan;
    CHECK(p.done==f1 && p.retired==1 && p.culprit_range==BENCH && p.culprit_fence==f2 && p.restart==f2 && p.replay_jobs==1 &&
          p.first_replay==f3 && gOps.fence==f2);
    CHECK(gShell.faults(BENCH)==1 && gShell.faults(WS)==0);
    checkFramed(0,true,1,f3,f2);
    complete(f3,0);
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess && W->object.deviceReferences()==1);
    /* WindowServer itself the culprit: its connection is faulted (Linux charges the hung submit's queue whoever it is), but never a
     * hang against the render server's name; its later job is replayed. */
    later();
    const uint32_t f4=submitJob(WS),f5=submitJob(BENCH);
    CHECK(gRecovery.recover(A830FaultCpHwError,2)==kIOReturnSuccess);
    CHECK(gShell.plan.culprit_range==WS && gShell.plan.culprit_fence==f4 && gShell.plan.first_replay==f5);
    CHECK(gShell.faults(WS)==1 && gShell.faults(BENCH)==1 && gShell.open(kRenderServer)==kIOReturnSuccess);
    CHECK(gShell.record(kRenderServer) && gShell.record(kRenderServer)->hangs==0);
}

/* A driver-own cause (GMU, HFI, fenced write, slumber, wake): no culprit, the CP restarts at the completed fence and every job in
 * flight, the one it stood in included, is replayed; nobody is faulted. Nothing in flight (the gpu-daily3c injections): nothing to
 * plan, nobody faulted, the restart at the newest fence. */
static void testDriverCauseAndIdle()
{
    boot();
    const uint32_t f1=submitJob(WS),f2=submitJob(BENCH),f3=submitJob(HOG);
    complete(f1,0);
    CHECK(gRecovery.recover(A830FaultGmu,0)==kIOReturnSuccess);
    const NativeAdrenoSubmitPlan &p=gShell.plan;
    CHECK(p.mode==NativeAdrenoSubmitPlanReplay && p.culprit_range==UINT32_MAX && !p.culprit_fence && p.restart==f1 && !p.failed &&
          !p.fault_ranges && p.replay_jobs==2 && p.first_replay==f2 && p.last_replay==f3 && gOps.fence==f1);
    CHECK(gShell.faults(WS)==0 && gShell.faults(BENCH)==0 && gShell.faults(HOG)==0);
    uint32_t at=checkFramed(0,true,1,f2,f1);
    checkFramed(at,true,1,f3,f2);
    complete(f3,0);
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess);
    later();
    CHECK(gRecovery.recover(A830FaultWatchdog,3)==kIOReturnSuccess);
    CHECK(gShell.plan.mode==NativeAdrenoSubmitPlanReplay && !gShell.plan.culprit_fence && !gShell.plan.replay_jobs &&
          gShell.plan.restart==f3 && gOps.fence==f3 && W->ring.replays()==2);
    CHECK(gShell.faults(WS)==0 && gShell.faults(BENCH)==0 && gShell.faults(HOG)==0 && gRecovery.stats().jobsFailed==0);
}

/* Repeated hangs from the same culprit: each time only its connection is faulted and the others are replayed; its name is refused
 * after NativeAdrenoClientHangLimit hangs (WindowServer never); the third recovery within 60 s is final (A830Recovery) and the
 * device loss faults every connection. */
static void testRepeatedCulprit()
{
    boot();
    for(unsigned round=1;round<=NativeAdrenoClientHangLimit;++round) {
        CHECK(gShell.open("PianoGpuHog")==kIOReturnSuccess);
        CHECK(W->submit.armHold(NativeAdrenoRangeBit(HOG))==kIOReturnSuccess);
        const uint32_t hog=submitJob(HOG),ws=submitJob(WS),bench=submitJob(BENCH);
        complete(hog-1,0);
        CHECK(gRecovery.recover(A830FaultWatchdog,round)==kIOReturnSuccess);
        CHECK(gShell.plan.culprit_range==HOG && gShell.plan.culprit_fence==hog && gShell.plan.replay_jobs==2);
        CHECK(gShell.faults(HOG)==round && gShell.faults(WS)==0 && gShell.faults(BENCH)==0);
        complete(bench,0);
        CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess && ws==hog+1);
        later();
    }
    CHECK(gShell.open("PianoGpuHog")==NativeAdrenoReturnNotPermitted && gShell.open("PianoGpuBench")==kIOReturnSuccess &&
          gShell.open(kRenderServer)==kIOReturnSuccess);
    CHECK(gRecovery.stats().replayRecoveries==2 && gRecovery.stats().jobsReplayed==4);
    /* Three within 60 s: the third is final, nothing restarts, every connection is faulted (the device is lost). */
    boot();
    for(unsigned round=1;round<=3;++round) {
        const uint32_t hog=submitJob(HOG),ws=submitJob(WS);
        complete(hog-1,0);
        gOps.calls.clear();
        const IOReturn r=gRecovery.recover(A830FaultWatchdog,round);
        if(round<3) {
            CHECK(r==kIOReturnSuccess && gShell.faults(WS)==0 && gShell.faults(HOG)==round);
            complete(ws,0);CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess);
            gEnv.now+=10ull*1000000000;
        } else {
            CHECK(r==A830ReturnNotResponding && gRecovery.lost() && gOps.count("plan")==0 && gOps.count("gmuStart")==0 &&
                  gOps.count("failInflight")==1 && gOps.gates.back()==A830GateLost);
            CHECK(gShell.lost && gShell.faults(WS)==1 && gShell.open("PianoGpuBench")==NativeAdrenoReturnNotResponding);
        }
    }
}

/* A hang inside a replayed job: the next recovery names that job (it is now at the completed fence + 1), faults its connection, and
 * replays the rest again with their own fences, the context entry waiting for the new restart. */
static void testHangDuringReplay()
{
    boot();
    const uint32_t f1=submitJob(HOG),f2=submitJob(WS),f3=submitJob(BENCH),f4=submitJob(WS);
    CHECK(gRecovery.recover(A830FaultWatchdog,1)==kIOReturnSuccess);
    CHECK(gShell.plan.culprit_fence==f1 && gShell.plan.replay_jobs==3 && gShell.faults(HOG)==1);
    /* The CP completes WindowServer's replayed frame, then stops in the bench's replayed job. */
    complete(f2,0);
    later();
    CHECK(gRecovery.recover(A830FaultWatchdog,2)==kIOReturnSuccess);
    const NativeAdrenoSubmitPlan &p=gShell.plan;
    CHECK(p.done==f2 && p.retired==0 && p.culprit_range==BENCH && p.culprit_fence==f3 && p.restart==f3 && p.replay_jobs==1 &&
          p.first_replay==f4 && p.last_replay==f4 && gOps.fence==f3);
    CHECK(gShell.faults(BENCH)==1 && gShell.faults(WS)==0 && gShell.faults(HOG)==1);
    checkFramed(0,true,1,f4,f3);
    CHECK(W->ring.lastSubmitted()==f4 && W->submit.outstanding()==1);
    complete(f4,0);
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess && W->object.deviceReferences()==1);
    CHECK(gRecovery.stats().replayRecoveries==2 && gRecovery.stats().jobsReplayed==4 && gRecovery.stats().jobsFailed==2);
}

/* When the jobs cannot be replayed the plan falls back to the old fail-all: restart at the newest fence, every connection with a job
 * in flight faulted, every job dropped. A kernel submission among them (nothing would complete its fence), a poisoned submit
 * object, jobs that would not fit the ring again. */
static void testFallbacks()
{
    boot();
    const uint32_t f1=submitJob(HOG);
    uint32_t kernelFence=0;const uint32_t nop[2]={0x70100001u,0};
    CHECK(W->ring.submit(nop,2,&kernelFence)==kIOReturnSuccess && kernelFence==f1+1);
    const uint32_t f3=submitJob(WS);
    CHECK(gRecovery.recover(A830FaultWatchdog,1)==kIOReturnSuccess);
    CHECK(gShell.plan.mode==NativeAdrenoSubmitPlanFailAll && gShell.plan.fallback==NativeAdrenoSubmitFallbackGap &&
          gShell.plan.restart==f3 && gOps.fence==f3 && gShell.plan.culprit_range==HOG &&
          gShell.plan.fault_ranges==(NativeAdrenoRangeBit(HOG)|NativeAdrenoRangeBit(WS)) && gShell.plan.failed==2);
    CHECK(gShell.faults(WS)==1 && gShell.faults(HOG)==1 && W->submit.outstanding()==0 && W->object.deviceReferences()==1 &&
          W->ring.replays()==0 && W->submit.stats().fallbacks==1 && W->submit.stats().abandoned==2);
    /* The CP stood in a kernel submission (fence done + 1 is no client's job): nobody is the culprit, nobody's name is charged, every
     * job fails (nothing would complete the kernel's fence). */
    boot();
    CHECK(W->ring.submit(nop,2,&kernelFence)==kIOReturnSuccess && kernelFence==1);
    const uint32_t g2=submitJob(HOG),g3=submitJob(WS);
    CHECK(gRecovery.recover(A830FaultHang,1)==kIOReturnSuccess);
    CHECK(gShell.plan.fallback==NativeAdrenoSubmitFallbackGap && gShell.plan.culprit_range==UINT32_MAX && !gShell.plan.culprit_fence &&
          gShell.plan.restart==g3 && g2==2 && gShell.faults(HOG)==1 && gShell.faults(WS)==1);
    CHECK(!gShell.record("PianoGpuHog")->hangs);
    /* No replay store (the shell's heap allocation failed): nothing is kept, every job in flight fails. The store attaches only while
     * no job is outstanding, and only whole. */
    boot();
    CHECK(W->submit.replayStore() && W->submit.attachReplayStore(nullptr,0)==kIOReturnSuccess && !W->submit.replayStore());
    submitJob(HOG);submitJob(WS);
    CHECK(W->submit.attachReplayStore(replayStore,NativeAdrenoSubmitReplayStoreWords)==kIOReturnBusy);
    CHECK(gRecovery.recover(A830FaultWatchdog,1)==kIOReturnSuccess);
    CHECK(gShell.plan.fallback==NativeAdrenoSubmitFallbackNoStore && gShell.plan.mode==NativeAdrenoSubmitPlanFailAll &&
          gShell.faults(WS)==1 && gShell.faults(HOG)==1 && W->submit.outstanding()==0 && W->ring.replays()==0);
    CHECK(W->submit.attachReplayStore(replayStore,NativeAdrenoSubmitReplayStoreWords-1)==kIOReturnBadArgument &&
          W->submit.attachReplayStore(replayStore,NativeAdrenoSubmitReplayStoreWords)==kIOReturnSuccess);
    /* Poisoned: a commit that poisoned the ring. */
    boot();
    submitJob(BENCH);
    cp.fail_publish=cp.publish+1;
    submitJob(WS,1,kIOReturnIOError);
    CHECK(W->submit.poisoned());
    CHECK(gRecovery.recover(A830FaultWptrFence,1)==kIOReturnSuccess);
    CHECK(gShell.plan.fallback==NativeAdrenoSubmitFallbackPoisoned &&
          gShell.plan.fault_ranges==(NativeAdrenoRangeBit(WS)|NativeAdrenoRangeBit(BENCH)) && !W->submit.poisoned() &&
          W->submit.outstanding()==0 && W->object.deviceReferences()==1);
    /* No room: WindowServer's thirteen frames of 128 IBs, the bench's and one of 30 IBs fill the ring (8002 of 8192 words); with the
     * margin they would not fit again. */
    boot();
    for(uint32_t i=0;i<1+NativeAdrenoSubmitSharedJobs;++i)submitJob(WS,NativeAdrenoSubmitMaxBuffers);
    submitJob(BENCH,NativeAdrenoSubmitMaxBuffers);
    const uint32_t last=submitJob(9,30);
    CHECK(last==15 && wptr()==8002);
    CHECK(gRecovery.recover(A830FaultGmu,1)==kIOReturnSuccess);
    CHECK(gShell.plan.fallback==NativeAdrenoSubmitFallbackRoom && gShell.plan.restart==last && gShell.faults(WS)==1 &&
          gShell.faults(BENCH)==1 && W->submit.outstanding()==0);
    /* The same frames with room to spare replay (thirteen of 32 IBs). */
    boot();
    for(uint32_t i=0;i<1+NativeAdrenoSubmitSharedJobs;++i)submitJob(WS,32);
    CHECK(gRecovery.recover(A830FaultGmu,1)==kIOReturnSuccess);
    CHECK(gShell.plan.mode==NativeAdrenoSubmitPlanReplay && gShell.plan.replay_jobs==1+NativeAdrenoSubmitSharedJobs &&
          gShell.faults(WS)==0);
    /* Only the first replayed job carries the context entry: they are all WindowServer's. */
    uint32_t at=0;
    for(uint32_t f=1;f<=1+NativeAdrenoSubmitSharedJobs;++f)at=checkFramed(at,f==1,32,f,f-1);
    CHECK(wptr()==at);
}

/* A replay the ring refuses (its publication fails: poisoned) makes the loss final at S6, step A830RecoveryStepReplay. */
static void testReplayFailure()
{
    boot();
    submitJob(HOG);submitJob(WS);
    gOps.during=[&](const std::string &name) {if(name=="replay")cp.fail_publish=cp.publish+1;};
    CHECK(gRecovery.recover(A830FaultWatchdog,1)==kIOReturnIOError);
    gOps.during=nullptr;
    CHECK(gRecovery.lost() && gOps.gates.back()==A830GateLost && gRecovery.stats().failures==1 && gRecovery.stats().lostStep==A830RecoveryStepReplay);
    CHECK(gStatus.lost.back()[0]==A830SCp && gStatus.lost.back()[1]==A830RecoveryStepReplay && gShell.lost);
}

/* The A/B switch: adreno830_recovery_set_replay(0) (piano-gpu-recovery-replay=0) or config.replay false is the old behaviour on the same
 * hang: the gate faults every connection with a job in flight (WindowServer: Turnip loses its device, the session ends), failInflight,
 * no plan, no replay, S6 at the newest fence, every job dropped. Read at each recovery's start: it switches back on the same boot. */
static void testAbSwitch()
{
    for(int how=0;how<2;++how) {
        boot(how==1?false:true);
        if(how==0)adreno830_recovery_set_replay(0);
        CHECK(adreno830_recovery_replay_mode()==(how==0?0u:1u));
        const uint32_t f1=submitJob(HOG),f2=submitJob(WS);
        CHECK(gRecovery.recover(A830FaultWatchdog,1)==kIOReturnSuccess);
        CHECK(!gRecovery.replaying() && gOps.count("failInflight")==1 && gOps.count("plan")==0 && gOps.count("replay")==0 &&
              gOps.fence==f2 && gShell.begins==1);
        CHECK(gShell.faults(WS)==1 && gShell.faults(HOG)==1 && W->submit.outstanding()==0 && W->submit.completed(f1) &&
              W->submit.completed(f2) && W->ring.replays()==0);
        CHECK(gShell.records[0].hangs==1);   /* the culprit's name is charged in both modes */
        adreno830_recovery_set_replay(1);
        if(how==0) {
            later();
            submitJob(HOG);const uint32_t f4=submitJob(WS);
            CHECK(gRecovery.recover(A830FaultWatchdog,2)==kIOReturnSuccess && gRecovery.replaying() && gShell.plan.first_replay==f4 &&
                  gShell.faults(WS)==1 && gShell.faults(HOG)==2);
        }
    }
    /* A final loss without a recovery attempt never replays (declareLost): every connection is faulted. */
    boot();
    submitJob(WS);
    gRecovery.declareLost(A830FaultGmu,0,kIOReturnTimeout);
    CHECK(!gRecovery.replaying() && gShell.begins==1 && gShell.lost && gOps.count("plan")==0);
    /* The replay ops come as a pair. */
    A830RecoveryOps half=kReplayOps;half.replay=nullptr;
    A830Recovery r;
    CHECK(r.init(A830RecoveryConfig{},gIo,gPowerEnv,powerOps(),half,&gPower)==kIOReturnBadArgument);
}

/* Replay at the ring level: a fence outside (after, lastSubmitted] is refused, the numbering never moves, and ring.recover's restart
 * past the last issued fence is ignored (every waiter wakes). */
static void testRingReplay()
{
    boot();
    const uint32_t f1=submitJob(WS),f2=submitJob(BENCH);
    const uint32_t words[4]={0x70ffc003u,0,0,0};
    uint32_t last=0,restart=f2+5;
    CHECK(W->ring.recover(&last,&restart)==kIOReturnSuccess && last==f2 && fencemem[1]==f2);
    restart=f1;
    CHECK(W->ring.recover(&last,&restart)==kIOReturnSuccess && fencemem[1]==f1 && fencemem[3]==f1 && fencemem[0]==0);
    CHECK(W->ring.replayFramed(words,4,f1,f1)==kIOReturnBadArgument);       /* not after `after` */
    CHECK(W->ring.replayFramed(words,4,f2+1,f1)==kIOReturnBadArgument);     /* never issued */
    CHECK(W->ring.replayFramed(words,4,0,f1)==kIOReturnBadArgument);
    CHECK(W->ring.replayFramed(words,4,f2,f1,NativeAdrenoRingSourceExternal)==kIOReturnSuccess && W->ring.lastSubmitted()==f2);
    CHECK(wptr()==NativeAdrenoRing::framedWords(4,true) && NativeAdrenoRing::framedWords(4,false)==4+NativeAdrenoRingPrologueWords+
          NativeAdrenoRingFramedFenceWords);
    CHECK(ringmem[6]==f1 && ringmem[NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords+4+1]==f2);
    /* A hold on a memptrs page too small for its word is refused; disarming always works. */
    static uint32_t ring2[2048],fence2[16];
    static NativeAdrenoRing r2;
    static NativeAdrenoSubmit s2;
    CHECK(r2.attach(&ringio,&ringcache,ring2,sizeof ring2,0x200000,fence2,sizeof fence2,0x208000)==kIOReturnSuccess);
    CHECK(s2.attach(&W->vm,&r2)==kIOReturnSuccess);
    CHECK(s2.armHold(NativeAdrenoRangeBit(HOG))==kIOReturnUnsupported && s2.armHold(0)==kIOReturnSuccess && !s2.holdArmed());
    CHECK(W->submit.armHold(NativeAdrenoRangeBit(HOG))==kIOReturnSuccess && W->submit.armHold(0)==kIOReturnSuccess &&
          !W->submit.holdArmed());
}

/* lead/v131y7 (gpu/recover2 merged onto the R3b daily line of gpu/sustain1): the replay also runs in the guard's daily mode (the daily
 * line's recovery: a client's hang never closes the qualification; the GMU restarts at level 1 / bus 2), with the same outcome as
 * testReplay: the culprit alone faulted, WindowServer's and the bench's jobs replayed, not lost, no unsafe exit; and never in the
 * strict qualification mode (R3b without daily), whose recovery keeps failing every job in flight (the gate's old fault-all). */
static unsigned gUnsafeExits;
static void unsafeExit(void *,A830ExitOrigin) {++gUnsafeExits;}
static void testDailyLine()
{
    boot();
    gUnsafeExits=0;
    gRecovery.qualification(unsafeExit,nullptr);gRecovery.qualificationR3b();gRecovery.qualificationDaily();
    const uint32_t f1=submitJob(WS);
    CHECK(W->submit.armHold(NativeAdrenoRangeBit(HOG))==kIOReturnSuccess);
    const uint32_t f2=submitJob(HOG),f3=submitJob(WS),f4=submitJob(BENCH);
    complete(f1,0);
    const uint64_t wsBefore=gShell.faults(WS);
    CHECK(gRecovery.recover(A830FaultWatchdog,0x7E57)==kIOReturnSuccess);
    CHECK(gShell.begins==0 && gShell.beginsReplay==1 && gOps.count("failInflight")==0 && gOps.count("plan")==1 && gOps.count("replay")==1);
    const NativeAdrenoSubmitPlan &p=gShell.plan;
    CHECK(p.mode==NativeAdrenoSubmitPlanReplay && p.culprit_range==HOG && p.culprit_fence==f2 && p.restart==f2 && p.replay_jobs==2 &&
          p.first_replay==f3 && p.last_replay==f4);
    CHECK(gShell.faults(HOG)==1 && gShell.faults(WS)==wsBefore && gShell.faults(BENCH)==0);
    CHECK(gRecovery.stats().replayRecoveries==1 && gRecovery.stats().jobsReplayed==2 && gRecovery.stats().recoveries==1 &&
          !gRecovery.lost() && gUnsafeExits==0 && gRecovery.replaying());
    complete(f4,0);
    CHECK(W->submit.waitAll(&ringclock,1000)==kIOReturnSuccess && W->submit.outstanding()==0);
    /* strict mode: no replay; every job in flight failed at the gate (WindowServer faulted), as before gpu/recover2 */
    boot();
    gUnsafeExits=0;
    gRecovery.qualification(unsafeExit,nullptr);gRecovery.qualificationR3b();
    submitJob(HOG);submitJob(WS);
    gRecovery.recover(A830FaultWatchdog,1);
    CHECK(!gRecovery.replaying() && gShell.beginsReplay==0 && gShell.begins==1 && gOps.count("plan")==0 && gOps.count("replay")==0);
    CHECK(gShell.faults(WS)>=1 && W->ring.replays()==0);   /* WindowServer faulted with the rest (the old fail-all) */
}

int main()
{
    adreno830_profile_value=A830ProfileDesign;
    testReplay();
    testCulpritAfterHalt();
    testDriverCauseAndIdle();
    testRepeatedCulprit();
    testHangDuringReplay();
    testFallbacks();
    testReplayFailure();
    testAbSwitch();
    testRingReplay();
    testDailyLine();
    delete W;W=nullptr;
    std::printf("PASS: A830 recovery replay end to end: culprit alone faulted (WindowServer kept), innocent jobs replayed with their own "
                "fences and context entries, S6 at the restart fence, the hold packet, culprit on the final fence, driver causes "
                "replay all, repeated culprit refused after %u hangs, third in 60 s final, hang during replay, poisoned/gap/room "
                "fallbacks, replay failure final at S6.0x18, A/B switch, the R3b daily line replays and strict mode does not (%u checks)\n",unsigned(NativeAdrenoClientHangLimit),gChecks);
    return 0;
}
