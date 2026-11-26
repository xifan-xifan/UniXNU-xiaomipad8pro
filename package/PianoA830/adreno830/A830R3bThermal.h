/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * Bounded engineering qualification only. Board NTCs are not a calibrated skin bound.
 * One 100 ms guard owns this state; consumers receive only the atomic packed snapshot.
 */
#ifndef A830_R3B_THERMAL_H
#define A830_R3B_THERMAL_H
#include "Sm8750Tsens.h"
#include "../../include/PianoBoardTemps.h"
struct A830R3bThermal {
    static constexpr uint64_t Lease=250000000,Period=100000000;
    /* gpu/daily4: the ladder's top GMU level, the qualified ceiling (7 = 607 MHz; 8 = 660, 9 = 734 on the 660/734 step;
     * gpu/fullopp1: up to 14 = 1100 MHz). */
    uint8_t top=7;
    uint8_t step=3,entryRounds=0,releaseRounds=0,minValid=255;
    bool boardCap=false,ready=false,healthy=false,halt=false;
    int32_t raw=0,filter16=0,trendRef=0,peaks[8]={};
    int8_t trend=0;
    uint32_t lastRound=0,reads=0,downs=0,ups=0,events=0;
    uint64_t sequence=0;
    uint64_t samples[2]={},last=0,freshAt=0,downAt=0,coolSince=0,trendAt=0,maxGap=0;
    bool filterValid=false;
    /* gpu/fullopp1 (private/host-checks/20261122-gpu-fullopp/ANALYSIS.md 3.3): Android's thermal mitigation table instead of the
     * R3b-607 constants. Silicon: the soft ladder from softDeci (Android's tj_cfg 95 C) down one GMU level per 500 ms, up one per 2 s once
     * below coolDeci (90 C), the guard exit at hardDeci (100 C raw), the 105 C halt unchanged. Skin (mi_thermald's normal scene, the
     * virtual sensor PianoThermal computes): >= 25 C caps GMU 13 (1050 MHz) until < 23 C, >= 45 C caps GMU 9 (734) until < 42 C; XO
     * >= 78 C caps GMU 9 until < 70 C; no usable virtual reading caps GMU 9. No board hold (msm 62 / quiet 37) and no board exit (65 / 40):
     * gpu-daily3c (11-22 19:30) latched at msm 65.4 C while Android's virtual sensor read 44.8 C, i.e. Android would have capped at 1050
     * MHz and never below. The only board-side hard limit is Android's XO shutdown trip (90 C, "hot"): the guard exit. Without android
     * the R3b-607 rules below are unchanged. */
    bool android=false;
    int32_t softDeci=800,coolDeci=750,hardDeci=850;
    static constexpr uint8_t Skin25Level=13,Skin45Level=9;
    static constexpr int32_t Skin25On=25000,Skin25Off=23000,Skin45On=45000,Skin45Off=42000,XoOn=78000,XoOff=70000;
    static constexpr int32_t XoHard=90000;
    enum Cause : uint8_t {CauseNone,CauseLadder,CauseSkin25,CauseSkin45,CauseXo,CauseSkinUnknown,CauseHard,CauseSkinBand};
    /* gpu/throttle1 (private/host-checks/20261124-gpu-throttle/TABLE.md): the docked skin table (skinTable 1) replaces the normal
     * scene's two rows (25 C -> 1050, 45 C -> 734 in one jump) by one GMU level per 2 C band; each band leaves only under its own
     * clear point. XO (78 C -> 734, 90 C exit), no reading (734), the silicon ladder (95/90 C), the guard exit (raw 100 C) and the
     * unsafe exit (105 C) are the same in both tables. */
    struct SkinBand {int32_t on,off;uint8_t level;};
    static constexpr unsigned DockedBands=7;
    static constexpr SkinBand Docked[DockedBands]={{35000,33000,13},{45000,43000,12},{47000,45000,11},{49000,47000,10},
                                                  {51000,49000,9},{53000,51000,7},{55000,53000,6}};
    uint8_t skinTable=0,band=0;   /* band: 0 none, n = Docked[n-1] holds */
    bool skinValid=false,skin25=false,skin45=false,xoHot=false,xoHard=false;
    int32_t skinMilli=0,xoMilli=0;
    uint8_t skinCap=14,cause=CauseNone;
    uint32_t skinChanges=0;
    // Published once after each complete guard round: cap, raw, filtered, validity and board hold.
    uint64_t snapshot=0;
    /* gpu/fullopp1: the skin side of the same round (deci-C of the virtual sensor | cause << 16 | valid << 24 | skinCap << 32). */
    uint64_t skinSnapshot=0;
    uint64_t published() const {return __atomic_load_n(&snapshot,__ATOMIC_ACQUIRE);}
    uint64_t publishedSkin() const {return __atomic_load_n(&skinSnapshot,__ATOMIC_ACQUIRE);}
    void useAndroid() {android=true;softDeci=950;coolDeci=900;hardDeci=1000;}
    /* The skin inputs of one round (android only); an absent or stale virtual reading is unknown, never cool. */
    void skinTick(const piano_skin_temps *skin) {
        const bool usable=skin && skin->version==PIANO_SKIN_TEMPS_VERSION && skin->state==PIANO_BOARD_TEMPS_RUNNING &&
            skin->virtual_valid && skin->virtual_milli_c>=-40000 && skin->virtual_milli_c<=125000;
        skinValid=usable;
        if(usable) {
            skinMilli=skin->virtual_milli_c;
            if(skinMilli>=Skin25On)skin25=true;else if(skinMilli<Skin25Off)skin25=false;
            if(skinMilli>=Skin45On)skin45=true;else if(skinMilli<Skin45Off)skin45=false;
            while(band<DockedBands && skinMilli>=Docked[band].on)++band;
            while(band && skinMilli<Docked[band-1].off)--band;
        }
        const bool xoValid=skin && skin->version==PIANO_SKIN_TEMPS_VERSION && (skin->valid&(1u<<PIANO_SKIN_XO));
        xoHard=false;
        if(xoValid) {
            xoMilli=skin->milli_c[PIANO_SKIN_XO];
            if(xoMilli>=XoOn)xoHot=true;else if(xoMilli<XoOff)xoHot=false;
            xoHard=xoMilli>=XoHard;
        }
        const uint8_t before=skinCap;
        if(skinTable==1) {
            skinCap=band?Docked[band-1].level:14;
            if((!usable || xoHot) && skinCap>Skin45Level)skinCap=Skin45Level;
        } else skinCap=!usable || skin45 || xoHot?Skin45Level:skin25?Skin25Level:14;
        if(skinCap!=before) {++skinChanges;events|=16;}
    }
    void tick(uint64_t now,const Sm8750TsensSample &g,const piano_gpu_temps &b,bool ok,bool running,
              const piano_skin_temps *skin=nullptr) {
        events=0;halt=false;
        const uint64_t gap=last && now>=last?now-last:0;
        if(gap>maxGap)maxGap=gap;
        bool gpu=g.valid==255;raw=-400;
        for(unsigned i=0;i<8;++i) {
            if(g.deci[i]<-400 || g.deci[i]>1250)gpu=false;
            if(g.valid&(1u<<i)) {
                if(g.deci[i]>raw)raw=g.deci[i];
                if(g.deci[i]>=1050)halt=true; // independent of filter, board API and recovery/HFI
            }
            if(!reads || g.deci[i]>peaks[i])peaks[i]=g.deci[i];
        }
        minValid&=g.valid;++reads;++sequence;
        const bool board=ok && b.version==PIANO_GPU_TEMPS_VERSION && b.state==PIANO_BOARD_TEMPS_RUNNING &&
            b.valid==3 && !b.disabled && b.period_ms==5000 && b.age_ms[0]<=10000 && b.age_ms[1]<=10000 &&
            b.sample_us[0] && b.sample_us[1] && b.milli_c[0]>=-40000 && b.milli_c[0]<=125000 &&
            b.milli_c[1]>=-40000 && b.milli_c[1]<=125000;
        const bool fresh=board && b.rounds!=lastRound && b.sample_us[0]>samples[0] && b.sample_us[1]>samples[1];
        if(fresh) {lastRound=b.rounds;samples[0]=b.sample_us[0];samples[1]=b.sample_us[1];freshAt=now;}
        const bool boardLive=board && freshAt && now>=freshAt && now-freshAt<=10000000000ull;
        healthy=gpu && boardLive && (!last || (now>=last && gap<=Lease));
        if(android)skinTick(skin);
        const bool entry=android?healthy && raw<softDeci && !xoHard:
                                 healthy && raw<800 && b.milli_c[0]<62000 && b.milli_c[1]<37000;
        if(!entry)entryRounds=0;
        else if(fresh && entryRounds<2)++entryRounds;
        ready=entry && entryRounds>=2;
        if(!android && boardLive && (b.milli_c[0]>=62000 || b.milli_c[1]>=37000)) {
            if(!boardCap)events|=4;
            boardCap=true;releaseRounds=0;
        }
        if(boardCap) {
            if(!healthy || b.milli_c[0]>60000 || b.milli_c[1]>35000)releaseRounds=0;
            else if(fresh && ++releaseRounds>=2) {boardCap=false;releaseRounds=0;events|=8;}
        }
        if(gpu && (!last || now-last>=Period)) {
            if(!filterValid) {filter16=raw*16;filterValid=true;trendRef=filter16;trendAt=now;}
            else filter16+=(raw*16-filter16)/4;
            if(now-trendAt>=500000000) {
                const int32_t delta=filter16-trendRef;
                trend=delta>32?1:delta<-32?-1:0;trendAt=now;trendRef=filter16;
            }
        }
        if(!running || !healthy || boardCap || raw>=softDeci || filter16>coolDeci*16)coolSince=0;
        else if(!coolSince)coolSince=now;
        else if(now-coolSince>=2000000000ull && step<top) {++step;++ups;events|=1;coolSince=now;}
        if(running && healthy && (raw>=softDeci || filter16>=softDeci*16) && (!downAt || now-downAt>=500000000)) {
            if(step>3) {--step;++downs;events|=2;}
            downAt=now; // a late window never catches up by dropping several levels
        }
        const bool hard=!healthy || raw>=hardDeci || (android?xoHard:boardLive && (b.milli_c[0]>=65000 || b.milli_c[1]>=40000));
        uint8_t cap=hard?1:boardCap?3:step;
        if(android && !hard && skinCap<cap)cap=skinCap;
        cause=hard?CauseHard:!android?(boardCap?CauseLadder:step<top?CauseLadder:CauseNone):
              skinCap<step && skinCap<top?(!skinValid?CauseSkinUnknown:skinTable==1?(xoHot && skinCap==Skin45Level &&
                  (!band || Docked[band-1].level>=Skin45Level)?CauseXo:CauseSkinBand):
                  skin45?CauseSkin45:xoHot?CauseXo:CauseSkin25):
              step<top?CauseLadder:CauseNone;
        __atomic_store_n(&snapshot,uint64_t(cap)|uint64_t(uint16_t(raw))<<8|
            uint64_t(uint16_t(filter16/16))<<24|uint64_t(healthy)<<40|uint64_t(boardCap)<<41,__ATOMIC_RELEASE);
        __atomic_store_n(&skinSnapshot,uint64_t(uint16_t(int16_t(skinMilli/100)))|uint64_t(cause)<<16|uint64_t(skinValid)<<24|
            uint64_t(skinCap)<<32,__ATOMIC_RELEASE);
        last=now;
    }
    void resetPeaks() {reads=0;minValid=255;maxGap=0;}
};
#endif
