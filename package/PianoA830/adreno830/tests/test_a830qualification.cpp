/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c */
#include "A830PowerFakes.h"
#include "A830Profile.h"
#include "A830Qualification.h"
#include "../../../PianoThermal/BoardTemps.h"
static A830PowerState power;
static A830Recovery recovery;
static A830Dcvs dcvs;
static A830Qualification qual;
static unsigned unsafe;
static void halt(void *,A830ExitOrigin) {++unsafe;}
static uint32_t raw=Sm8750TsensValidBit|450;
static uint32_t tempRead(void *,uint32_t) {return raw;}
static Sm8750TsensIo tsens={nullptr,tempRead};
static void voteFault(void *,uint8_t,uint8_t,IOReturn) {CHECK(!gEnv.locked);qual.fault(A830QualVote,gEnv.now);}
static void lease(void *,uint64_t now) {qual.checkLease(now);}
static void sample(void *,uint64_t now,const Sm8750TsensSample *s) {qual.sample(now,*s);}
static void boot(const A830RecoveryOps &opsRecovery=gRecoveryOps) {
    resetAll();unsafe=0;raw=Sm8750TsensValidBit|450;adreno830_profile_value=A830ProfileAnnibale;
    CHECK(power.init(A830PowerConfig{},gIo,gPowerEnv,gPowerOps)==kIOReturnSuccess);
    CHECK(recovery.init(A830RecoveryConfig{},gIo,gPowerEnv,gPowerOps,opsRecovery,&power)==kIOReturnSuccess);
    recovery.qualification(halt,nullptr);power.start(1,2);
    qual=A830Qualification{};qual.init(&power,&recovery);
    A830DcvsConfig c;c.qualification607=true;c.maxLevel=7;c.maxMhz=607;c.maxGiven=true;c.initialLevel=1;c.initialBus=2;
    A830DcvsOps ops=gDcvsOps;ops.qualificationFault=voteFault;
    ops.qualificationService=lease;ops.qualificationSample=sample;
    CHECK(dcvs.init(c,kTable,gIo,gPowerEnv,ops,&power,&recovery,&tsens)==kIOReturnSuccess);
    CHECK(dcvs.start()==kIOReturnSuccess);
}
static Sm8750TsensSample gpu() {Sm8750TsensSample s;CHECK(Sm8750TsensReadGpu(&tsens,&s)==0);return s;}
static piano_gpu_temps board(uint32_t round=1) {
    piano_gpu_temps b={};b.version=1;b.state=2;b.valid=3;b.period_ms=5000;b.rounds=round;
    b.milli_c[0]=45000;b.milli_c[1]=30000;b.sample_us[0]=b.sample_us[1]=uint64_t(round)*5000000;return b;
}
static void openPermit() {
    auto b=board();qual.tick(gEnv.now,gpu(),b,true);CHECK(!qual.arm());
    qual.tick(gEnv.now,gpu(),b,true);CHECK(!qual.ready()); // cached round cannot open
    b=board(2);qual.tick(gEnv.now,gpu(),b,true);CHECK(qual.ready() && qual.arm());
    qual.tick(gEnv.now,gpu(),b,true);CHECK(qual.high() && dcvs.ceiling()==7);
}
static void step(bool heartbeat=true) {
    gEnv.now+=16000000;
    if(heartbeat)qual.tick(gEnv.now,gpu(),board(3),true);
    gHw.busyTicks+=A830_XO_HZ*16/1000;dcvs.noteSubmit();dcvs.serviceTick(gEnv.now);
}
int main() {
    // Recorded R3a2: valid TSENS, fresh/in-bound ADC, but max >=70C. This is not a5s cadence/lease failure.
    boot();auto recorded=board(4);recorded.milli_c[0]=57753;recorded.milli_c[1]=33079;
    recorded.sample_us[0]=15838780;recorded.sample_us[1]=15839480;
    recorded.age_ms[0]=4096;recorded.age_ms[1]=4095;
    raw=Sm8750TsensValidBit|706;qual.tick(19935078000ull,gpu(),recorded,true);
    CHECK(!qual.ready() && !qual.reason() && !qual.arm() && !qual.high());
    CHECK(qual.inhibit()==(A830QualGpuOver|A830QualNeedRounds));
    recorded=board(41);recorded.milli_c[0]=64127;recorded.milli_c[1]=34481;
    recorded.age_ms[0]=recorded.age_ms[1]=3341;raw=Sm8750TsensValidBit|741;
    qual.tick(204251354000ull,gpu(),recorded,true);CHECK(!qual.ready() && !qual.reason());
    CHECK(qual.inhibit() & A830QualGpuOver);
    // Cool fresh samples with the real4.1s age are allowed, after two advancing rounds. Cached rounds still fail.
    raw=Sm8750TsensValidBit|672;recorded=board(42);recorded.age_ms[0]=4096;recorded.age_ms[1]=4095;
    qual.tick(209251354000ull,gpu(),recorded,true);CHECK(!qual.ready() && qual.inhibit()==A830QualNeedRounds);
    qual.tick(209251354001ull,gpu(),recorded,true);CHECK(!qual.ready());
    recorded=board(43);recorded.age_ms[0]=4096;recorded.age_ms[1]=4095;
    qual.tick(214251354000ull,gpu(),recorded,true);CHECK(qual.ready() && !qual.inhibit() && !qual.reason());
    recorded.age_ms[0]=10000;qual.tick(214251354001ull,gpu(),recorded,true);CHECK(qual.ready());
    recorded.age_ms[0]=10001;qual.tick(214251354002ull,gpu(),recorded,true);
    CHECK(!qual.ready() && (qual.inhibit() & A830QualBoardInvalid) && !qual.reason());
    boot();CHECK(dcvs.ceiling()==3);openPermit();
    qual.tick(gEnv.now+1,gpu(),board(3),true);qual.checkLease(gEnv.now);CHECK(!qual.reason());
    for(unsigned i=0;i<100;++i)step();CHECK(dcvs.level()==7);
    // Applied/unknown hardware is not modeled as the last ACK; this tests the actual post-unlock failure callback.
    gOps.fail["perfVote"]=kIOReturnTimeout;dcvs.forceQualificationVote();step();
    CHECK(qual.reason()==A830QualVote && power.qualificationLatched() && recovery.stats().recoveries==1 && !unsafe);
    CHECK(gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && power.votedLevel()==1);
    CHECK(gOps.index("gate:1")<gOps.index("gmuForceOff") && gOps.index("gmuForceOff")<gOps.index("gmuStart"));
    CHECK(!qual.arm());
    for(unsigned i=0;i<100;++i)step();CHECK(dcvs.level()==3 && !dcvs.stats().votesStopped);
    raw=Sm8750TsensValidBit|700;step();CHECK(dcvs.level()==1); // no permanent vote freeze
    raw=Sm8750TsensValidBit|590;for(unsigned i=0;i<100;++i)step();CHECK(dcvs.level()==3);
    power.setTarget(11,10);power.setWakeTargets(11,10,11,10);CHECK(power.targetLevel()==3);
    CHECK(power.enterSlumber(A830SlumberFull)==kIOReturnSuccess && power.wake(A830WakeTouch)==kIOReturnSuccess);
    CHECK(gOps.lastArgs.level<=3 && power.votedLevel()<=3);
#ifdef ADRENO830_MACOS
    adreno830_dcvs_set_ceiling(160000);
    power.setTarget(11,10);power.setWakeTargets(11,10,11,10);
    CHECK(power.enterSlumber(A830SlumberFull)==0 && power.wake(A830WakeTouch)==0 && power.votedLevel()==1);
    step();CHECK(dcvs.level()==1);
    adreno830_dcvs_set_ceiling(0);for(unsigned i=0;i<100;++i)step();CHECK(dcvs.level()==3);
#endif
    dcvs.park();step();CHECK(dcvs.level()==1);
    boot();openPermit();for(unsigned i=0;i<100;++i)step();
    gOps.fail["perfVote"]=kIOReturnTimeout;dcvs.forceQualificationVote();step();
    gOps.fail["perfVote"]=kIOReturnIOError;dcvs.forceQualificationVote();step();
    CHECK(recovery.stats().recoveries==2 && gOps.count("gmuForceOff")==2 && !unsafe);
    gOps.fail["perfVote"]=kIOReturnIOError;dcvs.forceQualificationVote();step();
    CHECK(recovery.lost() && recovery.safeOff() && gOps.gates.back()==A830GateLost); // recovery budget exhausted still powers off
    boot();openPermit();for(unsigned i=0;i<100;++i)step();
    gEnv.now+=3000000000ull;step(false);
    CHECK(qual.reason()==A830QualSensor && power.qualificationLatched() && recovery.stats().recoveries==1);
    // A short-lived hot sample seen only by DCVS cannot reopen high when the next heartbeat is cool.
    boot();openPermit();for(unsigned i=0;i<10;++i)step();raw=Sm8750TsensValidBit|700;step(false);
    raw=Sm8750TsensValidBit|590;for(unsigned i=0;i<10;++i)step(false);
    CHECK(power.qualificationLatched() && qual.reason()==A830QualGpuHot && dcvs.ceiling()<=3);
    // Each temperature/freshness failure closes the actual ring, requests immediate low restart and cannot re-arm.
    for(unsigned fault=0;fault<9;++fault) {
        boot();openPermit();auto s=gpu();auto b=board(3);
        if(fault==0)s.max_deci=s.deci[0]=700;
        if(fault==1)b.milli_c[0]=65000;
        if(fault==2)b.milli_c[1]=40000;
        if(fault==3)s.valid=254;
        if(fault==4)b.age_ms[0]=10001;
        if(fault==5)b.disabled=1;
        if(fault==6)b.state=3;
        if(fault==7)b.valid=1;
        if(fault==8)gEnv.now+=602000000000ull;
        qual.tick(gEnv.now,s,b,true);CHECK(qual.reason() && !qual.high() && !qual.arm() && power.qualificationLatched());
        CHECK(recovery.pending() && gOps.gates.back()==A830GateRecovering);
        CHECK(qual.deadlineMissed(gEnv.now+5000000000ull)); // independent wall deadline before service runs
        CHECK(recovery.runPending()==kIOReturnSuccess && gOps.lastArgs.level==1 && !unsafe);
        CHECK(!qual.deadlineMissed(gEnv.now+5000000000ull));
    }
    // Force-off and each restart stage fail closed. A failed strict stop escalates; no new M9 follows it.
    for(const char *fail:{"gmuForceOff","gmuStart","cpStart","secureResume"}) {
        boot();openPermit();gOps.fail[fail]=kIOReturnIOError;qual.fault(A830QualVote,gEnv.now);
        CHECK(recovery.runPending()!=kIOReturnSuccess && recovery.lost() && gOps.gates.back()==A830GateLost);
        if(std::string(fail)=="gmuForceOff")CHECK(unsafe==1 && gOps.count("gmuStart")==0);
        else CHECK(!unsafe && gOps.count("gmuForceOff")==2 && recovery.safeOff());
    }
    boot();gOps.fail["gmuStart"]=kIOReturnIOError;
    gOps.during=[](const std::string &name) {if(name=="gmuStart")gOps.fail["gmuForceOff"]=kIOReturnIOError;};
    qual.fault(A830QualVote,gEnv.now);CHECK(recovery.runPending()!=0 && unsafe==1);
    // Read-only board API: invalid/stopped/old snapshots never turn into cool values.
    using namespace pianothermal::adc;
    Snapshot s;s.state=2;s.periodMs=5000;s.rounds=3;
    s.validMask=(1u<<channelIndex("msm"))|(1u<<channelIndex("quiet"));
    s.atUs[channelIndex("msm")]=s.atUs[channelIndex("quiet")]=1000;
    piano_gpu_temps b={};b.version=1;CHECK(fillGpuTemps(s,10001000,&b) && b.valid==3);
    CHECK(fillGpuTemps(s,10002000,&b) && !b.valid);
    s.state=3;CHECK(fillGpuTemps(s,1000,&b) && !b.valid);
    std::printf("PASS: C5 qualification: two fresh rounds, partial/invalid/hot/expired guards, independent deadline, actual gate closure, strict recovery refusal/escalation, low M9, sticky wake/thermal/external/park and mutable160 (%u checks)\n",gChecks);
    return 0;
}
