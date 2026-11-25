/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 */
#ifndef A830_CX_GATE_H
#define A830_CX_GATE_H

/* The register words of the R3b exit's CX gate (Adreno830Start.cpp qualificationCxAccessible), as pure tests.
 *
 * GPU_CC AHB_CBCR (0x90BC) counts as on when its clock runs, by our enable or by another voter's: ENABLE (bit0) set, or CLK_OFF
 * (bit31) clear. gpu-607d1 (2026-11-21) read 08000000 at the 607 MHz permit expiry (ENABLE clear, CLK_OFF clear) after 625 s of
 * GMU/GPU register traffic, and the old test (ENABLE alone) refused the low vote and then the strict path (journal seq 11 and 19,
 * strict-end NotReady) -> the unsafe exit. Every boot's S1 reads the same word before it writes bit0, and two siblings of the same
 * block (DEMET 0x9010, HUB_AON 0x93E8) read bit0 back as 0 right after S1 set it: on this SoC the HLOS enable bit of these
 * branches does not read back, while the clock runs. Linux clocks this branch HALT_DELAY (gpucc-sm8750.c gpu_cc_ahb_clk, the GMU's
 * "ahb" clock, enabled in the GMU clock bulk) and never reads bit0 back. A word with CLK_OFF set and ENABLE clear is off.
 *
 * GPU_CC CX_GMU_CBCR (0x90D4) counts as on when CLK_OFF is clear or its NOC FSM reports ON (clk_branch2_check_halt), unchanged. */

#include <stdint.h>
#include "A830Regs.h"

static inline bool a830CxAhbOn(uint32_t ahb)
{
    return (ahb & SM8750_CBCR_ENABLE) || !(ahb & SM8750_CBCR_CLK_OFF);
}
/* On only through the running clock (our enable bit reads clear): the console flag bit5 of the low-failed line. */
static inline bool a830CxAhbByRunningClock(uint32_t ahb)
{
    return !(ahb & SM8750_CBCR_ENABLE) && !(ahb & SM8750_CBCR_CLK_OFF);
}
static inline bool a830CxGmuOn(uint32_t clock)
{
    return !(clock & SM8750_CBCR_CLK_OFF) || SM8750_CBCR_NOC_FSM_STATUS(clock) == SM8750_CBCR_NOC_FSM_ON;
}

/* flags of the last check (the low-failed console line): which words were read, which passed. */
enum : uint32_t {
    A830CxFlagCxOn = 1u << 0, A830CxFlagWindows = 1u << 1, A830CxFlagReadGdsc = 1u << 2, A830CxFlagReadAhb = 1u << 3,
    A830CxFlagReadCxGmu = 1u << 4, A830CxFlagAhbRunning = 1u << 5, A830CxFlagAccessible = 1u << 8,
};

#endif
