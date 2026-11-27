// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoIR's IORegistry interface (tooling/native/piano-ir and any other client). The kext checks that the caller is an
// administrator and does every check itself (IrWave.h's bounds, the clocks, the pins).
//   set (IORegistryEntrySetCFProperties on the PianoIR service):
//     "piano-ir-send"     data: native uint32 durations in us, mark first, odd count (1..1024), <= 500 ms in all
//     "piano-ir-carrier"  number, Hz (30000..56000; default 38000), with piano-ir-send
//     "piano-ir-duty"     number, % (10..70; default 40), with piano-ir-send
//     "piano-ir-burst"    number, ms (1..500): a carrier burst (the camera test), instead of piano-ir-send
//   the request runs synchronously; its result is the return code and "piano-ir-last".
//   read: "piano-ir" (level), "piano-ir-state" (ready / why not), "piano-ir-survey", "piano-ir-last", "piano-ir-sends"
#ifndef PIANO_IR_CLIENT_H
#define PIANO_IR_CLIENT_H
#include <stdint.h>
#define kPianoIRClass "PianoIR"
#define kPianoIRSend "piano-ir-send"
#define kPianoIRCarrier "piano-ir-carrier"
#define kPianoIRDuty "piano-ir-duty"
#define kPianoIRBurst "piano-ir-burst"
#define kPianoIRLevel "piano-ir"
#define kPianoIRState "piano-ir-state"
#define kPianoIRSurvey "piano-ir-survey"
#define kPianoIRLast "piano-ir-last"
#define kPianoIRSends "piano-ir-sends"
#define kPianoIRMaxBurstMs 500u

static inline int piano_ir_enabled(uint64_t level) { return level == 1 || level == 2; }
static inline uint32_t piano_ir_boot_test_count(uint64_t count) { return count <= 20 ? (uint32_t)count : 0; }
static inline int piano_ir_burst_valid(uint64_t ms) { return ms >= 1 && ms <= kPianoIRMaxBurstMs; }
#endif
