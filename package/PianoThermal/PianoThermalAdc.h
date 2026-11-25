// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6's hook (PianoThermalAdc.cpp): the three calls PianoThermal.cpp makes. docs/drivers/thermal.md section 12.
#ifndef PIANO_THERMAL_ADC_HOOK_H
#define PIANO_THERMAL_ADC_HOOK_H
#include <stdint.h>

class IOService;

namespace pianothermal::adc {
// piano-thermal-adc (0 off, the default; 1 the board thermistors; 2 the same with every round's raw codes).
uint32_t hookLevel();
// The stock virtual skin sensor's five inputs (Perf.h kSkinChannel order) from the last round, milli-C, each with its bit in
// validMask when its channel is valid and at most three periods old. False while the ADC is off, starting or stopped.
bool hookSkin(int32_t milliC[5], uint32_t &validMask);
// Starts the ADC thread (level >= 1): its first round after delayS, then every periodS; svc gets the IORegistry properties.
void hookStart(IOService *svc, uint32_t level, uint32_t periodS, uint32_t delayS);
// callPlatformFunction("piano-thermal-board-temps", param1 = struct piano_board_temps *): true when filled.
bool hookBoardTemps(void *param1);
bool hookGpuTemps(void *param1);
// callPlatformFunction("piano-thermal-skin-temps", param1 = struct piano_skin_temps *): true when filled (gpu/fullopp1).
bool hookSkinTemps(void *param1);
}  // namespace pianothermal::adc
#endif
