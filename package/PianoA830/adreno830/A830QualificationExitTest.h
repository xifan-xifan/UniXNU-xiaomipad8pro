/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 */
#ifndef A830_QUALIFICATION_EXIT_TEST_H
#define A830_QUALIFICATION_EXIT_TEST_H

#include "A830Qualification.h"

// Only used for the opt-in fault-exit commands. Sample age bounds this test
// independently of the faster ADC cadence required for the 607 MHz permit.
inline bool a830ExitTestAllowed(uint32_t command, const Sm8750TsensSample &gpu,
                                const piano_gpu_temps &board, bool boardOk,
                                const A830Qualification &qualification,
                                const A830PowerState &power, const A830Recovery &recovery)
{
    const bool cool = boardOk && board.version == PIANO_GPU_TEMPS_VERSION &&
        board.state == PIANO_BOARD_TEMPS_RUNNING && board.valid == 3 && !board.disabled &&
        board.sample_us[0] && board.sample_us[1] && board.age_ms[0] <= 10000 && board.age_ms[1] <= 10000 &&
        board.milli_c[0] >= -40000 && board.milli_c[0] < 62000 &&
        board.milli_c[1] >= -40000 && board.milli_c[1] < 37000 &&
        gpu.valid == 255 && gpu.max_deci >= -400 && gpu.max_deci < 800;
    if (!cool || qualification.high() || power.phase() != A830PowerActive ||
        recovery.pending() || recovery.busy()) return false;

    if (command == 3) {
        // An idle control can already be at 160 MHz. The exit still requires a fresh HFI ACK.
        const uint8_t level = power.votedLevel();
        return !qualification.reason() && level >= 1 && level <= 3;
    }
    return command == 4 && qualification.reason() == A830QualBoardHot && recovery.qualificationLowConfirmed();
}

#endif
