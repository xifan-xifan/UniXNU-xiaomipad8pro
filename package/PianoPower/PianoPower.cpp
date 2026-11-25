// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoPower: battery and charging for macOS 27 on piano (SM8750), native at EL1. Design and test plan: docs/drivers/charging.md.
//
// The charger (the buck stage of the PMIH010x PMIC) belongs to the audio DSP's firmware; this processor reads it over SPMI and writes
// only its watchdog (below). The DSP is not running when our UEFI hands over, and without it the PMIC charges on whatever configuration
// the boot stages left (this side also writes the charging enable command: ChargerEnable.h). So there are two levels:
//   piano-power=1  Every 30 s one report on the console: the battery from its two fuel gauges (voltage, current, state of charge,
//                  temperature), the USB-C port from the charger PMIC's Type-C block (attached, a source's Rp), the charger watchdog
//                  and state, the DSP's state from SMEM. The DSP is not started; the only writes are the charger watchdog's (pet, and
//                  the bite's charging stop cleared: piano-power-wdog below), the SPMI observer channel's command and the four gauge
//                  pins' TLMM configuration (taken as GPIOs for the read, then given back).
//   piano-power=2  the same, and the charger: the DSP is started (its images from modem_a, PAS) with GLINK and the IPC router's service
//                  locator, then the charger properties are read every 5 s and the conservative policy of PianoMca.c is applied: input
//                  current limit by charger type (at most 1.5 A at 5 V), charge current 1.0 A through 48 degC then -0.3 A/degC from
//                  49 degC (0.5 A floor, or without a temperature), the charger's enable bit set if it is clear, back to the firmware's own 100 mA / 500 mA on
//                  detach. USB-C port notifications are acknowledged (and handed, read only, to the port-event clients:
//                  PortEvents.c, PianoDp). Nothing else is written.
// The charger watchdog (ChargerWdog.h, docs/drivers/charging-policy.md §18), at both levels: the boot stages leave the charger PMIC's
// BARK/BITE watchdog armed with "a bite disables charging", and nothing pets it natively: the charger stops at about 40 s of every
// boot. piano-power-wdog=2 (default) pets it every quarter of its bark time (4-15 s) from this thread and clears that one bit (MISC
// +0x53 bit 7, read-modify-write, read back), as Linux's SCHG host driver does; =1 pets only; =0 reads and logs only. Every write is
// logged with its before/after values; the battery limits (float, currents, JEITA, safety timer) are not written. 2026-11-12 dev14:
// the watchdog read off (not armed) at a boot that started with the charger already disabled.
// The charging enable command (ChargerEnable.h, charging-policy.md §19), at both levels until the DSP runs: piano-power-chgen=1
// (default) sets CHGR +0x42 bit 0 again when it reads clear while CHGR_CFG2 says the command decides, the charger reads disabled, a
// source is on the port and both packs are below 4350 mV, not full, 15-45 C (at most five a boot); =2 also pulses an enable that
// reads on (bit 0 cleared 2 s, then set; three a boot, two minutes apart); =0 reads only. Every change of CHGR's and MISC's registers
// between two reports is logged ("chgr: changed since"), and a change of the enable command that is not ours says so.
// The charger complex (ChargerFix.h, charging-policy.md §20), at both levels: its seven blocks' configuration logged at start, all of
// them read and diffed with every report, their status every 2 s for the first piano-power-chgwatch seconds (180; 0 none) and after
// each recovery step (the stop's moment), and the picture in the IORegistry (PianoCharger, PianoChargerText, PianoChargerChanges);
// read only. piano-power-chgfix=1 (level 1; default 0) restarts a charger that reads DISABLED with its enable command on: the
// command pulsed (2 s), then AICL restarted (MISC +0x44); =2 also suspends the USB input for 1 s (USBIN +0x40); each step judged by
// the next reading, six attempts a boot, every write read back and logged, no limit written.
// Further boot arguments: piano-power-period=S (report interval, default 30), piano-power-gauge=0 (no gauge reads),
// piano-power-write=0 (level 2 without the charger writes: the DSP runs on its own defaults), piano-power-hv=1 (level 2 also asks a PD
// source with a fixed 9 V PDO for 9 V and then allows 1.6 A), piano-power-fw=DIR (the DSP's images as DIR/adsp.mbn and
// DIR/adsp_dtb.mbn instead of modem_a). Every step logs "PianoPower: ...".
// piano-power-b4pd=1 is the attended, opt-in first B4 step: it selects ChargePolicy, enables fixed-PDO/PPS requests and permits the
// HVDCP3 0x2011=9000 request. It never changes the ADSP lifecycle; level 2's existing start/ready path remains the sole owner.
// At both levels the PMIC RTC is read with every report from the first one after IOBSD is published (RtcReadout.h,
// docs/drivers/rtc.md): its count, enable bit and the date with the offset of piano-rtc-offset=N or else the SDAM cell. Read only;
// the kernel's calendar from it is k27 0060 (piano-rtc=1).
// piano-power-policy=1 (level 2; default 0 keeps the built-in policy above exactly): ChargePolicy.c decides the charger writes instead
// (ChargeLink.h, docs/drivers/charging-policy.md §8): after charger_pd is up and the board configuration is through; the class rows
// (CDP 900 mA), the charge current derated by the warmest battery temperature (the firmware's, the gauges', PianoThermal's battery
// thermistor), the connector and charger thermistors' holds, re-sent on a new generation and after a wake, the sleep and shutdown
// handovers; piano-power-rp=1 the Type-C Rp current as the 5 V limit, in steps from the row up to piano-power-rp-max (3000 mA) while
// the bus holds and the input is used up (piano-power-ramp=0: at once within 1600 mA; charging-policy.md §15), piano-power-health=1
// stop at 80 %, resume at 75 % (piano-power-health-stop=N, -resume=N) with the charging-hold assertion. PianoPower then joins the
// power tree. A charger write's answer is awaited piano-power-write-ms (3000; reads 1 s) and one that comes later is still taken.
// With the policy, piano-power-kick=1 (the default; 0 leaves it out) restarts a charger that keeps both packs at 0 mA while the input
// has current to spare (0x2003 = 0, 5 s, then 1; charging-policy.md §17), and every report ends with a `charging:` balance line
// (input, battery, the system by difference) at every level.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "SpmiObserver.h"
#include "PonHistory.h"
#include "ChargerPmic.h"
#include "ChargerWdog.h"
#include "ChargerEnable.h"
#include "ChargerFix.h"
#include "Gauge.h"
#include "ChargePumpImpl.h"
#include "ChargePumpSession.h"
#include <sys/random.h>     // read_random: the MiPPS challenge and key index   // the pump session (charge/cp-sess): gates, PPS ramp, CC/CV, the exits   // the SC8541 charge pump driver (ChargePump.h), on pack A's bus
#include <sys/sysctl.h>
#include "GaugeAlerts.h"
#include "PianoBattery.h"
#include "Servreg.h"
#include "RtcReadout.h"
#include <PianoPowerContract.h>
#include <PianoBoardTemps.h>
extern "C" {
#include "PianoAdsp.h"
#include "AdspHealth.h"
#include "PianoMca.h"
#include "PortEvents.h"
#include "UsbcObservation.h"
#include "UsbcRelay.h"   // the USB-C owners for one client kext (package/include/PianoUsbcContract.h, PianoUsbcRelay.cpp)
}
#include "ChargeLink.h"
#include "ChargeContract.h"
#include "ChargeMipps.h"            // MiPPS (piano-power-mipps): the engine and its private key file
#include "PianoPowerMippsFile.h"    // generated by build-native-piano-power.sh: PIANO_POWER_MIPPS_FILE 1 with the private pin
#if PIANO_POWER_MIPPS_FILE
#include <PianoFirmware.h>
#include "PianoPowerFirmwarePins.h"
#endif

using namespace pianopower;

extern "C" {
// Post-mortem breadcrumb (also in the console log): the last start stage reached, negative when it failed there.
volatile int32_t piano_power_stage = 0;
}

namespace {

struct Args {
  uint32_t level, period, gauge, write, hv, b4pd, unsuspend, ps, psFloor, psInject, board, ucsi, pdoMca;
  bool psGuard;
  uint32_t policy, rp, health, healthStop, healthResume;
  uint32_t ramp, rpMax, writeMs, kick;
  uint32_t wdog, chgen, chgfix, chgwatch;
  uint32_t hvOrder, hvCaps, qc, fvHot;   // charge/pd1 (charging-policy.md §21), each 0 by default
  uint32_t cp;                           // piano-power-cp=1: the SC8541 driver probes and may be armed (ChargePump.h)
  uint32_t mipps;                        // piano-power-mipps (ChargeMipps.h), 0 by default
  bool hasRtcOffset;
  bool ponHistory;
  uint32_t rtcOffset;
};
Args gArgs;

// MiPPS's keys (ChargeMipps.h): read once from the private file through its PRIVATE pin, kept in kernel memory only, never logged.
PianoMippsKeys gMippsKeys;        // the keys (mippsLoadKeys), the engine's (serviceMipps)
char gMippsText[200] = "off (piano-power-mipps=0)";
PianoMipps gMipps;                // the engine (ChargeMipps.h) and its transport: globals (PianoPower's instance is at its size cap)
PianoMcaAux gMippsAux;
bool gMippsActive = false, gMippsWasAttached = false;
uint32_t gMippsAttachSeq = 0, gMippsTriedSeq = 0, gMippsUps = 0;
uint64_t gMippsWakeMs = 0;

uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}

void readArgs()
{
  gArgs.level = bootArg("piano-power", 0);
  gArgs.ponHistory = bootArg("piano-pon-history", 0) == 1;
  gArgs.period = bootArg("piano-power-period", 30);
  if (gArgs.period < 5) gArgs.period = 5;
  if (gArgs.period > 3600) gArgs.period = 3600;
  gArgs.gauge = bootArg("piano-power-gauge", 1);
  gArgs.write = bootArg("piano-power-write", 1);
  gArgs.hv = bootArg("piano-power-hv", 0);
  gArgs.b4pd = bootArg("piano-power-b4pd", 0);
  gArgs.policy = bootArg("piano-power-policy", 0);
  if (gArgs.b4pd) { gArgs.policy = 1; gArgs.hv = 2; }
  gArgs.unsuspend = bootArg("piano-power-unsuspend", 0);
  gArgs.board = bootArg("piano-power-board", 1);   // the stock board configuration at level 2 with writes (PianoMca.h)
  gArgs.ucsi = bootArg("piano-power-ucsi", 1);     // policy: periodic/event status observer; otherwise legacy per-generation probe
  // policy + observer: a PD sink snapshot takes its source list from the charger firmware (0x21011) when the PPM offers no PDO
  // details (this tablet's UCSI 1.1 PPM: bmOptionalFeatures 0); without it no PD source ever gets a contract (ChargeContract.h)
  gArgs.pdoMca = bootArg("piano-power-pdo-mca", 0);
  // charge/pd1, the Ubuntu port's buck fast-charge set in the strict session (policy=1), each off by default (the daily 5 V path):
  // the other 9 V object when the mode's own is missing, the stock 9 V rows, QC2/QC3 9 V, and the 48 degC warm float with its hold
  gArgs.hvOrder = bootArg("piano-power-hv-order", 0) == 1;
  gArgs.hvCaps = bootArg("piano-power-hv-caps", 0) == 1;
  gArgs.qc = bootArg("piano-power-qc", 0) == 1;
  gArgs.fvHot = bootArg("piano-power-fv-hot", 0) == 1;
  gArgs.cp = bootArg("piano-power-cp", 0) == 1;     // the SC8541 pump: off by default; writes also need machdep.piano_power_cp_arm=1
  gArgs.mipps = bootArg("piano-power-mipps", 0) == 1;   // MiPPS: load the private key file (the engine runs only with it)
  gArgs.ps = bootArg("piano-power-ps", 0);            // the battery for macOS (PianoBattery.cpp)
  gArgs.psFloor = bootArg("piano-power-ps-floor", 0); // the lowest percentage shown on battery (BatteryMap.h)
  if (gArgs.psFloor > 50) gArgs.psFloor = 50;
  gArgs.psInject = bootArg("piano-power-ps-inject", 0); // deterministic bad first publisher cycles for CHGUI A/B
  gArgs.psGuard = bootArg("piano-power-ps-guard", 1) != 0; // keep the plausibility gate on unless explicitly testing the bug
  gArgs.rp = bootArg("piano-power-rp", 0);                  // with it: the Type-C Rp current as the 5 V input limit (<= 1600 mA)
  gArgs.health = bootArg("piano-power-health", 0);          // with it: stop charging at 80 %, resume at 75 %
  gArgs.healthStop = bootArg("piano-power-health-stop", 80);
  gArgs.healthResume = bootArg("piano-power-health-resume", 75);
  // with piano-power-rp=1: the Rp limit in steps from the row (default) up to piano-power-rp-max (1500..3000, default 3000: the 3.0 A
  // Rp's current at 5 V); piano-power-ramp=0 takes it at once within the 1600 mA cap, as before (ChargePolicy.h cfg.ramp)
  gArgs.ramp = bootArg("piano-power-ramp", 1);
  gArgs.rpMax = bootArg("piano-power-rp-max", PIANO_CHG_RP_ABS_MAX);
  if (gArgs.rpMax < 1500) gArgs.rpMax = 1500;
  if (gArgs.rpMax > PIANO_CHG_RP_ABS_MAX) gArgs.rpMax = PIANO_CHG_RP_ABS_MAX;
  // how long a charger write's answer is awaited (reads: 1 s); 2026-11-12 the firmware answered limit writes after 1 s and applied them
  gArgs.writeMs = bootArg("piano-power-write-ms", 3000);
  if (gArgs.writeMs < 1000) gArgs.writeMs = 1000;
  if (gArgs.writeMs > 5000) gArgs.writeMs = 5000;
  // with the policy: the charging restart (ChargePolicy cfg.kick, charging-policy.md §17): 0x2003 = 0 then 1 when the packs stay at
  // 0 mA with input to spare; piano-power-kick=0 leaves it out
  gArgs.kick = bootArg("piano-power-kick", 1);
  // the charger PMIC's BARK/BITE watchdog (ChargerWdog.h, charging-policy.md §18): 0 read and log only, 1 pet it, 2 (default) pet it
  // and clear "a bite disables charging" (MISC +0x53 bit 7), as Linux's host driver for the SCHG family does
  gArgs.wdog = bootArg("piano-power-wdog", 2);
  if (gArgs.wdog > 2) gArgs.wdog = 2;
  // the charging enable command (ChargerEnable.h, charging-policy.md §19): 0 read and log only, 1 (default) set CHGR +0x42 bit 0 when
  // it reads clear with the charger disabled under command control (the battery below its limits, a source on the port), 2 also
  // pulse an enable that reads on while the charger reads disabled (bit 0 cleared for 2 s, then set)
  gArgs.chgen = bootArg("piano-power-chgen", 1);
  if (gArgs.chgen > 2) gArgs.chgen = 2;
  // the recovery of a charger that reads DISABLED with its enable command on (ChargerFix.h, charging-policy.md §20), level 1 only:
  // 0 (default) none, 1 step A (the enable command pulsed) then B (AICL restarted), 2 also C (the USB input suspended 1 s)
  gArgs.chgfix = bootArg("piano-power-chgfix", 0);
  if (gArgs.chgfix > 2) gArgs.chgfix = 2;
  // the 2 s watch of the charger's state from PianoPower's start, in seconds (0: none; read only)
  gArgs.chgwatch = bootArg("piano-power-chgwatch", 180);
  if (gArgs.chgwatch > 1800) gArgs.chgwatch = 1800;
  gArgs.rtcOffset = 0;
  gArgs.hasRtcOffset = PE_parse_boot_argn("piano-rtc-offset", &gArgs.rtcOffset, sizeof gArgs.rtcOffset);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoPower: %s\n", line);
}

void stage(int32_t value, const char *what)
{
  piano_power_stage = value;
  note("%s %d: %s", value < 0 ? "FAILED at stage" : "stage", value < 0 ? -value : value, what);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr/str, as PianoDwc3 does: every store from a general register.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }

struct Region {
  IOMemoryMap *map;
  uintptr_t va;
};

bool mapRegion(Region &r, uint64_t pa, uint64_t bytes)
{
  if (r.va) return true;
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  r.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  r.va = r.map ? uintptr_t(r.map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
  return r.va != 0;
}

// ------------------------------------------------------------------------------------------------------------ SPMI (read only)
Region gCore, gCnfg, gObsv;
uintptr_t windowBase(Window w) { return w == Window::core ? gCore.va : w == Window::cnfg ? gCnfg.va : gObsv.va; }
uint32_t ioRd(void *, Window w, uint32_t offset) { return rd32(windowBase(w) + offset); }
void ioWr(void *, Window w, uint32_t offset, uint32_t value) { wr32(windowBase(w) + offset, value); }
void ioDelay(void *, unsigned us) { IODelay(us); }
const Io gIo = {nullptr, ioRd, ioWr, ioDelay};

// The three write channels PianoPower maps: the charger MISC APID's (the watchdog, ChargerWdog.h; ChargerFix.h's AICL restart), the
// CHGR APID's (the charging enable command, ChargerEnable.h) and the USBIN APID's (ChargerFix.h's input suspend, piano-power-chgfix
// only), each only when its gates pass (chnls 0x0C500000 + 0x1000 * APID, data +0x10, command +0x00 with opcode 0, status +0x08:
// the arbiter v7 owner-channel write PianoDwc3 and PianoTorch make). The classes' allowlists decide what goes through; this only
// refuses an APID without a mapped channel.
constexpr uint64_t kSpmiChnls = 0x0c500000;
constexpr uint32_t kChnlStride = 0x1000;   // the channel's registers: PianoSpmi.h
struct Chnl {
  Region page;
  int apid;
};
constexpr unsigned kChnlSlots = 3;
Chnl gChnls[kChnlSlots] = {{{nullptr, 0}, -1}, {{nullptr, 0}, -1}, {{nullptr, 0}, -1}};   // [0] MISC's, [1] CHGR's, [2] USBIN's
bool mapChnl(unsigned slot, int apid)
{
  if (slot >= kChnlSlots || apid < 0) return false;
  if (gChnls[slot].page.va) return gChnls[slot].apid == apid;   // mapped already (one APID a slot)
  if (!mapRegion(gChnls[slot].page, kSpmiChnls + uint64_t(kChnlStride) * uint32_t(apid), 0x20)) return false;
  gChnls[slot].apid = apid;
  return true;
}
bool chnlWrite(void *, int apid, uint8_t reg, uint8_t value, uint32_t *status)
{
  const Chnl *c = nullptr;
  for (const Chnl &k : gChnls)
    if (apid >= 0 && k.apid == apid && k.page.va) c = &k;
  if (!c) return false;
  // The transaction is PianoSpmi.h's, under the SPMI lock every kext shares (the data, the command and the poll in one hold).
  const uintptr_t va = c->page.va;
  uint32_t st = 0;
  const pianospmi::Result r = pianospmi::write([va](uint32_t off) { return rd32(va + off); },
                                               [va](uint32_t off, uint32_t v) { wr32(va + off, v); },
                                               [](unsigned us) { IODelay(us); }, reg, &value, 1, &st);
  if (status) *status = st;
  return r == pianospmi::Result::ok;
}
const SpmiWriter gWriter = {nullptr, chnlWrite};

// ------------------------------------------------------------------------------------------------- the gauges' pins (TLMM)
// TLMM (DT pinctrl@f100000): pin n's configuration at n * 0x1000 (pull 1:0, function 5:2, drive 8:6, output enable 9), its input
// (bit 0) and output (bit 1) at +4 (UniXNU-sheng TouchBoard.h). For the read a pin is a GPIO with the pull-up on, its output value
// 0: a line is released (output disabled) or driven low (output enabled). The bits above 9 stay as they were.
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kTlmmStride = 0x1000, kTlmmPullUp = 3, kTlmmOe = 1u << 9;

struct Pin {
  unsigned gpio;
  Region page;
  uint32_t saved;
};
struct GaugeBus {
  const char *name;   // as the log calls it
  const char *where;  // the DT node
  Pin sda, scl;
};
GaugeBus gBuses[2] = {
    {"A", "i2c@a94000 qup1_se5", {52, {}, 0}, {53, {}, 0}},
    {"B", "i2c@88c000 qup2_se3", {12, {}, 0}, {13, {}, 0}},
};

uint32_t gpioCfg(const Pin &p, bool drive) { return (p.saved & ~0x3ffu) | kTlmmPullUp | (drive ? kTlmmOe : 0); }
bool mapPin(Pin &p) { return mapRegion(p.page, kTlmm + uint64_t(p.gpio) * kTlmmStride, 8); }
void takePin(Pin &p)
{
  p.saved = rd32(p.page.va);
  wr32(p.page.va + 4, 0);                  // output value 0, used only while the output is enabled
  wr32(p.page.va, gpioCfg(p, false));      // a GPIO, released
}
void givePin(Pin &p) { wr32(p.page.va, p.saved); }

void busSda(void *ctx, bool release) { Pin &p = static_cast<GaugeBus *>(ctx)->sda; wr32(p.page.va, gpioCfg(p, !release)); }
void busScl(void *ctx, bool release) { Pin &p = static_cast<GaugeBus *>(ctx)->scl; wr32(p.page.va, gpioCfg(p, !release)); }
bool busReadSda(void *ctx) { return rd32(static_cast<GaugeBus *>(ctx)->sda.page.va + 4) & 1; }
bool busReadScl(void *ctx) { return rd32(static_cast<GaugeBus *>(ctx)->scl.page.va + 4) & 1; }
void busDelay(void *, unsigned us) { IODelay(us); }

// The SC8541 driver while piano-power-cp=1 (set in start): its kick and guard go ahead of any transaction on pack A's bus.
pianopower::Sc8541 *gCpHook = nullptr;   // the driver itself (allocated by probeCp: PianoPower's instance is at its 16 KiB limit)
uint32_t gCpFaults = 0;                  // every fault bit seen this boot (published)
void cpHookFn(void *ctx, I2cBus &bus) { static_cast<pianopower::Sc8541 *>(ctx)->beforeBusTransaction(bus); }
uint64_t cpNowMs(void *) { return nowMs(); }
// machdep.piano_power_cp_arm: the per-boot root arm of the pump (0/1), read by the worker every pass.
int gCpArm = 0;
int cpArmSysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
  int v = __atomic_load_n(&gCpArm, __ATOMIC_ACQUIRE);
  const int err = sysctl_handle_int(oidp, &v, 0, req);
  if (err || !req->newptr) return err;
  if (v != 0 && v != 1) return EINVAL;
  __atomic_store_n(&gCpArm, v, __ATOMIC_RELEASE);
  return 0;
}
struct sysctl_oid gCpArmOid;
bool gCpArmRegistered = false;
// The pump session (ChargePumpSession.h), allocated with the driver (probeCp): its state stays out of PianoPower's instance.
struct CpSession {
  PianoCpConfig cfg{};
  PianoCpState st{};
  PianoCpPlan plan{};
  PianoCpSys sys{};
  piano_cp_in drv{};
  uint32_t attachSeq = 0, faultsCarry = 0;
  bool wasAttached = false;
  bool ppsPending = false, qcPending = false;   // a request of the session queued and not answered
  uint32_t ppsWord = 0, ppsTaken = 0;
  int32_t qcTaken = 0;
  uint64_t reqMs = 0, nextMs = 0, packsMs = 0, boardMs = 0;
  GaugeReading packs[2];
  piano_board_temps board{};    // PianoThermal's last round, read by the session's own clock (not the policy's snapshot)
  bool boardOk = false;
  PortState port{};             // the charger PMIC's Type-C block, read with it
  bool portOk = false;
};
CpSession *gCpSess = nullptr;

GaugeReading readBus(GaugeBus &b)
{
  takePin(b.scl);
  takePin(b.sda);
  IODelay(20);
  const I2cIo io = {&b, busSda, busScl, busReadSda, busReadScl, busDelay};
  I2cBus bus(io);
  if (&b == &gBuses[0] && gCpHook) bus.setHook(cpHookFn, gCpHook);
  GaugeReading g = readGauge(bus);
  givePin(b.sda);
  givePin(b.scl);
  if (g.wordRereads)
    IOLog("PianoPower: GaugeWordReobserveV1 pack=%s first=%#x/%u tried=%#x credible=%#x reads=%u final-valid=%u soc=%d mv=%d error=%u failed=%#x\n",
          b.name, g.firstRetryCommand, g.firstRetryValue, g.wordRetryMask, g.wordRetryValid, g.wordRereads,
          unsigned(g.ok), g.socPct, g.voltageMv, unsigned(g.error), g.failedCommand);
  return g;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ the service
class PianoPower : public IOService {
  OSDeclareDefaultStructors(PianoPower)

  // SPMI
  Arbiter fArbiter;
  ChargerLayout fCharger;
  bool fSpmi = false;
  ChargerWdog fWdog;   // the charger's watchdog (ChargerWdog.h)
  ChargerEnable fChgEn;   // the charging enable command (ChargerEnable.h)
  RtcReading fRtc;
  bool fRtcProbed = false;
  bool fPdosLine = false;   // the PD source's PDO list has been logged this charger generation (logMcaEvents)
  // gauges
  bool fGauges = false;
  GaugeReading fPack[2];
  PackReading fBattery;
  // the DSP and its charger properties (level 2)
  PianoMca fMca;
  PianoChargeContract fContract{};
  uint8_t fContractLoggedStage = 0xff;
  uint32_t fContractLogCount = 0;
  uint64_t fContractRefresh = 0;
  bool fContractWasFresh = false;
  uint32_t fContractSeenGeneration = 0;
  // piano-power-pdo-mca: the firmware list offered, and the contracts it made, each logged on change (bounded)
  uint8_t fPdoOfferLogs = 0, fPdoOfferLoggedN = 0xff, fPdoContractLogs = 0, fPdoContractLoggedValid = 0xff;
  uint32_t fPdoContractLoggedRdo = 0;
  IOLock *fConfigLock = nullptr;
  struct ChargeRequest {uint32_t enabled, stop, resume, mode, inhibit, revision;} fConfigRequest{};
  bool fConfigPending = false;
  uint32_t fConfigRevision = 0;
  uint64_t fInhibitUntil = 0;
  int32_t fConfigError = 0;
  uint32_t fStatusSeq = 0;
  uint8_t fCpCtrl = 0;
  void serviceCp(uint64_t now);
  void runCpSession(uint64_t now, I2cBus &bus);
  void serviceMipps(uint64_t now);
  bool mippsEligibleNow() const;
  void cpAnswered();
  void probeCp();
  GaugeAlerts fAlerts;
  uint32_t fSafetyValid = 0;
  uint64_t fSafetyMs = 0;
  bool fHandoverFault = false;
  void applyChargeRequest(uint64_t now);
  void sampleSafety(uint64_t now);
  void publishCharge(uint64_t now, const piano_board_temps *board);

  PianoAdspChannel *fChannel = nullptr;
  IOSimpleLock *fRingLock = nullptr;
  static constexpr unsigned kRing = 8;
  struct {uint32_t length; uint8_t data[PIANO_ADSP_MAX_MESSAGE];} fRing[kRing];
  unsigned fRingHead = 0, fRingCount = 0;
  uint32_t fDropped = 0;
  volatile uint32_t fLinkUp = 0, fLinkChanged = 0;
  bool fWasUp = false;
  // the charger_pd gate (serviceMca): the channel's state, since when, whether the 60 s fallback was taken, the generation linked
  bool fChannelUp = false, fGateTimedOut = false;
  uint64_t fChannelSinceMs = 0;
  uint32_t fPdUpsLinked = 0;
  uint64_t fRequestDue = 0, fPanDue = 0, fUcsiDue = 0, fCycleDue = 0, fReadyWarnDue = 0;
  uint32_t fUcsiNotesLogged = 0;
  uint32_t fIgnoredSeen = 0, fIgnoredLogged = 0;   // the charger answers that matched no request: the first 8 are logged
  uint64_t fStartMs = 0;

  bool probeSpmi();
  void mippsLoadKeys();
  void surveyChargerBlocks();
  void startWdog();
  void wdogWriteLine(const char *what, const WdogWrite &r);
  void wdogLine(char *line, size_t size, uint64_t t);
  void startChgen(const WdogView &v);
  void chgenWriteLine(uint64_t t, const ChgEnWrite &r);
  void chgenStep(uint64_t t, const PortState *p, char *why, size_t whySize);
  char fChgenWhy[96] = "";   // the last report's reason for no enable write while the charger read disabled
  ChgEnInputs chgenInputs(const PortState *p);
  // The charger complex (ChargerFix.h, charging-policy.md §20): its snapshot with every report, the 2 s watch of its state, the
  // recovery (piano-power-chgfix), and the picture in the IORegistry (PianoCharger, PianoChargerText, PianoChargerChanges).
  ChargerComplex fCx;
  ChargerWatch fWatch;
  ChargerFix fFix;
  CxSnapshot fCxLast;
  bool fCxHave = false;
  static constexpr unsigned kChanges = 8;
  char fChanges[kChanges][300] = {};
  unsigned fChangesNext = 0, fChangesCount = 0;
  void startFix();
  void fixPoll(uint64_t now);
  void fixEventLine(uint64_t now, const FixEvent &ev);
  void watchLine(const CxTransition &x);
  void keepChange(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void publishCharger();
  void readRtcLine(char *line, size_t size);
  void surveyAdsp();
  void probeGauges();
  void sampleGauges();
  void report();
  void startCharger();
  void serviceMca(uint64_t now);
  void stepQuiet(uint64_t now);
  // The quiet flag for the DSP's other clients (PianoAdspContract.h): raised per charger generation until it is configured.
  PowerQuiet fQuiet = {};
  static constexpr uint32_t kQuietCapMs = 60000;
  void pumpMca(uint64_t now);
  void logMcaEvents();
  void offerFirmwarePdos();
  void logFirmwareContract();
  void mcaLine(char *line, size_t size);
  void diagLine(char *line, size_t size);
  void diagPortLine(char *line, size_t size, bool board);
  void publishBattery(uint64_t t, const PortState *port);
  void run();
  static void thread(void *self, wait_result_t);
  static void channelState(void *context, int up);
  static void channelReceive(void *context, const uint8_t *data, uint32_t length);

  // piano-power-policy=1 (ChargeLink.h): ChargePolicy decides the charger writes. All of it runs on the thread; setPowerState and
  // systemWillShutdown only raise the asks below and wait for (or are acknowledged by) the thread.
  bool fPolicyOn = false, fPolicyEnded = false;
  ChargeLink fLink;
  uint64_t fPackMs = 0;                       // when the packs were last read (the decisions take them for 3 periods)
  uint32_t fPsPublishCycles = 0;              // publisher cycles, including deterministic ps-inject A/B cycles
  IOService *fThermal = nullptr;              // PianoThermal, for its board temperatures (looked up every 10 s until found)
  uint64_t fThermalLookupMs = 0;
  bool fThermalNoted = false;
  uint32_t fBoardStateShown = ~0u, fBoardValidShown = ~0u;
  uint64_t fPdUpMs = 0;                       // when charger_pd was seen coming up (the charger thermistor counts from then)
  uint64_t fChargerEarlyNotedMs = ~0ull;      // the fPdUpMs whose too-early charger reading was logged
  uint32_t fPdUpsSeen = 0;
  bool fPdWasUp = false;
  IOPMDriverAssertionID fHoldId = 0;
  bool fHoldOn = false, fHoldFailed = false;
  volatile uint32_t fSleepAsk = 0, fShutdownAsk = 0, fShutdownDone = 0, fWakes = 0;
  bool fWentOff = false, fHandover = false, fHandoverAcked = false, fShutdownSeen = false;
  SleepReports fSleepReports;   // no report between the sleep handover's acknowledgement and the wake (ChargeLink.h)
  uint64_t fHandoverUntil = 0, fShutdownUntil = 0;
  void stepPolicy(uint64_t now);
  void logPolicy(const LinkOutcome &o, uint64_t now);
  void policyLine(char *line, size_t size);
  void inputLine(char *line, size_t size);
  void chargingLine(char *line, size_t size);
  bool boardTemps(piano_board_temps &t, uint64_t now);
  void setHold(bool on);
  void relinkMca(uint64_t now, const char *why);
  bool policyAsked();

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
  void systemWillShutdown(IOOptionBits specifier) override;
};

OSDefineMetaClassAndStructors(PianoPower, IOService)

IOService *PianoPower::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if ((!gArgs.level && !gArgs.ponHistory) || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoPower::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  if (!gArgs.level) {
    note("start: PON history only; charger/gauges/ADSP untouched");
    probeSpmi();
    registerService();
    return true;
  }
  note("start: piano-power=%u period %u s, gauges %s; %s", gArgs.level, gArgs.period, gArgs.gauge ? "on" : "off",
       gArgs.level >= 2 ? "level 2: the DSP's charger firmware is started and its charger properties are managed"
                        : gArgs.chgfix ? "level 1: the DSP is not started; the only PMIC writes are the charger watchdog's "
                                         "(piano-power-wdog), the charging enable command's (piano-power-chgen) and the recovery's "
                                         "(piano-power-chgfix)"
                        : gArgs.wdog || gArgs.chgen ? "level 1: the DSP is not started; the only PMIC writes are the charger watchdog's "
                                                      "(piano-power-wdog) and the charging enable command's (piano-power-chgen)"
                                                    : "level 1: read only (no PMIC register written, the DSP is not started)");
  fPolicyOn = gArgs.level >= 2 && gArgs.policy != 0;
  if (gArgs.level >= 2 && !fPolicyOn)
    note("level 2 policy: %s%s", gArgs.write ? "input limit by charger type (<= 1.5 A at 5 V), charge current 1.0 A in 15-48 C"
                                             : "no charger writes (piano-power-write=0): the DSP keeps its own 100 mA input limit",
         gArgs.write && gArgs.hv ? ", 9 V from a PD source's fixed PDO (piano-power-hv=1, then <= 1.6 A)" : "");
  if (gArgs.level >= 2 && gArgs.write && gArgs.unsuspend && !fPolicyOn)
    note("piano-power-unsuspend=1: the input-suspend flag (0x2019) is written to 0 when it reads non-zero (never to 1)");
  if (fPolicyOn) {
    note("piano-power-policy=1: ChargePolicy decides the charger writes%s, after msm/adsp/charger_pd is up and the board "
         "configuration is through: input limit by class (SDP 500, CDP 900, DCP 1500, PD its 5 V offer <= 1500 mA), charge current "
         "by class (<= 1500 mA) derated -300 mA/C above 48 C (500 mA below 15 C or without a temperature), a connector at 60 C -> "
         "100 mA input, the charger at 85 C -> 500 mA; re-sent on a new charger generation and after a wake",
         gArgs.write ? "" : " for the log only (piano-power-write=0: nothing is sent)");
    if (gArgs.b4pd)
      note("piano-power-b4pd=1: fixed PD 0x21018=9000, PPS 0x21017=(9000/20)<<16|(mA/50), and QC3 0x2011=9000 are opt-in; QC3 input <=1600 mA; warm-cell FCC derating starts at 48 C");
    if (gArgs.rp && gArgs.ramp)
      note("piano-power-rp=1 piano-power-ramp=1: a source without PD that advertises 1.5 or 3.0 A on its Rp is stepped from its class "
           "row to that current, at most %u mA (piano-power-rp-max): 1500, 2000, 2500, 3000, each held 10 s with the bus at 4.65 V or "
           "more and the input used up; above 1500 mA only while a connector temperature (PianoThermal) reads below 50 C; a sag below "
           "4.5 V, a detach within 30 s of a step or a refused step: one step back, guarded retry after %u s", gArgs.rpMax,
           PIANO_CHG_RAMP_RETRY_MS / 1000);
    else
      note("piano-power-rp=%u: %s", gArgs.rp ? 1u : 0u,
           gArgs.rp ? "a source without PD that advertises 1.5 or 3.0 A on its Rp gets that as its 5 V input limit, at most 1600 mA, "
                      "while a connector temperature (PianoThermal) reads below 50 C; a bus sag below 4.5 V under it ends the trust "
                      "(piano-power-ramp=0)"
                    : "the Type-C Rp current is not trusted: the class rows (a CDP gets 900 mA whatever its Rp says)");
    note("charger writes: an answer is awaited %u ms (reads 1000 ms); one that comes later is still taken and logged", gArgs.writeMs);
    note("piano-power-kick=%u: %s", gArgs.kick && gArgs.write ? 1u : 0u,
         gArgs.kick && gArgs.write
             ? "the charging restart: when both packs read within 30 mA of 0 on two readings 20 s apart while the bus has 150 mA or "
               "more to spare under its limit (below 95 %, 30 mV under the float voltage, 15-45 C, the firmware's own enable 1), "
               "0x2003 = 0 then 1 and the charge current again; three per attach, 2 min apart"
             : "no charging restart (0x2003 is written 0 only by battery health)");
    note("piano-power-health=%u%s; piano-power-hv=%u%s", gArgs.health ? 1u : 0u,
         gArgs.health ? ": charging stops at the stop charge (piano-power-health-stop, 80 %) and resumes at the resume charge "
                        "(-resume, 75 %), the higher pack's, with a charging hold against idle sleep below the stop"
                      : (gArgs.kick && gArgs.write ? " (battery health off: 0x2003 = 0 only within a charging restart)"
                                                   : " (battery health off: 0x2003 is never written 0)"),
         gArgs.hv > 2 ? 2u : gArgs.hv, gArgs.hv ? " (9 V asked from a PD source 15 s after the attach)" : "");
    if (gArgs.unsuspend) note("piano-power-unsuspend=1 is the built-in policy's: not used with piano-power-policy=1");
    note("piano-power-pdo-mca=%u: %s [PianoPdoFirmwareV1]", gArgs.pdoMca && gArgs.ucsi ? 1u : 0u,
         gArgs.pdoMca && gArgs.ucsi
             ? "a PD sink status without UCSI source PDOs takes the charger firmware's source list (0x21011, PD state SNK_Ready, "
               "vSafe5V first, fixed ascending before PPS) with a second agreeing status read; RDO, opmode and role stay UCSI's"
             : "a PD contract needs UCSI source PDOs (this PPM offers none: PD sources stay without a contract)");
    note("charge/pd1 [PianoFastChargeV1]: piano-power-hv-order=%u piano-power-hv-caps=%u piano-power-qc=%u piano-power-fv-hot=%u "
         "(order: mode 1 the fixed 9 V PDO else an APDO, mode 2 the APDO else the PDO; caps: 9 V rows PD 1600/3000, PPS 2500/3500, "
         "QC3 1600/3500, QC2 1200/2600 mA, FCC <= 2 x ICL; qc: 0x2011 9 V for type 5-8 with a non-PD UCSI status; fv-hot: 4060 mV "
         "from 48 C, held at that float; all within the strict session's gates)", gArgs.hvOrder, gArgs.hvCaps, gArgs.qc, gArgs.fvHot);
  }
  if (gArgs.ps)
    note("piano-power-ps=1: the battery is published to macOS as AppleSmartBattery (IOPMPowerSource) from the gauges and the "
         "Type-C state, refreshed every %u s%s%s%s", gArgs.period, gArgs.psFloor ? "; the percentage shown on battery stays at least " : "",
         gArgs.psFloor ? "the floor (piano-power-ps-floor)" : "",
         gArgs.psInject ? (gArgs.psGuard ? "; ps-inject is guarded" : "; ps-inject guard DISABLED (bug reproduction)") : "");
  if (gArgs.psInject)
    note("piano-power-ps-inject=%u: first cycle gets garbage FullChargeCapacity, second gets zero remaining capacity; later cycles are real",
         gArgs.psInject);
  setProperty("piano-power", gArgs.level, 32);
  fRingLock = IOSimpleLockAlloc();
  fConfigLock = IOLockAlloc();
  setProperty("PianoChargeBuild", "PianoBuckContractV1");
  note("buck: PianoBuckContractV1 status=PianoBuckStatusV2 alerts=PianoBuckAlertsV5 HV_ICL=1600 FCC=1500 FV=4400 CP=read-only");
  thread_t t = nullptr;
  if (!fConfigLock || !fRingLock || kernel_thread_start(&PianoPower::thread, this, &t) != KERN_SUCCESS) {
    stage(-1, "no thread");
    return true;   // stays attached and does nothing more
  }
  thread_deallocate(t);
  registerService();
  // With the policy the charger is handed over before a system sleep and told everything again after it (docs/power.md §4), and a
  // stop of ours is lifted at shutdown: setPowerState and systemWillShutdown need the power tree.
  if (fPolicyOn) {
    const IOReturn r = pianopower::join(this);
    note("policy: joined the power tree (%#x): sleep hands the charger over, a wake sends everything again", r);
  }
  return true;
}

// Only this complete, versioned transaction is writable. Hardware writes remain
// in the worker and pass both existing policy and codec allowlists.
IOReturn PianoPower::setProperties(OSObject *properties)
{
  if (!fPolicyOn || !fConfigLock || !gArgs.write) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
    return kIOReturnNotPrivileged;
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d || d->getCount() != 6) return kIOReturnBadArgument;
  static const char *const keys[] = {"ChargeLimitEnabled", "ChargeLimitStop", "ChargeLimitResume", "ChargeMode",
                                    "ChargeTestInhibitSeconds", "ChargeConfigRevision"};
  uint32_t v[6];
  for (unsigned i = 0; i < 6; ++i) {
    OSNumber *n = OSDynamicCast(OSNumber, d->getObject(keys[i]));
    if (!n || n->unsigned64BitValue() > 0xffffffffu) return kIOReturnBadArgument;
    v[i] = n->unsigned32BitValue();
  }
  PianoChargeConfig checked;
  piano_charge_config_default(&checked);
  if (v[4] > 180 || piano_charge_config_transaction(&checked, v[0], v[1], v[2], v[3], v[4] != 0))
    return kIOReturnBadArgument;
  IOLockLock(fConfigLock);
  if (__atomic_load_n(&fConfigPending, __ATOMIC_ACQUIRE) || v[5] != fConfigRevision) {
    IOLockUnlock(fConfigLock);
    return kIOReturnBusy;
  }
  fConfigRequest = {v[0], v[1], v[2], v[3], v[4], v[5]};
  __atomic_store_n(&fConfigPending, true, __ATOMIC_RELEASE);
  IOLockUnlock(fConfigLock);
  thread_wakeup((event_t)this);
  return kIOReturnSuccess;
}

void PianoPower::applyChargeRequest(uint64_t now)
{
  if (fInhibitUntil && now >= fInhibitUntil) {
    fInhibitUntil = 0;
    fLink.cfg.admin_stop = 0;
    fLink.requestDecision();
    note("buck: inhibit expired=1 restore=health");
  }
  if (!__atomic_load_n(&fConfigPending, __ATOMIC_ACQUIRE)) return;
  IOLockLock(fConfigLock);
  const ChargeRequest q = fConfigRequest;
  const int r = piano_charge_config_transaction(&fLink.cfg, q.enabled, q.stop, q.resume, q.mode, q.inhibit != 0);
  fConfigError = r;
  if (!r) {
    fInhibitUntil = q.inhibit ? now + uint64_t(q.inhibit) * 1000 : 0;
    ++fConfigRevision;
    fLink.cancelQueued(fMca);
    fLink.requestDecision();
  }
  __atomic_store_n(&fConfigPending, false, __ATOMIC_RELEASE);
  IOLockUnlock(fConfigLock);
  note("buck: config rev=%u enabled=%u stop=%u resume=%u mode=%u inhibit=%u error=%d", fConfigRevision,
       fLink.cfg.limit, fLink.cfg.stop_pct, fLink.cfg.resume_pct, fLink.cfg.hv, q.inhibit, r);
}

// Same worker and pin ownership as the gauge reads: no parallel I2C client and
// no CP or fuel-gauge configuration writes. The one-byte command selects a
// read-only register; SC8541 CTRL2 bit4 is observed, never cleared here.
void PianoPower::sampleSafety(uint64_t now)
{
  fSafetyValid = 0;
  fAlerts.beginSample();
  for (unsigned i = 0; i < 2; ++i) {
    GaugeBus &b = gBuses[i];
    if (!b.sda.page.va || !b.scl.page.va) continue;
    takePin(b.scl); takePin(b.sda); IODelay(20);
    const I2cIo io = {&b, busSda, busScl, busReadSda, busReadScl, busDelay};
    I2cBus bus(io);
    if (!i && gCpHook) bus.setHook(cpHookFn, gCpHook);
    if (!i && bus.readRegisterByte(0x6a, 0x0f, &fCpCtrl) == I2cResult::ok) fSafetyValid |= 1;
    fSafetyValid |= fAlerts.readPack(bus, i);
    givePin(b.sda); givePin(b.scl);
  }
  fAlerts.sampled(fSafetyValid);
  fSafetyValid = (fSafetyValid & 1) | fAlerts.valid;
  fSafetyMs = now;
}

void PianoPower::publishCharge(uint64_t now, const piano_board_temps *board)
{
  const PianoChargeState &s = fLink.st;
  const PianoChargePlan &p = fLink.plan;
  const PianoChargeInput &x = fLink.in;
  const PianoChargeContract &c = fContract;
  const bool fvValid = (x.valid & PIANO_CHG_IN_FV) && x.fv_raw >= 3600 && x.fv_raw <= 6400;
  const uint32_t fvMv = fvValid ? uint32_t(x.fv_raw + 3600) / 2 : 0;
  const bool enabledValid = piano_mca_has(&fMca, McaChargeEnable);
  const int32_t enabled = enabledValid ? fMca.value[McaChargeEnable] : -1;
  const bool applied = !s.pending && !fMca.queued && enabledValid && enabled == int(p.charge);
  OSDictionary *d = OSDictionary::withCapacity(80);
  if (!d) return;
  auto n = [&](const char *key, uint64_t value) {
    OSNumber *v = OSNumber::withNumber(value, 64);
    if (v) {d->setObject(key, v); v->release();}
  };
  n("Version", 1); n("Seq", ++fStatusSeq); n("UptimeMs", now);
  n("Generation", c.generation); n("LinkGeneration", fMca.links); n("ContractSeq", c.seq);
  n("RawType", uint32_t(piano_mca_has(&fMca, McaType) ? fMca.value[McaType] : -1));
  n("EffectiveType", uint32_t(x.type)); n("Connector", c.connector); n("Connected", c.connected);
  n("Sink", c.sink); n("PD", c.pd); n("ContractValid", piano_contract_fresh(&c, now));
  n("ContractStatusValid", c.status_valid); n("RDO", c.rdo); n("Protocol", c.protocol); n("ContractMV", c.mv); n("ContractMA", c.ma);
  n("PackSampleMs", fPackMs); n("PackAgeMs", now >= fPackMs ? now - fPackMs : ~0u);
  n("SourceMA", c.source_ma); n("SourcePDOCount", c.pdos); n("UCSIVersion", c.version); n("CCI", c.cci);
  n("ContractNotifyPending", c.notify_pending); n("ContractNotifyMs", c.notify_ms);
  n("UCSIStage", c.stage); n("UCSIInFlight", c.in_flight); n("UCSIQuarantined", c.quarantined);
  n("UCSILastCommand", c.command); n("UCSIControlReadback", c.control_readback);
  n("UCSIInitialized", c.initialized); n("UCSIEnabled", c.enabled); n("UCSINotifies", c.notifies);
  n("UCSIReads", c.reads); n("UCSIReadReplies", c.read_replies); n("UCSIWrites", c.writes); n("UCSIWriteReplies", c.write_replies);
  n("UCSIResets", c.resets); n("UCSITimeoutLayer", c.timeout_layer); n("UCSIRingDropped", fDropped);
  n("UCSIAttributes", c.capabilities); n("UCSIFeatures", c.features); n("UCSIConnectors", c.cap_connectors);
  n("UCSIMessageLength", c.message_length); n("UCSIMessageCommand", c.message_command);
  n("UCSIMessageIn0", c.message_in[0]); n("UCSIMessageIn1", c.message_in[1]);
  n("UCSIMessageIn2", c.message_in[2]); n("UCSIMessageIn3", c.message_in[3]);
  n("ContractTimeouts", c.timeouts); n("ContractFailures", c.failures);
  n("SourcePDOOrigin", c.pdo_origin); n("FirmwarePDOFallback", c.pdo_fallback); n("FirmwarePDOOffer", c.offer_n);
  n("FirmwarePDOOffers", c.offers); n("FirmwarePDORejects", c.offer_rejects); n("FirmwarePDOWithdrawals", c.offer_withdrawals);
  for (unsigned i = 0; i < 7; ++i) {
    char key[24];
    snprintf(key, sizeof key, "FirmwarePDO%u", i + 1);
    n(key, i < c.offer_n ? (uint64_t(c.offer_pdo[i][0]) << 32 | uint64_t(c.offer_pdo[i][1]) << 16 | c.offer_pdo[i][2]) : 0);
  }
  n("RequestedVoltageProperty", s.volt_want_prop); n("RequestedVoltageValue", s.volt_want_value);
  n("AcknowledgedVoltageProperty", s.volt_set_prop); n("AcknowledgedVoltageValue", s.volt_set_value);
  n("VoltagePending", s.contract_transition || (s.pending & (1u << PIANO_CHG_S_VOLT)));
  n("ModePending", s.contract_mode_change); n("RestoreRequired", s.volt_want_prop == PIANO_CHG_PD_FIXED && s.volt_want_value == 5000 && x.bus_mv >= 6000);
  n("ContractError", uint32_t(c.last_error)); n("ContractAgeMs", c.updated_ms && now >= c.updated_ms ? now - c.updated_ms : ~0u);
  for (unsigned i = 0; i < 7; ++i) {char key[20]; snprintf(key, sizeof key, "SourcePDO%u", i + 1); n(key, c.source_pdo[i]);}
  n("RpMA", x.rp_ma); n("TypeCSource", x.typec_source);
  n("InputValid", x.valid); n("Safety", x.safety); n("SafetyValid", fSafetyValid);
  n("CPControl", fCpCtrl);
  if (const pianopower::Sc8541 *cp = gCpHook) {
    n("CPPresent", cp->present()); n("CPArmed", cp->arm.armed()); n("CPSwitching", cp->switching()); n("CPLost", cp->lost());
    n("CPFaults", gCpFaults); n("CPBusOCP", cp->busOcpMa());
    n("CPKickAgeMs", cp->lastKickMs() && now >= cp->lastKickMs() ? now - cp->lastKickMs() : ~0u);
  }
  n("LPDValid", piano_mca_has(&fMca, McaLpd)); n("LPD", uint32_t(fMca.value[McaLpd]));
  n("DTPT", fAlerts.active()); n("SafetyAlarmMask", fAlerts.alarms);
  n("SafetyReobserved", fAlerts.retried); n("SafetyReobserveValid", fAlerts.retryValid); n("SafetyRereads", fAlerts.rereads);
  n("VBUS", uint32_t(x.bus_mv)); n("IBUS", uint32_t(x.ibus_ma));
  n("RequestedICL", p.icl_ma); n("AppliedICL", s.icl_set); n("ICLReadback", uint32_t(x.icl_ma)); n("AICL", uint32_t(x.aicl_ma));
  n("RequestedFCC", p.fcc_ma); n("AppliedFCC", s.fcc_set); n("FCCReadbackValid", 0); n("FVTarget", p.fv_mv ? p.fv_mv : 4400); n("FVValid", fvValid); n("FVRaw", uint32_t(x.fv_raw)); n("FV", fvMv);
  n("HealthEnabled", fLink.cfg.limit); n("HealthStop", fLink.cfg.stop_pct); n("HealthResume", fLink.cfg.resume_pct);
  n("Mode", fLink.cfg.hv); n("HealthHold", s.hold); n("HealthDesired", p.charge); n("HealthPending", !applied);
  n("HealthApplied", applied); n("HealthReason", p.why_off); n("EnableReadback", uint32_t(enabled));
  n("HealthInhibitSeconds", fInhibitUntil > now ? (fInhibitUntil - now + 999) / 1000 : 0);
  IOLockLock(fConfigLock);
  n("ConfigRevision", fConfigRevision); n("HealthGeneration", fConfigRevision); n("ConfigPending", __atomic_load_n(&fConfigPending, __ATOMIC_ACQUIRE));
  n("ConfigError", uint32_t(fConfigError));
  IOLockUnlock(fConfigLock);
  n("HandoverFault", fHandoverFault); n("PendingSlots", s.pending);
  n("LastACKProperty", fMca.last_property); n("LastACKValue", fMca.last_value); n("LastACKError", uint32_t(fMca.last_result));
  n("Timeouts", fMca.timeouts); n("Fallback", p.fallback);
  for (unsigned i = 0; i < 2; ++i) {
    const GaugeReading &g = fPack[i]; char key[32];
#define PACKNUM(name, value) snprintf(key, sizeof key, "Pack%u" name, i); n(key, uint32_t(value))
    PACKNUM("Valid", g.ok); PACKNUM("SOC", g.socPct); PACKNUM("MA", g.currentMa); PACKNUM("TempDC", g.tempDeciC);
    PACKNUM("MV", g.voltageMv); PACKNUM("Error", g.error); PACKNUM("FailedCommand", g.failedCommand);
    PACKNUM("ISC", fAlerts.isc[i]); PACKNUM("SOA", fAlerts.soa[i]);
    PACKNUM("FirstISC", fAlerts.firstIsc[i]); PACKNUM("FirstSOA", fAlerts.firstSoa[i]);
#undef PACKNUM
  }
  for (unsigned i = 0; i < PIANO_BOARD_TEMP_COUNT; ++i) {
    char key[32]; snprintf(key, sizeof key, "Board%uAgeMs", i); n(key, board ? board->age_ms[i] : ~0u);
  }
  n("BoardValid", board ? board->valid : 0);
  // the charge indicator (charge/pd1; the Ubuntu port's top bar: path, request, input and net battery power)
  const int path = piano_charge_path(&s, &p, &x);
  const int32_t inMw = (x.valid & PIANO_CHG_IN_BUS) && (x.valid & PIANO_CHG_IN_IBUS) && x.bus_mv >= 4000 ?
                       int32_t(int64_t(x.bus_mv) * x.ibus_ma / 1000) : 0;
  int32_t batMw = 0;
  for (unsigned i = 0; i < 2; ++i)
    if (fPack[i].ok) batMw += int32_t(int64_t(fPack[i].voltageMv) * fPack[i].currentMa / 1000);
  n("ChargePath", uint32_t(path)); n("HV", s.hv); n("HVPPS", s.hv_pps); n("HVQC", s.hv_qc); n("HVBudgetMA", s.hv_ma);
  n("InputMW", uint32_t(inMw)); n("BatteryMW", uint32_t(batMw)); n("TermHot", s.term_hot); n("FVHold", s.fv_hold);
  n("FastOptions", (fLink.cfg.hv_order ? 1u : 0u) | (fLink.cfg.hv_caps ? 2u : 0u) | (fLink.cfg.qc ? 4u : 0u) |
                   (fLink.cfg.fv_hot ? 8u : 0u));
  {
    OSString *name = OSString::withCString(piano_charge_path_name(path));
    if (name) {d->setObject("ChargePathName", name); name->release();}
  }
  setProperty("PianoCharge", d); d->release();
  note("buck-fast: seq=%u path=%s hv=%u pps=%u qc=%u budget=%u icl=%u fcc=%u in=%d mW battery=%d mW fv=%u hot=%u hold=%u fallback=%u "
       "[PianoFastChargeV1]", fStatusSeq, piano_charge_path_name(path), s.hv, s.hv_pps, s.hv_qc, s.hv_ma, p.icl_ma, p.fcc_ma, inMw,
       batMw, p.fv_mv ? p.fv_mv : 4400u, s.term_hot, s.fv_hold, p.fallback);
  note("buck: seq=%u gen=%u link=%u raw=%d effective=%d valid=%u connector=%u sink=%u pd=%u", fStatusSeq, c.generation,
       fMca.links, piano_mca_has(&fMca, McaType) ? fMca.value[McaType] : -1, x.type, c.valid, c.connector, c.sink, c.pd);
  note("buck-contract: gen=%u seq=%u rdo=%#x protocol=%u mv=%u ma=%u error=%d %s stage=%u inflight=%u quarantine=%u cci=%#x cmd=%#llx control=%#llx",
       c.generation, c.seq, c.rdo, c.protocol, c.mv, c.ma, c.last_error,
       piano_contract_fresh(&c, now) ? "validated" : !piano_contract_status_fresh(&c, now) ? "status unknown/stale" :
       !c.connected ? "fresh disconnected status" : !c.sink ? "fresh source status" : !c.pd ? "fresh non-PD status" : "fresh PD status/invalid RDO",
       c.stage, c.in_flight, c.quarantined, c.cci, c.command, c.control_readback);
  for (unsigned i = 0; i < c.pdos && i < 7; ++i) note("buck-pdo: gen=%u index=%u raw=%#x", c.generation, i + 1, c.source_pdo[i]);
  note("buck-current: seq=%u icl=%u/%u rb=%d fcc=%u/%u fcc_rb=unknown bus=%d/%d fv=%u/%d valid=%u", fStatusSeq, p.icl_ma, s.icl_set,
       x.icl_ma, p.fcc_ma, s.fcc_set, x.bus_mv, x.ibus_ma, fvMv, x.fv_raw, fvValid);
  note("buck-health: seq=%u rev=%u enabled=%u stop=%u resume=%u hold=%u desired=%u pending=%u applied=%u reason=%u inhibit=%u",
       fStatusSeq, fConfigRevision, fLink.cfg.limit, fLink.cfg.stop_pct, fLink.cfg.resume_pct, s.hold, p.charge, !applied,
       applied, p.why_off, fInhibitUntil > now ? 1u : 0u);
  note("buck-safety: seq=%u valid=%#x cp=%#x lpd=%d dtpt=%u ack=%#x/%u/%d timeout=%u fallback=%u", fStatusSeq, fSafetyValid,
       fCpCtrl, piano_mca_has(&fMca, McaLpd) ? fMca.value[McaLpd] : -1,
       fAlerts.active(), fMca.last_property, fMca.last_value, fMca.last_result, fMca.timeouts, p.fallback);
  note("buck-reobserve: PianoBuckReobserveV6 seq=%u uptime=%llu tried=%#x ok=%#x total=%u first=%u/%u/%u/%u final=%u/%u/%u/%u", fStatusSeq, now,
       fAlerts.retried, fAlerts.retryValid, fAlerts.rereads, fAlerts.firstIsc[0], fAlerts.firstSoa[0], fAlerts.firstIsc[1], fAlerts.firstSoa[1],
       fAlerts.isc[0], fAlerts.soa[0], fAlerts.isc[1], fAlerts.soa[1]);
  note("buck-board: seq=%u valid=%#x age=%u/%u/%u/%u", fStatusSeq, board ? board->valid : 0,
       board ? board->age_ms[0] : ~0u, board ? board->age_ms[1] : ~0u,
       board ? board->age_ms[2] : ~0u, board ? board->age_ms[3] : ~0u);
  for (unsigned i = 0; i < 2; ++i) note("buck-pack: seq=%u pack=%u valid=%u soc=%d ma=%d temp=%d isc=%u soa=%u age=%llu mv=%d error=%u failed=%#x",
       fStatusSeq, i, fPack[i].ok, fPack[i].socPct, fPack[i].currentMa, fPack[i].tempDeciC, fAlerts.isc[i], fAlerts.soa[i], now - fPackMs,
       fPack[i].voltageMv, unsigned(fPack[i].error), fPack[i].failedCommand);
}

// kOff: the thread hands the charger over (decide with the sleep flag until the plan is ready, at most 2 s) and acknowledges; the
// answer here gives it 3 s. kOn after an off: one more wake, which the next decision sends everything again for.
IOReturn PianoPower::setPowerState(unsigned long state, IOService *)
{
  if (!fPolicyOn) return kIOPMAckImplied;
  if (state == pianopower::kOff) {
    if (__atomic_load_n(&fSleepAsk, __ATOMIC_ACQUIRE)) return kIOPMAckImplied;
    fWentOff = true;
    __atomic_store_n(&fSleepAsk, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)this);
    return 3000 * 1000;
  }
  if (fWentOff) {
    fWentOff = false;
    __atomic_add_fetch(&fWakes, 1u, __ATOMIC_ACQ_REL);
  }
  __atomic_store_n(&fSleepAsk, 0u, __ATOMIC_RELEASE);
  thread_wakeup((event_t)this);
  return kIOPMAckImplied;
}

// Shutdown or restart: a stop of ours lifted and the source back at 5 V (piano_charge_shutdown), at most 2 s, then the assertion goes.
void PianoPower::systemWillShutdown(IOOptionBits specifier)
{
  if (fPolicyOn) {
    __atomic_store_n(&fShutdownAsk, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)this);
    for (unsigned waited = 0; waited < 2500 && !__atomic_load_n(&fShutdownDone, __ATOMIC_ACQUIRE); waited += 10) IOSleep(10);
  }
  IOService::systemWillShutdown(specifier);
}

// MiPPS's key file (ChargeMipps.h): the private EFI/UniXNU/firmware/PianoPower/mipps-keys.bin, checked against the kext's PRIVATE pin
// (size and SHA-256 taken at build time from private/charging/, never printed), parsed, and the loader's copy wiped. Thread context.
void PianoPower::mippsLoadKeys()
{
#if PIANO_POWER_MIPPS_FILE
  PianoFirmware fw = {};
  const int rc = piano_fw_load(&piano_fw_pianopower_mipps_keys_pin, piano_fw_pianopower_mipps_keys, piano_fw_pianopower_mipps_keys_size,
                               5, &fw);
  if (rc != PIANO_FW_OK) {
    snprintf(gMippsText, sizeof gMippsText, "key file PianoPower/mipps-keys.bin: %s (%s): MiPPS off, 5 A pump budget", piano_fw_error(rc),
             fw.trail[0] ? fw.trail : "no source tried");
  } else {
    const int parsed = piano_mipps_keys_parse(fw.data, fw.size, &gMippsKeys);
    if (fw.owned && fw.data) memset(const_cast<uint8_t *>(fw.data), 0, fw.size);
    if (parsed == PIANO_MIPPS_OK)
      snprintf(gMippsText, sizeof gMippsText, "%u keys from %s (private pin matched): MiPPS ready", gMippsKeys.count,
               piano_fw_source(fw.source));
    else
      snprintf(gMippsText, sizeof gMippsText, "key file PianoPower/mipps-keys.bin: %s: MiPPS off, 5 A pump budget",
               piano_mipps_error_name(parsed));
  }
  piano_fw_release(&fw);
#else
  strlcpy(gMippsText, "built without a key file (private/charging/mipps-keys.bin): MiPPS off, 5 A pump budget", sizeof gMippsText);
#endif
  note("MiPPS: %s", gMippsText);
  setProperty("PianoMipps", gMippsText);
}

void PianoPower::thread(void *self, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-power");
  static_cast<PianoPower *>(self)->run();
}

bool PianoPower::probeSpmi()
{
  if (!mapRegion(gCore, kSpmiCore, kSpmiCoreBytes) || !mapRegion(gCnfg, kSpmiCnfg, kSpmiCnfgBytes) ||
      !mapRegion(gObsv, kSpmiObsv, kSpmiObsvBytes)) {
    stage(-1, "SPMI arbiter windows do not map");
    return false;
  }
  fArbiter = spmiArbiter(gIo);
  note("spmi: arbiter version %#x, %u APIDs%s", fArbiter.version, fArbiter.count, fArbiter.v7 ? "" : " (not a v7 arbiter: stop)");
  if (!fArbiter.v7) {
    if (gArgs.ponHistory) readPonHistory(gIo, fArbiter, note);
    stage(-1, "SPMI arbiter");
    return false;
  }
  if (gArgs.ponHistory) readPonHistory(gIo, fArbiter, note);
  if (!gArgs.level) return true;
  fCharger = discoverCharger(gIo, fArbiter);
  note("charger PMIC (SID 7): %u peripherals, owned by EE0 %u, EE1 %u, EE2 %u, EE3 %u, others %u", fCharger.peripherals,
       fCharger.ownedBy[0], fCharger.ownedBy[1], fCharger.ownedBy[2], fCharger.ownedBy[3],
       fCharger.ownedBy[4] + fCharger.ownedBy[5] + fCharger.ownedBy[6] + fCharger.ownedBy[7]);
  if (!fCharger.found) {
    note("charger: not found (%s; REVID type %#x model %#x): no port state in the report", fCharger.why, fCharger.revType,
         fCharger.model);
    stage(1, "SPMI (no charger layout)");
    return true;
  }
  note("charger: PMIH010x model %#x, charger block %#x (subtype %#x, APID %d, owner EE %u), DCDC %#x (APID %d, EE %u), "
       "USBIN %#x (APID %d, EE %u), Type-C %#x (APID %d, EE %u)%s",
       fCharger.model, fCharger.charger.address, fCharger.subtype, fCharger.charger.apid, fCharger.charger.owner,
       fCharger.dcdc.address, fCharger.dcdc.apid, fCharger.dcdc.owner, fCharger.usbin.address, fCharger.usbin.apid,
       fCharger.usbin.owner, fCharger.typec.address, fCharger.typec.apid, fCharger.typec.owner,
       fCharger.charger.owner ? ": writes belong to that EE (the DSP's firmware), this side reads" : "");
  fSpmi = true;
  stage(1, "SPMI");
  return true;
}

// Once, read only: every SID 7 peripheral from the charger block to Type-C + 0x400 with its TYPE/SUBTYPE, owner, status bytes and
// real-time interrupt status, so a log shows the charger complex's map (ChargerWdog.h's places are inferred).
void PianoPower::surveyChargerBlocks()
{
  if (!fCharger.found) return;
  const uint16_t first = fCharger.charger.address, last = uint16_t(fCharger.typec.address + 0x400);
  for (uint32_t i = 0; i < fArbiter.count; ++i) {
    const uint32_t map = spmiMap(gIo, i);
    const uint16_t ppid = spmiPpid(map);
    if (!map || (ppid >> 8) != kChargerSid) continue;
    const uint16_t address = uint16_t((ppid & 0xff) << 8);
    if (address < first || address > last) continue;
    uint8_t id[2] = {}, st[8] = {}, rt = 0, cfg[3] = {};
    const bool idOk = spmiRead(gIo, int(i), 4, id, 2) == ReadResult::ok;
    const bool stOk = spmiRead(gIo, int(i), 0x06, st, 8) == ReadResult::ok;
    spmiRead(gIo, int(i), kBlockRt, &rt, 1);
    const bool cfgOk = spmiRead(gIo, int(i), 0x51, cfg, 3) == ReadResult::ok;
    note("charger block %#06x: APID %u EE %u, type %s%02x/%02x, +06 %02x %02x %02x %02x %02x %02x %02x %02x%s, rt %#x, +51..53 %02x %02x %02x%s",
         address, i, spmiOwner(gIo, i), idOk ? "" : "? ", id[0], id[1], st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7],
         stOk ? "" : " (not read)", rt, cfg[0], cfg[1], cfg[2], cfgOk ? "" : " (not read)");
  }
}

void PianoPower::wdogWriteLine(const char *what, const WdogWrite &r)
{
  if (!r.done) return;
  if (r.reg == kMiscPet)
    note("wdog: %s: MISC %#06x +0x43 = 0x01 (pet): %s (channel status %#x)", what, fWdog.misc.address, r.ok ? "ok" : "FAILED", r.status);
  else
    note("wdog: %s: MISC %#06x +0x53 %#04x -> %#04x (bit 7, a bite disables charging, cleared; bark %u s, bite %u s kept): %s, "
         "read back %s%#04x (channel status %#x)", what, fWdog.misc.address, r.before, r.value, barkSeconds(r.before),
         biteSeconds(r.before), r.ok ? "ok" : "FAILED", r.readBack ? "" : "not read, ", r.after, r.status);
}

// After the SPMI probe, before anything else that takes time: locate MISC and CHGR, log the watchdog as the boot stages left it,
// then (by piano-power-wdog) the clear and the first pet.
void PianoPower::startWdog()
{
  fWdog.mode = WdogMode(gArgs.wdog);
  fWdog.locate(gIo, fArbiter, fCharger);
  if (!fWdog.located) {
    note("wdog: %s", fWdog.why);
    note("chgen: CHGR not located: no write");
    note("complex: the charger's blocks are not located: no snapshot, no watch, no recovery");
    return;
  }
  const WdogBlock &m = fWdog.misc, &c = fWdog.chgr;
  note("wdog: piano-power-wdog=%u (%s); CHGR %#06x (APID %d, EE %u, type %02x/%02x), MISC %#06x (APID %d, EE %u, type %02x/%02x)%s%s",
       gArgs.wdog, wdogModeName(fWdog.mode), c.address, c.apid, c.owner, c.type, c.subtype, m.address, m.apid, m.owner, m.type,
       m.subtype, fWdog.why[0] ? ": no write: " : "", fWdog.why);
  if (!fWdog.why[0] && fWdog.mode != WdogMode::off) {
    if (!mapChnl(0, m.apid)) note("wdog: MISC's write channel does not map: no write");
  }
  const WdogView before = fWdog.read(gIo);
  if (before.cfgOk)
    note("wdog: as the boot stages left it: WD_CFG (+0x51) %#04x (timer %s, on plug-in %s, bark irq %u, bite irq %u, AFP on bite %u), "
         "timeouts (+0x53) %#04x (a bite %s charging, bark %u s, bite %u s, snarl field %u); MISC rt %#x; CHGR state %u (%s), "
         "+0x42 enable command %#04x, +0x51 cfg2 %#04x",
         before.wdCfg, before.wdCfg & kWdTimerOn ? "on" : "off", before.wdCfg & kWdOnPlugin ? "on" : "off",
         before.wdCfg & kWdBarkIrq ? 1u : 0u, before.wdCfg & kWdBiteIrq ? 1u : 0u, before.wdCfg & kWdAfp ? 1u : 0u, before.timeouts,
         before.biteStops() ? "DISABLES" : "does not disable", barkSeconds(before.timeouts), biteSeconds(before.timeouts),
         (before.timeouts >> 4) & 7u, before.miscRt, before.state(), chargerStateName(before.state()), before.chgrCmd[2],
         before.chgrCfg[1]);
  else
    note("wdog: MISC's configuration does not read");
  WdogWrite clear, pet;
  fWdog.check(gIo, gWriter, nowMs(), &clear, &pet);
  wdogWriteLine("start", clear);
  wdogWriteLine("first", pet);
  if (fWdog.petting)
    note("wdog: petting every %u ms from this thread (a quarter of the %u s bark time, 4-15 s)", fWdog.petMs, barkSeconds(fWdog.last.timeouts));
  else if (!fWdog.why[0] && fWdog.mode != WdogMode::off)
    note("wdog: not armed (WD_CFG bit 0/1 clear): no pet, no clear%s", fWdog.refused ? " (refused writes!)" : "");
  startChgen(before);
  startFix();
}

// The charger complex (ChargerFix.h, charging-policy.md §20): its seven blocks, their configuration as the boot stages left it (one
// line a block, read only), the watch from now (piano-power-chgwatch), and with piano-power-chgfix the recovery's channels and the
// start guard.
void PianoPower::startFix()
{
  fCx.locate(gIo, fArbiter, fCharger);
  fFix.mode = FixMode(gArgs.chgfix);
  fFix.locate(fCx, fChgEn);
  if (fFix.mode != FixMode::off && fChgEn.mode == ChgEnMode::pulse) {
    fChgEn.mode = ChgEnMode::restore;   // one pulser: two would confound each other's judgement
    note("chgen: piano-power-chgen=2 with piano-power-chgfix: the pulses are chgfix's step A (chgen sets an enable that reads off only)");
  }
  if (!fCx.located) {
    note("complex: not located (no Type-C block): no snapshot, no watch, no recovery");
    return;
  }
  char line[480];
  {
    CxText t(line, sizeof line);
    t.text("complex: the blocks at their SCHG places from Type-C (inferred):");
    for (unsigned i = 0; i < kCxBlocks; ++i) {
      const CxBlock &b = fCx.b[i];
      char one[64];
      snprintf(one, sizeof one, "%s %s%#06x APID %d EE %u type %02x/%02x", i ? "," : "", cxName(i), b.address, b.apid, b.owner, b.type,
               b.subtype);
      t.text(one);
    }
  }
  note("%s", line);
  const uint64_t now = nowMs();
  fCxLast = fCx.read(gIo, true, now);
  fCxHave = true;
  fWatch.see(fCxLast, nullptr);
  cxStatusText(fCxLast, line, sizeof line);
  note("complex: status +06..+0f, rt: %s", line);
  // the configuration once (what the boot stages left: currents, float voltage, JEITA, timers, AICL, thermal sources), +40..+bf
  for (unsigned i = 0; i < kCxBlocks; ++i) {
    const CxRegs &r = fCxLast.r[i];
    CxText t(line, sizeof line);
    for (unsigned k = 0; k < kCxCfgBytes; ++k) {
      if (k % 8 == 0 && k) t.put(k % 32 ? ' ' : '|');
      if (r.cfgOk & (1u << (k / 8))) t.hex(r.cfg[k]);
      else t.text("--");
    }
    note("complex: %s %#06x +40..+bf: %s", cxName(i), fCx.b[i].address, line);
  }
  if (gArgs.chgwatch) {
    fWatch.extend(now, uint64_t(gArgs.chgwatch) * 1000);
    note("complex: watching the charger's state every 2 s for %u s (piano-power-chgwatch; read only)", gArgs.chgwatch);
  }
  if (fFix.mode != FixMode::off && !fFix.why[0]) {
    // the channels: CHGR's (when chgen=0 left it unmapped), MISC's (when the watchdog's mode left it unmapped), USBIN's
    const bool chgr = mapChnl(1, fFix.chgr.apid), misc = !fFix.misc.ours() || mapChnl(0, fFix.misc.apid),
               usbin = !fFix.usbin.ours() || mapChnl(2, fFix.usbin.apid);
    if (!chgr) fFix.why = "CHGR's write channel does not map";
    if (!misc || !usbin) note("chgfix: %s's write channel does not map: its step fails", !misc ? "MISC" : "USBIN");
  }
  note("chgfix: piano-power-chgfix=%u (%s)%s%s%s", gArgs.chgfix, fixModeName(fFix.mode),
       gArgs.level >= 2 && fFix.mode != FixMode::off ? "; level 2: no step (the DSP's firmware owns the charger)" : "",
       fFix.why[0] ? "; no write: " : "", fFix.why);
  const PortState p = readPort(gIo, fCharger);
  fixEventLine(now, fFix.startGuard(gIo, gWriter, p.ok && p.sourceSeen));
  publishCharger();
}

void PianoPower::keepChange(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(fChanges[fChangesNext], sizeof fChanges[0], fmt, ap);
  va_end(ap);
  fChangesNext = (fChangesNext + 1) % kChanges;
  if (fChangesCount < kChanges) fChangesCount++;
}

// The watch saw CHGR's state change between two ticks: the line, the record, the picture published at once.
void PianoPower::watchLine(const CxTransition &x)
{
  const uint64_t a = x.fromMs > fStartMs ? x.fromMs - fStartMs : 0, b = x.toMs > fStartMs ? x.toMs - fStartMs : 0;
  note("[%llus] chgr: watch: CHGR state %u (%s) -> %u (%s)%s between +%llu.%llu s and +%llu.%llu s (XNU uptime %llu.%llu s): %s",
       b / 1000, x.from, chargerStateName(x.from), x.to, chargerStateName(x.to), x.stop ? ", the STOP," : "", a / 1000, a % 1000 / 100,
       b / 1000, b % 1000 / 100, x.toMs / 1000, x.toMs % 1000 / 100, x.diff);
  keepChange("+%llu.%llu s (uptime %llu.%llu s) state %u -> %u: %s", b / 1000, b % 1000 / 100, x.toMs / 1000, x.toMs % 1000 / 100,
             x.from, x.to, x.diff);
  publishCharger();
}

void PianoPower::fixEventLine(uint64_t now, const FixEvent &ev)
{
  if (ev.kind == FixEvent::none) return;
  const uint64_t t = now > fStartMs ? (now - fStartMs) / 1000 : 0;
  switch (ev.kind) {
    case FixEvent::skipped:
      note("[%llus] chgfix: step %c: %s", t, fixStepLetter(ev.step), ev.why);
      break;
    case FixEvent::wrote:
    case FixEvent::guard:
      note("[%llus] chgfix: %s%s (attempt %u): %s %#06x +%#04x %#04x -> %#04x: %s, read back %s%#04x, CHGR state now %u (%s) (channel "
           "status %#x)%s%s",
           t, ev.kind == FixEvent::guard ? "" : "step ", ev.kind == FixEvent::guard ? "start guard" : fixStepName(ev.step),
           ev.attempt, ev.block, ev.address, ev.reg, ev.before, ev.value, ev.ok ? "ok" : "FAILED", ev.readBack ? "" : "not read, ",
           ev.after, ev.state, chargerStateName(ev.state), ev.status, ev.why[0] ? "; " : "", ev.why);
      keepChange("+%llu s chgfix %s %s +%#04x %#04x -> %#04x %s", t, ev.kind == FixEvent::guard ? "guard" : ev.what, ev.block, ev.reg,
                 ev.before, ev.value, ev.ok ? "ok" : "FAILED");
      break;
    case FixEvent::judged:
      note("[%llus] chgfix: after step %s (attempt %u): CHGR state %u (%s), the packs %+d mA: %s", t, fixStepName(ev.step), ev.attempt,
           ev.state, chargerStateName(ev.state), ev.packsMa, ev.why);
      keepChange("+%llu s chgfix step %c judged: state %u, %+d mA: %s", t, fixStepLetter(ev.step), ev.state, ev.packsMa, ev.why);
      break;
    case FixEvent::finished:
      note("[%llus] chgfix: %s (%u attempts, %u worked)", t, ev.why, fFix.attempts, fFix.successes);
      break;
    default:
      break;
  }
  publishCharger();
}

// The recovery's turn (ChargerFix::poll): at its own deadlines, with every watch tick and every report. The port read afresh; the
// packs read afresh for a judge.
void PianoPower::fixPoll(uint64_t now)
{
  if (fFix.mode == FixMode::off || fFix.why[0]) return;
  if (fFix.judgeDue(now)) sampleGauges();
  const PortState p = readPort(gIo, fCharger);
  const ChgEnInputs in = chgenInputs(&p);
  const FixEvent ev = fFix.poll(gIo, gWriter, fChgEn, in, gArgs.level == 1, fBattery.ok ? fBattery.currentMa : 0, now);
  if (ev.kind == FixEvent::wrote || ev.kind == FixEvent::judged) fWatch.extend(now, 90000);   // a restart, and a stop again, timed
  fixEventLine(now, ev);
}

// The picture for a shell (read only; refreshed with every report, watch change and recovery event):
//   PianoCharger         a dictionary: time-s, uptime-ms, state, state-name, packs-mA (text, signed), packs-mV, soc, the status
//                        bytes +0x06..+0x10 of each block (data, named CHGR .. MISC), the configuration +0x40..+0xbf (CHGR-40 ..),
//                        stops, first-stop-s, chgfix, chgfix-attempts;
//   PianoChargerText     the same in one line;
//   PianoChargerChanges  the last eight changes (watch transitions, recovery writes and judges), oldest first.
void PianoPower::publishCharger()
{
  if (!fCx.located || !fCxHave) return;
  const uint64_t now = nowMs(), t = (now - fStartMs) / 1000;
  OSDictionary *d = OSDictionary::withCapacity(24);
  if (!d) return;
  auto num = [&](const char *key, uint64_t v) {
    if (OSNumber *n = OSNumber::withNumber((unsigned long long)v, 64)) {
      d->setObject(key, n);
      n->release();
    }
  };
  auto str = [&](const char *key, const char *v) {
    if (OSString *s = OSString::withCString(v)) {
      d->setObject(key, s);
      s->release();
    }
  };
  auto data = [&](const char *key, const void *bytes, unsigned n) {
    if (OSData *o = OSData::withBytes(bytes, n)) {
      d->setObject(key, o);
      o->release();
    }
  };
  const CxSnapshot &s = fCxLast;
  num("time-s", t);
  num("uptime-ms", now);
  num("snapshot-s", s.ms > fStartMs ? (s.ms - fStartMs) / 1000 : 0);
  num("state", s.chgrState());
  str("state-name", chargerStateName(s.chgrState()));
  char ma[16], text[160];
  snprintf(ma, sizeof ma, "%+d", fBattery.ok ? fBattery.currentMa : 0);
  str("packs-mA", fBattery.ok ? ma : "?");
  num("packs-mV", fBattery.ok ? uint64_t(fBattery.voltageMv) : 0);
  num("soc", fBattery.ok ? uint64_t(fBattery.socPct) : 0);
  for (unsigned i = 0; i < kCxBlocks; ++i) {
    const CxRegs &r = s.r[i];
    if (r.statusOk) data(cxName(i), r.status, kCxStatusBytes);
    if (s.full && r.cfgOk == 0xffff) {
      char key[16];
      snprintf(key, sizeof key, "%s-40", cxName(i));
      data(key, r.cfg, kCxCfgBytes);
    }
  }
  num("stops", fWatch.stops);
  num("first-stop-s", fWatch.firstStopMs > fStartMs ? (fWatch.firstStopMs - fStartMs) / 1000 : 0);
  num("first-stop-uptime-ms", fWatch.firstStopMs);
  num("watching", fWatch.active ? 1 : 0);
  snprintf(text, sizeof text, "piano-power-chgfix=%u: %s%s%c (attempt %u)", gArgs.chgfix, fFix.statusWhat,
           fFix.statusStep != FixStep::none ? "; step " : "", fFix.statusStep != FixStep::none ? fixStepLetter(fFix.statusStep) : ' ',
           fFix.statusAttempt);
  str("chgfix", text);
  num("chgfix-attempts", fFix.attempts);
  setProperty("PianoCharger", d);
  d->release();
  char line[600];
  const int n = snprintf(line, sizeof line, "[%llus] state %u (%s), packs %s mA %d mV %d %%, stops %u, %s | ", t, s.chgrState(),
                         chargerStateName(s.chgrState()), fBattery.ok ? ma : "?", fBattery.voltageMv, fBattery.socPct, fWatch.stops,
                         text);
  if (n > 0 && unsigned(n) < sizeof line) cxStatusText(s, line + n, unsigned(sizeof line - unsigned(n)));
  if (OSString *o = OSString::withCString(line)) {
    setProperty("PianoChargerText", o);
    o->release();
  }
  if (OSArray *a = OSArray::withCapacity(kChanges)) {
    for (unsigned k = 0; k < fChangesCount; ++k) {
      const unsigned i = (fChangesNext + kChanges - fChangesCount + k) % kChanges;
      if (OSString *o = OSString::withCString(fChanges[i])) {
        a->setObject(o);
        o->release();
      }
    }
    setProperty("PianoChargerChanges", a);
    a->release();
  }
}

// The charging enable command (ChargerEnable.h, charging-policy.md §19): CHGR from ChargerWdog's locate, its EE 0 APID the only one
// written (its channel mapped only when the gate passes), the other mapping of the same PPID named; the decisions are the reports'.
void PianoPower::startChgen(const WdogView &v)
{
  fChgEn.mode = ChgEnMode(gArgs.chgen);
  fChgEn.locate(fWdog);
  const WdogBlock &c = fChgEn.chgr;
  const Apid other = fChgEn.located ? otherMapping(gIo, fArbiter, c.address, c.apid) : Apid();
  if (!fChgEn.why[0] && fChgEn.mode != ChgEnMode::off && !mapChnl(1, c.apid)) {
    note("chgen: CHGR's write channel does not map: no write");
    fChgEn.mode = ChgEnMode::off;
  }
  const uint8_t cmd = v.chgrCmd[2], cfg2 = v.chgrCfg[1];
  note("chgen: piano-power-chgen=%u (%s); CHGR %#06x APID %d (EE %u: the one written), %s APID %d (EE %u%s); +0x42 %#04x (the "
       "enable command %s), +0x51 %#04x (%s, %s, inhibit %s)%s%s",
       gArgs.chgen, chgEnModeName(fChgEn.mode), c.address, c.apid, c.owner, other.apid >= 0 ? "also" : "no other mapping:",
       other.apid, other.owner, other.apid >= 0 && other.owner == 3 ? ": the DSP's channel (inferred), never written" : "", cmd,
       !v.chgrCmdOk ? "not read" : cmd & kChgEnableBit ? "ON" : "OFF", cfg2, !v.chgrCfgOk ? "not read" : cfg2 & kChgEnSrcPin ? "the PIN decides" : "the command decides",
       cfg2 & kChgEnPolarity ? "pin active low" : "active high", cfg2 & 1 ? "on" : "off", fChgEn.why[0] ? "; no write: " : "",
       fChgEn.why);
}

void PianoPower::chgenWriteLine(uint64_t t, const ChgEnWrite &r)
{
  if (!r.done) return;
  note("[%llus] chgen: %s: CHGR %#06x +0x42 %#04x -> %#04x (bit 0, the charging enable command): %s, read back %s%#04x, state now %u "
       "(%s) (channel status %#x)%s",
       t, r.what, fChgEn.chgr.address, r.before, r.value, r.ok ? "ok" : "FAILED", r.readBack ? "" : "not read, ", r.after,
       r.stateAfter, chargerStateName(r.stateAfter), r.status, fChgEn.refused ? "; refused writes!" : "");
}

// With every report, after the watchdog's read: what changed in CHGR and MISC since the last report, who changed the enable command
// (not us: ours are logged as they are made), then the decision. *why gets the reason no write was made (for the charging: line).
// The battery and the port as the gates of ChargerEnable and ChargerFix take them: the latest gauge readings, the port state given.
ChgEnInputs PianoPower::chgenInputs(const PortState *p)
{
  ChgEnInputs in;
  in.packsOk = fPack[0].ok && fPack[1].ok;
  for (unsigned i = 0; i < 2; ++i) {
    const GaugeReading &g = fPack[i];
    if (!g.ok) continue;
    if (g.voltageMv > in.maxPackMv) in.maxPackMv = g.voltageMv;
    if (g.socPct > in.maxSocPct) in.maxSocPct = g.socPct;
    in.anyFull |= (g.status & kGaugeFc) != 0;
    if (i == 0 || !fPack[0].ok || g.tempDeciC < in.minDeciC) in.minDeciC = g.tempDeciC;
    if (i == 0 || !fPack[0].ok || g.tempDeciC > in.maxDeciC) in.maxDeciC = g.tempDeciC;
  }
  in.sourcePresent = p && p->ok && p->sourceSeen;
  in.dspOwns = fWdog.dspOwns;
  if (gArgs.level >= 2 && !in.dspOwns) {
    PianoAdspStatus s;
    piano_adsp_status(&s);
    in.dspOwns = s.slave >= 0 && (s.slave & 2);
  }
  return in;
}

void PianoPower::chgenStep(uint64_t t, const PortState *p, char *why, size_t whySize)
{
  if (why && whySize) why[0] = 0;
  if (!fChgEn.located) return;
  const ChgEnInputs in = chgenInputs(p);
  char diff[400];
  const ChgEnResult r = fChgEn.check(gIo, gWriter, fWdog.last, in, nowMs(), diff, sizeof diff);
  const uint64_t since = r.sinceMs > fStartMs ? (r.sinceMs - fStartMs) / 1000 : 0;
  // the changes of CHGR and MISC alone, when the complex's snapshot (all seven blocks, report()) is not there to say them
  if (r.changed && !fCx.located) note("[%llus] chgr: changed since [%llus]: %s", t, since, diff);
  if (r.cmdCleared || r.cmdSet)
    note("[%llus] chgen: the enable command went %#04x -> %#04x between [%llus] and [%llus], not by PianoPower; CHGR state %u -> %u (%s); %s",
         t, r.cmdWas, r.cmdNow, since, t, r.stateWas, fWdog.last.state(), chargerStateName(fWdog.last.state()),
         in.dspOwns ? "the DSP runs: its firmware, or the PMIC" : "the DSP is not running: the PMIC itself or another EE");
  if (r.judged)
    note("[%llus] chgen: after the %s at [%llus]: CHGR state %u (%s), +0x42 %#04x, the packs %+d mA: %s", t, r.judgedWhat,
         r.judgedWriteMs > fStartMs ? (r.judgedWriteMs - fStartMs) / 1000 : 0, fWdog.last.state(), chargerStateName(fWdog.last.state()),
         r.cmdNow, fBattery.currentMa, fBattery.currentMa >= 50 ? "CHARGING again"
                                       : fWdog.last.state() != kChgrDisabled ? "the charger left DISABLED" : "no change");
  chgenWriteLine(t, r.write);
  if (why && whySize && r.why[0] && fWdog.last.state() == kChgrDisabled) strlcpy(why, r.why, whySize);
}

// "wdog: ..." with every report: the watchdog and charger state as read now and what the pets have done.
void PianoPower::wdogLine(char *line, size_t size, uint64_t t)
{
  const WdogView &v = fWdog.last;
  snprintf(line, size,
           "[%llus] wdog: WD_CFG %#04x timeouts %#04x (bite %s charging), MISC %02x %02x %02x %02x %02x %02x %02x %02x rt %#x; CHGR "
           "state %u (%s) %02x %02x %02x %02x %02x %02x %02x %02x rt %#x cmd %02x %02x %02x; pets %u (%u failed%s), clears %u (%u "
           "failed)%s%s",
           t, v.wdCfg, v.timeouts, v.biteStops() ? "disables" : "does not disable", v.misc[0], v.misc[1], v.misc[2], v.misc[3],
           v.misc[4], v.misc[5], v.misc[6], v.misc[7], v.miscRt, v.state(), chargerStateName(v.state()), v.chgr[0], v.chgr[1],
           v.chgr[2], v.chgr[3], v.chgr[4], v.chgr[5], v.chgr[6], v.chgr[7], v.chgrRt, v.chgrCmd[0], v.chgrCmd[1], v.chgrCmd[2],
           fWdog.pets, fWdog.petFailures, fWdog.petting ? "" : ", not petting", fWdog.clears, fWdog.clearFailures,
           fWdog.dspOwns ? "; the DSP owns the configuration" : "", fWdog.why[0] ? "; no write: " : "");
  if (fWdog.why[0]) strlcat(line, fWdog.why, size);
}

// "rtc: COUNT (DATE UTC, offset N from SOURCE), enabled E": tooling/native/rtc-offset.py pairs these lines with the Mac's clock.
void PianoPower::readRtcLine(char *line, size_t size)
{
  fRtc = readRtc(gIo, fArbiter, gArgs.hasRtcOffset, gArgs.rtcOffset, &fRtc);
  const RtcReading &r = fRtc;
  if (!r.ok) {
    snprintf(line, size, "rtc: not read (%s)", r.why);
    return;
  }
  struct piano_rtc_date d;
  piano_rtc_civil(r.utc, &d);
  if (r.source == RtcOffsetSource::none)
    snprintf(line, size, "rtc: %u (no offset: the SDAM cell does not read and there is no piano-rtc-offset), enabled %u", r.count,
             r.enabled ? 1u : 0u);
  else if (!r.dated)
    snprintf(line, size, "rtc: %u (+ offset %u from %s = %u, year %u: not a date, measure piano-rtc-offset), enabled %u", r.count,
             r.offset, rtcSourceName(r.source), r.utc, d.year, r.enabled ? 1u : 0u);
  else
    snprintf(line, size, "rtc: %u (%04u-%02u-%02u %02u:%02u:%02u UTC, offset %u from %s), enabled %u", r.count, d.year, d.month,
             d.day, d.hour, d.minute, d.second, r.offset, rtcSourceName(r.source), r.enabled ? 1u : 0u);
}

void PianoPower::surveyAdsp()
{
  PianoAdspSurvey s;
  if (piano_adsp_survey(&s)) {
    note("adsp: survey: window %d does not map", s.mapped);
    return;
  }
  const char *state = s.slave < 0 ? "no SMP2P state from the DSP: not running"
                      : s.slave & 1 ? "fatal"
                      : s.slave & 2 ? "running (ready)"
                      : "SMP2P item there, not ready";
  note("adsp: survey: SMEM APPS/ADSP partition %s, items %#x, slave-kernel %d (%s), AOSS %#x, RSC id %#x%s%s",
       s.partition ? "missing" : "found", s.items, s.slave, state, s.aoss, s.rsc_id, s.crash[0] ? ", crash reason: " : "", s.crash);
  stage(2, "DSP survey");
}

// piano-power-cp=1: the SC8541 read only (its device id), the sysctl arm registered; no register is written here.
void PianoPower::probeCp()
{
  if (!gArgs.cp) return;
  GaugeBus &b = gBuses[0];
  if (!b.sda.page.va || !b.scl.page.va) {note("cp: pack A's pins are not mapped: the SC8541 stays untouched"); return;}
  pianopower::Sc8541 *cp = new pianopower::Sc8541(pianopower::Sc8541::Clock{nullptr, cpNowMs});
  if (!cp) {note("cp: no memory for the driver: the pump stays off"); return;}
  takePin(b.scl); takePin(b.sda); IODelay(20);
  const I2cIo io = {&b, busSda, busScl, busReadSda, busReadScl, busDelay};
  I2cBus bus(io);
  const bool present = cp->probe(bus);
  givePin(b.sda); givePin(b.scl);
  gCpFaults |= cp->takeFaults();
  if (!present) {note("cp: no SC8541 at 0x6a (faults %#x): the pump stays off", gCpFaults); delete cp; return;}
  gCpHook = cp;
  if (!gCpSess) {
    gCpSess = new CpSession;
    if (gCpSess) {
      piano_cp_config_default(&gCpSess->cfg);
      piano_cp_init(&gCpSess->st);
      gCpSess->plan.sleep_ready = 1;  // nothing held until the session's first step
      gCpSess->cfg.full_charge = 0;   // the FFC float never with the 80/75 health limit
      gCpSess->cfg.mipps = 0;         // set per step from piano-power-mipps and the keys (runCpSession)
      piano_mca_allow_cp(1, 0);       // the QC3 pulse follows the buck's QC session (runCpSession)
    } else {
      note("cp: no memory for the pump session: the pump stays off");
    }
  }
  if (!gCpArmRegistered) {
    gCpArmOid.oid_parent = &sysctl__machdep_children;
    gCpArmOid.oid_number = OID_AUTO;
    gCpArmOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_OID2 | CTLFLAG_LOCKED);
    gCpArmOid.oid_arg1 = nullptr;
    gCpArmOid.oid_name = "piano_power_cp_arm";
    gCpArmOid.oid_handler = cpArmSysctl;
    gCpArmOid.oid_fmt = "I";
    gCpArmOid.oid_descr = "PianoPower: arm the SC8541 charge pump for this boot (root; 1 = armed)";
    gCpArmOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gCpArmOid);
    gCpArmRegistered = true;
  }
  note("cp: SC8541 at 0x6a (id %#x); unarmed until machdep.piano_power_cp_arm=1; no register written", PIANO_CP_DEVICE_ID);
}

// Every worker pass: the arm, the kick and the guard; a stop on disarm, sleep or shutdown. The bus is pack A's, taken here.
void PianoPower::serviceCp(uint64_t now)
{
  if (!gArgs.cp || !gCpHook) return;
  pianopower::Sc8541 &fCp = *gCpHook;
  fCp.arm.bootArg = true;
  fCp.arm.sysctl = __atomic_load_n(&gCpArm, __ATOMIC_ACQUIRE) == 1;
  const bool mustStop = fCp.switching() && (!fCp.arm.armed() || fHandover || fShutdownSeen);
  const bool sessionDue = gCpSess && fPolicyOn && fLink.cfg.strict_contract && !fHandoverAcked && now >= gCpSess->nextMs;
  if (!mustStop && !fCp.kickDue(now) && !fCp.guardDue(now) && !sessionDue) return;
  if (fHandoverAcked && !mustStop) return;   // the gauges' I2C idle while asleep (docs/power.md 4)
  GaugeBus &b = gBuses[0];
  takePin(b.scl); takePin(b.sda); IODelay(20);
  const I2cIo io = {&b, busSda, busScl, busReadSda, busReadScl, busDelay};
  I2cBus bus(io);
  if (mustStop) {
    fCp.stop(bus, !fCp.arm.armed() ? 0x44495341u /* DISA */ : fHandover ? 0x534c4550u /* SLEP */ : 0x444f574eu /* DOWN */);
    note("cp: stopped (%s)", !fCp.arm.armed() ? "disarmed" : fHandover ? "system sleep" : "shutdown");
  } else {
    fCp.beforeBusTransaction(bus);
  }
  if (sessionDue) runCpSession(now, bus);
  givePin(b.sda); givePin(b.scl);
  const uint32_t f = fCp.takeFaults();
  if (gCpSess) gCpSess->faultsCarry |= f;   // the session sees every fault, also those of this pass's own writes
  if (f) {gCpFaults |= f; note("cp: faults %#x (lost %u, switching %u)", f, fCp.lost(), fCp.switching());}
}

// MiPPS (ChargeMipps.h, MIPPS.md): with piano-power-mipps=1, the keys loaded and the pump session allocated, one authentication per
// attach of an eligible PD source, before any 9 V or pump step (the strict session reads it as an authentication in progress:
// LinkView.mippsBusy). The worker wakes at the engine's time (10 ms steps) while it runs.
bool PianoPower::mippsEligibleNow() const
{
  if (!gArgs.mipps || !gMippsKeys.count || !gCpSess || !fLink.st.attached) return false;
  uint32_t maxmv[7] = {};
  const unsigned n = fLink.in.pdos < 7 ? fLink.in.pdos : 7;
  for (unsigned i = 0; i < n; ++i) maxmv[i] = fLink.in.pdo[i][1];
  const bool hvOff = fLink.st.hv == PIANO_CHG_HV_OFF && (!(fLink.in.valid & PIANO_CHG_IN_BUS) || fLink.in.bus_mv < 6000);
  const bool cpOff = !(gCpHook && gCpHook->switching()) && gCpSess->st.state == PIANO_CPS_IDLE;
  return piano_mipps_eligible(1, &gMippsKeys, fLink.in.type, maxmv, n, hvOff, cpOff, gMippsTriedSeq == gMippsAttachSeq) != 0;
}

void PianoPower::serviceMipps(uint64_t now)
{
  if (!gArgs.mipps || !gMippsKeys.count || !gCpSess || !fPolicyOn || !fChannel || !fWasUp) return;
  uint32_t ups = 0;
  const bool pdUp = piano_qrtr_charger_pd(&ups) == SERVREG_STATE_UP;
  const bool attached = fLink.st.attached;
  if (attached && !gMippsWasAttached) gMippsAttachSeq++;
  gMippsWasAttached = attached;
  // the answer to the request in flight: the firmware's, or the owner's timeout (-60)
  if (gMippsActive) {
    int32_t rc = 0;
    uint8_t data[16] = {};
    uint32_t len = sizeof data;
    if (piano_mca_aux_take(&fMca, &rc, data, &len) == 1) piano_mipps_reply(&gMipps, now, rc, data, len);
  }
  if (!gMippsActive) {
    // never a new attempt while the last one's 0x21002 = 0 is owed
    if (!pdUp || fHandover || fShutdownSeen || piano_mipps_cleanup_pending(&gMipps) || !mippsEligibleNow()) return;
    uint8_t challenge[16];
    uint32_t pick = 0;
    read_random(challenge, sizeof challenge);
    read_random(&pick, sizeof pick);
    gMippsTriedSeq = gMippsAttachSeq;
    const int r = piano_mipps_start(&gMipps, &gMippsKeys, pick % gMippsKeys.count, challenge, now);
    if (r) {note("MiPPS: not started (%s): the 5 A pump budget", piano_mipps_error_name(r)); return;}
    gMippsUps = ups;
    gMippsActive = true;
    piano_mca_allow_mipps(&fMca, &gMippsAux);
    note("MiPPS: authentication started (key %u of %u)", pick % gMippsKeys.count + 1, gMippsKeys.count);
  }
  // again at once while the engine asks for no wait (the ID search: one 256-ID chunk per call), at most 512 calls a pass; a sleep, a
  // shutdown or a detach ends the attempt (the engine still writes its 0x21002 = 0 cleanup)
  const int current = attached && gMippsAttachSeq == gMippsTriedSeq && !fHandover && !fShutdownSeen;
  for (unsigned i = 0; i < 512; ++i) {
    PianoMippsRequest req{};
    piano_mipps_next(&gMipps, now, current, pdUp && ups == gMippsUps, &req);
    if (req.kind == PIANO_MIPPS_REQ_WRITE || req.kind == PIANO_MIPPS_REQ_READ) {
      if (piano_mca_aux_request(&fMca, req.kind == PIANO_MIPPS_REQ_WRITE, req.prop, req.data, req.len) != 0)
        piano_mipps_reply(&gMipps, now, -1, nullptr, 0);
      gMippsWakeMs = now + 10;
      break;
    }
    gMippsWakeMs = req.wait_until_ms;
    if (gMipps.state == PIANO_MIPPS_DONE || !req.wait_until_ms || req.wait_until_ms > now) break;
  }
  if (gMipps.state == PIANO_MIPPS_DONE && !piano_mipps_cleanup_pending(&gMipps)) {
    gMippsActive = false;
    gMippsWakeMs = 0;
    piano_mca_allow_mipps(&fMca, nullptr);
    note("MiPPS: %s (adapter %#x): the pump budget %u mA", gMipps.verified ? "verified" : piano_mipps_error_name(gMipps.error),
         gMipps.adapter_id, piano_mipps_budget_ma(&gMipps));
  }
}

// One step of the pump session (ChargePumpSession.h) with pack A's bus taken: its system side from the policy's last decision, the
// two packs read afresh while the pump may run, the driver's state and ADC, then the plan executed: the driver action, one PPS word or
// QC pulse through the codec, and the hand-back of the source to the buck's barrier.
void PianoPower::runCpSession(uint64_t now, I2cBus &bus)
{
  CpSession &S = *gCpSess;
  pianopower::Sc8541 &cp = *gCpHook;
  S.cfg.qc3 = fLink.cfg.qc;
  piano_mca_allow_cp(1, S.cfg.qc3);
  PianoCpSys &y = S.sys;
  piano_charge_cp_sys(&fLink.cfg, &fLink.in, &fLink.st, &y);
  y.now_ms = now;
  if (fLink.st.attached && !S.wasAttached) S.attachSeq++;
  S.wasAttached = fLink.st.attached;
  y.attach_seq = S.attachSeq;
  y.sleep_pending = fHandover || fShutdownSeen;
  y.awake = !y.sleep_pending;
  // the session's own requests: an answer clears them (cpAnswered); the codec's timer giving up does after 10 s
  if ((S.ppsPending || S.qcPending) && now >= S.reqMs + 10000) S.ppsPending = S.qcPending = false;
  y.volt_pending = y.volt_pending || S.ppsPending || S.qcPending;
  y.pps_taken_word = S.ppsTaken;
  y.qc_pulses_taken = S.qcTaken;
  // the packs: pack A on this bus, pack B on its own, every 700 ms while the pump may run, every 5 s otherwise
  const bool pumping = S.st.state >= PIANO_CPS_ENABLING && S.st.state <= PIANO_CPS_STOPPING;
  if (fGauges && now >= S.packsMs + (pumping ? 700u : 5000u)) {
    S.packs[0] = readGauge(bus);
    S.packs[1] = gBuses[1].sda.page.va && gBuses[1].scl.page.va ? readBus(gBuses[1]) : GaugeReading();
    S.packsMs = now;
  }
  // the board thermistors and the port, on the same clock: the connector rule, the attachment and the health hold from readings of
  // this pass, not the policy's (up to one 5 s codec cycle old); a connector reading older than 1.5 s counts as none (no session)
  if (now >= S.boardMs + (pumping ? 700u : 5000u)) {
    S.boardOk = boardTemps(S.board, now);
    S.portOk = false;
    if (fSpmi && fCharger.found) {S.port = readPort(gIo, fCharger); S.portOk = S.port.ok;}
    S.boardMs = now;
  }
  const bool packsOk = S.packs[0].ok && S.packs[1].ok && now - S.packsMs <= (pumping ? 1500u : 10000u);
  y.cells_valid = packsOk ? 1 : 0;
  if (packsOk) {
    y.vbat_min_mv = S.packs[0].voltageMv < S.packs[1].voltageMv ? S.packs[0].voltageMv : S.packs[1].voltageMv;
    const int vmax = S.packs[0].voltageMv > S.packs[1].voltageMv ? S.packs[0].voltageMv : S.packs[1].voltageMv;
    if (vmax > y.vbat_max_mv) y.vbat_max_mv = vmax;   // the fresher, higher reading
    y.ibat_a_ma = S.packs[0].currentMa;
    y.ibat_b_ma = S.packs[1].currentMa;
    y.t_cool_dc = S.packs[0].tempDeciC < S.packs[1].tempDeciC ? S.packs[0].tempDeciC : S.packs[1].tempDeciC;
    y.t_warm_dc = S.packs[0].tempDeciC > S.packs[1].tempDeciC ? S.packs[0].tempDeciC : S.packs[1].tempDeciC;
    // the health hold from the packs' charge of now (the policy's own hold stays in force from piano_charge_cp_sys)
    const int soc = S.packs[0].socPct > S.packs[1].socPct ? S.packs[0].socPct : S.packs[1].socPct;
    y.soc_pct = soc;
    if (fLink.cfg.limit && fLink.cfg.stop_pct < 100 && soc >= int(fLink.cfg.stop_pct)) y.health_hold = 1;
  }
  {
    const uint32_t boardAge = uint32_t(now - S.boardMs);
    const auto fresh = [&](unsigned i) {
      return S.boardOk && (S.board.valid & (1u << i)) && S.board.age_ms[i] + boardAge <= 1500u;
    };
    const bool c1 = fresh(PIANO_BOARD_TEMP_CONNECTOR), c2 = fresh(PIANO_BOARD_TEMP_CONNECTOR2);
    const int32_t t1 = S.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR] / 100, t2 = S.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR2] / 100;
    y.conn_valid = c1 || c2;
    y.t_conn_dc = c1 && c2 ? (t1 > t2 ? t1 : t2) : c1 ? t1 : t2;
    if (fresh(PIANO_BOARD_TEMP_BATTERY)) {
      const int32_t tb = S.board.milli_c[PIANO_BOARD_TEMP_BATTERY] / 100;
      if (tb > y.t_warm_dc) y.t_warm_dc = tb;
      if (tb < y.t_cool_dc) y.t_cool_dc = tb;
    }
    // detached now (the PMIC's Type-C block reads no source)
    if (S.portOk && !(S.port.attached && S.port.sourceSeen)) y.attached = 0;
  }
  // the pump
  cp.fill(&S.drv);
  S.drv.faults = S.faultsCarry | cp.takeFaults();
  S.faultsCarry = 0;
  gCpFaults |= S.drv.faults;
  S.drv.adc = piano_cp_adc{};
  if (cp.switching() || pumping) cp.readAdc(bus, &S.drv.adc);
  // MiPPS: done/verified/budget from the engine; not eligible (off, no key, not a PD source above 5 V, a start refused) = done,
  // unverified, 5000
  const bool mippsOn = gArgs.mipps && gMippsKeys.count;
  S.cfg.mipps = mippsOn ? 1 : 0;
  const bool tried = gMippsTriedSeq == gMippsAttachSeq && gMippsAttachSeq;
  const bool willRun = mippsOn && (gMippsActive || (tried && gMipps.state == PIANO_MIPPS_DONE) || (!tried && mippsEligibleNow()));
  piano_mipps_fill_cp_in(&gMipps, willRun ? 1 : 0, &S.drv);
  y.mipps_cleanup_pending = gMippsActive && piano_mipps_cleanup_pending(&gMipps);
  const uint8_t was = S.st.state;
  piano_cp_decide(&S.cfg, &y, &S.drv, &S.st, &S.plan);
  const PianoCpPlan &p = S.plan;
  switch (p.out.action) {
  case PIANO_CP_ACT_START:
    if (!cp.start(bus, p.out.busocp_ma)) note("cp session: the driver refused the start (faults %#x)", gCpFaults);
    break;
  case PIANO_CP_ACT_STOP: cp.stop(bus, 0x53455353u /* 'SESS' */ ^ p.out.stop_reason); break;
  case PIANO_CP_ACT_SET_BUSOCP: cp.setBusOcp(bus, p.out.busocp_ma); break;
  default: break;
  }
  if (p.out.req_valid & PIANO_CP_REQ_PPS) {
    if (piano_cp_write_allowed(&S.cfg, PIANO_CHG_PPS, p.pps_word, 4) && piano_mca_queue(&fMca, PIANO_MCA_PPS, p.pps_word, 4) == 0) {
      S.ppsPending = true; S.ppsWord = p.pps_word; S.reqMs = now;
    } else {
      note("cp session: PPS %u mV %u mA not queued", p.out.pps_mv, p.out.pps_ma);
    }
  }
  if (p.out.req_valid & PIANO_CPS_REQ_QC_PULSE) {
    if (piano_cp_write_allowed(&S.cfg, PIANO_CHG_QC_PULSE, p.qc_pulse, 4) && piano_mca_queue(&fMca, PIANO_MCA_QC_PULSE, p.qc_pulse, 4) == 0) {
      S.qcPending = true; S.reqMs = now;
    }
  }
  if (p.handback) {
    piano_charge_cp_handback(&fLink.st, p.handback_prop, p.handback_value, now);
    fLink.requestDecision();
    S.ppsTaken = 0; S.qcTaken = 0;
  }
  if (p.events || S.st.state != was)
    note("cp session: %s -> %s, events %#x, ibat target %u mA, bus target %u mA, exit %s (faults %#x)", piano_cp_state_name(was),
         piano_cp_state_name(S.st.state), p.events, p.ibat_target_ma, p.ibus_target_ma, piano_cp_exit_name(p.exit_reason), p.exit_faults);
  S.nextMs = now + (S.st.state == PIANO_CPS_IDLE || S.st.state == PIANO_CPS_LATCHED ? 1000u : 100u);
}

// The codec answered a write: the session's own PPS word or QC pulse is taken (or refused).
void PianoPower::cpAnswered()
{
  if (!gCpSess) return;
  CpSession &S = *gCpSess;
  if (fMca.last_property == PIANO_MCA_PPS && S.ppsPending && fMca.last_value == S.ppsWord) {
    S.ppsPending = false;
    if (fMca.last_result == 0) S.ppsTaken = S.ppsWord;
  } else if (fMca.last_property == PIANO_MCA_QC_PULSE && S.qcPending) {
    S.qcPending = false;
    if (fMca.last_result == 0) S.qcTaken += fMca.last_value == PIANO_CHG_QC_UP ? 1 : -1;
  }
}

void PianoPower::probeGauges()
{
  if (!gArgs.gauge) {
    note("gauges: off (piano-power-gauge=0)");
    return;
  }
  for (GaugeBus &b : gBuses) {
    if (!mapPin(b.sda) || !mapPin(b.scl)) {
      note("gauge %s: TLMM pins %u/%u do not map", b.name, b.sda.gpio, b.scl.gpio);
      continue;
    }
    note("gauge %s (%s, SDA gpio%u, SCL gpio%u): pins cfg %#x/%#x, lines %u/%u before the read", b.name, b.where, b.sda.gpio,
         b.scl.gpio, rd32(b.sda.page.va), rd32(b.scl.page.va), rd32(b.sda.page.va + 4) & 1, rd32(b.scl.page.va + 4) & 1);
    fGauges = true;
  }
  sampleGauges();
  for (unsigned i = 0; i < 2; ++i) {
    const GaugeReading &g = fPack[i];
    if (g.ok)
      note("gauge %s: bq27z561 answers: %d mV, %d mA, %d %%, %d/%d mAh, %d.%d C, status %#x, %d cycles", gBuses[i].name, g.voltageMv,
           g.currentMa, g.socPct, g.remainingMah, g.fullMah, g.tempDeciC / 10, (g.tempDeciC < 0 ? -g.tempDeciC : g.tempDeciC) % 10,
           g.status, g.cycles);
    else
      note("gauge %s: no reading (%s at command %#x)", gBuses[i].name, i2cName(g.error), g.failedCommand);
  }
  stage(3, "gauges");
}

void PianoPower::sampleGauges()
{
  if (!fGauges) return;
  for (unsigned i = 0; i < 2; ++i) fPack[i] = gBuses[i].sda.page.va && gBuses[i].scl.page.va ? readBus(gBuses[i]) : GaugeReading();
  fPackMs = nowMs();
  fBattery = combinePacks(fPack, 2);
  piano_mca_battery_temperature(&fMca, fBattery.ok, fBattery.tempDeciC / 10);
}

void PianoPower::mcaLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
  snprintf(line, size,
           "mca: bus %d mV %d mA, type %s, input limit %d mA (set %u), charge current set %u mA, tbat %d C, charge enable %d, "
           "status %#x, pack %d/%d, %u cycles, %u writes ok, %u failed, %u timeouts, %u port acks%s",
           HAS(McaBusVoltage) ? m.value[McaBusVoltage] / 1000 : -1, HAS(McaBusCurrent) ? m.value[McaBusCurrent] / 1000 : -1,
           HAS(McaType) ? piano_mca_type_name(m.value[McaType]) : "?", HAS(McaIcl) ? m.value[McaIcl] : -1, m.icl_set, m.fcc_set,
           HAS(McaTbat) ? m.value[McaTbat] : -99, HAS(McaChargeEnable) ? m.value[McaChargeEnable] : -1,
           HAS(McaStatus) ? m.value[McaStatus] : -1, HAS(McaPackVoltage) ? m.value[McaPackVoltage] : -1,
           HAS(McaPackCurrent) ? m.value[McaPackCurrent] : -1, m.cycles, m.writes_ok, m.writes_failed, m.timeouts, m.pan_acks,
           m.hv == McaHvOn ? ", 9 V" : m.hv == McaHvBlocked ? ", 9 V blocked" : "");
#undef HAS
}

// The input path as the firmware reports it, logged after every cycle of the first minute (and then with each report): the three
// properties that can hold the input off whatever the limit says, the decoded charger status, and whether the DSP has sent any
// USB-C port notification at all. Read only: nothing here writes.
// The port as the firmware sees it, the board configuration read back, the charger domain and the gauges (docs/ubuntu-history/
// device.md §3.1): a DSP that believes it sources, or is stuck in an adapter authentication, explains a live bus with no input.
void PianoPower::diagPortLine(char *line, size_t size, bool board)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
#define VAL(f) (HAS(f) ? m.value[f] : -1)
  uint32_t ups = 0;
  const uint32_t pd = piano_qrtr_charger_pd(&ups);
  const int termRaw = VAL(McaTermVolt);
  static const char *const kPan[] = {"not answered yet", "answered", "NO ANSWER"};
  static const char *const kUcsi[] = {"off", "due", "asked", "answered", "NO ANSWER"};
  const char *ucsi = kUcsi[m.ucsi < 5 ? m.ucsi : 0];
  if (fPolicyOn) {
    const uint64_t now = nowMs();
    ucsi = !gArgs.ucsi ? "observer disabled" : fContract.quarantined ? "observer quarantined" :
           piano_contract_fresh(&fContract, now) ? "observer PD valid" :
           piano_contract_status_fresh(&fContract, now) ? "observer non-PD valid" : "observer querying/unknown";
  }
  if (!board) {
    snprintf(line, size,
             "diag port: verify (0x21002) %d, PD state (0x2100d) %d, data role (0x2100c) %d, OTG boost (0x200d) %d, gate (0x200e) %d, "
             "plug-in (0x21020) %d, Type-C mode (0x21015) %d, CC toggle (0x21021) %d, sink/source (0x21022) %d, online (0x20001) %d",
             VAL(McaVerify), VAL(McaPdState), VAL(McaDataRole), VAL(McaOtgBoost), VAL(McaOtgGate), VAL(McaOtgPlugin),
             VAL(McaTypecMode), VAL(McaCcToggle), VAL(McaSnkSrc), VAL(McaUsbType));
  } else {
    snprintf(line, size,
             "diag board: CP (0x201c) %d, CID (0x201b) %#x, OTG (0x201a) %d, sent %#x%s; float (0x2007) %d = %d mV; port enable %s; "
             "UCSI %s; charger_pd %s (generation %u); battery (gauges) %d mA",
             VAL(McaCpState), (uint32_t)VAL(McaCidCfg), VAL(McaOtgCfg), m.board_done, m.board_allowed ? "" : " (off)", termRaw,
             HAS(McaTermVolt) ? (termRaw + 3600) / 2 : -1, kPan[m.pan_enabled < 3 ? m.pan_enabled : 0], ucsi,
             servreg_state_name(pd), ups, fBattery.ok ? fBattery.currentMa : 0);
  }
#undef VAL
#undef HAS
}

void PianoPower::diagLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
#define HAS(f) piano_mca_has(&m, f)
  snprintf(line, size,
           "diag: input suspend (0x2019) %s, AICL enable (0x2012) %s, AICL status (0x2029) %s%#x, charger status (0x2001) %#x (%s), "
           "bus current (0x20004) %d mA, input limit (0x2008) %d mA; port notifications %u received, %u acked, enable %s; "
           "%u reads, %u errors, %u timeouts, %u ignored, %u notifications",
           HAS(McaInputSuspend) ? (m.value[McaInputSuspend] ? "1 (SUSPENDED)" : "0 (not suspended)") : "not readable",
           HAS(McaAiclEnable) ? (m.value[McaAiclEnable] ? "1" : "0") : "not readable",
           HAS(McaAiclStatus) ? "" : "not readable ", HAS(McaAiclStatus) ? (uint32_t)m.value[McaAiclStatus] : 0u,
           HAS(McaStatus) ? (uint32_t)m.value[McaStatus] : 0u,
           HAS(McaStatus) ? piano_mca_status_name(m.value[McaStatus]) : "not readable",
           HAS(McaBusCurrent) ? m.value[McaBusCurrent] / 1000 : -1, HAS(McaIcl) ? m.value[McaIcl] : -1,
           m.pan_notifies, m.pan_acks, m.pan_enable ? "due" : "sent", m.answers, m.errors, m.timeouts, m.ignored, m.notifications);
#undef HAS
}

// piano-power-ps=1: the battery for macOS (PianoBattery.cpp), from this cycle's gauge and port readings; one line per cycle.
// The adapter shown to macOS is the firmware's own contract when it has answered (level 2), the Rp current otherwise.
void PianoPower::publishBattery(uint64_t t, const PortState *port)
{
  PackReading published = fBattery;
  const uint32_t cycle = fPsPublishCycles;
  if (fBattery.ok) ++fPsPublishCycles;
  if (gArgs.psInject && fBattery.ok && cycle < gArgs.psInject) {
    if (cycle == 0) {
      // The 12 % incident: a first-cycle FullChargeCapacity read of ~68 Ah made 8.2 Ah look like 12 %.
      published.fullMah = 68220;
      published.socPct = int((long(published.remainingMah) * 100 + published.fullMah / 2) / published.fullMah);
      note("[%llus] ps-inject: cycle %u garbage FullChargeCapacity %d mAh -> %d %% before the plausibility guard", t, cycle + 1,
           published.fullMah, published.socPct);
    } else {
      // A second first-window failure: a zero remaining-capacity sample must not become a low-battery publication.
      published.remainingMah = 0;
      published.socPct = 0;
      note("[%llus] ps-inject: cycle %u zero remaining capacity -> 0 %% before the plausibility guard", t, cycle + 1);
    }
  }
  AdapterReal real;
  if (fPolicyOn && piano_contract_fresh(&fContract, nowMs())) {
    real.ok = real.pd = true;
    real.mv = fContract.mv;
    real.ma = fContract.ma;
  } else if (fPolicyOn && fLink.st.hv_qc && fLink.st.hv == PIANO_CHG_HV_ON && fLink.st.icl_set) {
    real.ok = true;   // cfg.qc at 9 V: the source's 9 V at the input limit in force (QC has no contract to show)
    real.mv = 9000;
    real.ma = int32_t(fLink.st.icl_set);
  }
  clock_sec_t sec = 0;
  clock_usec_t usec = 0;
  clock_get_calendar_microtime(&sec, &usec);
  const uint32_t nowSec = sec > 1600000000 ? uint32_t(sec) : 0;   // before 2020: the calendar is not up yet
  BatteryValues v = mapBattery(fPack, 2, published, port, gArgs.psFloor, &real, nowSec, gArgs.psGuard);
  if (fPolicyOn) {
    const PianoChargeState &s = fLink.st;
    v.healthEnabled = fLink.cfg.limit != 0; v.healthStop = fLink.cfg.stop_pct; v.healthResume = fLink.cfg.resume_pct;
    v.healthHold = s.hold; v.healthReason = fLink.plan.why_off; v.healthRevision = fConfigRevision;
    // Independent power evidence keeps AC connected while charge is stopped,
    // even if this round's Type-C observer did not answer.
    const uint64_t now = nowMs();
    const bool ucsiPower = piano_contract_status_fresh(&fContract, now) &&
                           fContract.connected && fContract.sink;
    const bool busPower = piano_mca_has(&fMca, McaBusVoltage) && fMca.value[McaBusVoltage] >= 4000000;
    if (ucsiPower || busPower) v.external = v.chargeCapable = true;
    const bool enableKnown = piano_mca_has(&fMca, McaChargeEnable);
    const bool enable = enableKnown && fMca.value[McaChargeEnable] == 1;
    v.healthApplied = enableKnown && !s.pending && !fMca.queued && enable == (fLink.plan.charge != 0);
    v.healthPending = !v.healthApplied;
    // Actual positive pack current AND confirmed enabled, never the software
    // desired charge vote. A health stop never means FullyCharged.
    v.charging = v.external && fBattery.charging && enable && fBattery.currentMa >= 50;
    if (!v.charging) v.timeToFull = v.timeRemaining = kTimeUnknown;
  }
  if (v.implausible) {
    note("[%llus] ps: implausible pack reading (%d/%d mAh): the power source is %s", t, published.remainingMah, published.fullMah,
         pianoBatteryUpdate(this, v) ? "left as it was" : "not published yet");
    return;
  }
  if (!v.known) {
    note("[%llus] ps: no gauge reading: the power source is %s", t, pianoBatteryUpdate(this, v) ? "left as it was" : "not published yet");
    return;
  }
  pianoBatteryUpdate(this, v);
  note("[%llus] ps: %u %%%s, %s, %u mV, %d mA (instant %d), %u/%u mAh (design %u), %u cycles, time remaining %u min%s%s", t,
       v.percent, v.clamped ? " (floor)" : "",
       v.external ? (v.charging ? "on power, charging" : v.full ? "on power, full" : "on power, not charging") : "on battery",
       v.voltageMv, v.amperageMa, v.instantMa, v.rawMah, v.fullMah, v.designMah, v.cycles, v.timeRemaining,
       v.timeRemaining == kTimeUnknown ? " (unknown)" : "",
       v.adapter ? (v.adapterPd ? " | adapter PD contract" : " | adapter Rp") : "");
}

void PianoPower::report()
{
  const uint64_t t = (nowMs() - fStartMs) / 1000;
  sampleGauges();
  if (fGauges) {
    const PackReading &b = fBattery;
    if (b.ok)
      note("[%llus] battery: %d mV, %d mA (%s), %d %% (%d/%d mAh), %d.%d C, %u of 2 packs | A %d mV %d mA %d %% st %#x | B %d mV %d mA "
           "%d %% st %#x",
           t, b.voltageMv, b.currentMa, b.charging ? "charging" : b.discharging ? "discharging" : "idle", b.socPct, b.remainingMah,
           b.fullMah, b.tempDeciC / 10, (b.tempDeciC < 0 ? -b.tempDeciC : b.tempDeciC) % 10, b.packs, fPack[0].voltageMv,
           fPack[0].currentMa, fPack[0].socPct, fPack[0].status, fPack[1].voltageMv, fPack[1].currentMa, fPack[1].socPct,
           fPack[1].status);
    else
      note("[%llus] battery: no usable gauge sample (A: %s at %#x, B: %s at %#x)", t, i2cName(fPack[0].error),
           fPack[0].failedCommand, i2cName(fPack[1].error), fPack[1].failedCommand);
  }
  // The RTC from the first report after IOBSD is published: the kernel's calendar (k27 0060, IOKitInitializeTime) reads the same
  // observer channels earlier in bsd_init, and IOKitBSDInit publishes IOBSD after it, so the two never share a channel.
  const IOService *resources = IOService::getResourceService();
  if (fArbiter.v7 && (fRtcProbed || (resources && resources->getProperty(kIOBSDKey)))) {
    char line[200];
    readRtcLine(line, sizeof line);
    note("[%llus] %s", t, line);
    if (!fRtcProbed)
      note("rtc: RTC_RW APID %d (owner EE %u) type %#x subtype %#x EN_CTL %#x; SDAM APID %d (owner EE %u), offset cell %s %u (%#x)",
           fRtc.rtc.apid, fRtc.rtc.owner, fRtc.type, fRtc.subtype, fRtc.enCtl, fRtc.sdam.apid, fRtc.sdam.owner,
           fRtc.sdamOk ? "reads" : "does not read:", fRtc.sdamOffset, fRtc.sdamOffset);
    fRtcProbed = true;
  }
  PortState p;
  const bool havePort = fSpmi && fCharger.found;
  if (havePort) {
    p = readPort(gIo, fCharger);
    if (p.ok)
      note("[%llus] usb: %s%s, Rp bits %#x (%u mA), mode %#x | typec %02x %02x %02x %02x %02x %02x rt %#x | usbin %02x %02x "
           "%02x %02x %02x %02x %02x %02x rt %#x | dcdc %02x %02x %02x %02x %02x %02x %02x %02x rt %#x | chgr %02x %02x %02x %02x %02x "
           "%02x %02x %02x",
           t, !p.attached ? "nothing attached" : p.sourceSeen ? "a source is attached (charger present)" : p.sinkSeen ? "a device is attached (we source)" : "attached",
           p.attached ? (p.cc == 2 ? " on CC2" : " on CC1") : "", p.rpBits, rpMilliamps(p.rpBits), p.mode, p.typec[0], p.typec[1],
           p.typec[2], p.typec[3], p.typec[4], p.typec[5], p.typecRt, p.usbin[0], p.usbin[1], p.usbin[2], p.usbin[3], p.usbin[4],
           p.usbin[5], p.usbin[6], p.usbin[7], p.usbinRt, p.dcdc[0], p.dcdc[1], p.dcdc[2], p.dcdc[3], p.dcdc[4], p.dcdc[5], p.dcdc[6],
           p.dcdc[7], p.dcdcRt, p.charger[0], p.charger[1], p.charger[2], p.charger[3], p.charger[4], p.charger[5], p.charger[6],
           p.charger[7]);
    else
      note("[%llus] usb: Type-C status not readable", t);
  }
  if (fSpmi) {   // the SPMI lock every kext shares (PianoSpmi.h): this kext's transactions and waits, and every kext's
    char line[360];
    pianospmi::format(line, sizeof line);
    note("[%llus] %s", t, line);
  }
  if (fWdog.located) {
    WdogWrite clear, pet;
    fWdog.check(gIo, gWriter, nowMs(), &clear, &pet);
    wdogWriteLine("again", clear);
    wdogWriteLine("first", pet);
    char line[512];
    wdogLine(line, sizeof line, t);
    note("%s", line);
    chgenStep(t, havePort && p.ok ? &p : nullptr, fChgenWhy, sizeof fChgenWhy);
  }
  if (fCx.located) {
    // the whole complex (ChargerFix.h): what changed since the last report, the watch's state, the recovery's turn, the picture
    const uint64_t now = nowMs();
    const CxSnapshot s = fCx.read(gIo, true, now);
    if (fCxHave) {
      char diff[480];
      if (cxDiff(fCxLast, s, diff, sizeof diff))
        note("[%llus] chgr: changed since [%llus]: %s", t, fCxLast.ms > fStartMs ? (fCxLast.ms - fStartMs) / 1000 : 0, diff);
    }
    CxTransition x;
    const bool moved = fWatch.see(s, &x);
    fCxLast = s;
    fCxHave = true;
    if (moved) watchLine(x);
    fixPoll(now);
    publishCharger();
  }
  if (gArgs.ps) publishBattery(t, havePort && p.ok ? &p : nullptr);
  if (gArgs.level >= 2) {
    PianoAdspStatus s;
    piano_adsp_status(&s);
    if (fWdog.located && !fWdog.dspOwns && s.slave >= 0 && (s.slave & 2)) {
      fWdog.dspOwns = true;
      note("wdog: the DSP runs: its firmware owns the charger's configuration from here (no clear of ours); the pets go on");
    }
    uint32_t open = 0, delivered = 0, refused = 0;
    piano_qrtr_port_counters(&open, &delivered, &refused);
    PianoAdspHealth h;
    piano_adsp_health(&h);
    note("[%llus] adsp: stage %d (%s)%s, slave-kernel %d, %u fatal, %u doorbells, GLINK %u in %u out, channels %#x%s, qrtr %u (%u "
         "asked, %u answered, %u services), link generation %u, quiet %#x, client ports %u (%u delivered, %u refused)%s%s",
         t, s.stage, piano_adsp_stage_name(s.stage), s.stage < 0 ? " FAILED" : "", s.slave, s.fatal, s.doorbells, s.glink_in, s.glink_out,
         s.channels_up, s.broken ? " BROKEN" : "", s.qrtr_stage, s.qrtr_requests, s.qrtr_answers, s.qrtr_services, h.link_generation,
         h.quiet, open, delivered, refused, s.text[0] ? ", " : "", s.text);
    char line[520];
    mcaLine(line, sizeof line);
    note("[%llus] %s", t, line);
    diagLine(line, sizeof line);
    note("[%llus] %s", t, line);
    diagPortLine(line, sizeof line, false);
    note("[%llus] %s", t, line);
    diagPortLine(line, sizeof line, true);
    note("[%llus] %s", t, line);
    if (fPolicyOn) {
      policyLine(line, sizeof line);
      note("[%llus] %s", t, line);
      inputLine(line, sizeof line);
      note("[%llus] %s", t, line);
    }
    chargingLine(line, sizeof line);
    note("[%llus] %s", t, line);
  } else {
    PianoAdspSurvey s;
    if (!piano_adsp_survey(&s)) note("[%llus] adsp: slave-kernel %d (not started at level 1)", t, s.slave);
    char line[448];
    chargingLine(line, sizeof line);
    note("[%llus] %s", t, line);
  }
}

// ---------------------------------------------------------------------------------------- level 2: the charger properties
void PianoPower::channelState(void *context, int up)
{
  PianoPower *self = static_cast<PianoPower *>(context);
  __atomic_store_n(&self->fLinkUp, up ? 1u : 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&self->fLinkChanged, 1u, __ATOMIC_RELEASE);
  thread_wakeup((event_t)self);
}

void PianoPower::channelReceive(void *context, const uint8_t *data, uint32_t length)
{
  PianoPower *self = static_cast<PianoPower *>(context);
  IOSimpleLockLock(self->fRingLock);
  if (self->fRingCount < kRing && length <= PIANO_ADSP_MAX_MESSAGE) {
    auto &slot = self->fRing[(self->fRingHead + self->fRingCount) % kRing];
    memcpy(slot.data, data, length);
    slot.length = length;
    self->fRingCount++;
  } else {
    self->fDropped++;
  }
  IOSimpleLockUnlock(self->fRingLock);
  thread_wakeup((event_t)self);
}

void PianoPower::startCharger()
{
  piano_mca_init(&fMca, gArgs.write ? 1 : 0, gArgs.hv != 0);
  piano_mca_allow_unsuspend(&fMca, gArgs.unsuspend != 0);
  piano_mca_allow_board(&fMca, gArgs.board != 0);
  piano_contract_init(&fContract);
  piano_contract_allow_firmware_pdos(&fContract, fPolicyOn && gArgs.ucsi && gArgs.pdoMca);
  piano_mca_clamp_pdos(&fMca, fPolicyOn && gArgs.ucsi && gArgs.pdoMca);
  piano_mca_allow_ucsi(&fMca, gArgs.ucsi != 0 && !fPolicyOn);
  if (fBattery.ok) piano_mca_battery_temperature(&fMca, 1, fBattery.tempDeciC / 10);
  if (fPolicyOn) {
    LinkOptions o;
    o.strictContract = true;
    o.writes = gArgs.write != 0;
    o.hv = gArgs.hv;
    o.qc3 = gArgs.b4pd != 0;
    o.rp = gArgs.rp != 0;
    o.ramp = gArgs.ramp != 0;
    o.rpMax = gArgs.rpMax;
    o.health = gArgs.health != 0;
    o.stopPct = gArgs.healthStop;
    o.resumePct = gArgs.healthResume;
    o.kick = gArgs.kick != 0;
    o.hvOrder = gArgs.hvOrder != 0;
    o.hvCaps = gArgs.hvCaps != 0;
    o.qc = gArgs.qc != 0;
    o.fvHot = gArgs.fvHot != 0;
    fLink.configure(fMca, o);
    if (fLink.healthRefused)
      note("policy: piano-power-health-stop=%u / -resume=%u refused (stop 50..100, resume 40..stop - 1): battery health stays off",
           gArgs.healthStop, gArgs.healthResume);
  }
  static const PianoAdspClient client = {&PianoPower::channelState, &PianoPower::channelReceive};
  fChannel = piano_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this);
  if (!fChannel) {
    stage(-4, "no PMIC_RTR_ADSP_APPS channel slot");
    return;
  }
  note("level 2: starting the DSP after piano-power-delay (default 30 s); once it runs, its firmware limits the input to 100 mA until "
       "the input limit write takes (watch for 'mca: write input limit ... ok')");
  piano_adsp_start();
  fReadyWarnDue = nowMs() + 180000 + uint64_t(bootArg("piano-power-delay", 30)) * 1000;   // 3 minutes after the bring-up began
  stage(4, "DSP start requested, PMIC_RTR_ADSP_APPS registered");
}

void PianoPower::logMcaEvents()
{
  const uint32_t e = piano_mca_take_events(&fMca);
  if (!e && fPdosLine) return;
  const PianoMca &m = fMca;
  if (e & McaEvAttach)
    note("mca: a source is attached: %d mV, type %s", m.value[McaBusVoltage] / 1000,
         piano_mca_has(&m, McaType) ? piano_mca_type_name(m.value[McaType]) : "?");
  if (e & McaEvAttach) fPdosLine = false;   // a new source: its PDO list is logged once it has been read
  // The PD source's offers, logged once per charger generation (0x21011, read only): what a 9 V request (piano-power-hv) has to pick
  if (!fPdosLine && piano_mca_has(&m, McaPdos) && m.pdos) {
    char line[160];
    unsigned at = 0;
    for (unsigned i = 0; i < m.pdos && i < 7; ++i) {
      const int n = snprintf(line + at, sizeof line - at, "%s%u.%u-%u V %u mA", i ? ", " : "", m.pdo[i][0] / 1000,
                             (m.pdo[i][0] % 1000) / 100, m.pdo[i][1] / 1000, m.pdo[i][2]);
      if (n <= 0 || unsigned(n) >= sizeof line - at) break;
      at += unsigned(n);
    }
    note("mca: the source's PDOs (%u): %s", m.pdos, line);
    fPdosLine = true;
  }
  if (!e) return;
  if (e & McaEvDetach) note("mca: the source is gone: input limit and charge current back to the firmware's own values");
  if (e & McaEvHvAsked) note("mca: asking the PD source for its fixed 9 V PDO");
  if (e & McaEvHvOn) note("mca: the bus is at %d mV: 9 V on", m.value[McaBusVoltage] / 1000);
  if (e & McaEvHvBlocked) note("mca: 9 V not reached or lost: 5 V until the source is detached");
  if (e & McaEvRefused) note("mca: a write outside the allowed set was refused (%u so far)", m.refused);
  if (e & McaEvPanEnabled) note("mca: USB-C port notifications enabled: owner 0x800c answered the enable (opcode 0x15, cmd 0x10)");
  if (e & McaEvPanSilent) note("mca: no answer from owner 0x800c to the port enable in 5 s: the port owner is not served now");
  if (e & McaEvUcsiSilent) note("ucsi: no answer from owner 0x800b in 5 s");
  if (e & McaEvBoardDone) note("mca: board configuration taken (CP state, OTG configuration, CID): the policy follows");
  if (e & McaEvBoardGaveUp) note("mca: board configuration not taken after 5 cycles (sent %#x): the policy goes on without it", m.board_done);
  if (e & McaEvWrite) {
    const char *what = m.last_property == PIANO_MCA_ICL ? "input limit" : m.last_property == PIANO_MCA_FCC ? "charge current"
                       : m.last_property == PIANO_MCA_CHARGE_ENABLE ? "charge enable" : m.last_property == PIANO_MCA_PD_FIXED ? "PD fixed request"
                       : m.last_property == PIANO_MCA_INPUT_SUSPEND ? "input suspend" : m.last_property == PIANO_MCA_CP_STATE ? "CP state"
                       : m.last_property == PIANO_MCA_OTG_CFG ? "OTG configuration" : m.last_property == PIANO_MCA_CID_CFG ? "CID configuration"
                       : "property";
    note("mca: write %s (%#x) = %u: %s (%d)", what, m.last_property, m.last_value,
         m.last_result == 0 ? "ok" : m.last_result == -60 ? "no answer" : "refused by the firmware", m.last_result);
  }
}

// piano-power-pdo-mca: after each finished cycle, the charger firmware's decoded source list of this cycle, offered only while the
// same cycle read PD state SNK_Ready (ChargeContract.h decides whether a PD sink status may use it).
void PianoPower::offerFirmwarePdos()
{
  const PianoMca &m = fMca;
  const bool ready = piano_mca_has(&m, McaPdos) && piano_mca_has(&m, McaPdState) && m.value[McaPdState] == 2 && m.pdos;
  const uint32_t rejects = fContract.offer_rejects, withdrawals = fContract.offer_withdrawals;
  piano_contract_offer_pdos(&fContract, m.pdo, ready ? m.pdos : 0, ready);
  const uint8_t n = fContract.offer_n;
  if ((n == fPdoOfferLoggedN && rejects == fContract.offer_rejects && withdrawals == fContract.offer_withdrawals) ||
      fPdoOfferLogs >= 24) return;
  fPdoOfferLogs++;
  fPdoOfferLoggedN = n;
  char line[200];
  unsigned at = 0;
  line[0] = 0;
  for (unsigned i = 0; i < n && i < 7; ++i) {
    const int k = snprintf(line + at, sizeof line - at, "%s%u-%u mV %u mA", i ? ", " : "", unsigned(fContract.offer_pdo[i][0]),
                           unsigned(fContract.offer_pdo[i][1]), unsigned(fContract.offer_pdo[i][2]));
    if (k <= 0 || unsigned(k) >= sizeof line - at) break;
    at += unsigned(k);
  }
  note("ucsi: firmware source list offered (%u objects%s): %s; %u rejected, %u withdrawn [PianoPdoFirmwareV1]", n,
       ready ? "" : ", not SNK_Ready", n ? line : "none", fContract.offer_rejects, fContract.offer_withdrawals);
}

void PianoPower::logFirmwareContract()
{
  const PianoChargeContract &c = fContract;
  if (fPdoContractLogs >= 32) return;
  // one line per change of validity or RDO (a renegotiation, a new source)
  const uint32_t rdo = c.valid ? c.rdo : 0;
  if (rdo == fPdoContractLoggedRdo && c.valid == fPdoContractLoggedValid) return;
  fPdoContractLoggedRdo = rdo;
  fPdoContractLoggedValid = c.valid;
  fPdoContractLogs++;
  if (!c.valid) {
    note("ucsi: no PD contract (connected %u, sink %u, PD %u, RDO %#x, %u source objects, error %d, generation %u)", c.connected,
         c.sink, c.pd, c.rdo, c.pdos, int(c.last_error), c.generation);
    return;
  }
  note("ucsi: PD contract from the %s source list: object %u, %s %u mV %u mA (source %u mA), RDO %#x, generation %u",
       c.pdo_origin == PIANO_CONTRACT_PDO_FIRMWARE ? "firmware's" : "UCSI", c.object,
       c.protocol == PIANO_CONTRACT_PPS ? "PPS" : "fixed", c.mv, c.ma, c.source_ma, c.rdo, c.generation);
}

void PianoPower::pumpMca(uint64_t now)
{
  uint8_t out[PIANO_CONTRACT_REQUEST_MAX];
  if (fPolicyOn && gArgs.ucsi) {
    if (now >= fContractRefresh) {piano_contract_refresh(&fContract); fContractRefresh = now + 5000;}
    const uint32_t n = piano_contract_next(&fContract, now, out);
    if (n && piano_adsp_send(fChannel, out, n)) { fContract.timeout_layer = 1; piano_contract_timeout(&fContract, now); }
    // A bounded first-init trace distinguishes a silent transport from an
    // uninitialized PPM or a command that never completed. The bounded UCSI
    // MessageIn words distinguish capability fields from a CCI failure.
    if (fContract.stage != fContractLoggedStage && fContractLogCount < 32) {
      fContractLoggedStage = fContract.stage; ++fContractLogCount;
      note("ucsi-init: stage=%u initialized=%u cmd=%#llx cci=%#x reads=%u/%u writes=%u/%u notify=%u reset=%u timeout-layer=%u [PianoUcsiMessageInV4]",
           fContract.stage, fContract.initialized, fContract.command, fContract.cci,
           fContract.read_replies, fContract.reads, fContract.write_replies, fContract.writes,
           fContract.notifies, fContract.resets, fContract.timeout_layer);
      if (fContract.message_length)
        note("ucsi-message: cmd=%#x length=%u in=%08x/%08x/%08x/%08x attrs=%#x features=%#x connectors=%u",
             fContract.message_command, fContract.message_length, fContract.message_in[0], fContract.message_in[1],
             fContract.message_in[2], fContract.message_in[3], fContract.capabilities, fContract.features, fContract.cap_connectors);
    }
  }
  // Expiry/timeout may occur in next(), without a receive callback. Revoke
  // all unsent writes and decide the fallback BEFORE sending any MCA write.
  if (fPolicyOn) {
    const bool fresh = piano_contract_fresh(&fContract, now);
    if ((fContractWasFresh && !fresh) || fContractSeenGeneration != fContract.generation) {
      fLink.cancelQueued(fMca);
      fLink.requestDecision();
      stepPolicy(now);
    }
    fContractWasFresh = fresh;
    fContractSeenGeneration = fContract.generation;
  }
  if (!fRequestDue) {
    const uint32_t n = piano_mca_next(&fMca, out);
    if (n) {
      if (piano_adsp_send(fChannel, out, n)) note("mca: send queue full");
      else if (fPolicyOn) fLink.sent(fMca);
      fRequestDue = now + (fMca.sent_write ? gArgs.writeMs : 1000);   // a write's answer may take the firmware longer than a read's
    }
  }
  // The port owner's writes get 5 s each, as Linux gives them (kernel.md §2.5): no answer to the enable means the owner is not served.
  if (!fPanDue) {
    const uint32_t n = piano_mca_pan_next(&fMca, out);
    if (n) {
      piano_adsp_send(fChannel, out, n);
      fPanDue = now + 5000;
    }
  }
  if (!fPolicyOn && !fUcsiDue) {
    const uint32_t n = piano_mca_ucsi_next(&fMca, out);
    if (n) {
      note("ucsi: reading the buffer of owner 0x800b (read only)");
      piano_adsp_send(fChannel, out, n);
      fUcsiDue = now + 5000;
    }
  }
}

void PianoPower::serviceMca(uint64_t now)
{
  if (!fChannel) {stepPolicy(now); return;}
  // The channel, then the gate: like every Linux pmic_glink client, nothing is sent until msm/adsp/charger_pd reports up on the DSP's
  // service registry (PianoQrtr.c); without any answer for 60 s after the channel came up, the codec starts anyway (logged). Each
  // new charger generation (charger_pd up again) links the codec afresh, so the board configuration, the port enable and the
  // policy are all sent again (the firmware is back on its defaults; Ubuntu #18 needed the same re-send to recover).
  if (__atomic_exchange_n(&fLinkChanged, 0u, __ATOMIC_ACQ_REL)) {
    const bool up = __atomic_load_n(&fLinkUp, __ATOMIC_ACQUIRE) != 0;
    if (up != fChannelUp) {
      note("mca: PMIC_RTR_ADSP_APPS %s", up ? "up: waiting for msm/adsp/charger_pd to report up" : "down");
      fChannelUp = up;
      fChannelSinceMs = now;
      fGateTimedOut = false;
    }
  }
  uint32_t ups = 0;
  const uint32_t pd = piano_qrtr_charger_pd(&ups);
  const bool pdUp = pd == SERVREG_STATE_UP;
  if (fChannelUp && !pdUp && !fGateTimedOut && now - fChannelSinceMs >= 60000) {
    fGateTimedOut = true;
    note("mca: no 'up' from msm/adsp/charger_pd 60 s after the channel (state %s): starting without it", servreg_state_name(pd));
  }
  const bool ready = fChannelUp && (pdUp || fGateTimedOut);
  if (ready && fWasUp && pdUp && ups != fPdUpsLinked) {
    note("mca: msm/adsp/charger_pd up again (generation %u): sending everything again", ups);
    piano_mca_link(&fMca, 0);
    fWasUp = false;
  }
  if (ready != fWasUp) {
    note("mca: %s", ready ? (pdUp ? "msm/adsp/charger_pd is up: reading the charger properties" : "reading the charger properties")
                          : "the charger is not reachable now: requests stop");
    piano_mca_link(&fMca, ready);
    piano_contract_link(&fContract, ready);
    fContractRefresh = now;
    fRequestDue = fPanDue = fUcsiDue = 0;
    fCycleDue = now + 5000;
    fWasUp = ready;
    fPdUpsLinked = ups;
  }
  for (;;) {
    uint8_t data[PIANO_ADSP_MAX_MESSAGE];
    uint32_t length = 0;
    IOSimpleLockLock(fRingLock);
    if (fRingCount) {
      length = fRing[fRingHead].length;
      memcpy(data, fRing[fRingHead].data, length);
      fRingHead = (fRingHead + 1) % kRing;
      fRingCount--;
    }
    IOSimpleLockUnlock(fRingLock);
    if (!length) break;
    if (fPolicyOn && length >= 12 && data[0] == 0x0b && data[1] == 0x80 && !data[2] && !data[3]) {
      const bool cv = fContract.valid;
      const uint32_t cg = fContract.generation;
      const int cr = piano_contract_receive(&fContract, now, data, length);
      if ((cv && !fContract.valid) || cg != fContract.generation || cr < 0) {
        fLink.cancelQueued(fMca);
        fLink.requestDecision();
      }
      if (cr == 2 || cr < 0) fLink.requestDecision();
      if (cr == 2 && fContract.pdo_fallback) logFirmwareContract();
      continue;
    }
    if (piano_usbc_relay_receive(data, length)) continue;   // the UCSI client's traffic (UsbcRelay.h): not PianoMca's
    const int r = piano_mca_receive(&fMca, data, length);
    if (r == McaAnswer || r == McaCycleDone || r == McaWritten) fRequestDue = 0;
    if (r == McaWritten && fPolicyOn) {
      // each answer logged as it comes (several can arrive in one pass) and handed to the policy
      note("mca: write %s (%#x) = %u: %s (%d)", piano_mca_property_name(fMca.last_property), fMca.last_property, fMca.last_value,
           fMca.last_result == 0 ? "ok" : "refused by the firmware", fMca.last_result);
      fMca.events &= ~uint32_t(McaEvWrite);
      fLink.answered(fMca);
      cpAnswered();
    }
    if (r == McaLateWritten) {
      // the answer to a write already counted as "no answer": the firmware took (or refused) it after all
      note("mca: late answer to the write of %s (%#x) = %u: %s (%d), after the %u ms window (%u late so far)%s",
           piano_mca_property_name(fMca.last_property), fMca.last_property, fMca.last_value,
           fMca.last_result == 0 ? "ok" : "refused by the firmware", fMca.last_result, gArgs.writeMs, fMca.late_answers,
           fPolicyOn ? ": handed to the policy" : "");
      if (fPolicyOn) {fLink.answered(fMca); cpAnswered();}
    }
    if (r == McaIgnored && fMca.ignored_answers > fIgnoredSeen) {
      fIgnoredSeen = fMca.ignored_answers;
      if (fIgnoredLogged < 8) {
        fIgnoredLogged++;
        note("mca: an answer that matches no request: opcode %u, %s (%#x), sequence %u, return code %d (in flight: %u%s); %u so far",
             fMca.ignored_opcode, piano_mca_property_name(fMca.ignored_property), fMca.ignored_property, fMca.ignored_seq,
             int32_t(fMca.ignored_retcode), fMca.ignored_in_flight, fMca.ignored_in_flight ? "" : " = none", fMca.ignored_answers);
      }
    }
    if (r == McaPanDone) fPanDue = 0;
    if (r == McaUcsiDone) {
      fUcsiDue = 0;
      note("ucsi: owner 0x800b answers: version %x.%x, CCI %#x, %u-byte buffer (UCSI %s), return code %u", fMca.ucsi_version >> 8,
           fMca.ucsi_version & 0xff, fMca.ucsi_cci, fMca.ucsi_bytes, fMca.ucsi_bytes > 48 ? "2.x" : "1.x", fMca.ucsi_ret);
    }
    if (r == McaUcsiNotified && fUcsiNotesLogged < 8) {
      fUcsiNotesLogged++;
      note("ucsi: notification from owner 0x800b (%u so far; nothing is sent back: %s)", fMca.ucsi_notifies,
           piano_usbc_relay_attached() ? "the UCSI client is not ready yet" : "we run no UCSI client");
    }
    if (r == McaPanNotified) {
      if (fPolicyOn) piano_contract_refresh(&fContract);
      note("mca: USB-C port notification (port %u): acknowledged", data[12]);
      // Read only: the notification to the port-event clients (PortEvents.c, PianoAdspContract.h; PianoDp's D2), the acknowledgement
      // already queued. Nothing to do without a client.
      const uint64_t t0 = nowMs();
      if (piano_port_publish(data, length) && nowMs() - t0 > 20)
        note("port events: the clients took %llu ms (the charger waited)", (unsigned long long)(nowMs() - t0));
    }
    if (r == McaCycleDone && fContract.pdo_fallback) offerFirmwarePdos();
    if (r == McaCycleDone) {
      char line[520];
      if (fMca.cycles == 1) {
        mcaLine(line, sizeof line);
        note("first charger reading: %s", line);
      }
      // The input-path diagnostic after every cycle of the first minute (12 cycles of 5 s), then with each report.
      if (fMca.cycles <= 12) {
        diagLine(line, sizeof line);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
        diagPortLine(line, sizeof line, false);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
        diagPortLine(line, sizeof line, true);
        note("[%llus] %s", (nowMs() - fStartMs) / 1000, line);
      }
    }
  }
  if (!fWasUp) {stepPolicy(now); stepQuiet(now); return;}
  if (fRequestDue && now >= fRequestDue) {
    if (fPolicyOn) fLink.timeout(fMca);   // a write's timeout is the policy's answer too (-60)
    else piano_mca_timeout(&fMca);
    fRequestDue = 0;
  }
  if (fPanDue && now >= fPanDue) {piano_mca_pan_timeout(&fMca); fPanDue = 0;}
  if (fUcsiDue && now >= fUcsiDue) {piano_mca_ucsi_timeout(&fMca); fUcsiDue = 0;}
  if (now >= fCycleDue) {piano_mca_refresh(&fMca); fCycleDue = now + 5000;}
  stepPolicy(now);
  pumpMca(now);
  logMcaEvents();
  stepQuiet(now);
}

// Clients (audio, sensors) hold their traffic while a charger generation is configured: from the codec's link until a whole cycle ends
// with nothing left to write and the port enable answered (or timed out), at most 60 s.
void PianoPower::stepQuiet(uint64_t now)
{
  const int e = power_quiet_step(&fQuiet, fWasUp, fMca.links, piano_mca_configured(&fMca), now, kQuietCapMs);
  if (e == PowerQuietNone) return;
  piano_adsp_quiet_set(PIANO_ADSP_QUIET_CHARGER, fQuiet.raised);
  note("quiet: %s", e == PowerQuietRaised ? "raised for the DSP's other clients: configuring the charger"
                    : e == PowerQuietReleased ? "released: the charger is configured"
                    : e == PowerQuietTimedOut ? "released after 60 s with the charger not configured"
                                              : "dropped: the charger is not reachable now");
}

// ------------------------------------------------------------------------------------- piano-power-policy=1 (ChargeLink.h)
namespace {
// A board temperature for the log: whole degrees, "-" when it does not count.
const char *tempText(char *buf, size_t size, const piano_board_temps &t, unsigned index)
{
  if (!(t.valid & (1u << index))) return "-";
  snprintf(buf, size, "%d", t.milli_c[index] / 1000);
  return buf;
}
int whole(int32_t dc) { return dc >= 0 ? dc / 10 : -((-dc + 9) / 10); }
}  // namespace

// The sleep or shutdown asked since the thread last looked (the wait in run() must not sleep through it).
bool PianoPower::policyAsked()
{
  if (!fPolicyOn || fPolicyEnded) return false;
  const bool sleep = __atomic_load_n(&fSleepAsk, __ATOMIC_ACQUIRE) != 0;
  const bool down = __atomic_load_n(&fShutdownAsk, __ATOMIC_ACQUIRE) != 0;
  return sleep != fHandover || (down && !fShutdownSeen);
}

// The plan's resync (a wake; Ubuntu #18): what a rebind did, the board configuration, the port enable and the UCSI read with it.
void PianoPower::relinkMca(uint64_t now, const char *why)
{
  note("mca: %s: the charger codec is linked again (board configuration, port enable, UCSI read, every property read again)", why);
  piano_mca_link(&fMca, 0);
  piano_mca_link(&fMca, 1);
  piano_contract_link(&fContract, 1);
  fContractRefresh = now;
  fRequestDue = fPanDue = fUcsiDue = 0;
  fCycleDue = now + 5000;
}

// PianoThermal's board temperatures through its platform function (PianoBoardTemps.h): no link dependency, PianoThermal may be
// absent or inert; then every temperature is unknown, which the policy never takes for cool.
bool PianoPower::boardTemps(piano_board_temps &t, uint64_t now)
{
  if (!fThermal && now >= fThermalLookupMs) {
    fThermalLookupMs = now + 10000;
    OSDictionary *match = IOService::serviceMatching("PianoThermal");
    fThermal = match ? IOService::copyMatchingService(match) : nullptr;   // kept: a kext of the collection never unloads
    OSSafeReleaseNULL(match);
    if (fThermal) {
      note("policy: PianoThermal found: its board temperatures (battery, charger, connectors) join the rules");
    } else if (!fThermalNoted) {
      fThermalNoted = true;
      note("policy: no PianoThermal service: the board temperatures are unknown (never cool: no Rp limit); looked up every 10 s");
    }
  }
  if (!fThermal) return false;
  bzero(&t, sizeof t);
  t.version = PIANO_BOARD_TEMPS_VERSION;
  if (fThermal->callPlatformFunction("piano-thermal-board-temps", false, &t, nullptr, nullptr, nullptr) != kIOReturnSuccess) return false;
  if (t.state != fBoardStateShown || t.valid != fBoardValidShown) {
    fBoardStateShown = t.state;
    fBoardValidShown = t.valid;
    static const char *const kState[] = {"off (no piano-thermal-adc)", "starting", "running", "STOPPED"};
    char a[12], b[12], c[12], d[12];
    note("policy: board temperatures %s, valid %#x: battery %s, charger %s, connector %s, connector 2 %s C (disabled %#x)",
         t.state < 4 ? kState[t.state] : "?", t.valid, tempText(a, sizeof a, t, PIANO_BOARD_TEMP_BATTERY),
         tempText(b, sizeof b, t, PIANO_BOARD_TEMP_CHARGER), tempText(c, sizeof c, t, PIANO_BOARD_TEMP_CONNECTOR),
         tempText(d, sizeof d, t, PIANO_BOARD_TEMP_CONNECTOR2), t.disabled);
  }
  return true;
}

// The charging hold (charging-policy.md §5): no idle sleep while protected charging runs below the limit. Created on first use.
void PianoPower::setHold(bool on)
{
  if (on == fHoldOn) return;
  IOPMrootDomain *root = getPMRootDomain();
  if (!root) return;
  if (!fHoldId) {
    if (!on) return;
    fHoldId = root->createPMAssertion(kIOPMDriverAssertionPreventSystemIdleSleepBit, kIOPMDriverAssertionLevelOff, this,
                                      "com.unixnu.driver.PianoPower.charging-hold");
    if (!fHoldId) {
      if (!fHoldFailed) note("policy: the charging-hold assertion could not be created: idle sleep is not held off");
      fHoldFailed = true;
      return;
    }
  }
  root->setPMAssertionLevel(fHoldId, on ? kIOPMDriverAssertionLevelOn : kIOPMDriverAssertionLevelOff);
  fHoldOn = on;
  note("policy: charging hold %s", on ? "on: no idle sleep while protected charging runs below the limit" : "off");
}

void PianoPower::policyLine(char *line, size_t size)
{
  const PianoChargePlan &p = fLink.plan;
  const PianoChargeConfig &c = fLink.cfg;
  static const char *const kWhy[] = {"row", "Rp", "9 V row", "connector hot", "charger hot", "5 V RDO"};
  static const char *const kHv[] = {"5 V", "9 V asked", "9 V", "9 V blocked"};
  static const char *const kOff[] = {"", " (battery health limit)", " (battery too hot)", " (battery too cold)", " (asleep)",
                                    " (admin inhibit)", " (gauge data unavailable)", " (safety fault)", " (firmware inhibit)"};
  char rp[24] = "";
  if (p.rp_ma) snprintf(rp, sizeof rp, ", %u mA advertised", p.rp_ma);
  snprintf(line, size,
           "policy: %s, input limit %u mA (%s%s), charge current %u mA, %s, %s%s, hold %s; %u decisions, %u writes queued, %u "
           "refused, %u relinks; Rp %s; battery health %s",
           piano_charge_class_name(p.cls), p.icl_ma, kWhy[p.icl_why < 6 ? p.icl_why : 0], rp, p.fcc_ma, kHv[p.hv < 4 ? p.hv : 0],
           p.charge ? "charging" : "not charging", kOff[p.why_off <= PIANO_CHG_OFF_FIRMWARE ? p.why_off : 0], fLink.hold ? "on" : "off", fLink.decisions,
           fLink.queuedTotal, fLink.refusedTotal, fLink.resyncs,
           !c.rp ? "not trusted" : fLink.st.rp_distrust ? "DISTRUSTED (a sag or quick detaches)" : "trusted",
           c.limit ? (fLink.st.hold ? "stopped at the limit" : "on") : "off");
}

// The input as the charger firmware reads it and where the steps are, with each report (under the console's 255 characters): the
// verdict lines for a charging test are this one and "battery:".
void PianoPower::inputLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
  const PianoChargePlan &p = fLink.plan;
#define HAS(f) piano_mca_has(&m, f)
  static const char *const kWait[] = {"no steps", "writing", "holding", "bus low", "no current", "not used up", "at target", "cooling"};
  snprintf(line, size,
           "input: bus %d mV %d mA, limit %d mA asked, 0x2029 %d; steps %u mA (%s) of %u, ceiling %u; writes %u ok %u no answer "
           "%u late, %u unmatched",
           HAS(McaBusVoltage) ? m.value[McaBusVoltage] / 1000 : -1, HAS(McaBusCurrent) ? m.value[McaBusCurrent] / 1000 : -1,
           HAS(McaIcl) ? m.value[McaIcl] : -1, HAS(McaAiclStatus) ? m.value[McaAiclStatus] : -1, p.ramp_ma,
           kWait[p.ramp_wait < 8 ? p.ramp_wait : 0], p.ramp_target_ma, fLink.st.ramp_ceiling_ma, m.writes_ok, m.timeouts,
           m.late_answers, m.ignored_answers);
#undef HAS
}

// The balance of each report (ChargePolicy.c piano_charge_balance, charging-policy.md §17.4), under the console's 255 characters: what
// the input brings (the firmware's bus reading: level 2 only), what both packs take, the system by difference at 90 % conversion
// (an estimate), the limit the input is held to (the request, or 0x2029 when lower) and the verdict. With `battery:` the line a
// charging run is judged by: `charging: in 2480 mA at 4720 mV (11.7 W), battery +950 mA at 4300 mV (4.1 W), system ~6.4 W; ...`.
void PianoPower::chargingLine(char *line, size_t size)
{
  const PianoMca &m = fMca;
  const bool level2 = gArgs.level >= 2;
  PianoChargeBalanceIn bi{};
  bi.input_valid = level2 && piano_mca_has(&m, McaBusVoltage) && piano_mca_has(&m, McaBusCurrent) ? 1 : 0;
  if (bi.input_valid) {
    bi.bus_mv = m.value[McaBusVoltage] / 1000;
    bi.ibus_ma = m.value[McaBusCurrent] / 1000;
  }
  const int32_t asked = level2 && piano_mca_has(&m, McaIcl) ? m.value[McaIcl] : -1;
  const int32_t aicl = level2 && piano_mca_has(&m, McaAiclStatus) ? m.value[McaAiclStatus] : -1;
  bi.limit_ma = asked > 0 ? asked : 0;
  if (aicl >= 100 && (bi.limit_ma <= 0 || aicl < bi.limit_ma)) bi.limit_ma = aicl;
  bi.battery_valid = fBattery.ok && fBattery.packs == 2 ? 1 : 0;
  bi.vbat_mv = fBattery.voltageMv;
  bi.ibat_ma = fBattery.currentMa;
  PianoChargeBalance b;
  piano_charge_balance(&bi, &b);
  // watts with one decimal, signs kept (integers only: the kernel has no floating point here)
  auto w = [](int32_t mw, char *out, size_t n) {
    const int32_t a = mw < 0 ? -mw : mw;
    snprintf(out, n, "%s%d.%d", mw < 0 ? "-" : "", int(a / 1000), int(a % 1000 / 100));
  };
  char in[16], bat[16], sys[16], restart[40] = "";
  w(b.in_mw, in, sizeof in);
  w(b.bat_mw, bat, sizeof bat);
  w(b.sys_mw, sys, sizeof sys);
  if (fPolicyOn && fLink.cfg.kick) snprintf(restart, sizeof restart, "; restarts %u", unsigned(fLink.st.kick_count));
  char state[200] = "";
  if (fWdog.located && fWdog.last.chgrOk)
    snprintf(state, sizeof state, "; CHGR state %u (%s), enable command %#04x, cfg2 %#04x%s%s", fWdog.last.state(),
             chargerStateName(fWdog.last.state()), fWdog.last.chgrCmd[2], fWdog.last.chgrCfg[1], fChgenWhy[0] ? "; chgen: " : "",
             fChgenWhy);
  if (!level2)
    snprintf(line, size, "charging: in ? (level 1: no bus reading without the DSP), battery %+d mA at %d mV (%s W); %s%s; wdog pets %u, "
             "chgen sets %u, pulses %u",
             bi.ibat_ma, bi.vbat_mv, bat, !bi.battery_valid ? "no battery reading" : bi.ibat_ma >= 50 ? "charging"
                                                                     : bi.ibat_ma <= -50 ? "draining" : "not charging", state,
             fWdog.pets, fChgEn.sets, fChgEn.pulses);
  else if (!bi.input_valid)
    snprintf(line, size, "charging: in ? (no bus reading yet), battery %+d mA at %d mV (%s W); %s%s", bi.ibat_ma, bi.vbat_mv, bat,
             piano_charge_balance_name(b.verdict), restart);
  else
    snprintf(line, size, "charging: in %d mA at %d mV (%s W), battery %+d mA at %d mV (%s W), system ~%s W; limit %d asked, 0x2029 %d, "
             "enable %d; %s%s", bi.ibus_ma, bi.bus_mv, in, bi.ibat_ma, bi.vbat_mv, bat, sys, asked, aicl,
             piano_mca_has(&m, McaChargeEnable) ? m.value[McaChargeEnable] : -1, piano_charge_balance_name(b.verdict), restart);
}

void PianoPower::logPolicy(const LinkOutcome &o, uint64_t now)
{
  const uint64_t t = (now - fStartMs) / 1000;
  const PianoChargeInput &in = fLink.in;
  const PianoChargeConfig &c = fLink.cfg;
  if (o.waitingPd) note("policy: waiting for msm/adsp/charger_pd to report up: no policy write before it");
  if (o.waitingBoard) note("policy: waiting for the board configuration to be taken (or given up) before the first policy write");
  const uint32_t e = o.events;
  if (e & PIANO_CHG_EV_GENERATION) note("policy: charger generation %u: the firmware is on its defaults, everything is sent again", in.generation);
  if (e & PIANO_CHG_EV_WAKE) note("policy: wake %u: the codec is relinked and everything is sent again", in.wakes);
  if (e & PIANO_CHG_EV_ATTACH) {
    char rp[40] = "";
    if (in.valid & PIANO_CHG_IN_TYPEC) snprintf(rp, sizeof rp, ", Type-C Rp %u mA", in.rp_ma);
    note("policy: a source is attached: bus %d mV, type %s%s", in.bus_mv, (in.valid & PIANO_CHG_IN_TYPE) ? piano_mca_type_name(in.type) : "?", rp);
  }
  if (e & PIANO_CHG_EV_TYPE) note("policy: the source's type is now %s (%d)", piano_mca_type_name(in.type), in.type);
  if (e & PIANO_CHG_EV_DETACH)
    note("policy: the source is gone: the firmware's own input limit (100 mA) and charge current (500 mA) back where written, a "
         "stop of ours lifted");
  if (e & PIANO_CHG_EV_HV_ASKED) note("policy: asking the PD source for 9 V (%s)", fLink.st.hv_pps ? "its PPS APDO" : "its fixed 9 V PDO");
  if (e & PIANO_CHG_EV_HV_ON) note("policy: the bus is at %d mV: 9 V on", in.bus_mv);
  if (e & PIANO_CHG_EV_HV_BLOCKED) note("policy: 9 V not reached, lost or refused: 5 V until the source is detached");
  if (e & PIANO_CHG_EV_HV_RETRY) note("policy: the source went back to 5 V by itself: 9 V asked again in 10 s");
  if (e & PIANO_CHG_EV_LIMIT_STOP)
    note("policy: battery health: %d %% reached the stop (%u %%): charging stops (0x2003 = 0)", in.soc_max_pct, c.stop_pct);
  if (e & PIANO_CHG_EV_LIMIT_RESUME)
    note("policy: battery health: %d %% is down to the resume (%u %%): charging resumes", in.soc_max_pct, c.resume_pct);
  if (e & PIANO_CHG_EV_HOT) note("policy: battery health: the battery is at 55 C or more: charging stops until below 53 C");
  if (e & PIANO_CHG_EV_COLD) note("policy: battery health: the battery is below 0 C: charging stops until 2 C");
  if (e & PIANO_CHG_EV_SOC_LOST)
    note("policy: PianoBuck5VStaleV1: no fresh dual-gauge SOC; hold %u, charge vote %u, off reason %u",
         fLink.st.hold, fLink.plan.charge, fLink.plan.why_off);
  if (e & PIANO_CHG_EV_STOP_LEAK)
    note("policy: WARNING: 0x2003 reads 0 a minute after the stop and the packs still take %d mA: the bit does not stop charging "
         "here; turn piano-power-health off and report", in.ibat_ma);
  if (e & PIANO_CHG_EV_FW_DISABLED) note("policy: the firmware itself has charging disabled (0x2003 reads 0): not overridden");
  if (e & PIANO_CHG_EV_GAVE_UP) note("policy: a write refused 3 times in a row: left until the next attach, wake or generation");
  if (e & PIANO_CHG_EV_READBACK) note("policy: a value the firmware took 3 times reads back otherwise: left alone");
  if (e & PIANO_CHG_EV_REFUSED) note("policy: a computed write outside the policy's allowlist (a bug): not planned");
  if (e & PIANO_CHG_EV_WEDGE_RESYNC)
    note("policy: the firmware sees no source while the Type-C block sees one (30 s, Ubuntu #18): the codec is relinked");
  if (e & PIANO_CHG_EV_WEDGED) note("policy: WARNING: still no source after two relinks: replug the cable or reboot");
  if (e & PIANO_CHG_EV_SLEEP) note("policy: the sleep handover began");
  if (e & PIANO_CHG_EV_RP_DISTRUST)
    note("policy: the Rp limit is not trusted for the rest of the boot (the bus sagged below 4.5 V under it, or the source went "
         "twice within 30 s of it): the class rows");
  if (e & PIANO_CHG_EV_RP_WITHHELD)
    note("policy: the source advertises %u mA on its Rp, but no connector temperature reads below 50 C (%s): %s", in.rp_ma,
         fThermal ? "PianoThermal's connector channels" : "PianoThermal is not there",
         fLink.cfg.ramp ? "the steps stop at 1500 mA (BC 1.2's current) until one does" : "the class row");
  if (e & PIANO_CHG_EV_NTC_APART)
    note("policy: the battery thermistor (%d C) and the packs (%d C) are more than 10 C apart: the warmer counts", whole(in.t_ntc_dc),
         whole(in.t_warm_dc));
  if (e & PIANO_CHG_EV_CONN_HOT)
    note("policy: a connector thermistor reads 60 C or more (%d / %d C): input limit 100 mA until both read below 50 C",
         (in.valid & PIANO_CHG_IN_CONN1) ? whole(in.t_conn1_dc) : -99, (in.valid & PIANO_CHG_IN_CONN2) ? whole(in.t_conn2_dc) : -99);
  if (e & PIANO_CHG_EV_CONN_COOL) note("policy: both connector thermistors read below 50 C: the input limit follows the source again");
  if (e & PIANO_CHG_EV_CHGR_HOT) note("policy: the charger thermistor reads 85 C or more: input limit 500 mA at most until below 75 C");
  if (e & PIANO_CHG_EV_CHGR_COOL) note("policy: the charger thermistor reads below 75 C: the input limit follows the source again");
  // the stepped Rp limit (ChargePolicy.c ramp(), charging-policy.md §15): every step, every step back, each wait reason once a step
  const PianoChargePlan &pl = fLink.plan;
  static const char *const kBack[] = {"?", "the bus sagged below 4.5 V under it", "the source went within 30 s of the step",
                                      "the firmware refused the step three times"};
  static const char *const kWait[] = {"not stepping", "writing", "holding", "the bus is below 4.65 V under it",
                                      "no bus current reading", "the input is not used up", "at the target", "cooling before a guarded retry"};
  if (e & PIANO_CHG_EV_RAMP_BACK)
    note("policy: input steps: back from %u mA: %s (bus %d mV); ceiling %u mA, guarded retry after %u s", pl.ramp_back_from_ma,
         kBack[pl.ramp_back_why < 4 ? pl.ramp_back_why : 0], pl.ramp_back_mv, pl.ramp_ceiling_ma, PIANO_CHG_RAMP_RETRY_MS / 1000);
  // the limit the input was measured against: the step, or the firmware's own (0x2029) settled below it
  char used[48] = "";
  if (pl.ramp_used_ma && pl.ramp_used_ma != (e & PIANO_CHG_EV_RAMP_STEP ? pl.ramp_from_ma : pl.ramp_ma))
    snprintf(used, sizeof used, " (the firmware's own limit 0x2029 %u mA)", pl.ramp_used_ma);
  if (e & PIANO_CHG_EV_RAMP_STEP)
    note("policy: input steps: up %u -> %u mA (towards %u mA, %s %u mA): it held %d mV with %d mA taken%s", pl.ramp_from_ma, pl.ramp_ma,
         pl.ramp_target_ma, pl.icl_why == PIANO_CHG_ICL_CONTRACT_5V ? "5 V RDO" : "Rp",
         pl.icl_why == PIANO_CHG_ICL_CONTRACT_5V ? in.contract_ma : in.rp_ma,
         in.bus_mv, (in.valid & PIANO_CHG_IN_IBUS) ? in.ibus_ma : -1, used);
  if (e & PIANO_CHG_EV_RAMP_WAIT) {
    char ceiling[40] = "";
    if (pl.ramp_ceiling_ma) snprintf(ceiling, sizeof ceiling, ", temporary ceiling %u mA", pl.ramp_ceiling_ma);
    note("policy: input steps: at %u mA (towards %u mA%s): %s%s; bus %d mV, %d mA", pl.ramp_ma, pl.ramp_target_ma, ceiling,
         kWait[pl.ramp_wait < 8 ? pl.ramp_wait : 0], pl.ramp_wait == PIANO_CHG_RAMP_NOT_USED ? used : "", in.bus_mv,
         (in.valid & PIANO_CHG_IN_IBUS) ? in.ibus_ma : -1);
  }
  // the charging restart (ChargePolicy.c kick(), charging-policy.md §17)
  const uint32_t k = o.decided ? pl.kick_events : 0;   // a decision's own (the plan stays until the next one)
  if (k & PIANO_CHG_KICK_EV_START)
    note("policy: charging restart %u of 3: both packs at %d mA for 20 s or more while the input has %d mA to spare (bus %d mV %d mA, "
         "limit %d, 0x2029 %d), %d %%, %d mV, %d.%d C: 0x2003 = 0, then 1", pl.kick_count, pl.kick_ibat_ma, pl.kick_spare_ma, in.bus_mv,
         in.ibus_ma, in.icl_ma, (in.valid & PIANO_CHG_IN_AICL) ? in.aicl_ma : -1, in.soc_max_pct, in.vbat_max_mv, in.t_warm_dc / 10,
         (in.t_warm_dc < 0 ? -in.t_warm_dc : in.t_warm_dc) % 10);
  if (k & PIANO_CHG_KICK_EV_RESUMED)
    note("policy: charging restart %u: 0x2003 = 1 taken again; the charge current (%u mA) is sent again; the first battery reading "
         "15 s on judges it", pl.kick_count, pl.fcc_ma);
  if (k & PIANO_CHG_KICK_EV_CHARGING)
    note("policy: charging restart %u: the packs take %+d mA now: CHARGING again", pl.kick_count, pl.kick_ibat_ma);
  if (k & PIANO_CHG_KICK_EV_NONE)
    note("policy: charging restart %u: the packs at %+d mA: no change%s", pl.kick_count, pl.kick_ibat_ma,
         k & PIANO_CHG_KICK_EV_GAVE_UP ? "; three without charging: none more until the next attach, wake or charger generation" : "");
  if (k & PIANO_CHG_KICK_EV_ABANDONED)
    note("policy: charging restart %u: a write of it was not through within a minute: given up (0x2003 = 1 stays planned until taken)",
         pl.kick_count);
  for (unsigned i = 0; i < o.readbacks; ++i) {
    const LinkReadback &r = o.readback[i];
    const bool volt = r.property == PIANO_MCA_PD_FIXED || r.property == PIANO_MCA_PPS;
    if (!r.readable)
      note("policy: %s (%#x) = %u: not readable on the cycle after the write", piano_mca_property_name(r.property), r.property, r.written);
    else if (volt)
      note("policy: the bus reads %d mV after the %s request (%#x): %s", r.read, r.property == PIANO_MCA_PPS || r.written == 9000 ? "9 V" : "5 V",
           r.written, r.agrees ? "as asked" : "NOT as asked (the policy blocks 9 V if it stays so)");
    else
      note("policy: %s (%#x) reads %d after the write of %u: %s", piano_mca_property_name(r.property), r.property, r.read, r.written,
           r.agrees ? "as written" : "NOT as written (written again after a second such read)");
  }
  if (o.refused)
    note("policy: the codec's allowlist refused %s (%#x) = %u (%u so far): not sent", piano_mca_property_name(o.refusedProperty),
         o.refusedProperty, o.refusedValue, fLink.refusedTotal);
  if (o.full) note("policy: the codec's queue is full: %u writes are planned again later", o.full);
  if (o.planChanged) {
    char line[400];
    policyLine(line, sizeof line);
    note("[%llus] %s", t, line);
  }
}

// One pass of the policy: the gates and the decision in ChargeLink.h; here the inputs only this side has (the port, PianoThermal),
// the codec's relink, the assertion, and the sleep and shutdown acknowledgements.
void PianoPower::stepPolicy(uint64_t now)
{
  if (!fPolicyOn || fPolicyEnded) return;
  applyChargeRequest(now);
  const bool sleepAsk = __atomic_load_n(&fSleepAsk, __ATOMIC_ACQUIRE) != 0;
  const bool downAsk = __atomic_load_n(&fShutdownAsk, __ATOMIC_ACQUIRE) != 0;
  if (sleepAsk && !fHandover) {
    fHandover = true;
    fHandoverAcked = false;
    fHandoverUntil = now + 2000;
    note("policy: system sleep: handing the charger over (the class's 5 V limits%s), at most 2 s",
         fLink.cfg.limit ? ", charging stopped for the sleep below the limit" : "");
  } else if (!sleepAsk && fHandover) {
    fHandover = false;
    note("policy: awake again (wake %u)", __atomic_load_n(&fWakes, __ATOMIC_ACQUIRE));
  }
  if (downAsk && !fShutdownSeen) {
    fShutdownSeen = true;
    fShutdownUntil = now + 2000;
    note("policy: shutdown or restart: a stop of ours is lifted and the source put back at 5 V, at most 2 s");
  }
  const bool reachable = fChannel && fWasUp;
  LinkView v;
  v.nowMs = now;
  uint32_t ups = 0;
  v.chargerPdUp = reachable && piano_qrtr_charger_pd(&ups) == SERVREG_STATE_UP;
  if (v.chargerPdUp && (!fPdWasUp || ups != fPdUpsSeen)) fPdUpMs = now;   // up now, or up again since (a DSP restart)
  fPdWasUp = v.chargerPdUp;
  fPdUpsSeen = ups;
  v.chargerPdUpMs = fPdUpMs;
  v.packs = fPack;
  v.packsFresh = fGauges && fPackMs && now >= fPackMs && now - fPackMs <= 45000;
  v.packsMs = fPackMs;
  v.wakes = __atomic_load_n(&fWakes, __ATOMIC_ACQUIRE);
  v.willSleep = fHandover;
  v.shutdown = fShutdownSeen;
  v.contract = &fContract;
  v.mippsBusy = gMippsActive || piano_mipps_cleanup_pending(&gMipps);
  PortState port;
  piano_board_temps board;
  if (reachable && fLink.due(fMca, v)) {
    if (!fHandover && !fShutdownSeen) sampleSafety(now);
    v.safetyFresh = fSafetyMs && now >= fSafetyMs && now - fSafetyMs <= 10000;
    v.cpOff = (fSafetyValid & 1) && !(fCpCtrl & 0x10) && !(gCpSess && (gCpSess->plan.owns_volt || gCpSess->plan.handback));
    v.dtptValid = fAlerts.complete();
    v.dtpt = fAlerts.active();
    // the port every decision (the Rp current, the #18 check), not only every report
    if (fSpmi && fCharger.found) {
      port = readPort(gIo, fCharger);
      v.port = &port;
    }
    if (boardTemps(board, now)) v.board = &board;
    if (v.board && (board.valid & (1u << PIANO_BOARD_TEMP_CHARGER)) && v.chargerPdUp && !ChargeLink::chargerReadingCounts(v) &&
        fPdUpMs != fChargerEarlyNotedMs) {
      fChargerEarlyNotedMs = fPdUpMs;
      note("policy: the charger thermistor's reading (%d C) was taken %u ms before charger_pd came up: not used until a reading after "
           "it (the DSP firmware's pin)", int(board.milli_c[PIANO_BOARD_TEMP_CHARGER] / 1000),
           unsigned(board.age_ms[PIANO_BOARD_TEMP_CHARGER] > now - fPdUpMs ? board.age_ms[PIANO_BOARD_TEMP_CHARGER] - (now - fPdUpMs) : 0));
    }
  }
  LinkOutcome o;
  if (reachable) fLink.step(fMca, v, o);
  else o.handoverReady = !fLink.st.disabled && !fLink.st.pending;
  if (o.decided) {
    publishCharge(now, v.board);
    if (gArgs.ps) publishBattery((now - fStartMs) / 1000, v.port);
  }
  if (o.decided || o.waitingPd || o.waitingBoard || o.refused || o.full) logPolicy(o, now);
  if (o.resync) relinkMca(now, o.events & PIANO_CHG_EV_WAKE ? "after the wake" : "the firmware lost the source (#18)");
  setHold(reachable && fLink.hold && !fShutdownSeen);
  if (gCpSess && !gCpSess->plan.sleep_ready) o.handoverReady = false;   // the pump session holds the source or the pump
  if (gMippsActive || piano_mipps_cleanup_pending(&gMipps)) o.handoverReady = false;   // MiPPS runs or owes its 0x21002 = 0
  if (fHandover && !fHandoverAcked && (o.handoverReady || now >= fHandoverUntil)) {
    fHandoverAcked = true;
    if (!o.handoverReady) {
      fHandoverFault = true;
      setProperty("PianoChargeHandoverFault", true);
      note("buck: handover pending=1 fault=timeout recovery=required");
    }
    note("policy: sleep handover %s: acknowledged", o.handoverReady ? "done" : "not complete after 2 s");
    acknowledgeSetPowerState();
  }
  if (fShutdownSeen && (o.handoverReady || now >= fShutdownUntil)) {
    note("policy: shutdown handover %s; the policy stops", o.handoverReady ? "done" : "not complete after 2 s");
    setHold(false);
    if (fHoldId) {
      if (IOPMrootDomain *root = getPMRootDomain()) root->releasePMAssertion(fHoldId);
      fHoldId = 0;
    }
    if (!o.handoverReady) {fHandoverFault = true; setProperty("PianoChargeHandoverFault", true);}
    fPolicyEnded = true;
    __atomic_store_n(&fShutdownDone, 1u, __ATOMIC_RELEASE);
  }
}

void PianoPower::run()
{
  fStartMs = nowMs();
  if (!probeSpmi()) note("continuing without the SPMI view");
  if (fSpmi) {
    startWdog();
    surveyChargerBlocks();
  }
  surveyAdsp();
  probeGauges();
  probeCp();
  if (gArgs.level >= 2 && gArgs.mipps) mippsLoadKeys();
  else setProperty("PianoMipps", gMippsText);
  if (gArgs.level >= 2) startCharger();
  report();
  uint64_t reportDue = nowMs() + uint64_t(gArgs.period) * 1000;
  stage(5, "reporting");
  for (;;) {
    uint64_t now = nowMs();
    WdogWrite pet;
    if (fWdog.petIfDue(gWriter, now, &pet) && !pet.ok) wdogWriteLine("pet", pet);
    ChgEnWrite pulseOn;
    if (fChgEn.restoreIfDue(gIo, gWriter, now, &pulseOn)) chgenWriteLine((now - fStartMs) / 1000, pulseOn);
    if (fCx.located && !fSleepReports.quiet) {
      // the 2 s watch of the charger's state (ChargerFix.h), each tick also the recovery's turn; the recovery's own deadlines
      CxTransition x;
      const uint32_t ticks = fWatch.ticks;
      if (fWatch.tick(gIo, fCx, now, &x)) watchLine(x);
      if (fWatch.ticks != ticks || (fFix.nextDue() && now >= fFix.nextDue())) fixPoll(now);
    }
    serviceMca(now);
    serviceMipps(now);
    serviceCp(now);
    const PianoUsbcObservation observed = piano_usbc_observe(&fContract, fPolicyOn,
        gArgs.ucsi && fWasUp && !fQuiet.raised, now);
    piano_usbc_relay_observe(&observed);
    // The built-in contract observer exclusively owns UCSI while policy is on.
    // Otherwise the legacy relay is ready after link/quiet/probe completion.
    if (fChannel)
      piano_usbc_relay_step(fChannel, !fPolicyOn && fWasUp && !fQuiet.raised && fMca.ucsi != 1 && fMca.ucsi != 2, fMca.links, this);
    // Between the sleep handover's acknowledgement and the wake: no report (the gauges' I2C idle, docs/power.md 4); after the wake
    // the one that fell due runs at once.
    if (const int q = fSleepReports.update(fPolicyOn, fHandover, fHandoverAcked))
      note("policy: %s", q > 0 ? "reports paused until the wake (no gauge, Type-C or RTC read while the system goes to sleep)"
                               : "reports again (the battery, the port and the RTC read afresh)");
    if (now >= reportDue && !fSleepReports.quiet) {
      report();
      reportDue = now + uint64_t(gArgs.period) * 1000;
    }
    if (fReadyWarnDue && now >= fReadyWarnDue) {
      fReadyWarnDue = 0;
      if (fMca.icl_set == 0 && gArgs.write)
        note("WARNING: 3 minutes into the DSP bring-up no input limit write has taken: the charger may be held at the firmware's "
             "100 mA; reboot (or boot Ubuntu/Android) if the battery drains");
    }
    // Sleep until the next deadline or a message.
    now = nowMs();
    uint64_t next = fSleepReports.quiet ? now + 60000 : reportDue;
    if (fChannel && fWasUp) {
      if (fCycleDue && fCycleDue < next) next = fCycleDue;
      if (fRequestDue && fRequestDue < next) next = fRequestDue;
      if (fPanDue && fPanDue < next) next = fPanDue;
      if (fPolicyOn && now + 200 < next) next = now + 200;
    }
    if (fReadyWarnDue && fReadyWarnDue < next) next = fReadyWarnDue;
    if (fWdog.nextDue() && fWdog.nextDue() < next) next = fWdog.nextDue();
    if (fChgEn.nextDue() && fChgEn.nextDue() < next) next = fChgEn.nextDue();
    if (fWatch.nextDue() && fWatch.nextDue() < next) next = fWatch.nextDue();
    if (fFix.nextDue() && fFix.nextDue() < next) next = fFix.nextDue();
    if (const uint64_t cpDue = gCpHook ? gCpHook->nextDeadline() : 0) if (cpDue < next) next = cpDue;
    if (gCpSess && fPolicyOn && gCpSess->nextMs && gCpSess->nextMs < next) next = gCpSess->nextMs;
    if (gMippsActive) {const uint64_t w = gMippsWakeMs > now ? gMippsWakeMs : now + 10; if (w < next) next = w;}
    if (fHandover && !fHandoverAcked && fHandoverUntil < next) next = fHandoverUntil;
    if (fShutdownSeen && !fPolicyEnded && fShutdownUntil < next) next = fShutdownUntil;
    const uint32_t wait = next > now ? uint32_t(next - now < 60000 ? next - now : 60000) : 1;
    assert_wait_timeout((event_t)this, THREAD_UNINT, wait, kMillisecondScale);
    bool pending = __atomic_load_n(&fConfigPending, __ATOMIC_ACQUIRE) || __atomic_load_n(&fLinkChanged, __ATOMIC_ACQUIRE) != 0 || policyAsked() || (!fPolicyOn && piano_usbc_relay_pending());
    IOSimpleLockLock(fRingLock);
    pending |= fRingCount != 0;
    IOSimpleLockUnlock(fRingLock);
    if (pending) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
  }
}
