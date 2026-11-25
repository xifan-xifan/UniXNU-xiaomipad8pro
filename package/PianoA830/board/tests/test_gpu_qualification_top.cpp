/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * gpu/fullopp1b: the board allocates the C5LOG qualification ring (PianoA830::start) under the same rule that makes S10 a qualification
 * stage (A830QualificationTop.h). gpu-fullopp1 (maxlevel 14 / dcvs-max 1100) failed S10 with reason 12 because the board allocated the
 * ring only for 7 / 607 (SWEEP1-CLASSIFY.md). This test runs both halves from the boot-argument strings of the real lines:
 *  - the board's half: piano_a830_qualification_top() = adreno830_qualification_top_from(the board's getter) before the bring-up thread,
 *    and the ring allocated when it is not 0 (board/PianoA830.cpp registerQual607);
 *  - S10's half: the bring-up thread's own parse of the same arguments into its board (piano_a830_board_start), qualificationMode =
 *    adreno830_qualification_top(board) != 0 requires the ring (Adreno830Start.cpp stageDcvs), and its first record goes in.
 * The getter is the board's (PianoA830Board.cpp bootArgument): the driver's pagani-* names read as piano-* boot arguments.
 */
#include "../adreno830/A830QualificationTop.h"
#include "PianoGpuQualificationLog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {
struct Args {std::vector<std::pair<std::string,uint32_t>> words;};
Args parse(const char *line)
{
    Args a;
    std::string s(line);
    size_t at=0;
    while(at<s.size()) {
        const size_t end=s.find(' ',at);
        const std::string w=s.substr(at,end==std::string::npos?std::string::npos:end-at);
        const size_t eq=w.find('=');
        if(!w.empty() && eq!=std::string::npos)a.words.push_back({w.substr(0,eq),uint32_t(std::strtoul(w.c_str()+eq+1,nullptr,0))});
        if(end==std::string::npos)break;
        at=end+1;
    }
    return a;
}
/* PianoA830Board.cpp bootArgument: "pagani-X" is read as "piano-X" (PE_parse_boot_argn of the command line). */
bool bootArgument(void *cookie,const char *name,uint32_t *value)
{
    char piano[48];
    if(!std::strncmp(name,"pagani-",7)) {std::snprintf(piano,sizeof piano,"piano-%s",name+7);name=piano;}
    for(const auto &w:static_cast<const Args *>(cookie)->words)
        if(w.first==name) {*value=w.second;return true;}
    return false;
}
/* The rule before gpu/fullopp1b (board/PianoA830.cpp at e60e8c09): the ring only for maxlevel 7 / dcvs-max 607. */
bool oldBoardRule(const Args &a)
{
    uint32_t l=0,m=0;
    return bootArgument(const_cast<Args *>(&a),"piano-gpu-maxlevel",&l) && l==7 && bootArgument(const_cast<Args *>(&a),"piano-gpu-dcvs-max",&m) && m==607;
}
using QualLog=pianogpu::QualificationLog<>;
struct Boot {uint8_t boardTop,s10Top;bool ring,s10Needs,s10Got,firstRecord;};
/* One boot: the board's start() decision, then S10's requirement on the bring-up thread's board. */
Boot boot(const Args &a)
{
    Boot r{};
    r.boardTop=adreno830_qualification_top_from(bootArgument,const_cast<Args *>(&a));   /* PianoA830::start */
    static QualLog *log;                                                                 /* registerQual607(record) */
    delete log;log=nullptr;
    if(r.boardTop)log=new QualLog();
    r.ring=log!=nullptr;
    static Adreno830Board board;                                                         /* piano_a830_board_start */
    board={};Adreno830ParseKnobs(&board,bootArgument,const_cast<Args *>(&a));
    r.s10Top=adreno830_qualification_top(&board);                                        /* stageDcvs: qualificationMode */
    r.s10Needs=r.s10Top!=0;
    r.s10Got=!r.s10Needs || r.ring;                                                       /* adreno830_qualification_log_ready() */
    if(r.s10Needs && r.ring) {
        char line[96];std::snprintf(line,sizeof line,"R3b TOP v1 level=%u mhz=%u bus=11",unsigned(r.s10Top),unsigned(Adreno830FulloppMhz[r.s10Top]));
        r.firstRecord=log->push(line) && log->bounds().next==1;
    }
    return r;
}
/* The GPU words of the three lines' boot arguments (private/host-checks/20261122-gpu-{fullopp,daily3c,daily4}/extra-args.txt). */
const char *const kFullopp="piano-gpu=2 piano-gpu-user-vm=1 piano-gpu-input-floor=342 piano-gpu-boost=1 data-volatile-reserve-mib=2048 "
    "piano-gpu-r3b=1 piano-gpu-daily=1 piano-gpu-fullopp=1 piano-gpu-governor=2 piano-gpu-profile=1 piano-gpu-speedbin=1 piano-gpu-dcvs=1 "
    "piano-gpu-maxlevel=14 piano-gpu-dcvs-max=1100 piano-gpu-dcvs-min=222 piano-gpu-acd=1 piano-gpu-hfi=0 piano-gpu-rails=1 piano-gpu-ddr=11 "
    "piano-gpu-ddr-floor=11 piano-gpu-idle=1 piano-gpu-fault-test=1";
const char *const k607="piano-gpu=2 piano-gpu-user-vm=1 data-volatile-reserve-mib=2048 piano-gpu-r3b=1 piano-gpu-daily=1 piano-gpu-profile=1 "
    "piano-gpu-speedbin=0 piano-gpu-dcvs=1 piano-gpu-maxlevel=7 piano-gpu-dcvs-max=607 piano-gpu-dcvs-min=160 piano-gpu-acd=0 piano-gpu-hfi=0 "
    "piano-gpu-rails=1 piano-gpu-ddr=11 piano-gpu-ddr-floor=11 piano-gpu-idle=1 piano-gpu-input-floor=342 piano-gpu-boost=1 piano-gpu-fault-test=1";
const char *const kDaily4="piano-gpu=2 piano-gpu-user-vm=1 data-volatile-reserve-mib=2048 piano-gpu-r3b=1 piano-gpu-daily=1 piano-gpu-profile=1 "
    "piano-gpu-speedbin=0 piano-gpu-dcvs=1 piano-gpu-maxlevel=9 piano-gpu-dcvs-max=734 piano-gpu-dcvs-min=160 piano-gpu-acd=0 piano-gpu-hfi=0 "
    "piano-gpu-rails=1 piano-gpu-ddr=11 piano-gpu-ddr-floor=11 piano-gpu-idle=1 piano-gpu-input-floor=342 piano-gpu-boost=1 piano-gpu-fault-test=1";
}  // namespace

int main()
{
    /* The three lines: the ring exists, S10 needs it and gets it, its first record goes in. */
    struct {const char *args;uint8_t top;bool oldRing;} lines[]={{kFullopp,14,false},{k607,7,true},{kDaily4,9,false}};
    for(const auto &line:lines) {
        const Args a=parse(line.args);
        const Boot b=boot(a);
        CHECK(b.boardTop==line.top && b.s10Top==line.top);
        CHECK(b.ring && b.s10Needs && b.s10Got && b.firstRecord);
        /* known-positive: the old board rule reproduces gpu-fullopp1's S10 refusal (reason 12) for 14/1100 and daily4's 9/734 */
        CHECK(oldBoardRule(a)==line.oldRing);
    }
    /* Every top the S10 rule accepts, on the R3b line (660, and the fullopp curve 7..14 with its MHz). */
    {
        char text[256];
        std::snprintf(text,sizeof text,"piano-gpu-r3b=1 piano-gpu-maxlevel=8 piano-gpu-dcvs-max=660");
        const Boot b=boot(parse(text));CHECK(b.boardTop==8 && b.ring && b.s10Got && b.firstRecord);
        for(unsigned l=7;l<=14;++l) {
            std::snprintf(text,sizeof text,"piano-gpu-r3b=1 piano-gpu-fullopp=1 piano-gpu-maxlevel=%u piano-gpu-dcvs-max=%u",l,unsigned(Adreno830FulloppMhz[l]));
            const Boot f=boot(parse(text));CHECK(f.boardTop==l && f.ring && f.s10Got && f.firstRecord);
        }
    }
    /* Not qualification boots: no ring, S10 needs none. */
    {
        const char *const none[]={
            "", "piano-gpu=2 piano-gpu-maxlevel=9 piano-gpu-dcvs-max=734",                 /* a plain 734 boot keeps its design ceiling */
            "piano-gpu-maxlevel=9 piano-gpu-r3b=1",                                         /* dcvs-max not given (default 734, flag clear) */
            "piano-gpu-r3b=1 piano-gpu-fullopp=1 piano-gpu-maxlevel=14 piano-gpu-dcvs-max=1050",   /* MHz not the level's */
            "piano-gpu-fullopp=1 piano-gpu-maxlevel=14 piano-gpu-dcvs-max=1100",             /* fullopp without the R3b line */
            "piano-gpu-r3b=1 piano-gpu-fullopp=1 piano-gpu-maxlevel=6 piano-gpu-dcvs-max=525"};   /* below the curve's tops */
        for(const char *line:none) {
            const Boot b=boot(parse(line));
            CHECK(!b.boardTop && !b.ring && !b.s10Needs && b.s10Got);
        }
    }
    /* The whole knob space the rule reads: whenever S10 needs the ring, the board allocated it, and never otherwise. */
    {
        const int dcvsMax[]={-1,0,160,222,342,389,443,525,607,660,734,832,900,967,1050,1100,999};
        unsigned boots=0,qualification=0;
        for(int r3b=-1;r3b<=2;++r3b)for(int fullopp=-1;fullopp<=2;++fullopp)for(int l=-1;l<=16;++l)for(int m:dcvsMax) {
            Args a;
            if(r3b>=0)a.words.push_back({"piano-gpu-r3b",uint32_t(r3b)});
            if(fullopp>=0)a.words.push_back({"piano-gpu-fullopp",uint32_t(fullopp)});
            if(l>=0)a.words.push_back({"piano-gpu-maxlevel",uint32_t(l)});
            if(m>=0)a.words.push_back({"piano-gpu-dcvs-max",uint32_t(m)});
            const Boot b=boot(a);++boots;
            CHECK(b.boardTop==b.s10Top && b.ring==b.s10Needs && b.s10Got);
            if(b.s10Needs) {++qualification;CHECK(b.firstRecord);}
        }
        CHECK(boots==4*4*18*17 && qualification>0);
        std::printf("PASS: qualification ring under S10's rule: fullopp 14/1100, 607, daily4 9/734, 660 and fullopp 7..14 get it "
                    "(the old 7/607-only rule refused 14/1100 and 9/734); %u knob combinations, %u qualification boots, board == S10\n",
                    boots,qualification);
    }
    return 0;
}
