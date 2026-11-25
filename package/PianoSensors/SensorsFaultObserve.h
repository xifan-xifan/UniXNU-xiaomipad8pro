/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* SMMUv2 register facts only. Never plans a free bank, writes a route or clears a fault. */
#pragma once
#include <PianoObserve.h>
namespace sensorsobserve {
template <class Read> void faults(pianoobserve::Text &t, Read rd, uint64_t bytes, const uint16_t (&sids)[4]) {
    const uint32_t idr0=rd(0x20),idr1=rd(0x24),idr2=rd(0x28);
    const unsigned groups=idr0&255,banks=idr1&255,pages=1u<<(((idr1>>28)&7)+1);
    t.add("smmu idr0=%#x idr1=%#x idr2=%#x groups=%u banks=%u pages=%u\n",idr0,idr1,idr2,groups,banks,pages);
    if((idr1>>31)||!groups||groups>128||!banks||banks>128||pages>128||uint64_t(pages+banks)*4096>bytes) {
        t.add("status=unsupported-geometry no_decode=1\n");return;
    }
    t.add("global gfsr=%#x gfsynr0=%#x gfsynr1=%#x gfsynr2=%#x\n",rd(0x48),rd(0x50),rd(0x54),rd(0x58));
    for(unsigned sid:sids) {
        unsigned matches=0;
        for(unsigned slot=0;slot<groups;++slot) {
            const uint32_t smr=rd(0x800+4*slot),mask=(smr>>16)&0x7fff;
            if(!(smr&0x80000000)||((smr^sid)&0x7fff&~mask))continue;
            ++matches;
            const uint32_t s2cr=rd(0xc00+4*slot),type=(s2cr>>16)&3,bank=s2cr&255;
            t.add("route sid=%#x slot=%u smr=%#x s2cr=%#x type=%u bank=%u\n",sid,slot,smr,s2cr,type,bank);
            if(type==0 && bank<banks) {
                const uint32_t cb=(pages+bank)*4096;
                const uint32_t fsr=rd(cb+0x58),farlo=rd(cb+0x60),farhi=rd(cb+0x64);
                t.add("context sid=%#x bank=%u sctlr=%#x cbar=%#x cba2r=%#x fsr=%#x far=%#llx fsynr0=%#x fsynr1=%#x cbfrsynra=%#x fsr_after=%#x\n",
                      sid,bank,rd(cb),rd(0x1000+4*bank),rd(0x1800+4*bank),fsr,
                      (unsigned long long)(uint64_t(farhi)<<32|farlo),rd(cb+0x68),rd(cb+0x6c),rd(0x1400+4*bank),rd(cb+0x58));
            } else t.add("context sid=%#x status=%s\n",sid,type==0?"invalid-bank":"not-translating-no-context");
            t.add("route sid=%#x stable=%u\n",sid,smr==rd(0x800+4*slot)&&s2cr==rd(0xc00+4*slot));
        }
        t.add("sid=%#x matches=%u status=%s\n",sid,matches,matches==1?"ok":matches?"ambiguous":"unmatched");
    }
}
}
