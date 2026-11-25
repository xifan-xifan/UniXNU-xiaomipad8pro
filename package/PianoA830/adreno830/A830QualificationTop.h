/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * The qualification top: the one rule that makes a boot a GPU qualification boot (the R3b guard, the qualification thread and its
 * C5LOG ring). S10 takes it from the bring-up's board (Adreno830Start.cpp qualificationTop/qualificationMode); the board takes it
 * from the same boot arguments before the bring-up thread exists, to allocate the C5LOG ring S10 requires (board/PianoA830.cpp).
 * gpu/fullopp1b (private/host-checks/20261122-gpu-fullopp/SWEEP1-CLASSIFY.md): the board allocated the ring only for maxlevel 7 /
 * dcvs-max 607 while S10 took tops 7..14, so gpu-fullopp1 (14 / 1100) failed S10 with reason 12. One rule for both sides now.
 */
#ifndef A830_QUALIFICATION_TOP_H
#define A830_QUALIFICATION_TOP_H
#include <stdint.h>
#include "Adreno830Board.h"

/* gpu/fullopp1: bin 2's MHz by GMU level (Sm8750GpuLevels.h, Android's live DT pwrlevels-2; S0 checks the table against them). */
inline constexpr uint16_t Adreno830FulloppMhz[15]={0,160,222,342,389,443,525,607,660,734,832,900,967,1050,1100};

/* gpu/daily4: the qualified ceiling, GMU level 7 / 8 / 9 with its MHz (607 / 660 / 734); everything else as for 607 (the full table check
 * at S0, the R3b guard, ACD off). gpu/fullopp1: with pagani-gpu-fullopp=1 any bin-2 level 7..14 with its MHz. 0 when the knobs name
 * none of them. */
static inline uint8_t adreno830_qualification_top(const Adreno830Board *b)
{
    if(!b)return 0;
    const uint32_t l=b->knobs.maxlevel,m=b->knobs.dcvs_max_mhz;
    const bool explicitR3b=b->qualification_r3b==1 && (b->flags&(1u<<Adreno830KnobDcvsMax))!=0;
    /* gpu/fullopp1: any bin-2 level 7..14 as the top, only on the explicit R3b line with pagani-gpu-dcvs-max given */
    if(b->qualification_fullopp==1)return explicitR3b && l>=7 && l<=14 && m==Adreno830FulloppMhz[l]?uint8_t(l):0;
    if(l==7 && m==607)return 7;
    /* 660 and 734 only on the explicit R3b line with pagani-gpu-dcvs-max given (a plain maxlevel=9 boot keeps its design ceiling) */
    return explicitR3b && ((l==8 && m==660) || (l==9 && m==734))?uint8_t(l):0;
}

/* The board's half: the top S10 will take, from the boot-argument getter the board hands Adreno830ParseKnobs (the same parse the
 * bring-up thread makes later; only the knobs, flags and qualification words are filled, nothing else of the board is read). */
static inline uint8_t adreno830_qualification_top_from(bool (*get)(void *cookie,const char *name,uint32_t *value),void *cookie)
{
    Adreno830Board knobs={};
    Adreno830ParseKnobs(&knobs,get,cookie);
    return adreno830_qualification_top(&knobs);
}
#endif
