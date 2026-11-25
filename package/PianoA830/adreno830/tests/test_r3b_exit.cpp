/* SPDX-License-Identifier: MIT */
/* host-test: A830Dcvs.cpp A830PowerState.cpp A830Recovery.cpp platform/sm8750/Sm8750Tsens.c */
#define A830_R3B_ENTRY runOriginalR3b
#include "test_a830r3b.cpp"
#undef A830_R3B_ENTRY
#include "A830QualificationExitTest.h"

int main()
{
    for(bool armed:{false,true}) {
        bootB();
        if(armed) {openB();driveB(90);}
        else power.noteVoted(3,3);
        auto b=board(24);b.milli_c[0]=63677;b.milli_c[1]=33532;
        tickB(784,100000000,b);
        b=board(25);b.milli_c[0]=65179;b.milli_c[1]=33549;b.age_ms[0]=b.age_ms[1]=37;
        tickB(780,100000000,b);
        const auto at=qual.failedAt();
        CHECK(qual.reason()==A830QualBoardHot && at==gEnv.now && recovery.pending() && !qual.high());
        CHECK(power.qualificationLatched() && power.qualificationBlocked() && power.qualificationEffectiveCap()==1);
        gOps.lastSubmitted=2;gOps.retired=1;
        gOps.during=[](const std::string &call) {
            if(call=="confirmLow") {CHECK(power.acquire()!=0 && power.inFlight());++gOps.retired;}
        };
        CHECK(recovery.runPending()==0 && recovery.qualificationSafeLow());
        CHECK(gOps.count("confirmLow")==1 && gOps.gates.empty() && !gOps.count("gmuForceOff") && !gOps.count("snapshot"));
        CHECK(unsafe==0 && !power.inFlight() && power.acquire()==0);
        power.release();power.setTarget(11,10);power.setWakeTargets(11,10,11,10);
        CHECK(power.votedLevel()==1 && power.votedBus()==2 && power.targetLevel()==1 && !qual.arm());
        tickB(740,100000000,board(26));power.setTarget(11,10);
        CHECK(power.targetLevel()==3 && qual.failedAt()==at && !qual.deadlineMissed(at+5000000000ull));
    }

    // Replay the idle desktop at the ordinary ADC cadence and an existing 160 MHz ACK.
    bootB();
    auto idleBoard=board(4);
    idleBoard.period_ms=30000;idleBoard.milli_c[0]=59393;idleBoard.milli_c[1]=29583;
    idleBoard.age_ms[0]=4410;idleBoard.age_ms[1]=4410;
    idleBoard.sample_us[0]=90838872;idleBoard.sample_us[1]=90839682;
    raw=Sm8750TsensValidBit|660u;
    const auto idleGpu=gpu();
    qual.tick(gEnv.now,idleGpu,idleBoard,true);
    CHECK(power.votedLevel()==1 && !qual.ready() && !qual.high() && power.qualificationEffectiveCap()==1);
    const auto allowed=[&](uint32_t command) {return a830ExitTestAllowed(command,idleGpu,idleBoard,true,qual,power,recovery);};
    CHECK(allowed(3) && !allowed(4));
    power.noteVoted(0,0);CHECK(!allowed(3));
    power.noteVoted(3,3);CHECK(allowed(3));
    power.noteVoted(4,4);CHECK(!allowed(3));
    power.noteVoted(1,2);
    auto refusedBoard=idleBoard;refusedBoard.age_ms[1]=10001;
    CHECK(!a830ExitTestAllowed(3,idleGpu,refusedBoard,true,qual,power,recovery));
    refusedBoard=idleBoard;refusedBoard.milli_c[0]=62000;
    CHECK(!a830ExitTestAllowed(3,idleGpu,refusedBoard,true,qual,power,recovery));
    auto refusedGpu=idleGpu;refusedGpu.max_deci=800;
    CHECK(!a830ExitTestAllowed(3,refusedGpu,idleBoard,true,qual,power,recovery));
    CHECK(!a830ExitTestAllowed(3,idleGpu,idleBoard,false,qual,power,recovery));

    qual.tick(gEnv.now,idleGpu,idleBoard,true,allowed(3));
    CHECK(!allowed(3) && !allowed(4)); // Pending low vote blocks a second command.
    CHECK(qual.reason()==A830QualBoardHot && recovery.runPending()==0);
    CHECK(!allowed(3) && allowed(4));
    CHECK(gOps.count("confirmLow")==1 && gOps.gates.empty() && !gOps.count("gmuForceOff"));
    const auto at=qual.failedAt();
    gEnv.now+=60000000000ull;
    recovery.requestQualificationLow(A830QualBoardHot,gEnv.now,true);
    CHECK(!qual.deadlineMissed(gEnv.now) && qual.failedAt()==at);
    gOps.fail["dropLow"]=kIOReturnTimeout;
    CHECK(recovery.runPending()==0 && recovery.qualificationSafeLow());
    CHECK(gOps.index("dropLow")<gOps.index("gate:1") && gOps.index("strictQuiesce")<gOps.index("gmuForceOff"));
    CHECK(gOps.index("gmuForceOff")<gOps.index("gmuStart") && gOps.index("secureResume")<gOps.index("gate:0"));
    CHECK(gOps.count("snapshot")==0 && gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && !unsafe);
    CHECK(power.qualificationLatched() && power.votedLevel()==1 && !power.qualificationBlocked());

    // Concurrent genuine fault takes precedence, including one published from the HFI exchange.
    bootB();qual.fault(A830QualBoardHot,gEnv.now);
    gOps.during=[](const std::string &call) {if(call=="confirmLow")recovery.report(A830FaultGmu,0x12345678);};
    CHECK(recovery.runPending()==0 && recovery.snapshot().cause==A830FaultGmu && recovery.snapshot().detail==0x12345678);
    CHECK(gOps.count("gmuForceOff")==1 && gOps.count("snapshot")==0 && !unsafe);

    for(const char *fail:{"strictQuiesce","gmuForceOff","gmuStart","cpStart"}) {
        bootB();qual.fault(A830QualBoardHot,gEnv.now);
        gOps.fail["confirmLow"]=kIOReturnTimeout;gOps.fail[fail]=kIOReturnIOError;
        CHECK(recovery.runPending()!=0 && recovery.lost() && power.qualificationLatched());
        CHECK(gOps.count("gate:0")==0);
        if(std::string(fail)=="strictQuiesce")CHECK(unsafe==1 && gOps.count("gmuForceOff")==0);
    }
    bootB();CHECK(power.acquire()==0);qual.fault(A830QualBoardHot,gEnv.now);
    gOps.fail["confirmLow"]=kIOReturnTimeout;
    CHECK(recovery.runPending()!=0 && unsafe==1 && !gOps.count("strictQuiesce") && !gOps.count("gmuForceOff"));
    power.release();

    for(const auto mode:{A830SlumberFull,A830SlumberCxKeep}) {
        bootB();power.setTarget(3,3);power.setWakeTargets(3,3,3,3);
        CHECK(power.enterSlumber(mode)==0);qual.fault(A830QualBoardHot,gEnv.now);
        CHECK(recovery.runPending()==0 && !gOps.count("confirmLow") && !gOps.count("gmuStart"));
        CHECK(power.qualificationEffectiveCap()==1 && power.wake(A830WakeTouch)==0);
        CHECK(gOps.lastArgs.level==1 && gOps.lastArgs.bus==2 && power.qualificationLatched());
    }
    std::printf("PASS: recorded and injected board-hard exits, fresh-low routing, strict failure gates, fault race and deferred wake (%u checks)\n",gChecks);
}
