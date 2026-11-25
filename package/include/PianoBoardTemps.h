// SPDX-License-Identifier: BSD-2-Clause-Patent
// The board temperatures PianoThermal reads from piano's PMIC ADC (B6, docs/drivers/thermal.md section 12): the battery's, the
// charger's and the two connector thermistors, for charging safety (PianoPower) and any other kext. C and C++.
//
// Two ways to ask:
//   - linked: piano_thermal_board_temps(&t), with com.unixnu.driver.PianoThermal named as a library in the caller's
//     tooling/k27/kext/<name>.json and PianoThermal listed before the caller in configs/k27-native-kexts.txt;
//   - not linked: callPlatformFunction("piano-thermal-board-temps", false, &t, nullptr, nullptr, nullptr) on the registered
//     PianoThermal service (IOService::serviceMatching("PianoThermal")): kIOReturnSuccess when filled, kIOReturnBadArgument for a
//     null or wrong-version argument; no service = PianoThermal not loaded (neither piano-thermal nor piano-thermal-adc set).
// Either way it is lock-free (a sequence count around a copy), callable from any thread, never blocks and never touches the SPMI
// bus: it returns the last round's readings. The rounds run on PianoThermal's ADC thread every piano-thermal-adc-s (30 s default).
//
// Rules for a charging policy (docs/drivers/thermal.md 12.7):
//   - a temperature counts only with its bit in `valid`: its channel works and the reading is at most three periods old;
//   - a missing temperature is unknown, never "cool": take the conservative branch (PianoPower's "without a temperature" rule);
//   - these readings may only make charging more conservative than the DSP firmware's own JEITA rules, never less;
//   - STOPPED is for the rest of the boot (an SPMI or PBS error; the console says which).
#ifndef PIANO_BOARD_TEMPS_H
#define PIANO_BOARD_TEMPS_H
#include <stdint.h>

#define PIANO_BOARD_TEMPS_VERSION 1u

enum piano_board_temp_index {   // the DT labels (where on the board each sits is not documented beyond them)
  PIANO_BOARD_TEMP_BATTERY = 0,      // pmih010x_batt_therm (PMIH0108 SID 7, AMUX1 100k pull-up)
  PIANO_BOARD_TEMP_CHARGER = 1,      // pmih010x_charger1_therm (PMIH0108, AMUX4 GPIO = its GPIO 12)
  PIANO_BOARD_TEMP_CONNECTOR = 2,    // pmih010x_conn_therm (PMIH0108, AMUX4 100k pull-up)
  PIANO_BOARD_TEMP_CONNECTOR2 = 3,   // pm8550_conn_therm2 (PM8550 SID 1, AMUX2 GPIO)
  PIANO_BOARD_TEMP_COUNT = 4
};

enum piano_board_temps_state {
  PIANO_BOARD_TEMPS_OFF = 0,        // piano-thermal-adc unset: nothing is converted
  PIANO_BOARD_TEMPS_STARTING = 1,   // no round finished yet
  PIANO_BOARD_TEMPS_RUNNING = 2,
  PIANO_BOARD_TEMPS_STOPPED = 3     // a global error stopped the ADC for this boot
};

struct piano_board_temps {
  uint32_t version;                          // in: PIANO_BOARD_TEMPS_VERSION
  uint32_t state;                            // enum piano_board_temps_state
  uint32_t valid;                            // bit i: milli_c[i] counts
  uint32_t disabled;                         // bit i: channel i is off for the boot (an error)
  int32_t milli_c[PIANO_BOARD_TEMP_COUNT];   // milli-degrees C; 0 when not valid
  uint32_t age_ms[PIANO_BOARD_TEMP_COUNT];   // since that reading; 0 when not valid
  uint32_t period_ms;                        // the round period
  uint32_t rounds;                           // rounds done
};

#ifdef __cplusplus
extern "C" {
#endif
// Fills *t (t->version must be PIANO_BOARD_TEMPS_VERSION). Returns how many temperatures are valid (0..4), -1 for a null or
// wrong-version argument (then *t is untouched).
int piano_thermal_board_temps(struct piano_board_temps *t);
#ifdef __cplusplus
}
#endif
// Separate versioned API: preserve the charging ABI. Board thermistors, not calibrated skin limits.
#define PIANO_GPU_TEMPS_VERSION 1u
struct piano_gpu_temps {
  uint32_t version, state, valid, disabled, period_ms, rounds;
  int32_t milli_c[2]; // msm, quiet
  uint32_t age_ms[2];
  uint64_t sample_us[2];
};
// gpu/fullopp1 (private/host-checks/20261122-gpu-fullopp/ANALYSIS.md 3.3): the inputs of Android's virtual skin sensor (mi_thermald's
// normal scene: battery, quiet, charger, wifi, cpu_therm) and the XO thermistor, for PianoA830's guard. Separate versioned API: the two
// structures above stay as they are. callPlatformFunction("piano-thermal-skin-temps", false, &t, ...) on PianoThermal; lock-free, the
// last round's readings, never touches the SPMI bus. Board thermistors, not a calibrated skin bound.
#define PIANO_SKIN_TEMPS_VERSION 1u
enum piano_skin_temp_index {
  PIANO_SKIN_BATTERY = 0, PIANO_SKIN_QUIET = 1, PIANO_SKIN_CHARGER = 2, PIANO_SKIN_WIFI = 3, PIANO_SKIN_MSM = 4, PIANO_SKIN_XO = 5,
  PIANO_SKIN_COUNT = 6
};
struct piano_skin_temps {
  uint32_t version, state, valid, disabled, period_ms, rounds;   // valid bit i: milli_c[i] read and at most two periods old
  int32_t milli_c[PIANO_SKIN_COUNT];
  uint32_t age_ms[PIANO_SKIN_COUNT];
  uint64_t sample_us[PIANO_SKIN_COUNT];
  // The virtual skin temperature from the first five (PianoThermal Perf.h virtualSkin, the CPU policy's own number): valid only with
  // the battery and quiet or cpu_therm; used: bit i = input i was in the sum.
  int32_t virtual_milli_c;
  uint32_t virtual_valid, virtual_used;
};
#endif
