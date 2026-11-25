/* SPDX-License-Identifier: MIT */
/* The R3b exit's CX gate words (A830CxGate.h): the AHB_CBCR word gpu-607d1 read at the 607 MHz permit expiry (08000000: ENABLE
 * clear, CLK_OFF clear, the clock running on another voter) is on; the words S1 leaves (08000001) and the old test's acceptance
 * (ENABLE set, whatever CLK_OFF says) stay on; a branch with CLK_OFF set and ENABLE clear is off. CX_GMU_CBCR unchanged: on when
 * CLK_OFF is clear or the NOC FSM reports ON. */
#include "A830CxGate.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

int main()
{
    /* AHB_CBCR 0x90BC */
    CHECK(a830CxAhbOn(0x08000000u) && a830CxAhbByRunningClock(0x08000000u));   /* gpu-607d1 at the expiry; every S1 before its write */
    CHECK(a830CxAhbOn(0x00000000u) && a830CxAhbByRunningClock(0x00000000u));   /* no status bits at all: CLK_OFF clear = running */
    CHECK(a830CxAhbOn(0x08000001u) && !a830CxAhbByRunningClock(0x08000001u));  /* our enable, running */
    CHECK(a830CxAhbOn(0x88000001u) && !a830CxAhbByRunningClock(0x88000001u));  /* the old acceptance (ENABLE set) is kept */
    CHECK(!a830CxAhbOn(0x80000000u) && !a830CxAhbByRunningClock(0x80000000u)); /* off: CLK_OFF set, ENABLE clear */
    CHECK(!a830CxAhbOn(0x88000000u));
    CHECK(!a830CxAhbOn(0xf8000000u));                                            /* NOC FSM bits do not count for AHB */
    /* CX_GMU_CBCR 0x90D4 */
    CHECK(a830CxGmuOn(0x08002001u));                                             /* every S1 read */
    CHECK(a830CxGmuOn(0x00000000u));
    CHECK(!a830CxGmuOn(0x80000000u) && !a830CxGmuOn(0x80000001u) && !a830CxGmuOn(0x90000000u) && !a830CxGmuOn(0xb0000000u));
    CHECK(a830CxGmuOn(0xa0000000u) && a830CxGmuOn(0xa0000001u));                /* CLK_OFF with NOC_FSM ON (2) */
    /* the flag values the console line and gpu607d-grade.py share */
    CHECK(A830CxFlagCxOn == 0x1u && A830CxFlagWindows == 0x2u && A830CxFlagReadGdsc == 0x4u && A830CxFlagReadAhb == 0x8u &&
          A830CxFlagReadCxGmu == 0x10u && A830CxFlagAhbRunning == 0x20u && A830CxFlagAccessible == 0x100u);
    std::printf("PASS: R3b CX gate words: AHB_CBCR on by ENABLE or a running clock (gpu-607d1 08000000 on, CLK_OFF-only off), CX_GMU "
                "by CLK_OFF clear or NOC FSM ON, console flags (%u checks)\n", checks);
    return 0;
}
