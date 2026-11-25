/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c */
#define main r3aRegressionMain
#include "test_a830qualification.cpp"
#undef main
static uint64_t snapshot(void *) {return qual.thermalSnapshot();}
static IOReturn lowVote(void *,bool drop) {
    CHECK(gEnv.locked && power.qualificationBlocked() && power.qualificationEffectiveCap()==1);
    return gOps.hit(drop?"dropLow":"confirmLow");
}
static IOReturn quiesce(void *) {CHECK(gEnv.locked);return gOps.hit("strictQuiesce");}
static void bootB(bool residency607=false) {
    A830RecoveryOps ops=gRecoveryOps;ops.qualificationLowVote=lowVote;ops.qualificationQuiesce=quiesce;
    ops.qualificationGxOff=[](void *) {return true;};
    dcvs=A830Dcvs{};boot(ops);qual=A830Qualification{};qual.init(&power,&recovery,true,halt,residency607);
    A830DcvsConfig c;c.qualification607=true;c.qualificationR3b=true;c.maxLevel=7;c.maxMhz=607;c.maxGiven=true;
    if(residency607)c.minMhz=607;
    c.initialLevel=1;c.initialBus=2;A830DcvsOps o=gDcvsOps;
    o.qualificationFault=voteFault;o.qualificationService=lease;o.qualificationSample=sample;o.qualificationThermal=snapshot;
    CHECK(dcvs.init(c,kTable,gIo,gPowerEnv,o,&power,&recovery,&tsens)==0);CHECK(dcvs.start()==0);
}
static void tickB(int deci=740,uint64_t dt=100000000,piano_gpu_temps b={}) {
    gEnv.now+=dt;raw=Sm8750TsensValidBit|uint32_t(deci);
    if(!b.version)b=board(uint32_t(gEnv.now/5000000000ull)+1);
    qual.tick(gEnv.now,gpu(),b,true);
}
static void openB(int deci=740) {
    tickB(deci);CHECK(!qual.ready() && !qual.arm());
    for(unsigned i=0;i<51;++i)tickB(deci);
    CHECK(qual.ready() && qual.arm());tickB(deci);CHECK(qual.high() && dcvs.ceiling()==3);
}
static void driveB(unsigned n=1,int deci=740) {
    for(unsigned i=0;i<n;++i) {
        tickB(deci);gHw.busyTicks+=A830_XO_HZ/10;dcvs.noteSubmit();dcvs.serviceTick(gEnv.now);
    }
}
/* gpu/daily4: the qualified ceiling at 660 (level 8) or 734 (level 9): the guard's ladder climbs to it when cool, its bus is
 * Sm8750's default (6 at 660, 8 at 734), a hot reading steps down from it one level per 500 ms, and no other top is accepted. */
static void bootTop(uint8_t top,uint32_t mhz) {
    A830RecoveryOps ops=gRecoveryOps;ops.qualificationLowVote=lowVote;ops.qualificationQuiesce=quiesce;
    ops.qualificationGxOff=[](void *) {return true;};
    dcvs=A830Dcvs{};boot(ops);qual=A830Qualification{};qual.init(&power,&recovery,true,halt,false);
    CHECK(qual.setTop(top) && qual.top()==top);
    A830DcvsConfig c;c.qualification607=true;c.qualificationR3b=true;c.maxLevel=top;c.maxMhz=mhz;c.maxGiven=true;
    c.initialLevel=1;c.initialBus=2;A830DcvsOps o=gDcvsOps;
    o.qualificationFault=voteFault;o.qualificationService=lease;o.qualificationSample=sample;o.qualificationThermal=snapshot;
    CHECK(dcvs.init(c,kTable,gIo,gPowerEnv,o,&power,&recovery,&tsens)==0);CHECK(dcvs.start()==0);
}
static void topLevels() {
    CHECK(A830Qualification::defaultBus(3)==3 && A830Qualification::defaultBus(7)==6 && A830Qualification::defaultBus(8)==6 &&
          A830Qualification::defaultBus(9)==8);
    {A830Qualification q;CHECK(q.top()==7 && !q.setTop(6) && !q.setTop(10) && q.top()==7);}
    const uint32_t mhz[2]={660,734};
    for(uint8_t top=8;top<=9;++top) {
        bootTop(top,mhz[top-8]);openB();
        driveB(130);
        CHECK(qual.thermal().step==top && dcvs.level()==top && power.qualificationCap()==top && !qual.reason());
        driveB(40);
        CHECK(qual.thermal().step==top && dcvs.level()==top);   /* never past it */
        tickB(849);CHECK(qual.thermal().step==top-1 && !qual.reason());
        driveB(5,849);CHECK(qual.thermal().step==top-2);
        driveB(80,740);CHECK(qual.thermal().step==top);        /* cool again (the filter first): back up, one level per 2 s */
    }
    /* the default stays 607 */
    bootB();openB();driveB(130);CHECK(qual.thermal().step==7 && dcvs.level()==7);
}
/* gpu/fullopp1 (private/host-checks/20261122-gpu-fullopp/ANALYSIS.md 3.3): Android's thermal table in the guard and tops up to 14 on the
 * bin-2 table: the ladder climbs to 1100 MHz when cool; the virtual skin caps 1050 from 25 C (clear < 23) and 734 from 45 C (clear < 42);
 * no usable skin reading caps 734; XO caps 734 from 78 C (clear < 70) and exits at 90 C; silicon steps down from 95 C and exits at 100 C;
 * the board thermistors neither hold nor exit (gpu-daily3c latched at msm 65.4 C where Android caps at 1050). */
static const Adreno830Level kLevels14[14]={
    {160000,0x32,2,2,2,0,0},{222000,0x34,2,2,2,0,0},{342000,0x38,2,3,3,0,0},{389000,0x3c,2,4,6,0,0xc8285ffd},
    {443000,0x40,2,4,6,0,0xc02e5ffd},{525000,0x50,2,4,6,0,0xc02e5ffd},{607000,0x80,4,6,8,0,0xa82e5ffd},
    {660000,0x90,4,6,8,0,0x882c5ffd},{734000,0xc0,6,8,10,0,0xa82a5ffd},{832000,0xe0,7,10,10,0,0x882a5ffd},
    {900000,0x100,7,10,10,0,0x882a5ffd},{967000,0x140,11,11,11,0,0x882a5ffd},{1050000,0x1a0,11,11,11,0,0xa8295ffd},
    {1100000,0x1c0,11,11,11,0,0x88295ffd}};
static const A830DcvsTable kTable14={kLevels14,14,kDdr,12};
static piano_skin_temps skinOf(int32_t virtualMilli,int32_t xoMilli=40000) {
    piano_skin_temps k={};k.version=PIANO_SKIN_TEMPS_VERSION;k.state=PIANO_BOARD_TEMPS_RUNNING;k.period_ms=5000;k.rounds=1;
    k.valid=(1u<<PIANO_SKIN_COUNT)-1u;k.milli_c[PIANO_SKIN_XO]=xoMilli;k.virtual_milli_c=virtualMilli;k.virtual_valid=1;k.virtual_used=31;
    return k;
}
static void bootFull(uint8_t top=14,uint32_t mhz=1100) {
    A830RecoveryOps ops=gRecoveryOps;ops.qualificationLowVote=lowVote;ops.qualificationQuiesce=quiesce;
    ops.qualificationGxOff=[](void *) {return true;};
    dcvs=A830Dcvs{};boot(ops);qual=A830Qualification{};qual.init(&power,&recovery,true,halt,false);
    qual.fullopp();CHECK(qual.isFullopp() && qual.setTop(top) && qual.top()==top);
    A830DcvsConfig c;c.qualification607=true;c.qualificationR3b=true;c.maxLevel=top;c.maxMhz=mhz;c.maxGiven=true;c.acd=true;
    c.sustainKhz=0;c.initialLevel=1;c.initialBus=2;A830DcvsOps o=gDcvsOps;
    o.qualificationFault=voteFault;o.qualificationService=lease;o.qualificationSample=sample;o.qualificationThermal=snapshot;
    CHECK(dcvs.init(c,kTable14,gIo,gPowerEnv,o,&power,&recovery,&tsens)==0);CHECK(dcvs.start()==0);
}
static void tickF(int deci,const piano_skin_temps *k,piano_gpu_temps b={}) {
    gEnv.now+=100000000;raw=Sm8750TsensValidBit|uint32_t(deci);
    if(!b.version)b=board(uint32_t(gEnv.now/5000000000ull)+1);
    qual.tick(gEnv.now,gpu(),b,true,false,k);
}
static void driveF(unsigned n,int deci,const piano_skin_temps *k) {
    for(unsigned i=0;i<n;++i) {tickF(deci,k);gHw.busyTicks+=A830_XO_HZ/10;dcvs.noteSubmit();dcvs.serviceTick(gEnv.now);}
}
static void androidProfile() {
    {A830Qualification q;CHECK(!q.setTop(10) && !q.setTop(14));q.fullopp();CHECK(q.setTop(14) && !q.setTop(15) && !q.setTop(6));}
    CHECK(A830Qualification::defaultBus(10)==10 && A830Qualification::defaultBus(12)==11 && A830Qualification::defaultBus(14)==11);
    const piano_skin_temps cool=skinOf(20000);
    bootFull();openB();
    driveF(240,740,&cool);   /* cool: the ladder climbs one level per 2 s from 342 to the top */
    CHECK(qual.thermal().step==14 && dcvs.level()==14 && power.qualificationCap()==14 && !qual.reason());
    CHECK(qual.thermal().cause==A830R3bThermal::CauseNone && qual.thermal().skinValid && !qual.thermal().skin25);
    /* the virtual skin: 25 C -> 1050 (GMU 13), held until below 23 C */
    piano_skin_temps k=skinOf(25000);driveF(3,740,&k);
    CHECK(qual.thermal().skinCap==13 && qual.thermal().cause==A830R3bThermal::CauseSkin25 && dcvs.level()==13 && !qual.reason());
    k=skinOf(23000);driveF(3,740,&k);CHECK(dcvs.level()==13);
    k=skinOf(22999);driveF(3,740,&k);CHECK(qual.thermal().skinCap==14 && dcvs.level()==14);
    /* 45 C -> 734 (GMU 9) until below 42 C, then the 25 C cap again */
    k=skinOf(45000);driveF(3,740,&k);CHECK(qual.thermal().skinCap==9 && qual.thermal().cause==A830R3bThermal::CauseSkin45 &&
                                           dcvs.level()==9 && !qual.reason());
    k=skinOf(42000);driveF(3,740,&k);CHECK(dcvs.level()==9);
    k=skinOf(41999);driveF(3,740,&k);CHECK(qual.thermal().skinCap==13 && dcvs.level()==13);
    /* no usable virtual reading: unknown, never cool -> 734 */
    driveF(3,740,nullptr);CHECK(qual.thermal().skinCap==9 && qual.thermal().cause==A830R3bThermal::CauseSkinUnknown && dcvs.level()==9);
    k=skinOf(20000);k.virtual_valid=0;driveF(3,740,&k);CHECK(qual.thermal().skinCap==9);
    k=skinOf(20000);k.version=2;driveF(3,740,&k);CHECK(qual.thermal().skinCap==9 && !qual.reason());
    /* XO: 78 C -> 734 until below 70 C */
    k=skinOf(20000,78000);driveF(3,740,&k);CHECK(qual.thermal().skinCap==9 && qual.thermal().cause==A830R3bThermal::CauseXo);
    k=skinOf(20000,70000);driveF(3,740,&k);CHECK(qual.thermal().skinCap==9);
    k=skinOf(20000,69999);driveF(3,740,&k);CHECK(qual.thermal().skinCap==14);
    driveF(30,740,&k);CHECK(dcvs.level()==14);
    /* the board thermistors alone: no hold, no exit (the R3b-607 line exits at msm 65 / quiet 40) */
    for(unsigned i=0;i<60;++i) {   /* 6 s of hot board rounds (fresh every 5 s, as PianoThermal's) */
        auto b=board(uint32_t((gEnv.now+100000000)/5000000000ull)+1);b.milli_c[0]=70000;b.milli_c[1]=45000;tickF(740,&cool,b);
    }
    CHECK(!qual.reason() && !qual.thermal().boardCap && power.qualificationCap()==14 && qual.ready());
    driveF(20,740,&cool);CHECK(dcvs.level()==14 && !qual.reason());
    /* silicon: below tj 95 C nothing; at 95 C one level down per 500 ms; up again once below 90 C */
    driveF(10,949,&cool);CHECK(qual.thermal().step==14 && !qual.reason());
    driveF(1,950,&cool);CHECK(qual.thermal().step==13 && !qual.reason());
    driveF(5,950,&cool);CHECK(qual.thermal().step==12);
    driveF(5,950,&cool);CHECK(qual.thermal().step==11 && dcvs.level()==11 && qual.thermal().cause==A830R3bThermal::CauseLadder);
    driveF(80,850,&cool);
    CHECK(qual.thermal().step==14 && dcvs.level()==14);   /* the filter falls under 90 C, then 2 s per level */
    /* exits: GPU raw 100 C */
    tickF(1000,&cool);CHECK(qual.reason()==A830QualGpuHot && power.qualificationCap()==1);
    bootFull();openB();driveF(240,740,&cool);CHECK(dcvs.level()==14);
    raw=Sm8750TsensValidBit|999;qual.sample(gEnv.now,gpu());CHECK(!qual.reason());
    raw=Sm8750TsensValidBit|1000;qual.sample(gEnv.now,gpu());CHECK(qual.reason()==A830QualGpuHot);
    /* XO 90 C, Android's shutdown trip: the guard exit */
    bootFull();openB();driveF(240,740,&cool);
    k=skinOf(20000,90000);tickF(740,&k);CHECK(qual.reason()==A830QualBoardHot && power.qualificationCap()==1);
    /* a lower top (fullopp with maxlevel 12 = 967 MHz) stops there */
    bootFull(12,967);openB();driveF(240,740,&cool);CHECK(qual.thermal().step==12 && dcvs.level()==12);
    /* without fullopp the same readings keep the R3b-607 rules (the board hold and exit) */
    bootB();openB();auto b=board(100);b.milli_c[0]=65000;tickB(740,100000000,b);CHECK(qual.reason()==A830QualBoardHot);
}
static void residencyBudget() {
    bootB(true);
    openB();
    const uint64_t opened=qual.openedAt();
    driveB(80);
    CHECK(dcvs.level()<=6 && !qual.reason());
    driveB(10);
    CHECK(dcvs.level()==7 && !qual.reason());
    driveB(6020);
    CHECK(qual.high() && dcvs.stats().residency607LongestNs>=600000000000ull);
    while(gEnv.now-opened<625000000000ull)driveB();
    CHECK(qual.reason()==A830QualExpired && !qual.high() && power.qualificationLatched());
    driveB(20);
    CHECK(dcvs.level()==3 && !recovery.lost());

    bootB(true);
    openB();
    driveB(90);
    auto hot=board(100);
    hot.milli_c[0]=65000;
    tickB(740,100000000,hot);
    CHECK(qual.reason()==A830QualBoardHot && power.qualificationCap()==1);
}
static void pureBoundaries() {
    for(int d:{799,800,849,850}) {
        bootB();openB();tickB(d);CHECK((qual.reason()==0)==(d<850));
        CHECK(qual.high()==(d<850));CHECK(qual.ready()==(d<800));
    }
    // The exact entry boundary is strict and hot samples between board rounds invalidate the two-round permit.
    bootB();tickB(800);for(unsigned i=0;i<60;++i)tickB(800);CHECK(!qual.ready() && !qual.arm());
    tickB(799);tickB(800);tickB(799);CHECK(!qual.ready());
    for(unsigned i=0;i<101;++i)tickB(799);CHECK(qual.ready());
    bootB();auto b=board(1);tickB(740,100000000,b);tickB(740,100000000,b);CHECK(!qual.ready());
    b=board(2);b.sample_us[1]=5000000;tickB(740,100000000,b);CHECK(!qual.ready());
    b=board(3);tickB(740,100000000,b);CHECK(qual.ready());
    b.age_ms[0]=b.age_ms[1]=10000;tickB(740,100000000,b);CHECK(qual.ready());
    b.age_ms[0]=10001;tickB(740,100000000,b);CHECK(!qual.ready());
    bootB();openB();auto s=gpu();s.valid=254;qual.tick(gEnv.now+100000000,s,board(4),true);
    CHECK(qual.reason()==A830QualSensor && recovery.pending());
    for(int bad:{-401,1251}) {
        bootB();openB();auto s=gpu();s.deci[7]=bad;qual.tick(gEnv.now+100000000,s,board(4),true);
        CHECK(qual.reason() && recovery.pending());if(bad==1251)CHECK(unsafe==1);
    }
    bootB();openB();gEnv.now+=250000000;qual.checkLease(gEnv.now);CHECK(!qual.reason());
    uint8_t l=7,bus=6;power.limitQualification(l,bus);CHECK(!qual.reason() && !power.qualificationLatched());
    ++gEnv.now;power.setTarget(11,10);CHECK(qual.reason()==A830QualSensor && recovery.pending() && power.targetLevel()==1);
    CHECK(power.qualificationBlocked() && gOps.gates.empty()); // retry submissions until the service verifies the ACK
    bootB();openB();gEnv.now+=251000000;tickB();CHECK(qual.reason()==A830QualSensor && !qual.high());
    bootB();openB();gEnv.now+=251000000;CHECK(power.acquire()!=0);CHECK(recovery.pending());
    bootB();raw=Sm8750TsensValidBit|850;qual.sample(gEnv.now,gpu());
    CHECK(qual.reason()==A830QualGpuHot && recovery.pending() && power.qualificationBlocked());
    CHECK(recovery.runPending()==0);const auto recoveries=recovery.stats().recoveries;
    driveB(20,850);CHECK(recovery.stats().recoveries==recoveries && !recovery.lost() && dcvs.level()==1);
    bootB();openB();tickB(1050);CHECK(unsafe==1 && recovery.pending());
    CHECK(qual.deadlineMissed(gEnv.now+5000000000ull));
    tickB(1050,100000000);CHECK(unsafe>=2); // independent of HFI/deadline/recovery state
}
static void stepsAndBoard() {
    bootB();openB();driveB(90);CHECK(qual.thermal().step==7 && dcvs.level()==7 && !qual.reason());
    // No 70/60 callback path remains in this explicit mode; 80..84.9 is soft only.
    raw=Sm8750TsensValidBit|800;qual.sample(gEnv.now,gpu());CHECK(!qual.reason());
    const uint64_t base=gEnv.now;tickB(849);CHECK(qual.thermal().step==6 && !qual.reason());
    driveB(3,849);CHECK(qual.thermal().step==6);driveB(2,849);CHECK(qual.thermal().step==5);
    driveB(20,849);CHECK(qual.thermal().step==3 && !qual.reason());
    // Lateness cannot catch up several downward windows; pure policy is used to avoid the separate lease exit.
    A830R3bThermal t;t.step=7;auto b=board(1);auto s=gpu();s.deci[0]=800;
    t.tick(base,s,b,true,true);CHECK(t.step==6);
    t.tick(base+2000000000,s,board(2),true,true);CHECK(t.step==6); // late => unhealthy
    t.tick(base+2100000000,s,board(3),true,true);CHECK(t.step==5);
    driveB(60,740);CHECK(qual.thermal().step>=4);
    // Cooling dwell must be continuous, including raw>=80 and board interruptions.
    bootB();openB();driveB(19);CHECK(qual.thermal().step==3);driveB(1,800);driveB(19);CHECK(qual.thermal().step==3);
    driveB(3);CHECK(qual.thermal().step==4);
    driveB(70);CHECK(qual.thermal().step==7);
    b=board(100);b.milli_c[1]=37000;tickB(740,100000000,b);CHECK(qual.thermal().boardCap && dcvs.ceiling()==3 && !qual.reason());
    power.setTarget(11,10);power.setWakeTargets(11,10,11,10);CHECK(power.targetLevel()==3);
    CHECK(power.enterSlumber(A830SlumberFull)==0 && power.wake(A830WakeTouch)==0 && power.votedLevel()<=3);
    b=board(101);b.milli_c[0]=60000;b.milli_c[1]=35000;tickB(740,100000000,b);CHECK(qual.thermal().boardCap);
    tickB(740,100000000,b);CHECK(qual.thermal().boardCap); // cached round cannot release
    b=board(102);b.milli_c[0]=60000;b.milli_c[1]=35000;tickB(740,100000000,b);CHECK(!qual.thermal().boardCap);
    b=board(103);b.milli_c[0]=62000;tickB(740,100000000,b);CHECK(qual.thermal().boardCap && dcvs.ceiling()==3);
    b=board(104);tickB(740,100000000,b);CHECK(qual.thermal().boardCap);
    b=board(105);b.milli_c[0]=60001;tickB(740,100000000,b);CHECK(qual.thermal().releaseRounds==0);
    b=board(106);tickB(740,100000000,b);CHECK(qual.thermal().boardCap);
    b=board(107);tickB(740,100000000,b);CHECK(!qual.thermal().boardCap);
#ifdef ADRENO830_MACOS
    adreno830_dcvs_set_ceiling(160000);power.setTarget(11,10);power.setWakeTargets(11,10,11,10);CHECK(power.targetLevel()==1);
    CHECK(power.enterSlumber(A830SlumberFull)==0 && power.wake(A830WakeTouch)==0 && power.votedLevel()==1);
    driveB();CHECK(dcvs.level()==1);adreno830_dcvs_set_ceiling(0);
#endif
    // Last-moment M9 capping reports the actually acknowledged level back through the real power bringUp handler.
    driveB(30);power.setTarget(11,10);
    CHECK(power.enterSlumber(A830SlumberFull)==0);
    gOps.during=[](const std::string &n) {if(n=="gmuStart") {
        CHECK(gOps.lastArgs.ackLevel && gOps.lastArgs.ackBus);
        *gOps.lastArgs.ackLevel=1;*gOps.lastArgs.ackBus=2;
    }};
    CHECK(power.wake(A830WakeTouch)==0 && power.votedLevel()==1 && power.votedBus()==2);
    gOps.during={};
    dcvs.park();power.setTarget(11,10);CHECK(power.targetLevel()==1);driveB();CHECK(dcvs.level()==1);
    // Board entry uses37/62, not the hard40/65 thresholds.
    for(unsigned ch=0;ch<2;++ch) {
        bootB();b=board(1);b.milli_c[ch]=ch?37000:62000;tickB(740,100000000,b);
        b.rounds=2;b.sample_us[0]+=5000000;b.sample_us[1]+=5000000;tickB(740,100000000,b);CHECK(!qual.ready());
        bootB();openB();b=board(100);b.milli_c[ch]=ch?40000:65000;tickB(740,100000000,b);
        CHECK(qual.reason()==A830QualBoardHot && power.qualificationLatched() && power.qualificationCap()==1);
    }
}
static void failuresB() {
    for(unsigned f=0;f<10;++f) {
        bootB();openB();auto b=board(100);auto s=gpu();
        if(f==0)b.valid=1;if(f==1)b.disabled=1;if(f==2)b.state=3;if(f==3)b.period_ms=1000;
        if(f==4)b.age_ms[0]=10001;if(f==5)b.age_ms[1]=10001;if(f==6)b.sample_us[0]=0;
        if(f==7)b.milli_c[1]=-40001;if(f==8)b.milli_c[0]=125001;if(f==9)s.valid=0;
        gEnv.now+=100000000;qual.tick(gEnv.now,s,b,true);CHECK(qual.reason() && recovery.pending());
    }
    // An absolute deadline cannot be extended by repeated healthy callbacks.
    bootB();openB();const uint64_t opened=qual.openedAt();
    while(gEnv.now-opened<602000000000ull)tickB();
    CHECK(qual.reason()==A830QualExpired && !qual.high() && recovery.pending());

    bootB();openB();driveB(90);CHECK(dcvs.level()==7);
    gOps.fail["perfVote"]=kIOReturnTimeout;dcvs.forceQualificationVote();driveB();
    CHECK(qual.reason()==A830QualVote && power.qualificationLatched() && recovery.stats().recoveries==1 && !unsafe);
    CHECK(gOps.index("gate:1")<gOps.index("gmuForceOff") && gOps.index("gmuForceOff")<gOps.index("gmuStart"));
    CHECK(gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && !qual.arm());
    driveB(20);CHECK(dcvs.level()==3 && !dcvs.stats().votesStopped);
    tickB(850);CHECK(power.qualificationCap()==1);tickB(740);driveB(20);CHECK(dcvs.ceiling()==3);
    // A late successful vote while the independent guard already closed the gate cannot reopen high.
    bootB();openB();driveB(90);
    gOps.during=[](const std::string &n) {if(n=="perfVote")qual.fault(A830QualSensor,gEnv.now);};
    dcvs.forceQualificationVote();driveB();CHECK(power.qualificationLatched() && dcvs.ceiling()<=3);
    for(const char *fail:{"gmuForceOff","gmuStart","cpStart","secureResume"}) {
        bootB();openB();gOps.fail[fail]=kIOReturnIOError;qual.fault(A830QualVote,gEnv.now);
        CHECK(recovery.runPending()!=0 && recovery.lost());
        if(std::string(fail)=="gmuForceOff")CHECK(unsafe==1 && gOps.count("gmuStart")==0);
        else CHECK(recovery.safeOff());
    }
    bootB();openB();qual.fault(A830QualSensor,gEnv.now);gEnv.now+=5000000000ull;qual.tick(gEnv.now,gpu(),board(100),true);CHECK(unsafe==1);
}
#ifndef A830_R3B_ENTRY
#define A830_R3B_ENTRY main
#endif
int A830_R3B_ENTRY() {
    pureBoundaries();topLevels();androidProfile();stepsAndBoard();failuresB();residencyBudget();
    std::printf("PASS: R3b real policy/DCVS/power/recovery boundaries, thermal/board dwell, stale250/251, strict off/low M9/late ACK, independent105 halt, external/boost/wake/park (%u checks)\n",gChecks);
    return 0;
}
