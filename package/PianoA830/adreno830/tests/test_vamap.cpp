/* SPDX-License-Identifier: MIT */
/* UniXNU-piano (2026-11-12): the GPU address space as macOS uses it, from the first macOS desktop's failures (run attend4: WindowServer's
 * BoMap answered NoResources when the VM's table ranges ran out, and two processes' BoMaps BadArgument outside their window). The
 * ADRENO830_MACOS pass: the 16 GiB window table (valid, in order, no window across a multiple of 4 GiB, the render server's 3.5 GiB,
 * the large 768 MiB and the small 320 MiB windows, every address attributed to its slot, the slot choice unchanged), the BoMap refusal
 * line for every reason (which argument, which limit, with its figures) within the ring's line, and the address-space lines (the VM's
 * totals, a connection's mappings against its window). The upstream pass has none of it. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoPublish.h"
#ifdef ADRENO830_MACOS
#include "NativeAdrenoMapLines.h"
#include "NativeAdrenoSubmit.h"
#endif
#include <cstdio>
#include <cstring>
#ifdef ADRENO830_MACOS
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static unsigned checks;

static void failureSnapshots()
{
    NativeAdrenoFailureSnapshot s{};s.captured=true;s.sequence=UINT64_MAX;
    s.alloc={NativeAdrenoAllocWhyImport,NativeAdrenoMethodBoImport,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    s.alloc.failure={"import-readonly-descriptor",kIOReturnNoMemory};
    s.map={NativeAdrenoMapWhyVm,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,
        {NativeAdrenoVmWhySpans,UINT64_MAX,UINT64_MAX,UINT64_MAX,nullptr,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX},true};
    s.usage={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    s.objects=s.capacity=s.pool=s.other=s.imports=UINT64_MAX;
    s.backing={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    char line[NativeAdrenoMapLineBytes];
    for(uint32_t method:{uint32_t(NativeAdrenoMethodBoAlloc),uint32_t(NativeAdrenoMethodBoImport),uint32_t(NativeAdrenoMethodBoMap)})
        for(unsigned row=0;row<NativeAdrenoFailureLines;++row) {
            const int n=NativeAdrenoFailureLine(line,sizeof line,row,s,31,2147483647,method,kIOReturnNoMemory);
            CHECK(n>0 && size_t(n)<sizeof line && std::strstr(line,"refusal n=18446744073709551615"));
        }
    NativeAdrenoFailureRecord first{};
    CHECK(!first.line(line,sizeof line,0) && !line[0]);
    CHECK(!first.remember(s,2,2399,NativeAdrenoMethodBoImport,kIOReturnBadArgument));
    CHECK(first.remember(s,2,2399,NativeAdrenoMethodBoImport,kIOReturnNoMemory));
    s.sequence=2;s.alloc.failure.stage="overwritten";
    CHECK(!first.remember(s,7,159,NativeAdrenoMethodBoMap,kIOReturnNoResources));
    CHECK(first.line(line,sizeof line,0)>0 && std::strstr(line,"pid=2399 slot=2") && std::strstr(line,"n=18446744073709551615"));
    CHECK(first.line(line,sizeof line,1)>0 && std::strstr(line,"import-readonly-descriptor") && !std::strstr(line,"overwritten"));
    NativeAdrenoResidencyRecord residency{"1234567890123456789",UINT32_MAX,2147483647,
        {UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}};
    char residency_line[NativeAdrenoResidencyLineBytes];
    const int n=residency.line(residency_line,sizeof(residency_line)-1,31,"closed",UINT64_MAX,UINT64_MAX);
    CHECK(n>0 && size_t(n)<sizeof(residency_line)-1);
    CHECK(std::strstr(residency_line,"largest-vm-hole=18446744073709551615"));
}

static void windows()
{
    const NativeAdrenoClientWindow *w=NativeAdrenoClientWindowsFor(0x20000000ull+0x3DFFFE000ull);
    CHECK(w==NativeAdrenoClientWindows16G && NativeAdrenoClientWindowsValid(w,0x20000000ull,0x3FFFFE000ull));
    CHECK(NativeAdrenoClientWindowsEnd(w)==0x3FFFFE000ull && NativeAdrenoClientWindowsStayBelowLines(w));
    CHECK(w[0].start==0x20000000ull && w[0].start+w[0].bytes==0x100000000ull);   /* the render server: 3.5 GiB up to 4 GiB */
    for(unsigned s=1;s<=NativeAdrenoLargeSlots;++s)CHECK(w[s].bytes==0x30000000ull && w[s].start==0x100000000ull+(s-1)*0x30000000ull);
    for(unsigned s=NativeAdrenoLargeSlots+1;s<=NativeAdrenoDecoderSlot;++s) {
        CHECK(w[s].bytes==(s<NativeAdrenoDecoderSlot?320ull<<20:(1536ull<<20)-8192));
        CHECK(w[s].start>=w[s-1].start+w[s-1].bytes && w[s].start>>32==(w[s].start+w[s].bytes-1)>>32);
        CHECK(NativeAdrenoClientSlotAt(w,w[s].start)==s && NativeAdrenoClientSlotAt(w,w[s].start+w[s].bytes-1)==s);
    }
    CHECK(w[9].start==0x200000000ull && w[21].start==0x300000000ull);   /* the lines of 8 and 12 GiB start a window */
    CHECK(NativeAdrenoClientSlotAt(w,0x1F4000000ull)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w,0x2F0000000ull)==NativeAdrenoMaxClients);
    CHECK(NativeAdrenoClientSlotAt(w,0x20000000ull)==0 && NativeAdrenoClientSlotAt(w,0xFFFFF000ull)==0 &&
          NativeAdrenoClientSlotAt(w,0x100000000ull)==1 && NativeAdrenoClientSlotAt(w,0x3FFFFE000ull)==NativeAdrenoMaxClients);
    CHECK(NativeAdrenoDecoderClient(true,NativeAdrenoDecoderSigningId));
    CHECK(!NativeAdrenoDecoderClient(false,NativeAdrenoDecoderSigningId));
    CHECK(!NativeAdrenoDecoderClient(true,"VTDecoderXPCService"));
    CHECK(!NativeAdrenoDecoderClient(true,"com.apple.coremedia.videodecoder.other"));
    CHECK(!NativeAdrenoDecoderClient(true,nullptr));
    CHECK(!w[30].bytes && !w[31].bytes);
    /* The render server, large clients and listed clients cannot borrow the decoder window. */
    bool taken[NativeAdrenoMaxClients]={};
    CHECK(NativeAdrenoClientSlotFor(true,false,true,w,taken)==0 && NativeAdrenoClientSlotFor(false,true,true,w,taken)==1);
    CHECK(NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoDecoderSlot);
    CHECK(NativeAdrenoClientSlotFor(false,false,false,NativeAdrenoClientWindows4G,taken,true)==4);
    taken[NativeAdrenoDecoderSlot]=true;
    CHECK(NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==4);
    taken[NativeAdrenoDecoderSlot]=false;
    taken[0]=true;
    unsigned count=0,last=0;
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,false,w,taken))<NativeAdrenoMaxClients;taken[s]=true) {++count;last=s;}
    /* 2026-11-22 (GPU-SLOTS): 23 small (the keep is 2) and then large 1 and 2 (the last large one kept): 25 apps, not 21. */
    CHECK(count==25 && last==2 && NativeAdrenoClientSmallFree(w,taken)==2 && taken[1] && taken[2] && !taken[3]);
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,true,w,taken))<NativeAdrenoMaxClients;taken[s]=true) {
        CHECK(s!=NativeAdrenoDecoderSlot);
    }
    CHECK(taken[3] && !NativeAdrenoClientSmallFree(w,taken));   /* the listed: the kept two, then large 3 */
    CHECK(NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoDecoderSlot);
    /* The windows' sum: the client range less the gaps under the 8 and 12 GiB lines (slot 0 ends on 4 GiB, no gap there). */
    uint64_t total=0;
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s)total+=w[s].bytes;
    CHECK(total==0x3FFFFE000ull-0x20000000ull-(0x200000000ull-0x1F4000000ull)-(0x300000000ull-0x2F0000000ull));
}

/* 2026-11-22 (GPU-SLOTS): the 64 GiB aperture's table (pagani-gpu-va=64): 59 windows of the 64 slots. WindowServer's 3.5 GiB, large 1 at
 * 2 GiB and 2-3 at 1 GiB, 54 ordinary 1 GiB windows from 8 GiB, the decoder's 1.5 GiB at the top; every window inside one 4 GiB block,
 * every address attributed; the slot choice: 52 apps in ordinary windows (2 kept), then two large ones borrowed; the decoder its own. */
static void windows64()
{
    const NativeAdrenoClientWindow *w=NativeAdrenoClientWindowsFor(0x20000000ull+0xFE0000000ull);
    CHECK(w==NativeAdrenoClientWindows64G && NativeAdrenoClientWindowsFor(0x400000000ull)==NativeAdrenoClientWindows16G &&
          NativeAdrenoClientWindowsFor(0x400001000ull)==NativeAdrenoClientWindows64G);
    CHECK(NativeAdrenoMaxClients==64 && NativeAdrenoSubmitRanges==64 && NativeAdrenoSubmitJobs==76);
    CHECK(NativeAdrenoClientWindowsValid(w,0x20000000ull,0xFFFFFE000ull) && !NativeAdrenoClientWindowsValid(w,0x20000000ull,0xFFFFFD000ull));
    CHECK(NativeAdrenoClientWindowsEnd(w)==0xFFFFFE000ull && NativeAdrenoClientWindowsEnd(w)<=(UINT64_C(1)<<36) &&
          NativeAdrenoClientWindowsStayBelowLines(w) && NativeAdrenoClientWindowCount(w)==59);
    CHECK(w[0].start==0x20000000ull && w[0].bytes==0xE0000000ull);
    CHECK(w[1].start==0x100000000ull && w[1].bytes==2ull<<30 && w[2].start==0x180000000ull && w[2].bytes==1ull<<30 &&
          w[3].start==0x1C0000000ull && w[3].bytes==1ull<<30);
    for(unsigned s=NativeAdrenoLargeSlots+1;s<NativeAdrenoDecoderSlot64G;++s)
        CHECK(w[s].start==0x200000000ull+uint64_t(s-4)*(1ull<<30) && w[s].bytes==1ull<<30);
    CHECK(w[NativeAdrenoDecoderSlot64G].start==0xFA0000000ull && w[NativeAdrenoDecoderSlot64G].bytes==(1536ull<<20)-8192);
    for(unsigned s=NativeAdrenoDecoderSlot64G+1;s<NativeAdrenoMaxClients;++s)CHECK(!w[s].bytes);
    for(unsigned s=0;s<=NativeAdrenoDecoderSlot64G;++s)
        CHECK(NativeAdrenoClientSlotAt(w,w[s].start)==s && NativeAdrenoClientSlotAt(w,w[s].start+w[s].bytes-1)==s &&
              (!s || w[s].start>=w[s-1].start+w[s-1].bytes));
    CHECK(NativeAdrenoClientSlotAt(w,0x1FFFF000)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w,0xF80000000ull)==NativeAdrenoMaxClients &&
          NativeAdrenoClientSlotAt(w,0xFFFFFE000ull)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w,0x1000000000ull)==NativeAdrenoMaxClients);
    CHECK(NativeAdrenoIsDecoderSlot(w,NativeAdrenoDecoderSlot64G) && !NativeAdrenoIsDecoderSlot(w,NativeAdrenoDecoderSlot) &&
          NativeAdrenoListedSmallKeep(w)==2);
    bool taken[NativeAdrenoMaxClients]={};
    CHECK(NativeAdrenoClientSlotFor(true,false,true,w,taken)==0 && NativeAdrenoClientSlotFor(false,true,true,w,taken)==1 &&
          NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoDecoderSlot64G &&
          NativeAdrenoClientSlotFor(false,false,false,w,taken)==4 && NativeAdrenoClientSlotFor(false,false,true,w,taken)==4);
    taken[0]=taken[1]=taken[NativeAdrenoDecoderSlot64G]=true;   /* WindowServer, iconservicesagent, the decoder */
    unsigned count=0,last=0;
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,false,w,taken))<NativeAdrenoMaxClients;taken[s]=true) {
        CHECK(s!=NativeAdrenoDecoderSlot64G && s);
        ++count;last=s;
    }
    CHECK(count==52+1 && last==2 && NativeAdrenoClientSmallFree(w,taken)==2 && !taken[3]);   /* 52 ordinary, large 2; 3 kept */
    CHECK(NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoMaxClients);   /* the decoder slot is taken: as an app */
    CHECK(NativeAdrenoClientSlotFor(false,true,true,w,taken)==3);   /* the large list's kept one */
    count=0;
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,true,w,taken))<NativeAdrenoMaxClients;taken[s]=true)++count;
    CHECK(count==3 && !NativeAdrenoClientSmallFree(w,taken));   /* the listed: the kept two, then large 3 */
    uint64_t total=0;
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s)total+=w[s].bytes;
    CHECK(total==0xE0000000ull+(4ull<<30)+54*(1ull<<30)+(1536ull<<20)-8192);
}

/* 2026-11-25 (gpu/slots2): the 256 GiB aperture's table (pagani-gpu-va=256, paged mode): all 64 slots have a window. Slots 0-3 as in the
 * 64 GiB table, 59 ordinary 4 GiB windows on 4 GiB lines from 8 GiB (slot 62 ends at 244 GiB), the decoder's 1.5 GiB at the top below
 * the 8 KiB guard; every window inside one 4 GiB block, every address attributed; the per-object limit 1 GiB but in slot 0; the slot
 * choice: 57 apps in ordinary windows (2 kept), then two large ones borrowed; the decoder its own (slot 63). */
static void windows256()
{
    const NativeAdrenoClientWindow *w=NativeAdrenoClientWindowsFor(0x20000000ull+0x3FE0000000ull);
    CHECK(w==NativeAdrenoClientWindows256G && NativeAdrenoClientWindowsFor(0x1000000000ull)==NativeAdrenoClientWindows64G &&
          NativeAdrenoClientWindowsFor(0x1000001000ull)==NativeAdrenoClientWindows256G);
    CHECK(NativeAdrenoClientWindowsValid(w,0x20000000ull,0x3FFFFFE000ull) && !NativeAdrenoClientWindowsValid(w,0x20000000ull,0x3FFFFFD000ull));
    CHECK(NativeAdrenoClientWindowsEnd(w)==0x3FFFFFE000ull && NativeAdrenoClientWindowsEnd(w)<=(UINT64_C(1)<<38) &&
          NativeAdrenoClientWindowsEnd(w)>(UINT64_C(1)<<37) && NativeAdrenoClientWindowsStayBelowLines(w) &&
          NativeAdrenoClientWindowCount(w)==64);
    for(unsigned s=0;s<=NativeAdrenoLargeSlots;++s)
        CHECK(w[s].start==NativeAdrenoClientWindows64G[s].start && w[s].bytes==NativeAdrenoClientWindows64G[s].bytes);
    for(unsigned s=NativeAdrenoLargeSlots+1;s<NativeAdrenoDecoderSlot256G;++s)
        CHECK(w[s].start==0x200000000ull+uint64_t(s-4)*(4ull<<30) && w[s].bytes==4ull<<30 && !(w[s].start&((4ull<<30)-1)));
    CHECK(w[62].start==0x3C00000000ull && w[62].start+w[62].bytes==0x3D00000000ull);
    CHECK(NativeAdrenoDecoderSlot256G==63 && w[63].start==0x3FA0000000ull && w[63].bytes==(1536ull<<20)-8192);
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s)
        CHECK(NativeAdrenoClientSlotAt(w,w[s].start)==s && NativeAdrenoClientSlotAt(w,w[s].start+w[s].bytes-1)==s &&
              (!s || w[s].start>=w[s-1].start+w[s-1].bytes));
    CHECK(NativeAdrenoClientSlotAt(w,0x1FFFF000)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w,0x3D00000000ull)==NativeAdrenoMaxClients &&
          NativeAdrenoClientSlotAt(w,0x3FFFFFE000ull)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w,0x4000000000ull)==NativeAdrenoMaxClients);
    /* The per-object limit: 1 GiB in every slot but the render server's (whose window's 3.5 GiB stays its limit); none on the other tables. */
    CHECK(!NativeAdrenoClientObjectLimit(w,0) && NativeAdrenoClientObjectLimit(w,NativeAdrenoMaxClients)==0);
    for(unsigned s=1;s<NativeAdrenoMaxClients;++s)CHECK(NativeAdrenoClientObjectLimit(w,s)==1ull<<30);
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s)
        CHECK(!NativeAdrenoClientObjectLimit(NativeAdrenoClientWindows64G,s) && !NativeAdrenoClientObjectLimit(NativeAdrenoClientWindows16G,s));
    CHECK(NativeAdrenoIsDecoderSlot(w,NativeAdrenoDecoderSlot256G) && !NativeAdrenoIsDecoderSlot(w,NativeAdrenoDecoderSlot64G) &&
          !NativeAdrenoIsDecoderSlot(NativeAdrenoClientWindows64G,NativeAdrenoDecoderSlot256G) && NativeAdrenoListedSmallKeep(w)==2);
    bool taken[NativeAdrenoMaxClients]={};
    CHECK(NativeAdrenoClientSlotFor(true,false,true,w,taken)==0 && NativeAdrenoClientSlotFor(false,true,true,w,taken)==1 &&
          NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoDecoderSlot256G &&
          NativeAdrenoClientSlotFor(false,false,false,w,taken)==4 && NativeAdrenoClientSlotFor(false,false,true,w,taken)==4);
    taken[0]=taken[1]=taken[NativeAdrenoDecoderSlot256G]=true;   /* WindowServer, iconservicesagent, the decoder */
    unsigned count=0,last=0;
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,false,w,taken))<NativeAdrenoMaxClients;taken[s]=true) {
        CHECK(s!=NativeAdrenoDecoderSlot256G && s);
        ++count;last=s;
    }
    CHECK(count==57+1 && last==2 && NativeAdrenoClientSmallFree(w,taken)==2 && !taken[3]);   /* 57 ordinary, large 2; 3 kept */
    CHECK(NativeAdrenoClientSlotFor(false,false,false,w,taken,true)==NativeAdrenoMaxClients);
    CHECK(NativeAdrenoClientSlotFor(false,true,true,w,taken)==3);
    count=0;
    for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,true,w,taken))<NativeAdrenoMaxClients;taken[s]=true)++count;
    CHECK(count==3 && !NativeAdrenoClientSmallFree(w,taken));
    uint64_t total=0;
    for(unsigned s=0;s<NativeAdrenoMaxClients;++s)total+=w[s].bytes;
    CHECK(total==0xE0000000ull+(4ull<<30)+59*(4ull<<30)+(1536ull<<20)-8192);
}

static void refusalLines()
{
    char line[NativeAdrenoMapLineBytes];
    auto says=[&](const NativeAdrenoMapRefusal &r,const char *text) {
        NativeAdrenoMapRefusalLine(line,sizeof line,r);
        if(std::strcmp(line,text))std::fprintf(stderr,"got  \"%s\"\nwant \"%s\"\n",line,text);
        return !std::strcmp(line,text) && std::strlen(line)<NativeAdrenoMapLineBytes;
    };
    CHECK(says({NativeAdrenoMapWhyWindow,0xE0001000ull,0x100000,0x100000000ull,0x11C000000ull,{},false},
               "BoMap refused: iova 0xe0001000 + 1024 KiB ends past this connection's window 0x100000000-0x11c000000"));
    CHECK(says({NativeAdrenoMapWhyRequest,2,0,1,0,{},false},
               "BoMap refused: the request: permissions 0x2 writeback 0 shareability 1, reserved/iova low bits 0"));
    CHECK(says({NativeAdrenoMapWhyHandle,0x55,0,0,0,{},false},"BoMap refused: handle 0x55 is no live object"));
    CHECK(says({NativeAdrenoMapWhyHandle,0x55,1,0,0,{},false},"BoMap refused: handle 0x55 is not ready (being made, or its release failed)"));
    CHECK(says({NativeAdrenoMapWhyMapped,7,0x21000000,0,0,{},false},"BoMap refused: handle 0x7 is mapped already, at 0x21000000"));
    CHECK(says({NativeAdrenoMapWhyScanout,1,0,0,0,{},false},"BoMap refused: the scanout asked without write (permissions 0x1)"));
    CHECK(says({NativeAdrenoMapWhyNotOpen,0,0,0,0,{},false},"BoMap refused: the connection is not open"));
    /* The VM's: the table ranges told apart from the records, with the run's figures. */
    CHECK(says({NativeAdrenoMapWhyVm,0x5A000000,64u<<20,16384,0,{NativeAdrenoVmWhySpans,16384,11200,524288,nullptr},true},
               "BoMap refused by the VM: table ranges full, 16384 needed, 11200 free of 524288 (iova 0x5a000000, 65536 KiB in 16384 ranges)"));
    CHECK(says({NativeAdrenoMapWhyVm,0x5A000000,4096,1,0,{NativeAdrenoVmWhyObjects,32768,32768,0,nullptr},false},
               "BoMap refused by the VM: mapping records full, 32768 of 32768, another call's (iova 0x5a000000, 4 KiB in 1 ranges)"));
    CHECK(says({NativeAdrenoMapWhyVm,0x2000,8192,2,0,{NativeAdrenoVmWhyOverlap,0x2000,0x1000,0,nullptr},true},
               "BoMap refused by the VM: overlaps the mapping at 0x1000 (iova 0x2000, 8 KiB in 2 ranges)"));
    CHECK(says({NativeAdrenoMapWhyVm,0x4000000,4096,1,0,{NativeAdrenoVmWhyReserved,0x4000000,4096,0,nullptr},true},
               "BoMap refused by the VM: a reserved window (null guard, GMEM) (iova 0x4000000, 4 KiB in 1 ranges)"));
    CHECK(says({NativeAdrenoMapWhyVm,0x30000000,4096,1,0,{NativeAdrenoVmWhyReserveSpans,1,30000,32768,nullptr},true},
               "BoMap refused by the VM: the render server's reserve, 1 ranges needed, 30000 free, 32768 kept (iova 0x30000000, 4 KiB in 1 ranges)"));
    CHECK(says({200,0,0,0,0,{},false},"BoMap refused: no reason recorded"));
    /* Every reason at its widest stays inside the ring's line. */
    for(uint8_t why=NativeAdrenoVmWhyNone;why<=NativeAdrenoVmWhyNotReady;++why) {
        NativeAdrenoMapRefusal r{NativeAdrenoMapWhyVm,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,{why,UINT64_MAX,UINT64_MAX,UINT64_MAX,nullptr},false};
        NativeAdrenoMapRefusalLine(line,sizeof line,r);
        CHECK(std::strlen(line)<NativeAdrenoMapLineBytes && !std::strncmp(line,"BoMap refused by the VM: ",25));
    }
}

/* 2026-11-12: a BoAlloc's or BoImport's reason (lastAllocRefusal), every one within the ring's line at its widest. */
static void allocLines()
{
    char line[NativeAdrenoMapLineBytes];
    auto says=[&](const NativeAdrenoAllocRefusal &r,const char *text) {
        NativeAdrenoAllocRefusalLine(line,sizeof line,r);
        if(std::strcmp(line,text))std::fprintf(stderr,"got  \"%s\"\nwant \"%s\"\n",line,text);
        return !std::strcmp(line,text) && std::strlen(line)<NativeAdrenoMapLineBytes;
    };
    CHECK(says({NativeAdrenoAllocWhyLimit,NativeAdrenoMethodBoAlloc,(1025ull<<20)+4096,0x14000000ull,0x14000000ull,0,0},
               "BoAlloc refused: 1026 MiB is past this connection's per-object limit of 320 MiB (its GPU window)"));
    CHECK(says({NativeAdrenoAllocWhyLimit,NativeAdrenoMethodBoImport,4096ull<<20,0xFFFFF000ull,0,0,0},
               "BoImport refused: 4096 MiB is past the per-object limit of 4095 MiB"));
    CHECK(says({NativeAdrenoAllocWhyRequest,NativeAdrenoMethodBoAlloc,4097,12,0,0,0},
               "BoAlloc refused: the request: 4097 bytes (whole 4 KiB pages), alignment 2^12 (12..24), contiguous 0"));
    CHECK(says({NativeAdrenoAllocWhyRequest,NativeAdrenoMethodBoImport,8192,0x10000001,0,0,0},
               "BoImport refused: the request: 8192 bytes at 0x10000001 (both whole 4 KiB pages, nonzero)"));
    CHECK(says({NativeAdrenoAllocWhyObjects,NativeAdrenoMethodBoAlloc,1024,1024,0,0,0},"BoAlloc refused: no object slot free, 1024 live of 1024"));
    CHECK(says({NativeAdrenoAllocWhyMemory,NativeAdrenoMethodBoAlloc,1088ull<<20,0xe00002bdull,412ull<<20,96ull<<20,512ull<<20},
               "BoAlloc refused: 1088 MiB: pool 412 MiB free, largest 96, 512 kept for the render server; then no pages from XNU (0xe00002bd NoMemory)"));
    CHECK(says({NativeAdrenoAllocWhyMemory,NativeAdrenoMethodBoAlloc,64ull<<20,0xe00002bdull,0,0,0},
               "BoAlloc refused: 64 MiB: pool 0 MiB free, largest 0; then no pages from XNU (0xe00002bd NoMemory)"));
    CHECK(says({NativeAdrenoAllocWhyWriteCombine,NativeAdrenoMethodBoAlloc,32ull<<20,0xe00002c7ull,0,0,0},
               "BoAlloc refused: 32 MiB: its pages' write-combined attribute was refused (0xe00002c7)"));
    CHECK(says({NativeAdrenoAllocWhyImport,NativeAdrenoMethodBoImport,26ull<<20,0xe00002c8ull,18,0x104000000ull,0},
               "BoImport refused: 26 MiB at 0x104000000: the task's pages were not pinned (0xe00002c8 VMError, stage 18)"));
    CHECK(says({NativeAdrenoAllocWhyNotOpen,NativeAdrenoMethodBoAlloc,0,0,0,0,0},"BoAlloc refused: the connection is not open"));
    CHECK(says({200,NativeAdrenoMethodBoImport,0,0,0,0,0},"BoImport refused: no reason recorded"));
    for(uint8_t why=NativeAdrenoAllocWhyNone;why<=NativeAdrenoAllocWhyImport;++why)
        for(uint32_t method:{uint32_t(NativeAdrenoMethodBoAlloc),uint32_t(NativeAdrenoMethodBoImport)}) {
            const NativeAdrenoAllocRefusal r{why,method,UINT64_MAX,0xe00002c8ull,UINT64_MAX,UINT64_MAX,UINT64_MAX};
            NativeAdrenoAllocRefusalLine(line,sizeof line,r);
            CHECK(std::strlen(line)<NativeAdrenoMapLineBytes && (!std::strncmp(line,"BoAlloc refused: ",17) || !std::strncmp(line,"BoImport refused: ",18)));
        }
}

static void accounting()
{
    char line[NativeAdrenoMapLineBytes];
    NativeAdrenoVaTotalsLine(line,sizeof line,5316,32768,231002,524288,1795,936,1024,701);
    CHECK(!std::strcmp(line,"va/map: VM 5316/32768 records, 231002/524288 ranges (44.0%), 1795 MiB mapped; pool 936/1024 MiB, 701 MiB outside it"));
    NativeAdrenoVaTotalsLine(line,sizeof line,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);
    CHECK(std::strlen(line)<NativeAdrenoMapLineBytes);
    NativeAdrenoVaTotalsLine(line,sizeof line,0,32768,0,0,0,0,0,0);
    CHECK(!std::strncmp(line,"va/map: VM 0/32768 records, 0/0 ranges (0.0%)",45));
    char entry[64];
    NativeAdrenoMapUsage ws{3120,150123,1153ull<<20,0x20000000ull,0xE0000000ull,0x20000000ull+(1290ull<<20)};
    CHECK(NativeAdrenoVaClientEntry(entry,sizeof entry,0,"WindowServer",ws)>0 && !std::strcmp(entry," 0:WindowServer 1153/3584M pk1290 3120o 150kr"));
    NativeAdrenoMapUsage small{12,40,(1u<<20)+1,0x190000000ull,0x14000000ull,0x190000000ull+0x101000};
    CHECK(NativeAdrenoVaClientEntry(entry,sizeof entry,4,"com.apple.dock.extra",small)>0 && !std::strcmp(entry," 4:com.apple.dock 2/320M pk2 12o 40r"));
    NativeAdrenoMapUsage none{};
    CHECK(NativeAdrenoVaClientEntry(entry,sizeof entry,31,nullptr,none)>0 && !std::strcmp(entry," 31: 0/0M pk0 0o 0r"));
}

/* A connection's live objects by size (2026-11-12, desk-dev): the classes' bounds and the line within the ring's. */
static void sizeLines()
{
    CHECK(NativeAdrenoSizeClass(4096,false)==0 && NativeAdrenoSizeClass((1u<<20)-4096,false)==0 && NativeAdrenoSizeClass(1u<<20,false)==1);
    CHECK(NativeAdrenoSizeClass((16u<<20)-4096,false)==1 && NativeAdrenoSizeClass(16u<<20,false)==2 && NativeAdrenoSizeClass(64u<<20,false)==3);
    CHECK(NativeAdrenoSizeClass(UINT64_C(1)<<30,false)==3 && NativeAdrenoSizeClass(4096,true)==4 && NativeAdrenoSizeClass(UINT64_C(1)<<30,true)==4);
    char line[NativeAdrenoMapLineBytes];
    const NativeAdrenoSizeUsage ws{{900,100,25,3,300},{60ull<<20,300ull<<20,520ull<<20,240ull<<20,(150ull<<20)-1}};
    CHECK(NativeAdrenoVaSizesLine(line,sizeof line,0,"WindowServer",ws)>0);
    CHECK(!std::strcmp(line,"va/sizes: 0:WindowServer <1M 900/60M 1-16M 100/300M 16-64M 25/520M 64M+ 3/240M imp 300/150M"));
    const NativeAdrenoSizeUsage most{{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX},
                                     {UINT64_MAX/2,UINT64_MAX/2,UINT64_MAX/2,UINT64_MAX/2,UINT64_MAX/2}};
    NativeAdrenoVaSizesLine(line,sizeof line,31,"com.apple.WebKit.GPU",most);
    CHECK(std::strlen(line)<NativeAdrenoMapLineBytes && !std::strncmp(line,"va/sizes: 31:com.apple.WebK <1M ",32));
}

/* The failure lines' counts (2026-11-12, desk-dev): a process's own first failure is written although an earlier process of its name
 * failed the same way 10000 times; the same process keeps counting by decades. */
static void eventCounts()
{
    static NativeAdrenoEventTable table;
    const uint32_t kind=1|2u<<8,status=0xe00002bd;
    CHECK(NativeAdrenoEventNote(&table,"WindowServer",kind,status,98)==1);
    for(uint32_t i=2;i<=10000;++i)CHECK(NativeAdrenoEventNote(&table,"WindowServer",kind,status,98)==(NativeAdrenoEventDecade(i)?i:0));
    CHECK(NativeAdrenoEventNote(&table,"WindowServer",kind,status,1463)==1);   /* the next WindowServer: its first time */
    CHECK(!NativeAdrenoEventNote(&table,"WindowServer",kind,status,1463));
    CHECK(NativeAdrenoEventNote(&table,"WindowServer",kind,status,98)==1);
    CHECK(NativeAdrenoEventNote(&table,"",3,0)==1 && !NativeAdrenoEventNote(&table,"",3,0));   /* no process: never restarts */
    CHECK(table.keys[0].pid==98 && table.keys[1].pid==0 && !table.others);
}

/* 2026-11-21 (20261121-ws-crash: "WindowServer[102] slot 0 Submit -> BadArgument" said nothing more): a Submit's reason
 * (lastSubmitRefusal), the entry's figures for the entry checks, every reason within the ring's line at its widest. */
static void submitLines()
{
    char line[NativeAdrenoMapLineBytes];
    const IOReturn bad=IOReturn(0xe00002c2u);
    auto says=[&](const NativeAdrenoSubmitRefusal &r,const char *text) {
        NativeAdrenoSubmitRefusalLine(line,sizeof line,r);
        if(std::strcmp(line,text))std::fprintf(stderr,"got  \"%s\"\nwant \"%s\"\n",line,text);
        return !std::strcmp(line,text) && std::strlen(line)<NativeAdrenoMapLineBytes;
    };
    CHECK(says({NativeAdrenoSubmitWhyHandle,3,12,0,0x5d5b1000,8028,0,0,0x4d2,0,bad},
               "submit refused: handle unknown: entry 3/12 handle 0 iova 0x5d5b1000 bytes 8028 (not mapped), no live object, next handle 0x4d2"));
    CHECK(says({NativeAdrenoSubmitWhyNotMapped,1,2,0x56,0x21000000,64,0,16384,0,0,bad},
               "submit refused: not mapped: entry 1/2 handle 0x56 iova 0x21000000 bytes 64 (not mapped), object of 16 KiB"));
    CHECK(says({NativeAdrenoSubmitWhyNotMapped,1,2,0x56,0x21000000,64,0,0,0,0x99,bad},
               "submit refused: not mapped: entry 1/2 handle 0x56 iova 0x21000000 bytes 64 (not mapped), VM mapping 0x99: 0xe00002c2 BadArgument"));
    CHECK(says({NativeAdrenoSubmitWhyRange,0,1,0x55,0x5d5b1000,8192,0x5d5b0000,0x1000,3,0x99,bad},
               "submit refused: range: entry 0/1 handle 0x55 iova 0x5d5b1000 bytes 8192 (mapped 0x5d5b0000+0x1000), permissions 0x3"));
    CHECK(says({NativeAdrenoSubmitWhyBytes,5,6,0x57,0x21000000,6,0,0,0,0,bad},
               "submit refused: bytes: entry 5/6 handle 0x57 iova 0x21000000 bytes 6 (mapping not looked up)"));
    CHECK(says({NativeAdrenoSubmitWhyCount,UINT32_MAX,0,0,0,0,0,0,0,0,bad},"submit refused: count: 0 buffers (1..128), reserved 0"));
    CHECK(says({NativeAdrenoSubmitWhyQueue,UINT32_MAX,4,0,0,0,0,0,9,0,bad},
               "submit refused: queue: 9 is not an open queue of this connection (4 buffers)"));
    CHECK(says({NativeAdrenoSubmitWhyWaitFence,UINT32_MAX,1,0,0,0,0,0,3,0,bad},
               "submit refused: wait fence: 3 was never issued (0xe00002c2 BadArgument, 1 buffers)"));
    CHECK(says({NativeAdrenoSubmitWhyOther,UINT32_MAX,7,0,0,0,0,0,0,0,IOReturn(0xe00002bdu)},
               "submit refused: other: 0xe00002bd NoMemory (7 buffers)"));
    for(uint8_t why=NativeAdrenoSubmitWhyNone;why<=NativeAdrenoSubmitWhyOther;++why)
        for(uint32_t index:{0u,UINT32_MAX-1,UINT32_MAX}) {
            NativeAdrenoSubmitRefusal r{why,index,UINT32_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,bad};
            NativeAdrenoSubmitRefusalLine(line,sizeof line,r);
            CHECK(std::strlen(line)<NativeAdrenoMapLineBytes && !std::strncmp(line,"submit refused: ",16));
        }
}
#endif

int main()
{
#ifdef ADRENO830_MACOS
    failureSnapshots();windows();windows64();windows256();
    refusalLines();
    submitLines();
    allocLines();
    accounting();
    sizeLines();
    eventCounts();
    std::printf("PASS: macOS GPU address space: the 16 GiB windows (3.5 GiB render server, 768 MiB large, 320 MiB small, none across 4 GiB), "
                "the 64 GiB windows (59 of 64 slots: 54 ordinary 1 GiB, large 2/1/1 GiB, apps borrow large but the last), "
                "the 256 GiB windows (64 slots: 59 ordinary 4 GiB on 4 GiB lines, 1 GiB per-object limit but slot 0, the decoder's 63), "
                "BoMap, BoAlloc, BoImport and Submit refusal lines for every reason within the ring's line, the address-space totals and "
                "per-connection lines, the objects by size, failure counts per process (%u checks)\n",checks);
#else
    std::printf("PASS: macOS GPU address space: nothing in the upstream pass\n");
#endif
    return 0;
}
