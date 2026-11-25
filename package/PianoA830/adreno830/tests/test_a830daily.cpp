/* SPDX-License-Identifier: MIT */
/* The R3b guard's daily mode (pagani-gpu-daily=1, A830Qualification::daily; gpu/daily1, GPU-DAILY.md): the permit arms itself
 * once the guard is ready, has no expiry, and the governor moves freely between 160 and 607 MHz under it: 40 rounds of 10 s
 * saturated / 10 s idle (800 s, past the 625 s and 602 s windows) reach 607 and the floor every round, with ordinary votes only
 * (no fault, no latch, no low transaction). The hard exits still work in daily mode (board 65 C -> the low vote, <= 342 for the
 * boot), and without daily() nothing arms by itself (the operator permit stays required).
 * host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c */
#define main qualificationMain
#include "test_a830qualification.cpp"
#undef main

static uint64_t snapshotD(void *) {return qual.thermalSnapshot();}
static unsigned lowVotes;
static IOReturn lowVoteD(void *,bool drop) {++lowVotes;return gOps.hit(drop?"dropLow":"confirmLow");}
static IOReturn quiesceD(void *) {return gOps.hit("strictQuiesce");}
/* as Adreno830Start.cpp qualificationVoteFault: the fault, and in daily mode the recovery report */
static void voteFaultD(void *,uint8_t,uint8_t,IOReturn error) {
    qual.fault(A830QualVote,gEnv.now);
    if(qual.isDaily())recovery.report(A830FaultHfi,uint32_t(error));
}
static void bootD(bool daily) {
    A830RecoveryOps ops=gRecoveryOps;ops.qualificationLowVote=lowVoteD;ops.qualificationQuiesce=quiesceD;
    ops.qualificationGxOff=[](void *) {return true;};
    dcvs=A830Dcvs{};boot(ops);qual=A830Qualification{};qual.init(&power,&recovery,true,halt,false);
    if(daily)qual.daily();
    A830DcvsConfig c;c.qualification607=true;c.qualificationR3b=true;c.maxLevel=7;c.maxMhz=607;c.maxGiven=true;
    c.initialLevel=1;c.initialBus=2;A830DcvsOps o=gDcvsOps;
    o.qualificationFault=voteFaultD;o.qualificationService=lease;o.qualificationSample=sample;o.qualificationThermal=snapshotD;
    CHECK(dcvs.init(c,kTable,gIo,gPowerEnv,o,&power,&recovery,&tsens)==0);CHECK(dcvs.start()==0);lowVotes=0;
}
static void tickD(int deci=650,uint64_t dt=100000000,piano_gpu_temps b={}) {
    gEnv.now+=dt;raw=Sm8750TsensValidBit|uint32_t(deci);
    if(!b.version)b=board(uint32_t(gEnv.now/5000000000ull)+1);
    qual.tick(gEnv.now,gpu(),b,true);
}
static void driveD(unsigned n,bool busy) {
    for(unsigned i=0;i<n;++i) {
        tickD();
        if(busy) {gHw.busyTicks+=A830_XO_HZ/10;dcvs.noteSubmit();}
        dcvs.serviceTick(gEnv.now);
    }
}

/* gpu-daily2 (gpu-daily1: an iconservicesagent watchdog hang went strict and the box reset): in daily mode a GPU hang is the
 * daily line's recovery: no strict quiesce, no close/latch, a restart, 607 again afterwards, no native unsafe exit; a restart that
 * fails declares the GPU lost without the unsafe exit; an unacknowledged vote is reported and recovered (<= 342 for the boot, no
 * strict, no unsafe); no protection deadline in daily mode. */
static void dailyRecovery() {
    bootD(true);
    for(unsigned i=0;i<60 && !qual.high();++i)tickD();
    driveD(100,true);CHECK(qual.high() && dcvs.level()==7);
    const unsigned unsafe0=unsafe;
    recovery.report(A830FaultWatchdog,0x2710);
    driveD(5,true);
    CHECK(gOps.count("strictQuiesce")==0 && gOps.count("gmuForceOff")>=1 && gOps.count("gmuStart")>=1);
    CHECK(recovery.stats().recoveries==1 && !recovery.lost() && unsafe==unsafe0);
    CHECK(!power.qualificationLatched() && qual.high() && !qual.reason());
    driveD(100,true);CHECK(dcvs.level()==7);
    CHECK(!qual.deadlineMissed(gEnv.now+10000000000ull));
    /* a restart that fails: lost, no unsafe exit */
    gOps.fail["gmuStart"]=kIOReturnTimeout;gOps.sticky["gmuStart"]=true;
    recovery.report(A830FaultHang,0);
    driveD(5,true);
    CHECK(recovery.lost() && unsafe==unsafe0 && gOps.count("strictQuiesce")==0);
    gOps.sticky.erase("gmuStart");gOps.fail.erase("gmuStart");
    /* an unacknowledged vote: reported and recovered, <= 342 for the boot, no strict, no unsafe */
    bootD(true);
    for(unsigned i=0;i<60 && !qual.high();++i)tickD();
    driveD(100,true);CHECK(dcvs.level()==7);
    gOps.fail["perfVote"]=kIOReturnTimeout;
    driveD(100,false);
    driveD(40,true);
    CHECK(power.qualificationLatched() && dcvs.level()<=3 && !recovery.lost() && unsafe==unsafe0 && gOps.count("strictQuiesce")==0);
    CHECK(recovery.stats().recoveries>=1);
}

int main() {
    /* 1. auto-arm and no expiry */
    bootD(true);
    CHECK(qual.isDaily() && !qual.high());
    for(unsigned i=0;i<60 && !qual.high();++i)tickD();
    CHECK(qual.ready() && qual.high() && qual.openedAt());
    CHECK(!qual.arm());                                    /* already armed: the operator command changes nothing */
    const uint64_t opened=qual.openedAt();
    const uint32_t votes0=dcvs.stats().votes;
    unsigned top=0,floor=0;
    for(unsigned round=0;round<40;++round) {
        driveD(100,true);top+=dcvs.level()==7;
        driveD(100,false);floor+=dcvs.level()<=2;
    }
    CHECK(gEnv.now-opened>700000000000ull);
    CHECK(top==40 && floor==40);
    CHECK(!qual.reason() && qual.high() && !power.qualificationLatched() && lowVotes==0 && !recovery.lost());
    const uint32_t transitions=dcvs.stats().votes-votes0;
    CHECK(transitions>=80 && dcvs.stats().voteFailures==0);   /* at least one up and one down per round */
    /* 2. a hard exit in daily mode: the board's hard line -> the low vote (confirmed) -> <= 342 for the boot */
    driveD(100,true);CHECK(dcvs.level()==7);
    auto hot=board(uint32_t(gEnv.now/5000000000ull)+1);hot.milli_c[0]=65000;
    tickD(650,100000000,hot);
    CHECK(qual.reason()==A830QualBoardHot && power.qualificationCap()==1);
    driveD(30,true);
    CHECK(lowVotes==1 && dcvs.level()<=3 && !recovery.lost() && !qual.high());
    driveD(200,true);CHECK(dcvs.level()<=3);              /* no re-arm within the boot */
    /* 3. without daily(): ready, but nothing arms by itself */
    bootD(false);
    for(unsigned i=0;i<60;++i)tickD();
    CHECK(qual.ready() && !qual.high() && !qual.isDaily());
    driveD(100,true);CHECK(dcvs.level()<=3);
    dailyRecovery();
    std::printf("PASS: R3b daily mode: auto-armed, no expiry over 800 s, 40 x (607 and the floor) with %u ordinary votes, 0 faults; "
                "a board hard exit still takes the low vote (<= 342 for the boot); no auto-arm without daily(); daily recovery: a watchdog "
                "hang restarts without strict/latch/unsafe and 607 comes back, a failed restart is lost without the unsafe exit, an "
                "unacknowledged vote recovers to <= 342 without strict/unsafe, no protection deadline\n",
                unsigned(transitions));
    return 0;
}
