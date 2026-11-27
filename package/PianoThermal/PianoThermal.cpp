// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: SoC temperatures, thermal protection and CPU frequency for piano (SM8750) under native macOS 27 at EL1. Design,
// facts, boot arguments and the test procedure: docs/drivers/thermal.md. Inert unless piano-thermal or piano-cpufreq is set.
//   piano-thermal=1  read only: every TSENS sensor of the four controllers (Tsens.h) each poll (piano-thermal-poll-ms, 1000; every
//                    piano-thermal-fast-ms, 20, while a sensor is within 10 C of its trip or a cluster is thermally limited), the
//                    hottest per controller on the console every piano-thermal-log-s (30) and as IORegistry properties; at start the
//                    controllers' version/enable/thresholds (TM and SROT loads), the SCMI channel's status word (a load), and every 30 s
//                    each core's clock measured with a chain of dependent adds (no device access; piano-thermal-clock=0 skips it;
//                    since 2026-11-12 not while the display is off, which would wake every idle core for a log line: =2 measures
//                    then too, as before).
//                    Nothing is written at this level: every MMIO access is a load.
//   piano-thermal=2  level 1 + the protection policy (Policy.h): warm (85 C) logged; at a sensor's stock passive trip the GPU is
//                    capped (PianoA830's "piano-gpu-ceiling", reversible) or, while PianoA830 has none, parked at its lowest level
//                    for the rest of the boot (its exported adreno830_dcvs_park; at critical always); at the stock
//                    critical trip - piano-thermal-margin (10 C) held for 1 s: logged loudly (every 5 s), and with
//                    piano-thermal-shutdown=1 the kernel's halt (reboot_kernel(RB_HALT): processes signalled, disks synced and
//                    unmounted, IOKit's shutdown notifications, PSCI SYSTEM_OFF). Default: log only.
//   piano-cpufreq=1  the CPU clusters over SCMI (Scmi.h): protocol, domains, levels, the level each cluster is at (LEVEL_GET), then
//                    every 30 s again. Messages only, no level is set. Writes: the SCMI channel's shared memory and the doorbell.
//   piano-cpufreq=2  fixed level per cluster: the fastest <= piano-cpufreq-cap0/cap1 (MHz, default Ubuntu's Power Saver profile:
//                    1785 / 1958), thermal limit on top (Cluster.h: Linux step_wise on the stock trips).
//   piano-cpufreq=3  the governor (Governor.h): load-based per cluster every piano-cpufreq-tick-ms (100), caps on battery (Power Saver)
//                    / on power (Balanced: 3532 / 4089) from the IOPMPowerSource's ExternalConnected (unknown = battery), thermal
//                    limit on top. Its busy figure leaves out this thread's own time on each core (2026-11-12, P9), and the window
//                    holding the per-core clock measurement makes no decision.
//   piano-cpufreq-boost=S  (2026-11-14) the boot boost: every cluster at its sustained level until S s of uptime, the thermal limit
//                    on top, then the mode's own level (implies piano-cpufreq=1: then back to the boot stages' level); -cap=<MHz>.
//                    Its own thermal back-off (2026-11-15, ufs5r3): the hottest CPU sensor at -step C (88) steps the boost one
//                    level down every piano-thermal-fast-ms — never up again in this boot — and at -end C (90) the boost is over
//                    at once (Governor.h BoostBackoff; the ufs5 rerun peaked at 97.3 C against the 92 C line without it).
// The thermal limit of modes 2 and 3 (Cluster.h) works on a hard ceiling since 2026-11-12: one level down per 10 ms while any cpu-*
// sensor of the cluster is at piano-cpufreq-hot (default 90 C, never above the zone's stock passive trip; 0 = the stock trips),
// whatever piano-thermal says.
// piano-cpufreq >= 1 implies the level 1 readout (its thermal input). No clock, regulator or GDSC is voted by this kext.
// Suspend hooks (piano_thermal_suspend_prepare/finish, below): the threshold interrupt masks across system suspend, the only TSENS
// writes, and only when a caller asks: this service's setPowerState (in the power tree, docs/power.md 4) with
// piano-thermal-sleep-mask=1, or another driver through the platform functions. Across a system sleep the thread is parked (no
// SCMI request, no TSENS read in flight; Governor.h SleepPlan); the first zone update after the wake follows at once.
// The power profile (docs/policy.md 5.2): callPlatformFunction("piano-thermal-profile", mode) from PianoPolicy: 0 the caps by the
// power source, 1 Power Saver, 2 Balanced, 3 High Power (macOS Energy Mode, docs/policy.md 12), whatever the source (Governor.h
// Profile); the governor reads it every tick, the fixed level ignores it; the property piano-thermal-profile and one line per change;
// piano-thermal-profiles = 3 says that mode 3 is taken (PianoPolicy sends it only then). High Power acts with the perf policy >= 1:
// the clusters' top levels piano-cpufreq-hpm0/1 (3532 / 4320 MHz) as the perf cap, over sustained unless the firmware refused that
// this boot, no skin table, no onset burst; the hard caps, the power caps (-ac0/1) and the thermal limit stay on top.
// piano-thermal-adc=1|2 (B6, PianoThermalAdc.cpp; implies piano-thermal=1): the eleven board thermistors on the PMIC ADC, on a thread
// of their own, and piano_thermal_board_temps() for charging safety. Its only writes: the ADC SDAM's conversion bytes.
// piano-thermal-irq=1 (PianoThermalIrq.cpp, TsensIrq.h; with piano-thermal >= 1): the TSENS upper/lower threshold interrupts; at
// rest the base poll relaxes, an interrupt raises the tier at once; any failure: today's polling (docs/drivers/thermal.md section 13).
// The stock-level policy of piano-cpufreq=3 (2026-11-22, Perf.h; docs/drivers/thermal.md section 14), machdep.piano_cpufreq_perf
// (root, 0/1; boot default piano-cpufreq-perf, 0): 1 = the caps by PianoPolicy's profile whatever the power source (Low Power
// Mode = Power Saver, else Balanced), no hard cap but piano-cpufreq-cap0/1 when given, the stock skin step table on the virtual skin
// temperature (piano-cpufreq-skin, 1; the board thermistors of piano-thermal-adc), the onset burst (piano-cpufreq-burst0/1 MHz,
// 2745 / 4089, -burst-ms 1500, -burst-gap-ms 500; none from 48 C skin, -skin-boost-limit), a load decision every
// machdep.piano_cpufreq_perf_tick_ms (piano-cpufreq-perf-tick-ms, 20) with downs held piano-cpufreq-perf-down-ms (100), the
// sensors still read every 100 ms (10 ms near a trip). 0 = the policy before it: the caps by the measured power source, the hard
// caps piano-cpufreq-legacy-cap0/1 (2400 / 2438: the v13.1x daily line's piano-cpufreq-cap0/1), the 100 ms tick, 3 ticks down.
// 2 = beyond stock: as 1 without the skin table (the sustained levels 3532.8 / 4089.6 as ceilings) and the onset burst up to
// piano-cpufreq-burst-max0/1 (3532 / 4320 MHz; above the firmware's sustained level only while a burst is open; a refused level
// above sustained turns that off for the boot without counting toward the 20 refusals).
// Both keep the thermal limit (piano-cpufreq-hot, 90 C) on top. machdep.piano_cpufreq_state (read only): the mode, tick, skin and
// per cluster the level, the ceiling and the residency per level since boot (ms), for before/after windows from user space.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include "Cluster.h"
#include "Governor.h"
#include <PianoPowerContract.h>
#include <PianoDisplayFollow.h>
#include "PianoThermalAdc.h"
#include "PianoThermalIrq.h"
#include "Perf.h"
#include "Policy.h"
#include "Scmi.h"
#include "Tsens.h"

using namespace pianothermal;
SYSCTL_DECL(_machdep);

extern "C" {
// PianoA830 (listed before this kext; tooling/k27/kext/PianoThermal.json names it as a library). Both are lock-free no-ops while
// the GPU driver is inert (piano-gpu unset: no DCVS).
void adreno830_dcvs_park(void);
int32_t adreno830_thermal_state(uint32_t *flags);
// The kernel (resolved from its symbol table by the KC builder).
int reboot_kernel(int howto, char *message);
kern_return_t processor_info(processor_t processor, processor_flavor_t flavor, host_t *host, processor_info_t info,
                             mach_msg_type_number_t *count);
processor_t thread_bind(processor_t processor);
processor_t cpu_to_processor(int cpu);
extern int hz;
// Post-mortem breadcrumbs: the protection state (Policy State) and the last hottest reading in deci-C.
volatile int32_t piano_thermal_state = 0;
volatile int32_t piano_thermal_hottest_deci = 0;
}

namespace {

constexpr uint32_t kA830Valid = 1u << 0, kA830Limiting = 1u << 1, kA830Emergency = 1u << 2;   // A830Dcvs.h A830Thermal*
constexpr int kRbHalt = 0x08;                                                                   // bsd/sys/reboot.h RB_HALT

struct Args {
  uint32_t level, cpufreq, shutdown, pollMs, logS, clock, srot, warmC, marginC, delay, sleepMask, boostS, boostCap, boostStepC,
      boostEndC;
  uint32_t cap[2], bat[2], ac[2], minMhz[2], hispeed[2], tickMs, fastMs, warmMs, warmPollC, fastMhz, fastTempC, clockMs, gpuMhz, hotC,
      releaseC, cutC;
  bool capGiven[2];
  bool android, androidArmed;
  // the stock-level policy (Perf.h)
  uint32_t perf, perfTickMs, perfDownMs, legacyCap[2], burst[2], burstMax[2], burstMs, burstGapMs, skin, skinLimitC, hpmTop[2];
  // the launch and input boosts (Perf.h; 2026-11-22 01:02) and the silicon gate of bursts and boosts
  uint32_t boosts, launchMhz[2], launchMs, inputMhz[2], inputMs, gpuLaunchMhz, gpuLaunchCapMhz, gpuInputMhz, launchWatch, gateOpenC, gateEndC, gateQuietMs;
  // the full table (Perf.h fullopp; 2026-11-22 22:43): boot default of machdep.piano_cpufreq_fullopp / _walt, the thermal limit's
  // release rate (every policy) and the full table's Android rule (release point, rate, no temperature gate), the test-only sweep
  uint32_t fullopp, walt, riseMs, fullRiseMs, fullReleaseC, fullGate, sweep, fwCheckMs;
};
// The panel's display power, followed through NativeGopDisplay's messages (package/include/PianoDisplayFollow.h): the clock line's rule.
pianodisplay::Follow gDisplay;
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void buildTrips();

void readArgs()
{
  gArgs.level = bootArg("piano-thermal", 0);
  gArgs.cpufreq = bootArg("piano-cpufreq", 0);
  if (gArgs.cpufreq > 3) gArgs.cpufreq = 3;
  // piano-cpufreq-boost=S (2026-11-14, boot time): every cluster that allows LEVEL_SET at its sustained level (3532.8 / 4089.6 MHz)
  // until S s of uptime, the thermal limit on top, then the mode's own level; it implies piano-cpufreq=1, whose clusters go back to
  // the level the boot stages left (the clock line of 2026-11-14's boots: cluster 0 about 2.0 GHz, cluster 1, the ADT's P cores,
  // about 1.0 GHz). piano-cpufreq-boost-cap=<MHz>: no faster than that. 0 (the default): nothing changes.
  gArgs.boostS = clampU(bootArg("piano-cpufreq-boost", 0), 0, 3600);
  gArgs.boostCap = bootArg("piano-cpufreq-boost-cap", 0);
  // the boost's own thermal back-off (2026-11-15, ufs5r3): the hottest CPU sensor at boostStepC steps the boost one level down
  // (never up again in this boot), at boostEndC the boost is over at once; the ufs5 rerun's boost peaked at 97.3 C (92 C line)
  gArgs.boostStepC = clampU(bootArg("piano-cpufreq-boost-step", 88), 0, 110);
  gArgs.boostEndC = clampU(bootArg("piano-cpufreq-boost-end", 90), 0, 110);
  if (gArgs.boostS && !gArgs.cpufreq) gArgs.cpufreq = 1;
  if (gArgs.level > 2) gArgs.level = 2;
  if (gArgs.cpufreq && !gArgs.level) gArgs.level = 1;
  if (adc::hookLevel() && !gArgs.level) gArgs.level = 1;   // B6: the board thermistors come with the level 1 readout
  gArgs.shutdown = bootArg("piano-thermal-shutdown", 0);
  gArgs.pollMs = clampU(bootArg("piano-thermal-poll-ms", 1000), 100, 10000);
  gArgs.logS = clampU(bootArg("piano-thermal-log-s", 30), 5, 3600);
  gArgs.clock = bootArg("piano-thermal-clock", 1);
  gArgs.srot = bootArg("piano-thermal-srot", 1);
  gArgs.warmC = clampU(bootArg("piano-thermal-warm", 85), 50, 104);
  gArgs.marginC = clampU(bootArg("piano-thermal-margin", 10), 5, 30);
  gArgs.delay = clampU(bootArg("piano-thermal-delay", 0), 0, 600);
  gArgs.sleepMask = bootArg("piano-thermal-sleep-mask", 0);   // a system sleep also masks the threshold interrupts (the hooks)
  // Ubuntu's GNOME profiles on piano (docs/ubuntu-history/device.md 2.6): Power Saver caps cpu0-5 at 1785.6 MHz and cpu6-7 at
  // 1958.4 MHz, Balanced at 3532.8 / 4089.6 MHz (boost off). piano-cpufreq-cap (both clusters) or -cap0/-cap1: the fixed level of
  // mode 2 (default Power Saver's); in mode 3 a hard ceiling, only when given. A cap in MHz admits the levels of that whole MHz.
  static const uint32_t kSaver[2] = {1785, 1958}, kBalanced[2] = {3532, 4089};
  uint32_t capBoth = 0;
  const bool bothGiven = PE_parse_boot_argn("piano-cpufreq-cap", &capBoth, sizeof capBoth);
  for (unsigned d = 0; d < 2; ++d) {
    uint32_t v = bothGiven ? capBoth : kSaver[d];
    const bool given = PE_parse_boot_argn(d ? "piano-cpufreq-cap1" : "piano-cpufreq-cap0", &v, sizeof v);
    gArgs.cap[d] = clampU(v, 300, 5000);
    gArgs.capGiven[d] = given || bothGiven;
  }
  gArgs.android = bootArg("piano-cpufreq-android", 0) == 1;
  gArgs.androidArmed = gArgs.android && gArgs.capGiven[0] && gArgs.capGiven[1] &&
      androidCpuArmed(CpuMode(gArgs.cpufreq), gArgs.cap[0], gArgs.cap[1]);
  gArgs.bat[0] = bootArg("piano-cpufreq-bat0", kSaver[0]);
  gArgs.bat[1] = bootArg("piano-cpufreq-bat1", kSaver[1]);
  gArgs.ac[0] = bootArg("piano-cpufreq-ac0", kBalanced[0]);
  gArgs.ac[1] = bootArg("piano-cpufreq-ac1", kBalanced[1]);
  gArgs.minMhz[0] = bootArg("piano-cpufreq-min0", 0);
  gArgs.minMhz[1] = bootArg("piano-cpufreq-min1", 0);
  gArgs.hispeed[0] = bootArg("piano-cpufreq-hispeed0", 1500);
  gArgs.hispeed[1] = bootArg("piano-cpufreq-hispeed1", 1800);
  gArgs.tickMs = clampU(bootArg("piano-cpufreq-tick-ms", 100), 20, 1000);
  gArgs.perf = clampU(bootArg("piano-cpufreq-perf", 0), 0, 2);
  gArgs.perfTickMs = clampU(bootArg("piano-cpufreq-perf-tick-ms", 20), 20, 1000);
  gArgs.perfDownMs = clampU(bootArg("piano-cpufreq-perf-down-ms", 100), 0, 5000);
  gArgs.legacyCap[0] = clampU(bootArg("piano-cpufreq-legacy-cap0", 2400), 300, 5000);
  gArgs.legacyCap[1] = clampU(bootArg("piano-cpufreq-legacy-cap1", 2438), 300, 5000);
  gArgs.burst[0] = clampU(bootArg("piano-cpufreq-burst0", 2745), 0, 5000);
  gArgs.burst[1] = clampU(bootArg("piano-cpufreq-burst1", 4089), 0, 5000);
  gArgs.burstMax[0] = clampU(bootArg("piano-cpufreq-burst-max0", 3532), 0, 5000);
  gArgs.burstMax[1] = clampU(bootArg("piano-cpufreq-burst-max1", 4320), 0, 5000);
  // High Power (Profile::performance, docs/policy.md 12): the perf cap per cluster (the tables' tops)
  gArgs.hpmTop[0] = clampU(bootArg("piano-cpufreq-hpm0", 3532), 300, 5000);
  gArgs.hpmTop[1] = clampU(bootArg("piano-cpufreq-hpm1", 4320), 300, 5000);
  gArgs.burstMs = clampU(bootArg("piano-cpufreq-burst-ms", 1500), 0, 10000);
  gArgs.burstGapMs = clampU(bootArg("piano-cpufreq-burst-gap-ms", 500), 0, 60000);
  gArgs.skin = bootArg("piano-cpufreq-skin", 1) ? 1 : 0;
  gArgs.skinLimitC = clampU(bootArg("piano-cpufreq-skin-boost-limit", 48), 30, 60);
  gArgs.boosts = bootArg("piano-cpufreq-boosts", 1) ? 1 : 0;
  gArgs.launchMhz[0] = clampU(bootArg("piano-cpufreq-launch0", 2745), 0, 5000);
  gArgs.launchMhz[1] = clampU(bootArg("piano-cpufreq-launch1", 4089), 0, 5000);
  gArgs.launchMs = clampU(bootArg("piano-cpufreq-launch-ms", 1500), 0, 5000);
  gArgs.inputMhz[0] = clampU(bootArg("piano-cpufreq-input0", 1152), 0, 5000);
  gArgs.inputMhz[1] = clampU(bootArg("piano-cpufreq-input1", 1958), 0, 5000);
  gArgs.inputMs = clampU(bootArg("piano-cpufreq-input-ms", 200), 0, 5000);
  gArgs.gpuLaunchMhz = clampU(bootArg("piano-cpufreq-gpu-launch-mhz", 443), 0, 1100);
  // gpu/fullopp1: Android's launch hint also caps the GPU (max_pwrlevel 2 = 967 MHz for the launch); 0 = no cap (as before)
  gArgs.gpuLaunchCapMhz = clampU(bootArg("piano-cpufreq-gpu-launch-cap-mhz", 0), 0, 1100);
  gArgs.gpuInputMhz = clampU(bootArg("piano-cpufreq-gpu-input-mhz", 0), 0, 1100);
  gArgs.launchWatch = bootArg("piano-cpufreq-launch-watch", 1) ? 1 : 0;
  gArgs.gateOpenC = clampU(bootArg("piano-cpufreq-gate-open", 0), 0, 110);
  gArgs.gateEndC = clampU(bootArg("piano-cpufreq-gate-end", 0), 0, 110);
  gArgs.gateQuietMs = clampU(bootArg("piano-cpufreq-gate-quiet-ms", 300), 0, 5000);
  // the full table (Perf.h): Android's table and scenes behind machdep.piano_cpufreq_fullopp (boot default piano-cpufreq-fullopp, 1 since gpu/throttle1),
  // the WALT-like demand behind machdep.piano_cpufreq_walt (piano-cpufreq-walt, 0); with the full table the thermal limit releases
  // under piano-cpufreq-fullopp-release (87 C: the stock 95 C trip's 8 C hysteresis) one level per piano-cpufreq-fullopp-rise-ms
  // (100; the stock rule polls every 4 ms) and no temperature gate holds bursts or boosts (piano-cpufreq-fullopp-gate=1 keeps it);
  // piano-cpufreq-sweep=1 (test only) arms machdep.piano_cpufreq_sweep0/1 (a cluster pinned at one level) and
  // machdep.piano_cpufreq_measure (each core's clock at the next poll); piano-cpufreq-fw-check-ms: LEVEL_GET per cluster
  // (1000; with the full table or the sweep only), the firmware's level against ours
  // gpu/throttle1 (owner 2026-11-24 05:45): the full table is the default (it acts only with piano-cpufreq=3 and perf policy >= 1);
  // piano-cpufreq-fullopp=0 keeps the stock-level table of section 14
  gArgs.fullopp = bootArg("piano-cpufreq-fullopp", 1) ? 1 : 0;
  gArgs.walt = bootArg("piano-cpufreq-walt", 0) ? 1 : 0;
  gArgs.riseMs = clampU(bootArg("piano-cpufreq-rise-ms", 1000), 10, 10000);
  gArgs.fullRiseMs = clampU(bootArg("piano-cpufreq-fullopp-rise-ms", 100), 10, 10000);
  gArgs.fullReleaseC = clampU(bootArg("piano-cpufreq-fullopp-release", 87), 40, 118);
  gArgs.fullGate = bootArg("piano-cpufreq-fullopp-gate", 0) ? 1 : 0;
  gArgs.sweep = bootArg("piano-cpufreq-sweep", 0) ? 1 : 0;
  gArgs.fwCheckMs = clampU(bootArg("piano-cpufreq-fw-check-ms", 1000), 100, 60000);
  // Poll tiers (Cluster.h pollTier): 10 ms near a trip (Linux: 10 ms while passive), 250 ms from 70 C, the base below.
  gArgs.fastMs = clampU(bootArg("piano-thermal-fast-ms", 10), 5, 1000);
  gArgs.warmMs = clampU(bootArg("piano-thermal-warm-ms", 250), 10, 10000);
  gArgs.warmPollC = clampU(bootArg("piano-thermal-warm-poll", 70), 30, 104);
  gArgs.fastMhz = clampU(bootArg("piano-thermal-fast-mhz", 2500), 300, 5000);
  // Above the fast-poll clock: 10 ms only from piano-thermal-fast-temp (70 C) up, else every piano-thermal-clock-ms (100: one tick of
  // the governor, so no wakeup of its own while it governs). Cluster.h pollTier has the arithmetic.
  gArgs.fastTempC = clampU(bootArg("piano-thermal-fast-temp", 70), 30, 104);
  gArgs.clockMs = clampU(bootArg("piano-thermal-clock-ms", 100), 5, 100);
  gArgs.gpuMhz = clampU(bootArg("piano-thermal-gpu-mhz", 160), 100, 1100);   // the GPU ceiling at passive (160: the A830's floor)
  // The cluster thermal limit's thresholds (Cluster.h): the ceiling piano-cpufreq-hot, absolute degrees C for every cpu-* zone,
  // default 90 C (2026-11-12, P9), each zone's stock passive trip (105 C or 118 C) when that is lower; 0 = the stock DT's per zone
  // (105 C or 118 C passive, 5 or 8 C hysteresis). The release point piano-cpufreq-release (default: 5 C under the ceiling, or the
  // zone's hysteresis with the stock trips); critical 125 C - margin unless piano-cpufreq-cut.
  gArgs.hotC = clampU(bootArg("piano-cpufreq-hot", 90), 0, 118);
  if (gArgs.hotC && gArgs.hotC < 40) gArgs.hotC = 40;
  gArgs.releaseC = clampU(bootArg("piano-cpufreq-release", 0), 0, 118);
  gArgs.cutC = clampU(bootArg("piano-cpufreq-cut", 0), 0, 125);
  // The silicon gate of bursts and boosts follows the ceiling unless given (perf-cpu2): open below ceiling - 15 C, end at ceiling - 5 C
  // (90 C: 75 / 85; Android's 95 C trip: 80 / 90); with the stock trips (hot 0) 90 / 100.
  {
    const uint32_t ceil = gArgs.hotC ? gArgs.hotC : 105;
    if (!gArgs.gateOpenC) gArgs.gateOpenC = ceil - 15;
    if (!gArgs.gateEndC) gArgs.gateEndC = ceil - 5;
  }
  buildTrips();
}

ClusterTrips gTrips, gTripsFull;   // gTripsFull: the full table's (Perf.h fullopp) release point, the rest as gTrips
char gTripsText[160];

void buildTrips()
{
  gTrips.hotDeci = int32_t(gArgs.hotC) * 10;
  gTrips.releaseDeci = int32_t(gArgs.releaseC) * 10;
  gTrips.cutDeci = int32_t(gArgs.cutC) * 10;
  gTrips.marginDeci = int32_t(gArgs.marginC) * 10;
  gTripsFull = gTrips;
  gTripsFull.releaseDeci = int32_t(gArgs.fullReleaseC) * 10;
  if (gTrips.hotDeci)
    snprintf(gTripsText, sizeof gTripsText, "one level down per %u ms at %u C (the stock 105/118 C if lower), up per 1 s under %u C, "
             "slowest at %u C", gArgs.fastMs, gArgs.hotC, gArgs.releaseC ? gArgs.releaseC : gArgs.hotC - 5,
             gArgs.cutC ? gArgs.cutC : 125 - gArgs.marginC);
  else
    snprintf(gTripsText, sizeof gTripsText, "stock trips (cpu-* passive 105/118 C, hysteresis 5/8 C): one level down per %u ms, up "
             "per 1 s, slowest at %u C", gArgs.fastMs, gArgs.cutC ? gArgs.cutC : 125 - gArgs.marginC);
}
const char *tripsText() { return gTripsText; }

uint64_t nowNs()
{
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns;
}
uint64_t gStartNs;
uint32_t upS() { return uint32_t((nowNs() - gStartNs) / 1000000000ull); }

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoThermal: %s\n", line);
}

// Append to line[at]: the new end, clamped to the buffer (snprintf returns what the text wanted, not what fitted).
int catf(char *line, size_t size, int at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int catf(char *line, size_t size, int at, const char *fmt, ...)
{
  if (at < 0 || size_t(at) + 1 >= size) return at;
  va_list ap;
  va_start(ap, fmt);
  const int wanted = vsnprintf(line + at, size - size_t(at), fmt, ap);
  va_end(ap);
  if (wanted < 0) return at;
  return size_t(at) + size_t(wanted) >= size ? int(size - 1) : at + wanted;
}

struct Deci {
  char s[16];
  explicit Deci(int32_t d) { deciText(d, s, sizeof s); }
};

// Explicit ldr/str, as the other piano kexts do.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------- MMIO
struct Window {
  IOMemoryMap *map;
  uintptr_t va;
  uint32_t bytes;
};
Window gTm[kControllers], gSrot[kControllers], gShmem, gMbox;
unsigned gMapFailures;

bool mapWindow(Window &w, uint64_t pa, uint32_t bytes)
{
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) {
    ++gMapFailures;
    return false;
  }
  w = {map, uintptr_t(map->getVirtualAddress()), bytes};
  return true;
}

uint32_t tsensRead(void *, unsigned c, uint32_t off)
{
  if (c >= kControllers || !gTm[c].va || off + 4 > gTm[c].bytes) return 0xffffffffu;
  return rd32(gTm[c].va + off);
}
bool tsensMapped(void *, unsigned c) { return c < kControllers && gTm[c].va; }
const TsensIo kTsensIo = {nullptr, tsensRead, tsensMapped};

uint32_t srotRead(unsigned c, uint32_t off)
{
  if (c >= kControllers || !gSrot[c].va || off + 4 > gSrot[c].bytes) return 0xffffffffu;
  return rd32(gSrot[c].va + off);
}

// SCMI channel (level 1 only reads it; the transport writes only with piano-cpufreq >= 1)
uint32_t shmRead(void *, uint32_t off) { return gShmem.va && off + 4 <= gShmem.bytes ? rd32(gShmem.va + off) : 0; }
void shmWrite(void *, uint32_t off, uint32_t v)
{
  if (gShmem.va && off + 4 <= gShmem.bytes) wr32(gShmem.va + off, v);
}
void ring(void *)
{
  __builtin_arm_dsb(0xf);
  if (gMbox.va) wr32(gMbox.va + scmi::kDoorbell, 1);
  __builtin_arm_dsb(0xf);
}
uint64_t scmiNowUs(void *) { return nowNs() / 1000; }
void scmiPause(void *, uint32_t us) { IODelay(us); }

// ------------------------------------------------------------------------------------------------------------- CPU topology
constexpr unsigned kMaxCpus = 16, kDomains = 2;
struct Cpu {
  unsigned id, domain;
  uint32_t phys;
  processor_t processor;
  uint64_t selfNs;           // this thread's own time on the core (noteSelf), cumulative
  CoreLoad load;             // the load window (Governor.h): idle and own time at the last reading
  int32_t busy;              // the last window's busy per mille (-1 none): the WALT-like demand's input (Perf.h WaltCore)
  perf::WaltCore walt;
};
Cpu gCpus[kMaxCpus];
unsigned gCpuCount;
uint64_t gSelfNsTotal;       // this thread's own time on all cores, for the report
bool gRebase;                // the next load reading only starts a window (the clock measurement ran in the last one)

// This thread's own time (a poll's work, measured around it) on the core it ran on: taken out of that core's busy figure.
void noteSelf(uint64_t ns)
{
  gSelfNsTotal += ns;
  const unsigned me = unsigned(cpu_number());
  for (unsigned i = 0; i < gCpuCount; ++i)
    if (gCpus[i].id == me) {
      gCpus[i].selfNs += ns;
      return;
    }
}

void readTopology()
{
  const ml_topology_info_t *t = ml_get_topology_info();
  gCpuCount = 0;
  for (unsigned i = 0; t && i < t->num_cpus && gCpuCount < kMaxCpus; ++i) {
    Cpu &c = gCpus[gCpuCount++];
    c.id = t->cpus[i].cpu_id;
    c.phys = t->cpus[i].phys_id;
    c.domain = (c.phys >> 16) & 0xff;   // MPIDR Aff2: cluster 0 = cpu0-5 (domain 0), cluster 1 = cpu6-7 (domain 1)
    c.processor = cpu_to_processor(int(c.id));
    c.selfNs = 0;
    c.load = CoreLoad();
    c.busy = -1;
    c.walt.reset();
  }
}

bool processorRunning(processor_t p)
{
  processor_basic_info_data_t info;
  mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
  host_t host;
  return p && processor_info(p, PROCESSOR_BASIC_INFO, &host, processor_info_t(&info), &count) == KERN_SUCCESS && info.running;
}

// The idle time of one core in ns (XNU's per-processor recount idle time, in scheduler ticks of 1/hz s: processor_info's
// PROCESSOR_CPU_LOAD_INFO, the same numbers host_processor_info gives top(1)).
bool idleNs(processor_t p, uint64_t &ns)
{
  processor_cpu_load_info_data_t info;
  mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
  host_t host;
  if (!p || processor_info(p, PROCESSOR_CPU_LOAD_INFO, &host, processor_info_t(&info), &count) != KERN_SUCCESS) return false;
  ns = uint64_t(info.cpu_ticks[CPU_STATE_IDLE]) * (1000000000ull / uint64_t(hz > 0 ? hz : 100));
  return true;
}

// Busiest core of each domain since the last call, per mille, this thread's own time on each core left out; -1 where no core of the
// domain could be read, and for every domain right after the clock measurement (its window is dropped, gRebase).
void domainBusy(int32_t busy[kDomains])
{
  for (unsigned d = 0; d < kDomains; ++d) busy[d] = -1;
  const uint64_t now = nowNs();
  for (unsigned i = 0; i < gCpuCount; ++i) {
    Cpu &c = gCpus[i];
    uint64_t idle = 0;
    if (c.domain >= kDomains || !idleNs(c.processor, idle)) {
      c.load.fail();
      c.busy = -1;
      continue;
    }
    const int32_t b = c.load.read(now, idle, c.selfNs, gRebase);
    c.busy = b;
    if (b > busy[c.domain]) busy[c.domain] = b;
  }
  gRebase = false;
}

// About the core's clock in MHz: 200000 dependent adds (one cycle each on an in-order chain), the fastest of three runs; no device
// access. Interrupts stay enabled (an interrupt only makes a run slower, and the fastest run wins).
uint32_t chainMhz()
{
  uint64_t best = ~0ull;
  for (unsigned run = 0; run < 3; ++run) {
    uint64_t x = 0;
    const uint64_t t0 = mach_absolute_time();
    for (unsigned i = 0; i < 200; ++i) __asm__ volatile(".rept 1000\n\tadd %0, %0, #1\n\t.endr" : "+r"(x));
    const uint64_t dt = mach_absolute_time() - t0;
    if (dt < best) best = dt;
  }
  uint64_t ns;
  absolutetime_to_nanoseconds(best, &ns);
  return ns ? uint32_t(200000ull * 1000ull / ns) : 0;
}

// Each running core's clock, measured on that core (thread_bind + thread_block, as the kernel's own per-CPU loops do). It runs on
// every core, so the governor's next load window would count it as load there: that window is dropped (gRebase).
void measureClocks(uint32_t mhz[kMaxCpus], bool on[kMaxCpus])
{
  gRebase = true;
  for (unsigned i = 0; i < gCpuCount; ++i) {
    mhz[i] = 0;
    on[i] = false;
    if (!processorRunning(gCpus[i].processor)) continue;
    thread_bind(gCpus[i].processor);
    thread_block(THREAD_CONTINUE_NULL);
    if (unsigned(cpu_number()) == gCpus[i].id) {
      mhz[i] = chainMhz();
      on[i] = true;
    }
  }
  thread_bind(PROCESSOR_NULL);
  thread_block(THREAD_CONTINUE_NULL);
}

// ------------------------------------------------------------------------------------------------------------- power source
IOService *copyService(const char *name)
{
  OSDictionary *match = IOService::serviceMatching(name);
  IOService *service = match ? IOService::copyMatchingService(match) : nullptr;
  OSSafeReleaseNULL(match);
  return service;
}

PowerSource readPowerSource(int32_t *soc = nullptr)
{
  if (soc) *soc = -1;
  IOService *ps = copyService("IOPMPowerSource");
  if (!ps) return PowerSource::unknown;
  PowerSource r = PowerSource::unknown;
  if (OSBoolean *b = OSDynamicCast(OSBoolean, ps->getProperty("ExternalConnected"))) r = b->isTrue() ? PowerSource::external : PowerSource::battery;
  // the battery percentage (PianoPower's AppleSmartBattery: CurrentCapacity in %, MaxCapacity 100) for the full table's limit
  if (soc)
    if (OSNumber *n = OSDynamicCast(OSNumber, ps->getProperty("CurrentCapacity")))
      if (n->unsigned32BitValue() <= 100) *soc = int32_t(n->unsigned32BitValue());
  ps->release();
  return r;
}

uint32_t gAndroidRequested;
struct sysctl_oid gAndroidOid;

int sysctlAndroidCpu(struct sysctl_oid *oid, void *, int, struct sysctl_req *req)
{
  if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
  if (req->newptr && req->newlen != sizeof(int)) return EINVAL;
  int value = int(__atomic_load_n(&gAndroidRequested, __ATOMIC_ACQUIRE));
  const int status = sysctl_handle_int(oid, &value, 0, req);
  if (status || !req->newptr) return status;
  if (!androidCpuRequestValid(value)) return EINVAL;
  __atomic_store_n(&gAndroidRequested, uint32_t(value), __ATOMIC_RELEASE);
  return 0;
}

void registerAndroidCpu()
{
  gAndroidOid.oid_parent = &sysctl__machdep_children;
  gAndroidOid.oid_number = OID_AUTO;
  gAndroidOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gAndroidOid.oid_name = "piano_cpufreq_android";
  gAndroidOid.oid_handler = sysctlAndroidCpu;
  gAndroidOid.oid_fmt = "I";
  gAndroidOid.oid_descr = "Root-only opt-in CPU ceiling step on external power";
  gAndroidOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gAndroidOid);
}

// ------------------------------------------------------------------------------------------------------------- perf (Perf.h)
// machdep.piano_cpufreq_perf (0 the policy before 2026-11-22, 1 the stock-level one), machdep.piano_cpufreq_perf_tick_ms (the load
// decision period in mode 1), machdep.piano_cpufreq_state (read only). The writes are root-only; the thread reads them each poll.
uint32_t gPerf, gPerfTickMs = 20;
volatile uint32_t gProfileNow;   // PianoPolicy's profile mode as last taken (machdep.piano_cpufreq_state profile=)
struct PerfDomain {
  uint32_t count;
  uint32_t khz[scmi::kMaxLevels];
  uint32_t set, ceilKhz, perfKhz, bursts;   // written by the thread only; the sysctl's reads may be one update old
  int32_t step;
  uint32_t limitKhz;   // the thermal limit's level (energy-e23: a ceiling under a mode's cap is told apart from the cap)
  int32_t hotDeci;     // the cluster's hottest cpu zone at the last tick (0.1 C; -9999 no valid reading)
  // the full table (Perf.h fullopp): the scene's step and cap, the sweep pin, the firmware's level (LEVEL_GET) and the times it
  // differed from ours, the WALT-like demand of the last decision (kHz-equivalent)
  int32_t sceneStep;
  uint32_t sceneKhz, sweepKhz, fwKhz, fwChecks, fwMismatch, demandKhz;
  uint64_t sinceNs, burstMs;
  uint64_t totalNs[scmi::kMaxLevels];
};
PerfDomain gPd[kDomains];
// the boosts' shared state (written by the launch listener, the input platform function and the thread; section below)
uint32_t gBoost = 1;
uint64_t gLaunchNs, gInputNs;
uint32_t gLaunches, gInputs, gInputBy[5], gLaunchIgnored;
char gLastApp[40];
uint64_t gBoostMs[3];               // ms the CPU spent boosted, by perf::BoostKind
uint32_t gBoostStarts[3];
uint32_t gGpuCalls, gGpuTaken, gGpuState;   // gGpuState: 0 not asked yet, 1 taken, 2 unsupported (no more calls), 3 no PianoA830
bool gAboveRefused[kDomains];   // a level above sustained was refused: mode 2's burst stays at sustained for the boot
int32_t gSkinMilli;
uint32_t gSkinValid, gSkinUsed, gPerfReady;
struct sysctl_oid gPerfOid, gPerfTickOid, gPerfStateOid;
// the full table (Perf.h fullopp): machdep.piano_cpufreq_fullopp / _walt (0/1, root writes), the test-only sweep pins in MHz per
// cluster (machdep.piano_cpufreq_sweep0/1, 0 = off) and the clock request (machdep.piano_cpufreq_measure: 1 = measure each core's
// clock at the next poll; reads the number of measurements so far); the battery percentage the thread read last (-1 none), the
// limit's level; the last measurement (MHz per cpu, 0 off) and its uptime in ms
uint32_t gFullopp, gWalt, gSweepMhz[kDomains], gMeasureReq, gMeasures;
int32_t gSoc = -1;
uint32_t gSocLevel, gClkMhz[kMaxCpus];
bool gClkOn[kMaxCpus];
uint64_t gClkAtMs;
struct sysctl_oid gFulloppOid, gWaltOid, gSweepOid[kDomains], gMeasureOid;
void (*gKick)(void);                // the thread's wake-up (set in start)

int sysctlPerfInt(struct sysctl_oid *oid, void *, int which, struct sysctl_req *req)
{
  if (req->newptr && !kauth_cred_issuser(kauth_cred_get())) return EPERM;
  if (req->newptr && req->newlen != sizeof(int)) return EINVAL;
  uint32_t *slot = which == 1 ? &gPerfTickMs : which == 2 ? &gFullopp : which == 3 ? &gWalt : which == 4 ? &gSweepMhz[0]
                 : which == 5 ? &gSweepMhz[1] : which == 6 ? &gMeasures : &gPerf;
  int value = int(__atomic_load_n(slot, __ATOMIC_ACQUIRE));
  const int status = sysctl_handle_int(oid, &value, 0, req);
  if (status || !req->newptr) return status;
  const bool bad = which == 1 ? (value < 20 || value > 1000)
                 : which == 2 || which == 3 ? (value < 0 || value > 1)
                 : which == 4 || which == 5 ? (value < 0 || value > 5000)
                 : which == 6 ? value != 1
                 : (value < 0 || value > 2);
  if (bad) return EINVAL;
  if (which == 6) {   // a request, not a value: the thread measures at its next poll and counts gMeasures
    __atomic_store_n(&gMeasureReq, 1u, __ATOMIC_RELEASE);
    if (gKick) gKick();
    return 0;
  }
  __atomic_store_n(slot, uint32_t(value), __ATOMIC_RELEASE);
  return 0;
}

int sysctlPerfState(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  if (req->newptr) return EPERM;
  char buf[2400];
  const uint64_t now = nowNs();
  int at = snprintf(buf, sizeof buf, "perf=%u profile=%u above_refused=%u/%u tick_ms=%u skin_mC=%d skin_valid=%u skin_inputs=%#x "
                    "uptime_ms=%llu boost=%u "
                    "launches=%u inputs=%u launch_boosts=%u input_boosts=%u launch_boost_ms=%llu input_boost_ms=%llu gpu_boost=%u/%u/%u",
                    __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE), __atomic_load_n(&gProfileNow, __ATOMIC_ACQUIRE), gAboveRefused[0] ? 1u : 0u,
                    gAboveRefused[1] ? 1u : 0u, __atomic_load_n(&gPerfTickMs, __ATOMIC_ACQUIRE), gSkinMilli,
                    gSkinValid, gSkinUsed, (unsigned long long)(now / 1000000ull), __atomic_load_n(&gBoost, __ATOMIC_ACQUIRE),
                    __atomic_load_n(&gLaunches, __ATOMIC_RELAXED), __atomic_load_n(&gInputs, __ATOMIC_RELAXED), gBoostStarts[2],
                    gBoostStarts[1], (unsigned long long)gBoostMs[2], (unsigned long long)gBoostMs[1], gGpuCalls, gGpuTaken, gGpuState);
  for (unsigned d = 0; d < kDomains && __atomic_load_n(&gPerfReady, __ATOMIC_ACQUIRE); ++d) {
    const PerfDomain &pd = gPd[d];
    if (!pd.count) continue;
    const uint32_t set = pd.set < pd.count ? pd.set : 0;
    at = catf(buf, sizeof buf, at, " | c%u now_mhz=%u ceil_mhz=%u perfcap_mhz=%u step=%d bursts=%u burst_ms=%llu res_ms=", d,
              pd.khz[set] / 1000, pd.ceilKhz / 1000, pd.perfKhz / 1000, pd.step, pd.bursts, (unsigned long long)pd.burstMs);
    for (unsigned i = 0; i < pd.count; ++i) {
      uint64_t ns = pd.totalNs[i];
      if (i == set && now > pd.sinceNs) ns += now - pd.sinceNs;
      at = catf(buf, sizeof buf, at, "%s%u:%llu", i ? "," : "", pd.khz[i] / 1000, (unsigned long long)(ns / 1000000ull));
    }
    // appended after the residency list, so the parsers of the fields before it keep working
    at = catf(buf, sizeof buf, at, " limit_mhz=%u hot_dC=%d", pd.limitKhz / 1000, pd.hotDeci);
    if (gArgs.fullopp || gArgs.sweep)   // the full table's fields after the older ones (their parsers keep working)
      at = catf(buf, sizeof buf, at, " scene_step=%d scene_mhz=%u sweep_mhz=%u fw_mhz=%u fw_checks=%u fw_mismatch=%u demand_mhz=%u",
                pd.sceneStep, pd.sceneKhz / 1000, pd.sweepKhz / 1000, pd.fwKhz / 1000, pd.fwChecks, pd.fwMismatch, pd.demandKhz / 1000);
  }
  if (gArgs.fullopp || gArgs.sweep) {
    at = catf(buf, sizeof buf, at, " | fo fullopp=%u walt=%u soc=%d soc_level=%u measures=%u clk_ms=%llu clk=",
              __atomic_load_n(&gFullopp, __ATOMIC_ACQUIRE), __atomic_load_n(&gWalt, __ATOMIC_ACQUIRE), gSoc, gSocLevel,
              __atomic_load_n(&gMeasures, __ATOMIC_ACQUIRE), (unsigned long long)gClkAtMs);
    for (unsigned i = 0; i < gCpuCount; ++i) at = catf(buf, sizeof buf, at, "%s%u:%u", i ? "," : "", gCpus[i].id, gClkOn[i] ? gClkMhz[i] : 0u);
  }
  return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}

void registerPerf()
{
  auto fill = [](struct sysctl_oid &oid, const char *name, unsigned kind, int (*handler)(struct sysctl_oid *, void *, int, struct sysctl_req *),
                 int arg2, const char *fmt, const char *descr) {
    oid.oid_parent = &sysctl__machdep_children;
    oid.oid_number = OID_AUTO;
    oid.oid_kind = int(kind);
    oid.oid_name = name;
    oid.oid_handler = handler;
    oid.oid_arg2 = arg2;
    oid.oid_fmt = fmt;
    oid.oid_descr = descr;
    oid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&oid);
  };
  fill(gPerfOid, "piano_cpufreq_perf", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 0, "I",
       "CPU policy: 0 before 2026-11-22 (v13.1x caps), 1 stock-level (profile caps, skin steps, burst, fast tick), 2 beyond stock "
       "(no skin table, burst to 4320); root writes");
  fill(gPerfTickOid, "piano_cpufreq_perf_tick_ms", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 1, "I",
       "load decision period of piano_cpufreq_perf=1 (20..1000 ms); root writes");
  fill(gPerfStateOid, "piano_cpufreq_state", CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfState, 0, "A",
       "CPU policy state: mode, tick, skin, per cluster level/ceiling and residency per level since boot (ms)");
  fill(gFulloppOid, "piano_cpufreq_fullopp", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 2, "I",
       "CPU full table (Android's 16 + 16 levels, its scenes by Energy Mode, battery limit, 95/87 C rule) under perf policy >= 1; "
       "root writes 0/1");
  fill(gWaltOid, "piano_cpufreq_walt", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 3, "I",
       "CPU WALT-like demand (max of the latest window and the mean of 5, frequency-invariant) under perf policy >= 1; root writes 0/1");
  if (gArgs.sweep) {   // test only
    fill(gSweepOid[0], "piano_cpufreq_sweep0", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 4, "I",
         "TEST: cluster 0 pinned at the fastest level <= MHz (0 off), the thermal limit on top; root writes");
    fill(gSweepOid[1], "piano_cpufreq_sweep1", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 5, "I",
         "TEST: cluster 1 pinned at the fastest level <= MHz (0 off), the thermal limit on top; root writes");
    fill(gMeasureOid, "piano_cpufreq_measure", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2, sysctlPerfInt, 6, "I",
         "TEST: write 1 = each core's clock measured at the next poll (piano_cpufreq_state clk=); reads the count");
  }
}

uint32_t curTickMs() { return __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE) ? __atomic_load_n(&gPerfTickMs, __ATOMIC_ACQUIRE) : gArgs.tickMs; }

// ------------------------------------------------------------------------------------------------------------- boosts (Perf.h)
// The launch boost's event: an exec of an app bundle's executable (a KAUTH_SCOPE_FILEOP listener); the input boost's: a HID report
// of PianoTouch, PianoKeyboard, PianoDwc3 or PianoInput (PianoPerfInput.h: callPlatformFunction("piano-perf-input", source) on this
// service). Both only stamp a time and wake the thread when no boost of that kind holds; the thread applies them.
// machdep.piano_cpufreq_boost (root, 0/1; boot default piano-cpufreq-boosts) switches them for a same-boot A/B.
kauth_listener_t gLaunchListener;
struct sysctl_oid gBoostOid;

int sysctlBoost(struct sysctl_oid *oid, void *, int, struct sysctl_req *req)
{
  if (req->newptr && !kauth_cred_issuser(kauth_cred_get())) return EPERM;
  if (req->newptr && req->newlen != sizeof(int)) return EINVAL;
  int value = int(__atomic_load_n(&gBoost, __ATOMIC_ACQUIRE));
  const int status = sysctl_handle_int(oid, &value, 0, req);
  if (status || !req->newptr) return status;
  if (value != 0 && value != 1) return EINVAL;
  __atomic_store_n(&gBoost, uint32_t(value), __ATOMIC_RELEASE);
  return 0;
}

void perfInputEvent(uint32_t source)
{
  const uint64_t now = nowNs();
  const uint64_t before = __atomic_exchange_n(&gInputNs, now, __ATOMIC_ACQ_REL);
  __atomic_fetch_add(&gInputs, 1u, __ATOMIC_RELAXED);
  __atomic_fetch_add(&gInputBy[source < 5 ? source : 0], 1u, __ATOMIC_RELAXED);
  if ((!before || now - before > uint64_t(gArgs.inputMs) * 1000000ull) && gKick) gKick();
}

int launchListener(kauth_cred_t, void *, kauth_action_t action, uintptr_t, uintptr_t arg1, uintptr_t, uintptr_t)
{
  if (action != KAUTH_FILEOP_EXEC || !arg1) return KAUTH_RESULT_DEFER;
  const char *path = reinterpret_cast<const char *>(arg1);
  if (!perf::isAppLaunchPath(path)) return KAUTH_RESULT_DEFER;
  char name[sizeof gLastApp];
  perf::appName(path, name, sizeof name);
  memcpy(gLastApp, name, sizeof gLastApp);   // for the log only; a racing writer can only mix two names
  __atomic_store_n(&gLaunchNs, nowNs(), __ATOMIC_RELEASE);
  __atomic_fetch_add(&gLaunches, 1u, __ATOMIC_RELAXED);
  if (gKick) gKick();
  return KAUTH_RESULT_DEFER;
}

// PianoA830's boost hook (gpu/boost342 PianoGpuBoost.h; CPU-PERF.md section 7): callPlatformFunction("piano-gpu-boost", param1 =
// a floor in MHz (> 16; 0 = PianoA830's own input boost, 1..16 a GMU level), param2 = the hold in ms, param3 = our reason (ignored
// there), param4 = a cap in MHz for the hold (gpu/fullopp1: Android's launch cap; 0 none)) on its service; requests combine (the highest floor until the latest end), every ceiling wins; kIOReturnUnsupported
// (PianoA830 without piano-gpu-boost=1, or a PianoA830 before boost342) ends the calls for this boot.
const OSSymbol *gGpuBoostSym;
void gpuBoost(uint32_t mhz, uint32_t ms, uint32_t reason, uint32_t capMhz = 0)
{
  if (gGpuState == 2) return;
  if (!gGpuBoostSym) gGpuBoostSym = OSSymbol::withCStringNoCopy("piano-gpu-boost");
  OSDictionary *match = IOService::serviceMatching("PianoA830");
  IOService *gpu = match ? IOService::copyMatchingService(match) : nullptr;
  OSSafeReleaseNULL(match);
  ++gGpuCalls;
  if (!gpu || !gGpuBoostSym) {
    if (gGpuState != 3) note("boost: piano-gpu-boost %u MHz %u ms: no PianoA830 service (the GPU is not boosted)", mhz, ms);
    gGpuState = 3;
    OSSafeReleaseNULL(gpu);
    return;
  }
  const IOReturn r = gpu->callPlatformFunction(gGpuBoostSym, false, reinterpret_cast<void *>(uintptr_t(mhz)),
                                               reinterpret_cast<void *>(uintptr_t(ms)), reinterpret_cast<void *>(uintptr_t(reason)),
                                               reinterpret_cast<void *>(uintptr_t(capMhz)));
  gpu->release();
  if (r == kIOReturnSuccess) {
    if (gGpuState != 1 || gGpuTaken < 3) note("boost: piano-gpu-boost %u MHz for %u ms (reason %u): taken by PianoA830", mhz, ms, reason);
    gGpuState = 1;
    ++gGpuTaken;
  } else {
    note("boost: piano-gpu-boost %u MHz for %u ms (reason %u): PianoA830 answered %#x: %s", mhz, ms, reason, r,
         r == kIOReturnUnsupported ? "not implemented, no more GPU boost calls this boot" : "refused");
    if (r == kIOReturnUnsupported) gGpuState = 2;
  }
}

void registerBoost()
{
  gBoostOid.oid_parent = &sysctl__machdep_children;
  gBoostOid.oid_number = OID_AUTO;
  gBoostOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
  gBoostOid.oid_name = "piano_cpufreq_boost";
  gBoostOid.oid_handler = sysctlBoost;
  gBoostOid.oid_fmt = "I";
  gBoostOid.oid_descr = "the launch and input boosts of piano_cpufreq_perf >= 1: 0 off, 1 on; root writes";
  gBoostOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&gBoostOid);
}

// Probe an absent publisher at most once per second and drop a detached service.
void refreshService(IOService *&service, const char *name, uint64_t now, uint64_t &next)
{
  if (service && service->isInactive()) OSSafeReleaseNULL(service);
  if (!service && now >= next) {
    service = copyService(name);
    next = now + 1000000000ull;
  }
}

// PianoA830's reversible GPU ceiling (kHz, 0 clears). A missing service may still publish later.
IOReturn gpuCeiling(uint32_t khz)
{
  OSDictionary *match = IOService::serviceMatching("PianoA830");
  if (!match) return kIOReturnNoMemory;
  IOService *gpu = IOService::copyMatchingService(match);
  match->release();
  if (!gpu) return kIOReturnNotReady;
  const IOReturn r = gpu->callPlatformFunction("piano-gpu-ceiling", false, reinterpret_cast<void *>(uintptr_t(khz)), nullptr, nullptr,
                                               nullptr);
  gpu->release();
  return r;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- suspend hooks
// For the system-suspend driver (docs/power.md's contract when it lands; until then these two C functions, or
// callPlatformFunction("piano-thermal-suspend-prepare" / "-finish") on the registered PianoThermal service):
//   prepare  masks every sensor's lower-threshold interrupt on the four controllers and pulses their clears (Linux 08e750858038 on
//            piano: a lower threshold crossed while the device cools in suspend refires, wake-armed, and aborts or ends every
//            sleep); the upper ones too while PianoThermal does not service the uplow interrupt itself (today: nothing would clear
//            it); the critical interrupts stay as they are. The masks found are kept.
//   finish   puts the masks found back.
// They work whatever piano-thermal says (the controllers are mapped by the hooks themselves), and are the only TSENS writes of
// this kext. prepare returns the number of controllers masked (0..4), -1 when already prepared.
namespace {
Window gSuspendTm[kControllers];
uint32_t gSavedUpLowMask[kControllers];
bool gSuspendSaved[kControllers];
uint32_t gSuspendState;            // 0 idle, 1 prepared
constexpr bool kServicesUplow = false;   // this path: the uplow interrupt not serviced (piano-thermal-irq live: irq::hookSuspend*)
}

extern "C" int piano_thermal_suspend_prepare(void)
{
  uint32_t idle = 0;
  if (!__atomic_compare_exchange_n(&gSuspendState, &idle, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -1;
  const int uplow = irq::hookSuspendPrepare();   // piano-thermal-irq live: lower masked, upper kept armed (PianoThermalIrq.cpp)
  if (uplow >= 0) return uplow;
  int done = 0;
  char line[200];
  int at = 0;
  line[0] = 0;
  for (unsigned c = 0; c < kControllers; ++c) {
    gSuspendSaved[c] = false;
    if (!gSuspendTm[c].va && !mapWindow(gSuspendTm[c], kMap[c].tm, kTmBytes)) {
      gSuspendTm[c] = {};
      continue;
    }
    const uintptr_t va = gSuspendTm[c].va;
    const uint32_t found = rd32(va + kTmUpLowIntMask);
    const SuspendMask sm = suspendMask(found, kMap[c].count, !kServicesUplow);
    wr32(va + kTmUpLowIntMask, sm.mask);
    wr32(va + kTmUpLowIntClear, sm.clear);
    wr32(va + kTmUpLowIntClear, 0);
    gSavedUpLowMask[c] = found;
    gSuspendSaved[c] = true;
    ++done;
    at = catf(line, sizeof line, at, " tsens%u %#x->%#x", c, found, sm.mask);
  }
  __builtin_arm_dsb(0xf);
  note("suspend prepare: lower%s threshold interrupts masked on %d controllers:%s", kServicesUplow ? "" : " and upper", done, line);
  return done;
}

extern "C" void piano_thermal_suspend_finish(void)
{
  if (__atomic_load_n(&gSuspendState, __ATOMIC_ACQUIRE) != 1) return;
  if (irq::hookSuspendFinish()) {   // the masks are the uplow path's: nothing of this path's to restore
    __atomic_store_n(&gSuspendState, 0u, __ATOMIC_RELEASE);
    return;
  }
  int done = 0;
  for (unsigned c = 0; c < kControllers; ++c) {
    if (!gSuspendSaved[c] || !gSuspendTm[c].va) continue;
    wr32(gSuspendTm[c].va + kTmUpLowIntMask, gSavedUpLowMask[c]);
    gSuspendSaved[c] = false;
    ++done;
  }
  __builtin_arm_dsb(0xf);
  __atomic_store_n(&gSuspendState, 0u, __ATOMIC_RELEASE);
  note("suspend finish: the threshold interrupt masks found before suspend are back on %d controllers", done);
}

// ------------------------------------------------------------------------------------------------------------- the service
class PianoThermal : public IOService {
  OSDeclareDefaultStructors(PianoThermal)

  static void thread(void *self, wait_result_t);
  void run();
  void surveyControllers();
  void peekChannel();
  void report(const Monitor &m, bool clock);
  void publish(const Monitor &m);
  void protect(const PolicyEvent &e);
  void cpuStart();
  void cpuTick(const Monitor &m, bool slow, bool loadTick);
  void cpuApply(unsigned d, unsigned want, uint64_t now);
  void cpuReport(uint64_t spanNs);
  void cpuAndroidInputs(uint64_t now);
  void cpuAndroidReport(const AndroidCpuInput &in, const AndroidCpuSelection &selection, bool slow);
  void cpuAndroidSuspend();

  Monitor monitor_;
  Policy policy_;
  int32_t peakDeci_ = 0;
  unsigned peakC_ = 0, peakS_ = 0;
  bool peakValid_ = false, parked_ = false, gpuCapped_ = false, halting_ = false, criticalPendingLogged_ = false;
  uint64_t criticalLogNs_ = 0;
  // cpufreq
  scmi::Channel channel_;
  scmi::Protocol protocol_{};
  scmi::Domain domain_[kDomains];
  Governor governor_[kDomains];
  bool domainOk_[kDomains] = {};
  int boot_[kDomains] = {-1, -1};
  unsigned set_[kDomains] = {};          // the level last set (or found)
  uint64_t residencyNs_[kDomains][scmi::kMaxLevels] = {};
  uint64_t busySum_[kDomains] = {}, busyTicks_[kDomains] = {};
  uint32_t busyMax_[kDomains] = {}, changes_[kDomains] = {}, refusals_[kDomains] = {}, cuts_[kDomains] = {};
  uint64_t limitedPolls_[kDomains] = {};
  uint64_t levelSinceNs_[kDomains] = {};
  PowerSource power_ = PowerSource::unknown;
  uint64_t powerAtNs_ = 0;
  bool cpuOk_ = false;
  IOService *androidPolicy_ = nullptr, *androidPower_ = nullptr;
  uint64_t androidPolicyLookup_ = 0, androidPowerLookup_ = 0, androidInputsAt_ = 0;
  int androidLpm_ = -1;
  bool androidActive_ = false, androidDomains_ = false;
  int androidPhase_ = -1;
  uint32_t androidRequestKey_[kDomains] = {};
  int32_t androidRequestStatus_[kDomains] = {};
  bool androidRequestedLevel_[kDomains] = {};
  // piano-cpufreq-boost: its end (uptime ns, 0 off) and, per domain, whether it has ended / its level was reached (one line each)
  uint64_t boostEndNs_ = 0;
  // the boost's own thermal back-off (Governor.h BoostBackoff): its state, the heat end (uptime ns, 0 = not by heat; the
  // per-domain boost-over path sees it as the boost's end), and the boost's sticky cap level per domain (only steps down)
  BoostBackoff boostBackoff_{};
  uint64_t boostHotEndNs_ = 0;
  unsigned boostCapLevel_[kDomains] = {};
  bool boostEnded_[kDomains] = {}, boostNoted_[kDomains] = {};
  // PianoPolicy's profile (any thread writes it; the governor reads it each tick) and what the 30 s report last named
  volatile uint32_t profile_ = 0;
  // the stock-level policy (Perf.h): per cluster the skin step and the burst; the last skin reading; the mode last applied
  perf::SkinCap skinCap_[kDomains];
  perf::Burst burst_[kDomains];
  perf::Skin skin_{};
  int32_t skinIn_[perf::kSkinInputs] = {};
  uint32_t skinInMask_ = 0, perfSeen_ = 9, perfChanges_ = 0;
  void cpuPerfReport();
  // the full table (Perf.h fullopp): the scene each cluster's skin step belongs to (a change restarts the step), the battery limit,
  // the firmware level checks, what the last decision saw (for the change lines)
  perf::Scene sceneSeen_[kDomains] = {perf::Scene::normal, perf::Scene::normal};
  perf::SocLimit socLimit_;
  uint64_t fwAtNs_[kDomains] = {};
  uint32_t fullSeen_ = 9, waltSeen_ = 9, sweepSeen_[kDomains] = {};
  // boosts: per cluster whether the last decision applied one (the silicon gate's hysteresis), the kind in force, the launch
  // event last handed to the GPU, the time of the last poll (boosted ms), the GPU boost's end (for a cancel under Power Saver)
  bool boostOn_[kDomains] = {};
  // the readings of each cluster's cpu zones per report window: how many and how long at or above 90 / 95 / 105 C, the highest, and
  // when the last reading at the gate's opening temperature came (the gate opens only after piano-cpufreq-gate-quiet-ms without one)
  uint32_t hotN_[kDomains][3] = {};
  uint64_t hotNs_[kDomains][3] = {}, hotSeenNs_[kDomains] = {}, hotAccNs_ = 0;
  int32_t hotMax_[kDomains] = {-1000, -1000};
  void hotAccount(uint64_t now);
  perf::BoostKind boostKind_ = perf::BoostKind::none;
  uint64_t gpuLaunchSeen_ = 0, gpuInputAt_ = 0, boostTickNs_ = 0, boostNs_[3] = {};
 public:
  void kick();
 private:
  // system sleep: the thread parks between kOff and kOn (SleepPlan says what each does)
  IOLock *sleepLock_ = nullptr;
  SleepPlan sleep_;
  bool sleepWanted_ = false, threadParked_ = false, threadRunning_ = false;
  bool park();
  void pollWait(uint32_t ms);

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3,
                                void *param4) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoThermal, IOService)

namespace {
PianoThermal *gSelfThermal;
void kickThermal()
{
  if (gSelfThermal) gSelfThermal->kick();
}
}

// An input or launch event: end the thread's wait now (a spurious wake-up only runs one more poll).
void PianoThermal::kick()
{
  if (!sleepLock_) return;
  IOLockLock(sleepLock_);
  IOLockWakeup(sleepLock_, &sleepWanted_, false);
  IOLockUnlock(sleepLock_);
}

// "piano-thermal-suspend-prepare" (param1: int *, the result, may be null) and "piano-thermal-suspend-finish": the suspend hooks
// above for a caller that does not link against this kext.
IOReturn PianoThermal::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                            void *param3, void *param4)
{
  // PianoPerfInput.h: an input kext's HID report (param1 = the source); lock-free, any thread context but interrupts
  if (functionName && functionName->isEqualTo("piano-perf-input")) {
    perfInputEvent(uint32_t(uintptr_t(param1)));
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-thermal-suspend-prepare")) {
    const int r = piano_thermal_suspend_prepare();
    if (param1) *static_cast<int *>(param1) = r;
    return r < 0 ? kIOReturnBusy : kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-thermal-suspend-finish")) {
    piano_thermal_suspend_finish();
    return kIOReturnSuccess;
  }
  if (functionName && functionName->isEqualTo("piano-thermal-board-temps"))   // B6: param1 = struct piano_board_temps *
    return adc::hookBoardTemps(param1) ? kIOReturnSuccess : kIOReturnBadArgument;
  if (functionName && functionName->isEqualTo("piano-thermal-gpu-temps"))
    return adc::hookGpuTemps(param1) ? kIOReturnSuccess : kIOReturnBadArgument;
  if (functionName && functionName->isEqualTo("piano-thermal-skin-temps"))   // gpu/fullopp1: param1 = struct piano_skin_temps *
    return adc::hookSkinTemps(param1) ? kIOReturnSuccess : kIOReturnBadArgument;
  // docs/policy.md 5.2: param1 = the mode (0 by the power source, 1 Power Saver, 2 Balanced). Taken by the governor (mode 3) at its
  // next tick and by the fixed level (mode 2, which has no power caps: ignored there); without CPU frequency control unsupported.
  if (functionName && functionName->isEqualTo("piano-thermal-profile")) {
    const uintptr_t mode = uintptr_t(param1);
    if (!profileValid(mode)) return kIOReturnBadArgument;
    if (gArgs.cpufreq < 2) return kIOReturnUnsupported;
    const uint32_t before = __atomic_exchange_n(&profile_, uint32_t(mode), __ATOMIC_ACQ_REL);
    __atomic_store_n(&gProfileNow, uint32_t(mode), __ATOMIC_RELEASE);
    if (before != mode) {
      setProperty("piano-thermal-profile", uint64_t(mode), 32);
      note("[%us] cpufreq: profile %s (PianoPolicy)%s", upS(), profileName(Profile(mode)),
           gArgs.cpufreq == 2 ? ": the fixed level (piano-cpufreq=2) has no power caps, ignored"
           : Profile(mode) == Profile::performance && !__atomic_load_n(&gPerf, __ATOMIC_ACQUIRE)
               ? ": High Power's top levels need machdep.piano_cpufreq_perf >= 1 (policy 0 keeps the measured source's caps)"
           : Profile(mode) == Profile::performance
               ? ": the top levels as the cap, no skin table, the thermal limit on top" : "");
    }
    return kIOReturnSuccess;
  }
  return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

// The thread's wait between polls; a system sleep's park request ends it early.
void PianoThermal::pollWait(uint32_t ms)
{
  if (irq::hookSleep(ms, &sleepWanted_)) return;   // piano-thermal-irq live: an uplow interrupt or the park request ends it
  IOLockLock(sleepLock_);
  if (!sleepWanted_) {
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    IOLockSleepDeadline(sleepLock_, &sleepWanted_, deadline, THREAD_UNINT);
  }
  IOLockUnlock(sleepLock_);
}

// At the top of each poll (no SCMI request or TSENS read in flight): parked here from kOff to kOn. True after a park.
bool PianoThermal::park()
{
  IOLockLock(sleepLock_);
  if (sleepWanted_ && androidActive_) {
    IOLockUnlock(sleepLock_);
    cpuAndroidSuspend();
    IOLockLock(sleepLock_);
  }
  const bool parking = sleepWanted_;
  if (parking) {
    threadParked_ = true;
    IOLockWakeup(sleepLock_, &threadParked_, false);
    while (sleepWanted_) IOLockSleep(sleepLock_, &sleepWanted_, THREAD_UNINT);
    threadParked_ = false;
  }
  IOLockUnlock(sleepLock_);
  return parking;
}

// docs/power.md 4: off = the thread parked (no SCMI request: the clusters stay at their level; no TSENS access) and, with
// piano-thermal-sleep-mask=1, the threshold interrupts masked (the suspend hooks; Ubuntu's 08e750858038); on = the masks back, the
// thread on (its first zone update at once). Synchronous: the park waits for the poll in progress (at most a few ms of SCMI).
IOReturn PianoThermal::setPowerState(unsigned long state, IOService *)
{
  if (!sleepLock_) return pianopower::kAckNow;
  IOLockLock(sleepLock_);
  const SleepSteps s = state == pianopower::kOff ? sleep_.off() : sleep_.on();
  if (s.park && threadRunning_) {
    sleepWanted_ = true;
    IOLockWakeup(sleepLock_, &sleepWanted_, false);
    irq::hookKick();   // and the uplow path's wait
    uint64_t deadline = 0;
    clock_interval_to_deadline(500, kMillisecondScale, &deadline);
    while (!threadParked_ && IOLockSleepDeadline(sleepLock_, &threadParked_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
  }
  const bool parked = threadParked_;
  IOLockUnlock(sleepLock_);
  if (s.park) {
    int masked = 0;
    if (s.mask) sleep_.maskResult(masked = piano_thermal_suspend_prepare());
    note("[%us] sleep: %s; %s", upS(), !threadRunning_ ? "no thread to park" : parked ? "the thread is parked (no SCMI, no TSENS)"
                                                                                       : "the thread did not park within 500 ms",
         !s.mask ? "threshold interrupts as they are (piano-thermal-sleep-mask=0)"
                 : masked < 0 ? "threshold interrupts already masked by another caller" : "threshold interrupts masked");
  }
  if (s.finish) piano_thermal_suspend_finish();
  if (s.unpark) {
    IOLockLock(sleepLock_);
    sleepWanted_ = false;
    IOLockWakeup(sleepLock_, &sleepWanted_, false);
    IOLockUnlock(sleepLock_);
  }
  return pianopower::kAckNow;
}

IOService *PianoThermal::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoThermal::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  gStartNs = nowNs();
  note("start: piano-thermal=%u piano-cpufreq=%u shutdown=%u poll %u ms, log every %u s, warm %u C, critical margin %u C, clock %u, "
       "srot %u, delay %u s",
       gArgs.level, gArgs.cpufreq, gArgs.shutdown, gArgs.pollMs, gArgs.logS, gArgs.warmC, gArgs.marginC, gArgs.clock, gArgs.srot,
       gArgs.delay);
  note("level %u: %s%s", gArgs.level,
       gArgs.level >= 2 ? "readout + protection (GPU park at the stock passive trip, alarm at critical - margin)"
                        : "readout only: loads, nothing written",
       gArgs.cpufreq ? "; piano-cpufreq: SCMI messages to the CPUCP (shared memory + doorbell)" : "");
  PolicyConfig pc;
  pc.warmDeci = int32_t(gArgs.warmC) * 10;
  pc.criticalMarginDeci = int32_t(gArgs.marginC) * 10;
  policy_ = Policy(pc);
  setProperty("piano-thermal", gArgs.level, 32);
  setProperty("piano-cpufreq", gArgs.cpufreq, 32);
  if (gArgs.cpufreq >= 2) {
    setProperty("piano-thermal-profile", 0ull, 32);
    setProperty("piano-thermal-profiles", uint64_t(kProfileMax), 32);   // mode 3 (High Power) is taken
  }
  sleepLock_ = IOLockAlloc();
  // piano-thermal-irq=1 uses the threshold interrupts, so a sleep masks them (the hooks: lower only while it is live, as Linux)
  sleep_ = SleepPlan(gArgs.sleepMask != 0 || irq::hookLevel() != 0);
  registerService();
  gSelfThermal = this;
  gKick = &kickThermal;
  if (gArgs.cpufreq == 3 && gArgs.launchWatch) {
    gLaunchListener = kauth_listen_scope(KAUTH_SCOPE_FILEOP, launchListener, nullptr);
    note("boost: launch events: %s", gLaunchListener ? "a KAUTH_SCOPE_FILEOP listener (exec of an app bundle's executable)"
                                                     : "kauth_listen_scope FAILED: no launch boost");
  }
  if (gArgs.androidArmed) registerAndroidCpu();
  else if (gArgs.android)
    note("PianoCpuAndroidV1 refused arm: mode=%u configured_cap_mhz=%u,%u explicit=%u,%u",
         gArgs.cpufreq, gArgs.cap[0], gArgs.cap[1], gArgs.capGiven[0], gArgs.capGiven[1]);
  if (gArgs.clock == 1) gDisplay.start(nullptr, nullptr);   // the clock line waits while the display is off
  adc::hookStart(this, adc::hookLevel(), gArgs.logS, gArgs.delay);   // B6 (PianoThermalAdc.cpp): nothing without piano-thermal-adc
  thread_t t = nullptr;
  if (!sleepLock_ || kernel_thread_start(&PianoThermal::thread, this, &t) != KERN_SUCCESS) {
    note("FAILED: no thread");
    return true;
  }
  thread_deallocate(t);
  threadRunning_ = true;
  note("system sleep: in the power tree (%#x): the thread parks while suspended%s", pianopower::join(this),
       gArgs.sleepMask ? ", the threshold interrupts masked (piano-thermal-sleep-mask=1)"
       : irq::hookLevel() ? ", the threshold interrupts masked (piano-thermal-irq=1: the lower ones while it is live)" : "");
  return true;
}

void PianoThermal::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-thermal");
  static_cast<PianoThermal *>(self)->run();
}

void PianoThermal::surveyControllers()
{
  for (unsigned c = 0; c < kControllers; ++c) {
    const Controller &k = kMap[c];
    if (!gTm[c].va) {
      note("tsens%u %s @%#llx: NOT MAPPED", c, k.name, (unsigned long long)k.tm);
      continue;
    }
    char srot[160] = "SROT skipped (piano-thermal-srot=0)";
    if (gSrot[c].va) {
      const uint32_t ver = srotRead(c, kSrotVersion), ctrl = srotRead(c, kSrotCtrl);
      const Version v = decodeVersion(ver);
      const Ctrl k2 = decodeCtrl(ctrl);
      snprintf(srot, sizeof srot, "version %u.%u.%u (%#x), ctrl %#x: %s, %u sensors enabled (DT %u%s), result %s", v.major, v.minor,
               v.step, ver, ctrl, k2.enabled ? "enabled" : "NOT ENABLED", popcount(k2.sensorMask), k.count,
               popcount(k2.sensorMask) == k.count ? ", matches" : ", DIFFERS", k2.temperatureFormat ? "temperature" : "ADC CODES");
    } else if (gArgs.srot) {
      snprintf(srot, sizeof srot, "SROT @%#llx not mapped", (unsigned long long)k.srot);
    }
    int32_t critLo = 0, critHi = 0, upLo = 0, upHi = 0;
    for (unsigned s = 0; s < k.count; ++s) {
      const Thresholds th = decodeThresholds(tsensRead(nullptr, c, kTmUpLowThreshold + 4 * s), tsensRead(nullptr, c, kTmCriticalThreshold + 4 * s));
      if (!s || th.criticalDeci < critLo) critLo = th.criticalDeci;
      if (!s || th.criticalDeci > critHi) critHi = th.criticalDeci;
      if (!s || th.upperDeci < upLo) upLo = th.upperDeci;
      if (!s || th.upperDeci > upHi) upHi = th.upperDeci;
    }
    note("tsens%u %s @%#llx: %s", c, k.name, (unsigned long long)k.tm, srot);
    note("tsens%u %s: TM int-en %#x, up/low status %#x, critical status %#x, trdy %#x; hardware thresholds: critical %s..%s C, upper "
         "%s..%s C over %u sensors",
         c, k.name, tsensRead(nullptr, c, kTmIntEn), tsensRead(nullptr, c, kTmUpLowIntStatus), tsensRead(nullptr, c, kTmCriticalIntStatus),
         tsensRead(nullptr, c, kTmTrdy), Deci(critLo).s, Deci(critHi).s, Deci(upLo).s, Deci(upHi).s, k.count);
  }
}

void PianoThermal::peekChannel()
{
  if (!gShmem.va) {
    note("scmi: tx channel @%#llx not mapped", (unsigned long long)scmi::kShmemTx);
    return;
  }
  const uint32_t st = shmRead(nullptr, scmi::kChanStatus), h = shmRead(nullptr, scmi::kHeader);
  const scmi::Header hd = scmi::decodeHeader(h);
  note("scmi: tx channel @%#llx (read only): status %#x (%s%s), flags %#x, length %u, last header %#x (protocol %#x message %#x token %u)",
       (unsigned long long)scmi::kShmemTx, st, (st & scmi::kStatusFree) ? "free" : "BUSY", (st & scmi::kStatusError) ? ", error" : "",
       shmRead(nullptr, scmi::kChanFlags), shmRead(nullptr, scmi::kLength), h, hd.protocol, hd.message, hd.token);
}

void PianoThermal::report(const Monitor &m, bool clock)
{
  char line[480];
  int at = snprintf(line, sizeof line, "[%us]", upS());
  for (unsigned c = 0; c < kControllers && at < int(sizeof line); ++c) {
    int32_t d;
    unsigned s;
    if (m.hottest(c, d, s))
      at = catf(line, sizeof line, at, "%s %s %s C (s%u %s)", c ? "," : "", kMap[c].name, Deci(d).s, s, kMap[c].sensors[s].name);
    else
      at = catf(line, sizeof line, at, "%s %s no reading", c ? "," : "", kMap[c].name);
  }
  if (at < int(sizeof line)) {
    if (peakValid_)
      at = catf(line, sizeof line, at, "; peak %s C (%s s%u)", Deci(peakDeci_).s, kMap[peakC_].name, peakS_);
    at = catf(line, sizeof line, at, "; state %s%s", stateName(policy_.state()), policy_.unknown() ? " UNKNOWN" : "");
  }
  uint32_t flags = 0;
  const int32_t gpu = adreno830_thermal_state(&flags);
  if (at < int(sizeof line) && (flags & kA830Valid))
    at = catf(line, sizeof line, at, "; a830 %s C%s%s%s", Deci(gpu).s, (flags & kA830Limiting) ? " limiting" : "",
             (flags & kA830Emergency) ? " EMERGENCY" : "", parked_ ? " parked" : "");
  note("%s", line);
  note("[%us] reads: %llu polls, %llu loads, %llu without VALID, %llu implausible, %llu stale stand-ins", upS(),
       (unsigned long long)m.polls(), (unsigned long long)m.reads(), (unsigned long long)m.invalidReads(),
       (unsigned long long)m.implausible(), (unsigned long long)m.staleUses());
  // Not while the display is off (2026-11-12): binding a thread onto every running core wakes each idle one for a diagnostic line.
  if (clock && gArgs.clock && (gArgs.clock >= 2 || !gDisplay.dark())) {
    uint32_t mhz[kMaxCpus];
    bool on[kMaxCpus];
    measureClocks(mhz, on);
    at = snprintf(line, sizeof line, "[%us] clock (dependent adds, MHz):", upS());
    for (unsigned i = 0; i < gCpuCount && at < int(sizeof line); ++i) {
      if (on[i]) at = catf(line, sizeof line, at, " cpu%u/%u %u", gCpus[i].id, gCpus[i].domain, mhz[i]);
      else at = catf(line, sizeof line, at, " cpu%u/%u off", gCpus[i].id, gCpus[i].domain);
    }
    note("%s", line);
  }
}

void PianoThermal::publish(const Monitor &m)
{
  OSDictionary *t = OSDictionary::withCapacity(8);
  if (!t) return;
  for (unsigned c = 0; c < kControllers; ++c) {
    int32_t d;
    unsigned s;
    if (!m.hottest(c, d, s)) continue;
    if (OSNumber *n = OSNumber::withNumber(uint64_t(int64_t(d)), 32)) {
      t->setObject(kMap[c].name, n);
      n->release();
    }
  }
  setProperty("TemperaturesDeciC", t);
  t->release();
  int16_t all[kControllers][kMaxSensors];
  for (unsigned c = 0; c < kControllers; ++c)
    for (unsigned s = 0; s < kMaxSensors; ++s) all[c][s] = m.usable(c, s) ? int16_t(m.deci(c, s)) : int16_t(-32768);
  if (OSData *data = OSData::withBytes(all, sizeof all)) {
    setProperty("SensorsDeciC", data);   // [controller][sensor] int16, -32768 = no reading
    data->release();
  }
  setProperty("ThermalState", stateName(policy_.state()));
  if (peakValid_) setProperty("PeakDeciC", uint64_t(int64_t(peakDeci_)), 32);
}

void PianoThermal::protect(const PolicyEvent &e)
{
  const Sensor &sn = kMap[e.controller].sensors[e.sensor];
  if (e.unknownAfter != e.unknownBefore) {
    if (e.unknownAfter) note("[%us] NO READING: no TSENS sensor gave a usable reading for %u s; protection is blind", upS(),
                             policy_.config().unknownMs / 1000);
    else note("[%us] readings are back", upS());
  }
  if (e.before != e.after)
    note("[%us] state %s -> %s: %s s%u %s %s C", upS(), stateName(e.before), stateName(e.after), kMap[e.controller].name, e.sensor,
         sn.name, Deci(e.deci).s);
  if (gArgs.level < 2) return;
  // The GPU: at passive a reversible ceiling (PianoA830's "piano-gpu-ceiling", once it has one: docs/drivers/thermal.md section
  // 10), else the boot-long park; at critical always the park.
  if (e.gpuLimited() && !parked_ && !gpuCapped_) {
    const IOReturn result = gpuCeiling(gArgs.gpuMhz * 1000u);
    if (result == kIOReturnSuccess) {
      gpuCapped_ = true;
      note("[%us] GPU capped at %u MHz (%s)", upS(), gArgs.gpuMhz, e.incomplete ? "incomplete TSENS poll" : "passive trip");
    } else if (result == kIOReturnUnsupported) {
      parked_ = true;
      adreno830_dcvs_park();
      note("[%us] GPU parked at its lowest level (PianoA830 has no reversible ceiling)", upS());
    }
  }
  if (e.after == State::critical && !parked_) {
    parked_ = true;
    adreno830_dcvs_park();
    note("[%us] CRITICAL: GPU parked at its lowest level for the rest of the boot (adreno830_dcvs_park)", upS());
  }
  if (!e.gpuLimited() && gpuCapped_ && !parked_ && gpuCeiling(0) == kIOReturnSuccess) {
    gpuCapped_ = false;
    note("[%us] GPU ceiling cleared (state %s)", upS(), stateName(e.after));
  }
  if (!e.criticalPending && e.after != State::critical) criticalPendingLogged_ = false;
  const uint64_t now = nowNs();
  if (e.after == State::critical && (e.before != State::critical || now - criticalLogNs_ >= 5000000000ull)) {
    criticalLogNs_ = now;   // at entry, then every 5 s while it lasts
    note("[%us] CRITICAL: %s %s C >= %s C (stock critical %s C - %u C margin)%s; %s", upS(), sn.name, Deci(e.deci).s,
         Deci(sn.criticalDeci - int32_t(gArgs.marginC) * 10).s, Deci(sn.criticalDeci).s, gArgs.marginC,
         gArgs.cpufreq >= 2 ? "; CPU clusters at their slowest level" : "",
         gArgs.shutdown ? "HALTING the system (piano-thermal-shutdown=1)" : "piano-thermal-shutdown=0: NOT shutting down (log only)");
    if (gArgs.shutdown && !halting_) {
      halting_ = true;
      IOSleep(200);   // let the console drain
      static char why[] = "PianoThermal: critical temperature";
      reboot_kernel(kRbHalt, why);
      note("[%us] reboot_kernel(RB_HALT) returned: the system did not halt", upS());
    }
  } else if (e.criticalPending && !criticalPendingLogged_) {
    criticalPendingLogged_ = true;
    note("[%us] critical threshold met (hottest %s s%u %s C): critical if it holds for %u ms", upS(), kMap[e.controller].name, e.sensor,
         Deci(e.deci).s, policy_.config().criticalHoldMs);
  }
}

// ------------------------------------------------------------------------------------------------------------- cpufreq
void PianoThermal::cpuStart()
{
  readTopology();
  if (!gShmem.va || !gMbox.va) {
    note("cpufreq: SCMI channel or doorbell not mapped (%#llx, %#llx); off", (unsigned long long)scmi::kShmemTx,
         (unsigned long long)scmi::kMboxTx);
    return;
  }
  channel_.io = {nullptr, shmRead, shmWrite, ring, scmiNowUs, scmiPause};
  int32_t r = scmi::queryProtocol(channel_, protocol_);
  if (r) {
    note("cpufreq: SCMI performance protocol: no answer (%d, %s); off for this boot", r, scmi::statusName(r));
    return;
  }
  note("cpufreq: SCMI performance protocol %u.%u, %u domains", protocol_.version >> 16, protocol_.version & 0xffff, protocol_.domains);
  const CpuMode mode = CpuMode(gArgs.cpufreq);
  for (unsigned d = 0; d < kDomains && d < protocol_.domains; ++d) {
    scmi::Domain &dom = domain_[d];
    r = scmi::describeDomain(channel_, protocol_.version, d, dom);
    if (r) {
      note("cpufreq: domain %u: no description (%d, %s)", d, r, scmi::statusName(r));
      continue;
    }
    char list[300];
    int at = 0;
    list[0] = 0;
    for (unsigned i = 0; i < dom.count && at < int(sizeof list) - 12; ++i)
      at = catf(list, sizeof list, at, " %u", dom.levels[i].khz / 1000);
    note("cpufreq: domain %u \"%s\": attributes %#x (%s, %s), rate limit %u us, sustained %u kHz (level %u -> %u MHz), %u levels (MHz):%s", d,
         dom.name, dom.attributes, dom.canSet ? "level set allowed" : "LEVEL SET NOT ALLOWED", dom.indexing ? "by index" : "by value",
         dom.rateLimitUs, dom.sustainedKhz, dom.sustainedLevel, dom.levels[dom.sustained].khz / 1000, dom.count, list);
    {   // the firmware's own figures per level (SCMI DESCRIBE_LEVELS: power cost, transition latency)
      char t[420];
      int tat = 0;
      t[0] = 0;
      for (unsigned i = 0; i < dom.count && tat < int(sizeof t) - 24; ++i)
        tat = catf(t, sizeof t, tat, " %u:%u:%u", dom.levels[i].khz / 1000, dom.levels[i].power, dom.levels[i].latencyUs);
      note("cpufreq: domain %u table (MHz:power cost:latency us):%s", d, t);
    }
    int idx = -1;
    r = scmi::levelGet(channel_, dom, d, idx);
    if (r || idx < 0) {
      note("cpufreq: domain %u: current level unknown (%d, %s)", d, r, scmi::statusName(r));
      continue;
    }
    boot_[d] = idx;
    set_[d] = unsigned(idx);
    note("cpufreq: domain %u: at boot %u MHz (level %d of %u%s)", d, dom.levels[idx].khz / 1000, idx + 1, dom.count,
         unsigned(idx) >= dom.sustained ? ", at or above sustained: the boot stages left it fast" : "");
    uint32_t khz[scmi::kMaxLevels];
    for (unsigned i = 0; i < dom.count; ++i) khz[i] = dom.levels[i].khz;
    GovernorConfig gc;
    gc.capKhz = mode == CpuMode::fixed || gArgs.capGiven[d] ? capKhz(gArgs.cap[d]) : 0;
    gc.batteryKhz = capKhz(gArgs.bat[d]);
    gc.externalKhz = capKhz(gArgs.ac[d]);
    gc.floorKhz = gArgs.minMhz[d] * 1000;
    gc.hispeedKhz = gArgs.hispeed[d] * 1000;
    gc.stepMs = gArgs.fastMs;
    governor_[d].init(dom.canSet ? mode : CpuMode::report, gc, khz, dom.count, dom.sustained, unsigned(idx));
    levelSinceNs_[d] = nowNs();
    gPd[d].count = dom.count < scmi::kMaxLevels ? dom.count : scmi::kMaxLevels;
    for (unsigned i = 0; i < gPd[d].count; ++i) gPd[d].khz[i] = dom.levels[i].khz;
    gPd[d].set = unsigned(idx);
    gPd[d].sinceNs = levelSinceNs_[d];
    gPd[d].step = -1;
    domainOk_[d] = true;
    if (mode >= CpuMode::fixed) {
      if (!dom.canSet) note("cpufreq: domain %u: the firmware does not allow LEVEL_SET; reporting only", d);
      else if (mode == CpuMode::fixed)
        note("cpufreq: domain %u: fixed at the fastest level <= %u MHz (and <= sustained): %u MHz; thermal limit: %s", d, gArgs.cap[d],
             dom.levels[governor_[d].ceiling(PowerSource::battery)].khz / 1000, tripsText());
      else
        note("cpufreq: domain %u: governor every %u ms, target 80 %% busy, hispeed %u MHz, floor %u MHz, caps: battery %u MHz, power %u "
             "MHz%s; thermal limit: %s",
             d, gArgs.tickMs, gArgs.hispeed[d], gArgs.minMhz[d], gArgs.bat[d], gArgs.ac[d], gc.capKhz ? ", fixed cap given" : "",
             tripsText());
    }
  }
  cpuOk_ = domainOk_[0] || domainOk_[1];
  if (cpuOk_ && mode == CpuMode::govern) {
    __atomic_store_n(&gPerfTickMs, gArgs.perfTickMs, __ATOMIC_RELEASE);
    __atomic_store_n(&gPerf, gArgs.perf, __ATOMIC_RELEASE);
    __atomic_store_n(&gPerfReady, 1u, __ATOMIC_RELEASE);
    registerPerf();
    __atomic_store_n(&gBoost, gArgs.boosts, __ATOMIC_RELEASE);
    registerBoost();
    note("boost: machdep.piano_cpufreq_boost now %u: launch %u / %u MHz (floor and ceiling) for %u ms, input %u / %u MHz floor for %u "
         "ms after the last HID report, GPU floor %u MHz at a launch (cap %u MHz, 0 none)%s; only with perf policy >= 1 on Balanced, below %u C skin, and "
         "while the cluster's hottest cpu zone is under %u C with none there in the last %u ms (an open boost ends at %u C) and its thermal limit is not active",
         gArgs.boosts, gArgs.launchMhz[0], gArgs.launchMhz[1], gArgs.launchMs, gArgs.inputMhz[0], gArgs.inputMhz[1], gArgs.inputMs,
         gArgs.gpuLaunchMhz, gArgs.gpuLaunchCapMhz, gArgs.gpuInputMhz ? " and on input" : "", gArgs.skinLimitC, gArgs.gateOpenC, gArgs.gateQuietMs, gArgs.gateEndC);
    note("cpufreq: perf policy (machdep.piano_cpufreq_perf, now %u): 1 = caps by PianoPolicy's profile whatever the source, skin steps "
         "%s (virtual skin of the board thermistors, %s), burst %u / %u MHz for <= %u ms at a load onset (gap %u ms, none from %u C "
         "skin), tick %u ms (machdep.piano_cpufreq_perf_tick_ms), downs held %u ms; 0 = caps by the measured source, hard caps %u / "
         "%u MHz, tick %u ms, 3 ticks down; thermal limit on top in both: %s",
         gArgs.perf, gArgs.skin ? "on" : "off", adc::hookLevel() ? "piano-thermal-adc on" : "piano-thermal-adc OFF: no skin reading, "
         "the lowest step's cap", gArgs.burst[0], gArgs.burst[1], gArgs.burstMs, gArgs.burstGapMs, gArgs.skinLimitC, gArgs.perfTickMs,
         gArgs.perfDownMs, gArgs.legacyCap[0], gArgs.legacyCap[1], gArgs.tickMs, tripsText());
    note("cpufreq: perf policy 2 = beyond stock: as 1 without the skin table (sustained ceilings), the onset burst up to %u / %u MHz "
         "(over sustained only while open)", gArgs.burstMax[0], gArgs.burstMax[1]);
    __atomic_store_n(&gFullopp, gArgs.fullopp, __ATOMIC_RELEASE);
    __atomic_store_n(&gWalt, gArgs.walt, __ATOMIC_RELEASE);
    note("fullopp: machdep.piano_cpufreq_fullopp now %u (piano-cpufreq-fullopp): with perf policy >= 1 the whole table (tops %u / %u "
         "MHz, over sustained unless refused), the stock scenes by Energy Mode (Automatic / Low Power: normal, High Power: nolimits "
         "51 C), the battery limit (<= 3 %%: 1785 / 1401, <= 1 %%: 1363 / 1401 MHz), the thermal limit at %u C released under %u C one "
         "level per %u ms, %s; walt: machdep.piano_cpufreq_walt now %u (max of the latest window and the mean of %u, frequency-"
         "invariant); %s",
         gArgs.fullopp, gArgs.hpmTop[0], gArgs.hpmTop[1], gArgs.hotC, gArgs.fullReleaseC, gArgs.fullRiseMs,
         gArgs.fullGate ? "the temperature gate of bursts and boosts kept (piano-cpufreq-fullopp-gate=1)"
                        : "no temperature gate of bursts and boosts beyond the thermal limit",
         gArgs.walt, perf::kWaltHist,
         gArgs.sweep ? "sweep control armed: machdep.piano_cpufreq_sweep0/1 (MHz pin), machdep.piano_cpufreq_measure (test only)"
                     : "no sweep control");
  }
  if (gArgs.androidArmed) {
    androidDomains_ = true;
    for (unsigned d = 0; d < kDomains; ++d) {
      const Governor &g = governor_[d];
      const bool known = domainOk_[d] && domain_[d].canSet &&
          g.khz(g.fastestAtMost(kAndroidBaseKhz[d])) == kAndroidBaseKhz[d] &&
          g.khz(g.fastestAtMost(kAndroidNextKhz[d])) == kAndroidNextKhz[d];
      androidDomains_ &= known;
      if (!known) note("PianoCpuAndroidV1 refused domain=%u required_khz=%u,%u", d, kAndroidBaseKhz[d], kAndroidNextKhz[d]);
    }
  }
  power_ = readPowerSource();
  powerAtNs_ = nowNs();
  if (mode >= CpuMode::fixed) note("cpufreq: power source: %s", powerName(power_));
  if (gArgs.boostS && cpuOk_) {
    boostEndNs_ = uint64_t(gArgs.boostS) * 1000000000ull;
    boostHotEndNs_ = 0;
    boostBackoff_ = BoostBackoff{};
    boostBackoff_.stepDeci = int32_t(gArgs.boostStepC) * 10;
    boostBackoff_.endDeci = int32_t(gArgs.boostEndC) * 10;
    boostBackoff_.stepMs = gArgs.fastMs;
    for (unsigned d = 0; d < kDomains; ++d) boostCapLevel_[d] = governor_[d].boostLevel(capKhz(gArgs.boostCap));
    char cap[48] = "";
    if (gArgs.boostCap) snprintf(cap, sizeof cap, " (no faster than %u MHz)", gArgs.boostCap);
    note("cpufreq: boot boost: every cluster that allows LEVEL_SET at its sustained level%s until %u s of uptime (piano-cpufreq-boost), "
         "the thermal limit on top (%s); then %s", cap, gArgs.boostS, tripsText(),
         mode == CpuMode::report ? "back to the level the boot stages left" : mode == CpuMode::fixed ? "the fixed level" : "the governor");
    if (gArgs.boostStepC)
      note("cpufreq: boot boost back-off: the hottest CPU sensor at %u.%u C steps the boost one level down every %u ms (never up "
           "again this boot), at %u.%u C the boost is over (piano-cpufreq-boost-step / -end)", gArgs.boostStepC / 10,
           gArgs.boostStepC % 10, gArgs.fastMs, gArgs.boostEndC / 10, gArgs.boostEndC % 10);
    else
      note("cpufreq: boot boost back-off: no stepping (piano-cpufreq-boost-step=0); at %u.%u C the boost is over "
           "(piano-cpufreq-boost-end)", gArgs.boostEndC / 10, gArgs.boostEndC % 10);
  }
}

// The level the governor wants for domain d, sent when it differs from the one set last.
void PianoThermal::cpuApply(unsigned d, unsigned want, uint64_t now)
{
  Governor &g = governor_[d];
  if (want == set_[d]) return;
  const int32_t r = scmi::levelSet(channel_, domain_[d], d, want);
  if (r && want > domain_[d].sustained && !gAboveRefused[d]) {   // mode 2's burst: the firmware does not take levels over sustained
    gAboveRefused[d] = true;
    note("[%us] cpufreq: domain %u: %u MHz (above sustained) refused (%d, %s): bursts and High Power stay at sustained for this boot", upS(), d,
         domain_[d].levels[want].khz / 1000, r, scmi::statusName(r));
    g.setLevel(set_[d]);
    return;
  }
  if (gArgs.androidArmed) {
    androidRequestedLevel_[d] = true;
    androidRequestKey_[d] = domain_[d].levels[want].key;
    androidRequestStatus_[d] = r;
  }
  if (r) {
    if (++refusals_[d] <= 3 || refusals_[d] % 100 == 0)
      note("[%us] cpufreq: domain %u: %u MHz refused (%d, %s; %u refusals)", upS(), d, domain_[d].levels[want].khz / 1000, r,
           scmi::statusName(r), refusals_[d]);
    g.setLevel(set_[d]);
    if (refusals_[d] >= 20) {
      note("[%us] cpufreq: domain %u: 20 refusals: left at %u MHz for this boot", upS(), d, domain_[d].levels[set_[d]].khz / 1000);
      domainOk_[d] = false;
    }
    return;
  }
  residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
  if (set_[d] < scmi::kMaxLevels) gPd[d].totalNs[set_[d]] += now - gPd[d].sinceNs;
  gPd[d].sinceNs = now;
  gPd[d].set = want;
  levelSinceNs_[d] = now;
  set_[d] = want;
  ++changes_[d];
}

void PianoThermal::cpuAndroidInputs(uint64_t now)
{
  androidInputsAt_ = now;
  refreshService(androidPower_, "IOPMPowerSource", now, androidPowerLookup_);
  OSObject *external = androidPower_ ? androidPower_->copyProperty("ExternalConnected") : nullptr;
  OSBoolean *connected = OSDynamicCast(OSBoolean, external);
  const PowerSource source = !connected ? PowerSource::unknown : connected->isTrue() ? PowerSource::external : PowerSource::battery;
  if (source != power_) note("[%us] cpufreq: power source: %s -> %s", upS(), powerName(power_), powerName(source));
  power_ = source;
  OSSafeReleaseNULL(external);

  refreshService(androidPolicy_, "PianoPolicy", now, androidPolicyLookup_);
  OSObject *lpm = androidPolicy_ ? androidPolicy_->copyProperty("piano-policy-lpm") : nullptr;
  OSNumber *number = OSDynamicCast(OSNumber, lpm);
  androidLpm_ = number ? androidCpuLpmValue(number->numberOfBits(), number->unsigned64BitValue()) : -1;
  OSSafeReleaseNULL(lpm);
}

void PianoThermal::cpuAndroidReport(const AndroidCpuInput &in, const AndroidCpuSelection &selection, bool slow)
{
  const int phase = int(in.requested) | int(selection.enabled) << 1 | int(in.source) << 2 | int(in.profile) << 4 |
      (in.lpm + 1) << 6 | int(in.boost) << 8 | int(in.valid) << 9 | int(in.domains) << 10 | int(in.sleeping) << 11;
  if (!slow && phase == androidPhase_) return;
  androidPhase_ = phase;
  uint32_t selected[kDomains] = {}, sustained[kDomains] = {}, thermal[kDomains] = {};
  for (unsigned d = 0; d < kDomains; ++d) {
    if (boot_[d] < 0) continue;
    const Governor &g = governor_[d];
    const unsigned limit = in.boost ? g.boostLevel(capKhz(gArgs.boostCap), boostCapLevel_[d]) : g.ceiling(selection.power);
    selected[d] = g.khz(limit);
    sustained[d] = g.khz(g.sustained());
    thermal[d] = g.khz(g.limit());
  }
  note("PianoCpuAndroidV1 enabled=%u requested=%u source=%s profile=%u lpm=%u lpm_valid=%u boost=%u valid=%u base_khz=2400000,2438400 "
       "selected_khz=%u,%u sustained_khz=%u,%u thermal_limit=%u,%u",
       selection.enabled, in.requested, in.source == PowerSource::external ? "external" : in.source == PowerSource::battery ? "battery" : "unknown",
       unsigned(in.profile), unsigned(in.lpm == 1), in.lpm >= 0, in.boost, in.valid,
       selected[0], selected[1], sustained[0], sustained[1], thermal[0], thermal[1]);
  for (unsigned d = 0; d < kDomains && !channel_.lastError; ++d) {
    if (!domainOk_[d]) continue;
    int actual = -1;
    const int32_t status = scmi::levelGet(channel_, domain_[d], d, actual);
    const bool known = !status && actual >= 0;
    note("PianoCpuAndroidV1 scmi domain=%u get_key=%u get_known=%u get_status=%d actual_khz=%u "
         "request_key=%u request_valid=%u request_status=%d changes=%u refusals=%u",
         d, known ? domain_[d].levels[actual].key : 0u, known, status, known ? domain_[d].levels[actual].khz : 0u,
         androidRequestKey_[d], androidRequestedLevel_[d], androidRequestStatus_[d], changes_[d], refusals_[d]);
  }
}

void PianoThermal::cpuAndroidSuspend()
{
  androidActive_ = false;
  const Profile profile = Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE));
  const AndroidCpuInput in{true, __atomic_load_n(&gAndroidRequested, __ATOMIC_ACQUIRE) == 1,
      domainOk_[0] && domainOk_[1], false, false, true, PowerSource::unknown, profile, androidLpm_};
  const auto selection = androidCpuSelection(in);
  const uint64_t now = nowNs();
  for (unsigned d = 0; d < kDomains && !channel_.lastError; ++d) {
    if (!domainOk_[d]) continue;
    Governor &g = governor_[d];
    g.selectAndroidCap(0);
    cpuApply(d, g.enforce(selection.power), now);
  }
  cpuAndroidReport(in, selection, false);
}

// Every sensor poll: the thermal limit (fixed and governor modes) and, at most once per piano-cpufreq-tick-ms (governor) or at
// once (fixed), the level; report mode asks LEVEL_GET at the log cadence only (slow).
void PianoThermal::cpuTick(const Monitor &m, bool slow, bool loadTick)
{
  if (!cpuOk_) {
    if (gArgs.androidArmed && androidPhase_ < 0) {
      const AndroidCpuInput in{true, false, false, false, false, false, PowerSource::unknown, Profile::bySource, -1};
      cpuAndroidReport(in, androidCpuSelection(in), false);
    }
    return;
  }
  if (channel_.lastError) {
    note("[%us] cpufreq: the SCMI channel failed (%d, %s): no more messages this boot; the clusters stay where they are", upS(),
         channel_.lastError, scmi::statusName(channel_.lastError));
    cpuOk_ = false;
    return;
  }
  const uint64_t now = nowNs();
  if (gArgs.androidArmed) {
    if (loadTick || !androidInputsAt_) cpuAndroidInputs(now);
  }
  else if (now - powerAtNs_ >= 10000000000ull) {
    int32_t soc = -1;
    const PowerSource p = readPowerSource(gArgs.fullopp ? &soc : nullptr);
    if (p != power_ && gArgs.cpufreq == 3) note("[%us] cpufreq: power source: %s -> %s", upS(), powerName(power_), powerName(p));
    power_ = p;
    powerAtNs_ = now;
    if (gArgs.fullopp) {   // the full table's battery limit (Perf.h SocLimit): one line per level change
      const unsigned before = socLimit_.level();
      gSoc = soc;
      gSocLevel = socLimit_.update(soc);
      if (gSocLevel != before)
        note("[%us] fullopp: battery %d %%: limit level %u -> %u (%u / %u MHz)", upS(), soc, before, gSocLevel, socLimit_.khz(0) / 1000,
             socLimit_.khz(1) / 1000);
    }
  }
  const Profile profile = Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE));
  // The boost's own thermal back-off (2026-11-15, ufs5r3): one decision per poll on the hottest CPU reading, before the
  // domains and only while the boost is live (a step lowers every domain's boost cap level at once; the heat end shortens the
  // boost so each domain's boostStep takes its over transition on this same poll). The cap only steps down: a cooler spell
  // must not raise it again; past the boost's seconds there is nothing left to step or end.
  if (boostEndNs_ && !boostBackoff_.ended && now < boostEndNs_) {
    int32_t hotDeci = -32768;
    if (cpuHottest(m, hotDeci)) {
      const BoostBackoffEvent backoff = boostBackoff(boostBackoff_, hotDeci, now / 1000000ull);
      if (backoff == BoostBackoffEvent::step) {
        for (unsigned d = 0; d < kDomains; ++d) {
          if (!domainOk_[d] || !domain_[d].canSet || !boostCapLevel_[d]) continue;
          --boostCapLevel_[d];
          note("[%us] cpufreq: domain %u: boost: capped at %u.%u C: %u MHz", upS(), d, uint32_t(hotDeci) / 10,
               uint32_t(hotDeci) % 10,
               domain_[d].levels[governor_[d].boostLevel(capKhz(gArgs.boostCap), boostCapLevel_[d])].khz / 1000);
        }
      } else if (backoff == BoostBackoffEvent::end) {
        boostHotEndNs_ = now;
        note("[%us] cpufreq: boost: %u.%u C ends the boot boost (the thermal back-off, at or above %u C)", upS(),
             uint32_t(hotDeci) / 10, uint32_t(hotDeci) % 10, gArgs.boostEndC);
      }
    }
  }
  int32_t busy[kDomains] = {-1, -1};
  if (loadTick) domainBusy(busy);
  // A load decision needs a busy figure: none on the first reading (it only starts the window), after the clock measurement, or
  // when no core of the domain could be read. Before 2026-11-12 such a tick counted as 0 % busy, a step toward the slowest level.
  const bool critical = gArgs.level >= 2 && policy_.state() == State::critical;
  // the full table (Perf.h fullopp) and the WALT-like demand act under the perf policy only; the full table releases the thermal
  // limit at its own (the stock) point
  const bool fullNow = gArgs.cpufreq == 3 && __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE) != 0 && __atomic_load_n(&gFullopp, __ATOMIC_ACQUIRE);
  const bool waltNow = gArgs.cpufreq == 3 && __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE) != 0 && __atomic_load_n(&gWalt, __ATOMIC_ACQUIRE);
  const ClusterTrips &trips = fullNow ? gTripsFull : gTrips;
  const ThermalInput thermal[kDomains] = {clusterThermal(m, 0, trips, critical), clusterThermal(m, 1, trips, critical)};
  const AndroidCpuInput in{
    gArgs.androidArmed, gArgs.androidArmed && __atomic_load_n(&gAndroidRequested, __ATOMIC_ACQUIRE) == 1,
    androidDomains_ && domainOk_[0] && domainOk_[1],
    gArgs.androidArmed && thermal[0].valid && thermal[1].valid && !thermal[0].cut && !thermal[1].cut,
    boostEndNs_ && !(boostEnded_[0] && boostEnded_[1]), false, power_, profile, androidLpm_,
  };
  const AndroidCpuSelection selection = androidCpuSelection(in);
  // The stock-level policy (Perf.h, machdep.piano_cpufreq_perf): its power is PianoPolicy's profile whatever the source (Low Power
  // Mode = Power Saver = the battery caps, else Balanced = the power caps); mode 0 keeps the measured source as before 2026-11-22
  // (the v13.1x line never had a profile: piano-policy-profile=0).
  const uint32_t perfNow = gArgs.cpufreq == 3 ? __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE) : 0;
  const bool perfMode = perfNow != 0, beyond = perfNow == 2;
  if (perfNow != perfSeen_) {
    if (perfSeen_ != 9) ++perfChanges_;
    perfSeen_ = perfNow;
    note("[%us] cpufreq: perf policy %u (%s), tick %u ms", upS(), perfNow,
         perfNow == 2 ? "beyond stock: profile caps, no skin table, onset burst to the burst maximum"
         : perfNow ? "stock-level: profile caps, skin steps, onset burst" : "before 2026-11-22: source caps, hard caps", curTickMs());
  }
  if (gArgs.cpufreq == 3 && (uint32_t(fullNow) != fullSeen_ || uint32_t(waltNow) != waltSeen_)) {
    fullSeen_ = fullNow;
    waltSeen_ = waltNow;
    note("[%us] fullopp: %s, walt %s%s", upS(), fullNow ? "on (the whole table, the stock scenes, the stock 95 / 87 C rule)" : "off",
         waltNow ? "on" : "off", perfMode ? "" : " (both need perf policy >= 1)");
  }
  const bool highNow = profile == Profile::performance;
  const perf::SceneTable sceneNow = perf::sceneTable(perf::sceneFor(fullNow && highNow));
  const int32_t boostLimitMilli = fullNow ? sceneNow.boostLimitMilli : int32_t(gArgs.skinLimitC) * 1000;
  const PowerSource power = perfMode ? profilePower(power_, profile) : gArgs.androidArmed ? selection.power : power_;
  if (perfMode && loadTick) {
    int32_t in5[perf::kSkinInputs] = {};
    uint32_t mask = 0;
    skin_ = adc::hookSkin(in5, mask);
    for (unsigned i = 0; i < perf::kSkinInputs; ++i) skinIn_[i] = in5[i];
    skinInMask_ = mask;
    gSkinMilli = skin_.milliC;
    gSkinValid = skin_.valid;
    gSkinUsed = skin_.used;
  }
  // The launch / input boost in force (Perf.h): only with perf policy >= 1, machdep.piano_cpufreq_boost, the Balanced caps (not
  // Power Saver) and the skin under its boost limit; the silicon gate below decides per cluster.
  const uint64_t launchNs = __atomic_load_n(&gLaunchNs, __ATOMIC_ACQUIRE), inputNs = __atomic_load_n(&gInputNs, __ATOMIC_ACQUIRE);
  const bool boostsAllowed = perfMode && __atomic_load_n(&gBoost, __ATOMIC_ACQUIRE) && power == PowerSource::external &&
      skin_.valid && skin_.milliC < boostLimitMilli;
  const perf::BoostKind bk = boostsAllowed ? perf::boostAt(now, launchNs, gArgs.launchMs, inputNs, gArgs.inputMs) : perf::BoostKind::none;
  androidActive_ = selection.enabled;
  for (unsigned d = 0; d < kDomains; ++d) {
    if (gArgs.androidArmed) governor_[d].selectAndroidCap(selection.enabled ? kAndroidNextKhz[d] : 0);
    if (!domainOk_[d]) continue;
    Governor &g = governor_[d];
    // piano-cpufreq-boost: the sustained level (the thermal limit and the boost's own sticky thermal cap on top, in every mode)
    // until its end — its seconds, or the back-off's heat end, whichever is first — then once the mode's own
    const BoostStep step = domain_[d].canSet
        ? boostStep(now / 1000000ull, (boostHotEndNs_ ? boostHotEndNs_ : boostEndNs_) / 1000000ull, boostEnded_[d])
        : BoostStep::none;
    if (step != BoostStep::none) {
      g.thermal(thermal[d], now / 1000000ull);
      if (step == BoostStep::boost) {
        const unsigned want = g.boostLevel(capKhz(gArgs.boostCap), boostCapLevel_[d]);
        cpuApply(d, want, now);
        if (!boostNoted_[d] && set_[d] == want) {
          boostNoted_[d] = true;
          note("[%us] cpufreq: domain %u: boot boost at %u MHz (uptime %llu ms)", upS(), d, domain_[d].levels[want].khz / 1000,
               (unsigned long long)(now / 1000000ull));
        }
        continue;
      }
      boostEnded_[d] = true;
      const bool byTime = now >= boostEndNs_;   // else the back-off ended it below its seconds
      if (g.mode() == CpuMode::report) {
        cpuApply(d, unsigned(boot_[d]), now);
        if (byTime)
          note("[%us] cpufreq: domain %u: boot boost over at %u s of uptime: back to the boot stages' %u MHz", upS(), d, gArgs.boostS,
               domain_[d].levels[set_[d]].khz / 1000);
        else
          note("[%us] cpufreq: domain %u: boot boost over at %u.%u C (the thermal back-off): back to the boot stages' %u MHz",
               upS(), d, uint32_t(boostBackoff_.endedDeci) / 10, uint32_t(boostBackoff_.endedDeci) % 10,
               domain_[d].levels[set_[d]].khz / 1000);
        continue;
      }
      g.setLevel(set_[d]);
      if (byTime)
        note("[%us] cpufreq: domain %u: boot boost over at %u s of uptime: %s from %u MHz", upS(), d, gArgs.boostS,
             g.mode() == CpuMode::fixed ? "the fixed level" : "the governor", domain_[d].levels[set_[d]].khz / 1000);
      else
        note("[%us] cpufreq: domain %u: boot boost over at %u.%u C (the thermal back-off): %s from %u MHz", upS(), d,
             uint32_t(boostBackoff_.endedDeci) / 10, uint32_t(boostBackoff_.endedDeci) % 10,
             g.mode() == CpuMode::fixed ? "the fixed level" : "the governor", domain_[d].levels[set_[d]].khz / 1000);
    }
    if (g.mode() == CpuMode::report) {
      if (!slow) continue;
      int idx = -1;
      if (!scmi::levelGet(channel_, domain_[d], d, idx) && idx >= 0 && unsigned(idx) != set_[d]) {
        residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
        levelSinceNs_[d] = now;
        set_[d] = unsigned(idx);
        ++changes_[d];
      }
      continue;
    }
    const unsigned limitBefore = g.limit();
    if (g.thermal(thermal[d], now / 1000000ull) && g.limit() < limitBefore && g.limit() == 0)
      ++cuts_[d];
    if (g.limit() < g.count() - 1u) ++limitedPolls_[d];
    if (busy[d] >= 0) {
      busySum_[d] += uint32_t(busy[d]);
      ++busyTicks_[d];
      if (uint32_t(busy[d]) > busyMax_[d]) busyMax_[d] = uint32_t(busy[d]);
    }
    if (g.mode() == CpuMode::govern) {
      // The silicon gate of bursts and boosts (perf-cpu1: a burst from the skin cap straight to 4089.6 MHz on a zone already at
      // 78-87 C read 101.9 C one poll later): none opens while the cluster's hottest cpu zone is at piano-cpufreq-gate-open (75 C)
      // or its thermal limit holds, an open one ends at piano-cpufreq-gate-end (85 C).
      const int32_t hottest = thermal[d].valid ? thermal[d].hotExcessDeci + (gTrips.hotDeci ? gTrips.hotDeci : 1050) : 9999;
      const bool gateOpen = boostOn_[d] || burst_[d].open();
      // the full table: Android has no temperature gate on its boosts, only the thermal limit (the first term) holds them
      const bool gateOk = g.limit() >= g.count() - 1u &&
          ((fullNow && !gArgs.fullGate) ||
           (hottest < int32_t(gateOpen ? gArgs.gateEndC : gArgs.gateOpenC) * 10 &&
            (gateOpen || !hotSeenNs_[d] || now - hotSeenNs_[d] >= uint64_t(gArgs.gateQuietMs) * 1000000ull)));
      g.setRiseMs(fullNow ? gArgs.fullRiseMs : gArgs.riseMs);
      const uint32_t sweepMhz = gArgs.sweep ? __atomic_load_n(&gSweepMhz[d], __ATOMIC_ACQUIRE) : 0;
      if (sweepMhz != sweepSeen_[d]) {
        sweepSeen_[d] = sweepMhz;
        note("[%us] sweep: cluster %u %s%u MHz", upS(), d, sweepMhz ? "pinned at " : "free (", sweepMhz ? g.khz(g.fastestAtMost(capKhz(sweepMhz))) / 1000 : 0u);
      }
      const perf::Scene scene = perf::sceneFor(fullNow && profile == Profile::performance);
      if (perfMode && scene != sceneSeen_[d]) {   // another scene's steps: the step restarts (its hysteresis with it)
        skinCap_[d].reset();
        sceneSeen_[d] = scene;
      }
      gPd[d].sweepKhz = 0;
      if (sweepMhz) {
        // TEST (piano-cpufreq-sweep): floor = ceiling = the level, over sustained unless refused this boot; no scene, burst or boost;
        // the power caps, the hard caps and the thermal limit (95 C) stay on top (Governor::ceilingFor)
        const uint32_t pin = g.khz(g.fastestAtMost(capKhz(sweepMhz)));
        boostOn_[d] = false;
        g.setBoostFloor(pin);
        g.setDownTicks(perf::downTicksFor(gArgs.perfDownMs, curTickMs()));
        g.setRuntimeCaps(false, 0, pin, pin > g.khz(g.sustained()) && !gAboveRefused[d]);
        gPd[d].sweepKhz = pin;
      } else if (perfMode && fullNow) {
        // the full table (Perf.h fullopp): the scene's step cap (normal / nolimits by Energy Mode; none below its first trigger =
        // the table's top), the battery limit on top, the onset burst and the launch pin as in policy 1, over sustained allowed
        const bool high = profile == Profile::performance;
        const uint32_t stepKhz = gArgs.skin ? skinCap_[d].update(sceneNow.steps[d], sceneNow.count[d], skin_.valid, skin_.milliC) : 0;
        gPd[d].sceneKhz = stepKhz;
        const uint32_t topKhz = capKhz(gArgs.hpmTop[d]);
        const uint32_t capNow = perf::fullCapKhz(stepKhz ? stepKhz : topKhz, socLimit_.khz(d));
        const uint32_t burstKhz = capKhz(gArgs.burst[d]);
        const bool burstAllowed = power == PowerSource::external && gateOk && !high && !socLimit_.level() && skin_.valid;
        if (loadTick) {
          perf::BurstConfig bc;
          bc.khz = burstKhz;
          bc.windowMs = gArgs.burstMs;
          bc.gapMs = gArgs.burstGapMs;
          bc.limitMilli = boostLimitMilli;
          burst_[d].tick(bc, now / 1000000ull, g.rawWant() > g.ceilingFor(power, capNow),
                         burstAllowed, skin_.valid, skin_.milliC);
        } else if (burst_[d].open() && !burstAllowed) {
          perf::BurstConfig bc;
          bc.khz = burstKhz;
          burst_[d].tick(bc, now / 1000000ull, true, false, skin_.valid, skin_.milliC);
        }
        uint32_t boostFloor = 0, boostCap = 0;
        if (bk != perf::BoostKind::none && gateOk && !socLimit_.level()) {
          const uint32_t mhz = bk == perf::BoostKind::launch ? gArgs.launchMhz[d] : gArgs.inputMhz[d];
          boostFloor = mhz * 1000u;
          if (bk == perf::BoostKind::launch) boostCap = capKhz(mhz);
        }
        boostOn_[d] = boostFloor != 0;
        g.setBoostFloor(boostFloor);
        g.setDownTicks(perf::downTicksFor(gArgs.perfDownMs, curTickMs()));
        uint32_t perfKhz = burst_[d].open() && burstKhz > capNow ? burstKhz : capNow;
        if (boostCap > perfKhz) perfKhz = boostCap;
        g.setRuntimeCaps(false, 0, perfKhz, !gAboveRefused[d]);
      } else if (perfMode) {
        const bool high = profile == Profile::performance;   // High Power (docs/policy.md 12): the top levels, no skin table
        const uint32_t skinKhz = gArgs.skin && !beyond && !high ? skinCap_[d].update(d ? perf::kCluster1 : perf::kCluster0,
                                                                            d ? perf::kCluster1Steps : perf::kCluster0Steps,
                                                                            skin_.valid, skin_.milliC)
                                                       : 0;
        gPd[d].sceneKhz = skinKhz;
        const uint32_t burstKhz = capKhz(beyond ? gArgs.burstMax[d] : gArgs.burst[d]);
        const bool burstAllowed = power == PowerSource::external && gateOk && !high && skin_.valid;
        if (loadTick) {
          perf::BurstConfig bc;
          bc.khz = burstKhz;
          bc.windowMs = gArgs.burstMs;
          bc.gapMs = gArgs.burstGapMs;
          bc.limitMilli = boostLimitMilli;
          burst_[d].tick(bc, now / 1000000ull, g.rawWant() > g.ceilingFor(power, skinKhz),
                         burstAllowed, skin_.valid, skin_.milliC);
        } else if (burst_[d].open() && !burstAllowed) {
          perf::BurstConfig bc;
          bc.khz = burstKhz;
          burst_[d].tick(bc, now / 1000000ull, true, false, skin_.valid, skin_.milliC);   // the gate closes it at once
        }
        // the boost: a floor (launch: floor and ceiling, the stock min = max pin) under the thermal limit
        uint32_t boostFloor = 0, boostCap = 0;
        if (bk != perf::BoostKind::none && gateOk) {
          const uint32_t mhz = bk == perf::BoostKind::launch ? gArgs.launchMhz[d] : gArgs.inputMhz[d];
          boostFloor = mhz * 1000u;
          if (bk == perf::BoostKind::launch) boostCap = capKhz(mhz);
        }
        boostOn_[d] = boostFloor != 0;
        g.setBoostFloor(boostFloor);
        g.setDownTicks(perf::downTicksFor(gArgs.perfDownMs, curTickMs()));
        const perf::HighPowerCap hp = perf::highPowerCap(capKhz(gArgs.hpmTop[d]), gAboveRefused[d]);
        uint32_t perfKhz = high ? hp.khz
                         : beyond ? (burst_[d].open() ? burstKhz : 0) : perf::perfCapKhz(skinKhz, burst_[d].open(), burstKhz);
        if (boostCap && perfKhz && boostCap > perfKhz) perfKhz = boostCap;
        if (high)     // the top level as the cap, over sustained unless refused this boot; the thermal limit on top
          g.setRuntimeCaps(false, 0, perfKhz, hp.aboveSustained);
        else if (beyond)   // no skin table: the sustained ceiling, the burst over it while open (unless the firmware refused that)
          g.setRuntimeCaps(false, 0, perfKhz, burst_[d].open() && !gAboveRefused[d]);
        else
          g.setRuntimeCaps(false, 0, perfKhz);
      } else {
        boostOn_[d] = false;
        g.setBoostFloor(0);
        g.setDownTicks(3);
        g.setRuntimeCaps(true, capKhz(gArgs.legacyCap[d]), 0);
      }
      if (perfMode && gArgs.skin && !skin_.valid)
        g.setRuntimeCaps(false, 0, perf::fullCapKhz(g.perfKhz(), perf::skinFaultCap(d)));
    }
    // the WALT-like demand (Perf.h WaltCore): each core's window work at the level it ran at, its history kept in every perf mode so
    // switching it on starts from a fresh history; used for the decision only with machdep.piano_cpufreq_walt
    uint32_t demand = 0;
    if (perfMode && loadTick && busy[d] >= 0 && g.mode() == CpuMode::govern) {
      const uint32_t khzNow = g.khz(set_[d]);
      for (unsigned i = 0; i < gCpuCount; ++i) {
        if (gCpus[i].domain != d || gCpus[i].busy < 0) continue;
        const uint32_t v = gCpus[i].walt.push(perf::workKhz(uint32_t(gCpus[i].busy), khzNow));
        if (v > demand) demand = v;
      }
      gPd[d].demandKhz = demand;
    }
    const unsigned want = loadTick && busy[d] >= 0 && g.mode() == CpuMode::govern
                              ? g.tick({uint32_t(busy[d]), power, waltNow ? demand : 0u})
                              : g.enforce(power);
    cpuApply(d, want, now);
    // the full table and the sweep: the firmware's level (LEVEL_GET) against ours, once per piano-cpufreq-fw-check-ms
    if ((gArgs.fullopp || gArgs.sweep) && domainOk_[d] && domain_[d].canSet && !channel_.lastError &&
        now - fwAtNs_[d] >= uint64_t(gArgs.fwCheckMs) * 1000000ull) {
      fwAtNs_[d] = now;
      int idx = -1;
      if (!scmi::levelGet(channel_, domain_[d], d, idx) && idx >= 0) {
        gPd[d].fwKhz = domain_[d].levels[idx].khz;
        ++gPd[d].fwChecks;
        if (unsigned(idx) != set_[d] && ++gPd[d].fwMismatch <= 5)
          note("[%us] fullopp: domain %u: the firmware reports %u MHz, ours %u MHz (%u of %u checks differ)", upS(), d,
               domain_[d].levels[idx].khz / 1000, domain_[d].levels[set_[d]].khz / 1000, gPd[d].fwMismatch, gPd[d].fwChecks);
      }
    }
    gPd[d].ceilKhz = g.khz(g.ceiling(power));
    gPd[d].perfKhz = g.perfKhz();
    gPd[d].limitKhz = g.khz(g.limit());
    gPd[d].hotDeci = thermal[d].valid ? thermal[d].hotExcessDeci + (gTrips.hotDeci ? gTrips.hotDeci : 1050) : -9999;
    gPd[d].step = skinCap_[d].step();
    gPd[d].bursts = burst_[d].windows();
    gPd[d].burstMs = burst_[d].openMs(now / 1000000ull);
  }
  // boost bookkeeping and the GPU's side (PianoA830 "piano-gpu-boost": a launch once per exec, input at most every 250 ms only when
  // piano-cpufreq-gpu-input-mhz is given: the input kexts already ask PianoA830 themselves on gpu/boost342)
  {
    const bool any = boostOn_[0] || boostOn_[1];
    const perf::BoostKind applied = any ? bk : perf::BoostKind::none;
    if (applied != perf::BoostKind::none && applied != boostKind_) ++gBoostStarts[unsigned(applied)];
    if (boostTickNs_ && now > boostTickNs_ && boostKind_ != perf::BoostKind::none) {
      boostNs_[unsigned(boostKind_)] += now - boostTickNs_;
      gBoostMs[unsigned(boostKind_)] = boostNs_[unsigned(boostKind_)] / 1000000ull;
    }
    boostTickNs_ = now;
    boostKind_ = applied;
    if (bk == perf::BoostKind::launch && launchNs != gpuLaunchSeen_ && gArgs.gpuLaunchMhz) {
      gpuLaunchSeen_ = launchNs;
      gpuBoost(gArgs.gpuLaunchMhz, gArgs.launchMs, 1, gArgs.gpuLaunchCapMhz);
    } else if (bk == perf::BoostKind::input && gArgs.gpuInputMhz && now - gpuInputAt_ >= 250000000ull) {
      gpuInputAt_ = now;
      gpuBoost(gArgs.gpuInputMhz, gArgs.inputMs, 2);
    }
    // No cancel: PianoA830's ceilings (Power Saver's 342 MHz slot, its thermal guard) always win over a boost floor, and its
    // param1 0 means "the input boost", not "stop" (gpu/boost342's PianoGpuBoost.h).
  }
  if (gArgs.androidArmed && !in.boost) {
    for (unsigned d = 0; d < kDomains; ++d)
      androidActive_ |= boot_[d] >= 0 && governor_[d].khz(set_[d]) > kAndroidBaseKhz[d];
  }
  if (gArgs.androidArmed) cpuAndroidReport(in, selection, slow);
}

void PianoThermal::cpuReport(uint64_t spanNs)
{
  if (!cpuOk_ && !channel_.messages) return;
  const uint64_t now = nowNs();
  const Profile profile = Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE));
  const AndroidCpuInput in{gArgs.androidArmed, false, false, false, false, false, power_, profile, androidLpm_};
  const bool perfMode = gArgs.cpufreq == 3 && __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE) != 0;
  const PowerSource power = perfMode ? profilePower(power_, profile) : gArgs.androidArmed ? androidCpuSelection(in).power : power_;
  for (unsigned d = 0; d < kDomains; ++d) {
    if (boot_[d] < 0) continue;
    const scmi::Domain &dom = domain_[d];
    residencyNs_[d][set_[d]] += now - levelSinceNs_[d];
    levelSinceNs_[d] = now;
    char line[480];
    int at = snprintf(line, sizeof line, "[%us] cpufreq d%u %s: now %u MHz (boot %u MHz);", upS(), d, dom.name,
                      dom.levels[set_[d]].khz / 1000, dom.levels[boot_[d]].khz / 1000);
    // The window these residencies cover: the first one starts at the domain's init, before the thread's first report (whose
    // spanNs is a few ms: "1996 MHz 26718%", queue5 P5); later ones equal spanNs.
    uint64_t windowNs = 0;
    for (unsigned i = 0; i < dom.count; ++i) windowNs += residencyNs_[d][i];
    if (!windowNs) windowNs = spanNs;
    for (unsigned i = 0; i < dom.count && at < int(sizeof line) - 24; ++i) {
      const uint64_t pct = windowNs ? residencyNs_[d][i] * 100 / windowNs : 0;
      if (pct) at = catf(line, sizeof line, at, " %u MHz %llu%%", dom.levels[i].khz / 1000, (unsigned long long)pct);
      residencyNs_[d][i] = 0;
    }
    const Governor &g = governor_[d];
    if (at < int(sizeof line))
      at = catf(line, sizeof line, at, "; %u changes; busy avg %llu%% max %u%%; ceiling %u MHz (%s%s%s), thermal limit %u MHz (%llu "
               "limited polls, %u cuts)",
               changes_[d], (unsigned long long)(busyTicks_[d] ? busySum_[d] / busyTicks_[d] / 10 : 0), busyMax_[d] / 10,
               g.khz(g.ceiling(power)) / 1000, powerName(power_), profile == Profile::bySource ? "" : "; profile ",
               profile == Profile::bySource ? "" : profileName(profile), g.khz(g.limit()) / 1000, (unsigned long long)limitedPolls_[d],
               cuts_[d]);
    note("%s", line);
    if (hotMax_[d] > -1000)
      note("[%us] hot d%u: cpu zones at or above 90 / 95 / 105 C: %u / %u / %u readings, %llu / %llu / %llu ms; max %s C", upS(), d,
           hotN_[d][0], hotN_[d][1], hotN_[d][2], (unsigned long long)(hotNs_[d][0] / 1000000ull),
           (unsigned long long)(hotNs_[d][1] / 1000000ull), (unsigned long long)(hotNs_[d][2] / 1000000ull), Deci(hotMax_[d]).s);
    for (unsigned i = 0; i < 3; ++i) hotN_[d][i] = 0, hotNs_[d][i] = 0;
    hotMax_[d] = -1000;
    changes_[d] = 0;
    busySum_[d] = busyTicks_[d] = 0;
    busyMax_[d] = 0;
    limitedPolls_[d] = 0;
    cuts_[d] = 0;
  }
  note("[%us] scmi: %u messages, %u failed, %llu us each on average, %u us at most; own work %llu us/s (left out of busy)", upS(),
       channel_.messages, channel_.failures, (unsigned long long)(channel_.messages ? channel_.totalUs / channel_.messages : 0),
       channel_.maxUs, (unsigned long long)(spanNs >= 1000000000ull ? gSelfNsTotal / (spanNs / 1000000000ull) / 1000 : 0));
  gSelfNsTotal = 0;
  if (gArgs.cpufreq == 3) cpuPerfReport();
  OSDictionary *dict = OSDictionary::withCapacity(4);
  if (dict) {
    for (unsigned d = 0; d < kDomains; ++d) {
      if (boot_[d] < 0) continue;
      if (OSNumber *n = OSNumber::withNumber(uint64_t(domain_[d].levels[set_[d]].khz / 1000), 32)) {
        dict->setObject(d ? "cluster1-MHz" : "cluster0-MHz", n);
        n->release();
      }
    }
    setProperty("CpuFreq", dict);
    dict->release();
  }
}

// Every fresh sensor poll: each cluster's hottest cpu zone into the window's 90 / 95 / 105 C counts and times, and the gate's
// last-hot time.
void PianoThermal::hotAccount(uint64_t now)
{
  const uint64_t dt = hotAccNs_ && now > hotAccNs_ ? now - hotAccNs_ : 0;
  hotAccNs_ = now;
  for (unsigned d = 0; d < kDomains; ++d) {
    const ThermalInput in = clusterThermal(monitor_, d, gTrips, false);
    if (!in.valid) continue;
    const int32_t hottest = in.hotExcessDeci + (gTrips.hotDeci ? gTrips.hotDeci : 1050);
    if (hottest > hotMax_[d]) hotMax_[d] = hottest;
    if (hottest >= int32_t(gArgs.gateOpenC) * 10) hotSeenNs_[d] = now;
    static const int32_t kLines[3] = {900, 950, 1050};
    for (unsigned i = 0; i < 3; ++i)
      if (hottest >= kLines[i]) {
        ++hotN_[d][i];
        hotNs_[d][i] += dt;
      }
  }
}

// The stock-level policy's 30 s line: the mode, the tick, the virtual skin and its inputs, per cluster the skin step and its cap, the
// bursts (windows, ms open) since boot.
void PianoThermal::cpuPerfReport()
{
  const uint32_t perfNow = __atomic_load_n(&gPerf, __ATOMIC_ACQUIRE);
  char line[480];
  int at = snprintf(line, sizeof line, "[%us] perf: policy %u (%s), tick %u ms, %u mode changes; skin ", upS(), perfNow,
                    perfNow == 2 ? "beyond stock" : perfNow ? "stock-level" : "before 2026-11-22", curTickMs(), perfChanges_);
  if (skin_.valid) at = catf(line, sizeof line, at, "%d.%d C", skin_.milliC / 1000, (skin_.milliC % 1000 + 1000) % 1000 / 100);
  else at = catf(line, sizeof line, at, "no reading");
  at = catf(line, sizeof line, at, " (");
  for (unsigned i = 0; i < perf::kSkinInputs; ++i)
    at = catf(line, sizeof line, at, "%s%s %s", i ? ", " : "", perf::kSkinChannel[i],
              skinInMask_ & (1u << i) ? Deci(skinIn_[i] / 100).s : "-");
  at = catf(line, sizeof line, at, ")");
  const uint64_t nowMs = nowNs() / 1000000ull;
  for (unsigned d = 0; d < kDomains; ++d) {
    if (!domainOk_[d]) continue;
    at = catf(line, sizeof line, at, "; c%u step %d cap %u MHz, perf cap %u MHz, %u bursts %llu ms", d, skinCap_[d].step(),
              skinCap_[d].step() < 0 ? 0u : (d ? perf::kCluster1 : perf::kCluster0)[skinCap_[d].step()].khz / 1000,
              governor_[d].perfKhz() / 1000, burst_[d].windows(), (unsigned long long)burst_[d].openMs(nowMs));
  }
  note("%s", line);
  if (gArgs.fullopp || gArgs.sweep) {   // the full table's 30 s line
    const Profile profile = Profile(__atomic_load_n(&profile_, __ATOMIC_ACQUIRE));
    const bool full = perfNow && __atomic_load_n(&gFullopp, __ATOMIC_ACQUIRE);
    at = snprintf(line, sizeof line, "[%us] fullopp: %s, scene %s, walt %s, battery %d %% (limit level %u)", upS(), full ? "on" : "off",
                  full ? perf::sceneTable(perf::sceneFor(profile == Profile::performance)).name : "-",
                  perfNow && __atomic_load_n(&gWalt, __ATOMIC_ACQUIRE) ? "on" : "off", gSoc, gSocLevel);
    for (unsigned d = 0; d < kDomains; ++d) {
      if (!domainOk_[d]) continue;
      at = catf(line, sizeof line, at, "; c%u step %d cap %u MHz, ceiling %u MHz, sweep %u MHz, fw %u MHz (%u/%u differ), demand %u MHz", d,
                skinCap_[d].step(), gPd[d].sceneKhz / 1000, gPd[d].ceilKhz / 1000, gPd[d].sweepKhz / 1000, gPd[d].fwKhz / 1000,
                gPd[d].fwMismatch, gPd[d].fwChecks, gPd[d].demandKhz / 1000);
    }
    note("%s", line);
  }
  note("[%us] boost: %s; launches %u (last %s, %u not apps), input reports %u (touch %u, keyboard %u, pointer %u, buttons %u); "
       "boosted: launch %u x %llu ms, input %u x %llu ms; GPU boost calls %u, taken %u (%s)", upS(),
       __atomic_load_n(&gBoost, __ATOMIC_ACQUIRE) ? "on" : "off (machdep.piano_cpufreq_boost=0)", __atomic_load_n(&gLaunches, __ATOMIC_RELAXED),
       gLastApp[0] ? gLastApp : "-", gLaunchIgnored, __atomic_load_n(&gInputs, __ATOMIC_RELAXED), gInputBy[1], gInputBy[2], gInputBy[3],
       gInputBy[4], gBoostStarts[2], (unsigned long long)gBoostMs[2], gBoostStarts[1], (unsigned long long)gBoostMs[1], gGpuCalls,
       gGpuTaken, gGpuState == 1 ? "PianoA830 takes it" : gGpuState == 2 ? "PianoA830: unsupported" : gGpuState == 3 ? "no PianoA830" : "not asked yet");
}

// ------------------------------------------------------------------------------------------------------------- the thread
void PianoThermal::run()
{
  if (gArgs.delay) {
    note("waiting %u s", gArgs.delay);
    IOSleep(gArgs.delay * 1000);
  }
  for (unsigned c = 0; c < kControllers; ++c) {
    if (!mapWindow(gTm[c], kMap[c].tm, kTmBytes)) gTm[c] = {};
    if (gArgs.srot && !mapWindow(gSrot[c], kMap[c].srot, kSrotBytes)) gSrot[c] = {};
  }
  if (!mapWindow(gShmem, scmi::kShmemTx, 0x1000)) gShmem = {};
  else gShmem.bytes = scmi::kShmemBytes;
  if (gArgs.cpufreq && !mapWindow(gMbox, scmi::kMboxTx, 0x1000)) gMbox = {};
  surveyControllers();
  peekChannel();
  if (gArgs.cpufreq) cpuStart();
  else readTopology();
  const bool governing = cpuOk_ && gArgs.cpufreq == 3;
  // The poll period by tier (Cluster.h pollTier): base = the governor's tick (100 ms; the perf policy's tick, 20 ms) or 1 s; warm
  // (hottest >= 70 C) 250 ms; clock (a cluster above piano-thermal-fast-mhz, every sensor under piano-thermal-fast-temp) 100 ms; fast
  // (10 ms) within 10 C of a trip or of the ceiling, while a cluster is thermally limited, or above the fast-poll clock from the
  // fast-poll temperature up. With a tick under 100 ms the sensors are still read every 100 ms outside the fast tier (the perf
  // policy adds load decisions, not sensor reads).
  uint32_t base = governing ? curTickMs() : gArgs.pollMs;
  note("running: %s every %u ms, %u ms from %u C, %u ms above %u MHz (%u ms from %u C), %u ms near a trip; the policy every %u ms; a "
       "report every %u s%s",
       governing ? "the governor and the sensors" : "the sensors", base, pollPeriodMs(Poll::warm, base, gArgs.warmMs, gArgs.fastMs),
       gArgs.warmPollC, pollPeriodMs(Poll::clock, base, gArgs.warmMs, gArgs.fastMs, gArgs.clockMs), gArgs.fastMhz, gArgs.fastMs,
       gArgs.fastTempC, gArgs.fastMs, gArgs.pollMs, gArgs.logS, gMapFailures ? " (some windows not mapped)" : "");
  const uintptr_t tmVa[kControllers] = {gTm[0].va, gTm[1].va, gTm[2].va, gTm[3].va};
  irq::hookStart(this, tmVa, base, governing, gArgs.logS, gStartNs);   // piano-thermal-irq (PianoThermalIrq.cpp): nothing without it
  uint64_t lastPolicy = 0, lastLoad = 0, lastLog = nowNs(), lastCpuLog = nowNs(), tierSince = nowNs(), lastSensors = 0;
  uint64_t tierNs[kPollTiers] = {};
  uint32_t tierChanges = 0;
  Poll tier = Poll::base;
  bool first = true, fastNoted = false, policyFresh = false;
  for (;;) {
    const bool woke = park();
    if (woke) {   // back from a system sleep: the power source read again at the next CPU tick, the zones now
      powerAtNs_ = 0;
      androidInputsAt_ = 0;
      policy_.resetCriticalStreak();
      note("[%us] wake %u: polling again", upS(), sleep_.wakes());
    }
    const uint64_t workStart = mach_absolute_time();   // this poll's own work, up to the wait: noteSelf, left out of busy
    if (governing) base = curTickMs();
    const uint64_t before = nowNs();
    const bool sensorsDue = first || woke || tier != Poll::base || base >= 100 || before - lastSensors >= 100000000ull;
    if (sensorsDue) {
      monitor_.poll(kTsensIo);
      lastSensors = before;
      if (gArgs.cpufreq == 3) hotAccount(nowNs());
    }
    const uint64_t now = nowNs();
    if (sensorsDue) irq::hookPolled(monitor_, now / 1000000ull);
    int32_t d;
    unsigned c, s;
    if (monitor_.hottestAll(d, c, s)) {
      piano_thermal_hottest_deci = d;
      if (!peakValid_ || d > peakDeci_) {
        peakValid_ = true;
        peakDeci_ = d;
        peakC_ = c;
        peakS_ = s;
      }
    }
    bool limited = false, clusterFast = false;
    for (unsigned k = 0; k < kDomains; ++k) {
      if (!domainOk_[k]) continue;
      limited |= (governor_[k].mode() >= CpuMode::fixed || (boostEndNs_ && !boostEnded_[k])) &&
                 governor_[k].limit() < governor_[k].count() - 1u;
      clusterFast |= boot_[k] >= 0 && domain_[k].levels[set_[k]].khz > gArgs.fastMhz * 1000u;
    }
    // with piano-thermal-irq at rest: the same without the warm rule and the fast-clock rule (the upper interrupt stands in for them)
    const Poll next = irq::hookTier(pollTier(monitor_, gTrips, limited, clusterFast, int32_t(gArgs.warmPollC) * 10,
                                             int32_t(gArgs.fastTempC) * 10),
                                    pollTier(monitor_, gTrips, limited, false, 0x7fffffff), now / 1000000ull);
    if (next != tier) {
      tierNs[unsigned(tier)] += now - tierSince;
      tierSince = now;
      ++tierChanges;
      if (next == Poll::fast && !fastNoted) {   // the first fast spell of a report window, with its reason
        fastNoted = true;
        note("[%us] polling every %u ms: %s", upS(), gArgs.fastMs,
             irq::hookHolding(now / 1000000ull) ? "an upper-threshold interrupt"
             : limited ? "a cluster's thermal limit holds it down"
                     : clusterFast ? "a cluster runs above the fast-poll clock, a sensor at the fast-poll temperature"
                                   : "a sensor is within 10 C of its trip or of the ceiling");
      }
      tier = next;
    }
    if (sensorsDue && (first || woke || tier != Poll::base || monitor_.allFresh() != policyFresh ||
                       now - lastPolicy >= uint64_t(gArgs.pollMs) * 1000000ull)) {
      lastPolicy = now;
      const PolicyEvent e = policy_.update(monitor_, now / 1000000ull);
      policyFresh = !e.incomplete;
      if (e.gpuLimited() && gArgs.level >= 2 && e.after < State::passive) piano_thermal_state = int32_t(State::passive);
      else piano_thermal_state = int32_t(e.after);
      protect(e);
    }
    const bool slow = first || now - lastLog >= uint64_t(gArgs.logS) * 1000000000ull;
    const bool loadTick = first || woke || now - lastLoad >= uint64_t(governing ? base : gArgs.tickMs) * 1000000ull;
    if (loadTick) lastLoad = now;
    if (gArgs.cpufreq) cpuTick(monitor_, slow, loadTick);
    // TEST (piano-cpufreq-sweep): machdep.piano_cpufreq_measure asked for each core's clock (the dependent-add chain on each core)
    if (gArgs.sweep && cpuOk_ && __atomic_exchange_n(&gMeasureReq, 0u, __ATOMIC_ACQ_REL)) {
      measureClocks(gClkMhz, gClkOn);
      gClkAtMs = nowNs() / 1000000ull;
      const uint32_t n = __atomic_add_fetch(&gMeasures, 1u, __ATOMIC_ACQ_REL);
      char line[300];
      int at = snprintf(line, sizeof line, "[%us] sweep: clock %u (MHz):", upS(), n);
      for (unsigned i = 0; i < gCpuCount && at < int(sizeof line); ++i)
        at = catf(line, sizeof line, at, " cpu%u/%u %u", gCpus[i].id, gCpus[i].domain, gClkOn[i] ? gClkMhz[i] : 0u);
      for (unsigned k = 0; k < kDomains && at < int(sizeof line); ++k)
        if (domainOk_[k]) at = catf(line, sizeof line, at, "; d%u set %u MHz", k, domain_[k].levels[set_[k]].khz / 1000);
      note("%s", line);
    }
    if (slow) {
      report(monitor_, true);
      publish(monitor_);
      if (gArgs.cpufreq) {
        cpuReport(now - lastCpuLog);
        lastCpuLog = now;
      }
      tierNs[unsigned(tier)] += now - tierSince;
      tierSince = now;
      if (tierNs[1] || tierNs[2] || tierNs[3])
        note("[%us] polling: base %llu ms, warm %llu ms, clock %llu ms, fast %llu ms; %u changes", upS(),
             (unsigned long long)(tierNs[0] / 1000000), (unsigned long long)(tierNs[1] / 1000000),
             (unsigned long long)(tierNs[2] / 1000000), (unsigned long long)(tierNs[3] / 1000000), tierChanges);
      for (unsigned k = 0; k < kPollTiers; ++k) tierNs[k] = 0;
      tierChanges = 0;
      fastNoted = false;
      lastLog = now;
      peakValid_ = false;   // the peak is per report window
    }
    first = false;
    uint64_t workNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - workStart, &workNs);
    noteSelf(workNs);
    // the base relaxed at rest (piano-thermal-irq)
    const uint32_t wait = pollPeriodMs(tier, irq::hookBase(base), gArgs.warmMs, gArgs.fastMs, gArgs.clockMs);
    pollWait(gArgs.androidArmed && wait > gArgs.tickMs ? gArgs.tickMs : wait);
  }
}
