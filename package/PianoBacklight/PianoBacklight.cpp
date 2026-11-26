// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoBacklight: the panel backlight of the Xiaomi Pad 8 Pro (piano, SM8750) for native macOS 27 at EL1. Two Kinetic KTZ8866 ICs at
// I2C address 0x11 drive the panel's LED strings and get the same brightness (Ktz8866.h: the register facts, their sources and the
// state machine). The primary sits on the I2C hub's SE3 (DT i2c@98c000, pins gpio70 SDA / gpio71 SCL, function i2chub0_se3), the
// secondary on QUP2 SE0 (DT i2c@880000, gpio0 SDA / gpio1 SCL, function qup2_se0). Both buses are driven in software (BitI2c.h) on
// their TLMM pins, as PianoKeyboard drives the cover's: for each burst of accesses the two pins are switched from their QUP function
// to GPIO (open drain: released = input with the pull-up, low = output enable with the output value 0) and given back exactly as
// found afterwards (CTL and IO registers rewritten with the values the survey read, then read back). No QUP engine, GCC clock or RPMh
// vote is touched, so no lock of PianoA830's is needed. Never touched: the ICs' HWEN (TLMM gpio2, read only), the panel's bias
// enables (gpio117/118), its reset (gpio98), DSI, DPU, and every KTZ8866 register but 0x04, 0x05 and 0x08 bit 6.
//
// One kernel thread does everything:
//   1 survey   read only: the five pins' TLMM registers (gpio70, 71, 0, 1 and HWEN gpio2), then both ICs' registers (ID, the
//              backlight and bias configuration, the code, BL_EN); nothing is written at level 1
//   2 control  the found state adopted, the boot level applied, then events: NativeGopDisplay's display power (off -> BL_EN cleared on
//              both ICs; on -> the code, then BL_EN), brightness steps (PianoBacklightClient.h), the cover's hall sensor (PianoKeyboard's
//              "piano-keyboard-hall" property, polled; acted on only with piano-backlight-lid=1), "piano-backlight-percent" /
//              "piano-backlight-step" set as properties by an administrator; a failed access retried every second
// Every write is logged ("PianoBacklight: write ..."), at most 24 such lines per 10 s (the rest counted).
// System sleep (docs/power.md 4, PianoPowerContract.h): at level 2 the service is in the power tree. setPowerState(off) has the
// thread clear BL_EN on both ICs (whatever the display path said; retried twice) and waits for it (at most 800 ms); on has the
// thread restore what the display and the lid want. HWEN (gpio2) stays untouched: it is read once, never written.
//
// Boot arguments (default: inert, the service does not even probe):
//   piano-backlight=1           the survey (pins, both ICs read back, the found brightness); writes nothing
//   piano-backlight=2           control (and NativeGopDisplay calls out its display power: it reads this argument too)
//   piano-backlight-level=P     boot brightness, P % of the 11-bit range, set whatever was found (default at level 2: 45, applied
//                               only when the boot stages left more; 0: leave as found)
//   piano-backlight-min=CODE    the floor of steps and levels (default 128 of 2047; 32..1024); off is BL_EN, never code 0
//   piano-backlight-lid=1       cover closed (PianoKeyboard's hall sensor, piano-keyboard>=1) -> backlight off, open -> on again
//   piano-backlight-keys=1      read by the producers (PianoDwc3's HID input from the Mac): brightness usages 0x6F/0x70 and Right
//                               Ctrl + 1/2 step the backlight
//   piano-backlight-flags=1     the survey also reads FLAG (0x0f), which clears its latched fault bits
//   piano-backlight-halfbit=US  I2C half-bit time (default 5 us: ~100 kHz; 2..50)
//   piano-backlight-swap=B      SDA/SCL swapped on bus 0 (bit 0, primary) / bus 1 (bit 1, secondary): only if the survey's reads
//                               fail with "no ack (address)" on idle-high pins
//   piano-backlight-auto=1      automatic brightness (level 2): targets of the ambient light policy, permille of the range, from
//                               the platform function "PianoBacklightAmbient" or PianoSensors' property piano-sensors-light-target
//                               (polled every 250 ms with its piano-sensors-seq; nothing happens until PianoSensors publishes it);
//                               the brightness keys then shift the curve by the steps (kept for the boot); piano-backlight-percent
//                               switches to manual, piano-backlight-auto=0/1 as a property leaves / enters again. Without it nothing
//                               of this runs (docs/drivers/backlight.md, sensors.md 5a.4)
//   piano-backlight-native=1    publish the macOS SDR brightness adapter (level 2, with piano-backlight-keys=0); root's
//                               "piano-backlight-sync" (16.16 nits) announces that piano-brightness-sync gives CoreBrightness the
//                               panel's level: WindowServer's next request (its echo) is not taken for the user's move;
//                               root's "piano-backlight-managed" (a lease, seconds) hands the user's moves to the daemon (the
//                               stream then never ends automatic mode: EDR ramps and CoreBrightness's restore arrive there too),
//                               "piano-backlight-moved" (16.16) is the move it saw in CoreBrightness's own level: while
//                               automatic mode is requested it stays on and the move becomes the curve's trim (2026-11-22,
//                               published as piano-backlight-trim); in manual mode it is the level
//   piano-backlight-android=1   opt in to the bounded Android-reference SDR adapter on compatible ICs
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/codesign.h>
#include <sys/proc.h>
#include <sys/ubc.h>
#include "BitI2c.h"
#include "Ktz8866.h"
#include "NativeBrightness.h"
#include <IOKit/IOMessage.h>
#include "PianoBacklightClient.h"
#include <PianoPowerContract.h>

using namespace pianobacklight;

extern "C" {
// Post-mortem breadcrumb: 1 survey, 2 control running, 3 level 1 done; negative: failed there.
volatile int32_t piano_backlight_stage = 0;
}

namespace {

struct Args {
  uint32_t level, percent, percentGiven, floor, lid, keys, flags, halfUs, swap, autoAllowed;
  bool native, android;
};
Args gArgs;

uint32_t bootArg(const char *name, uint32_t fallback, bool *given = nullptr)
{
  uint32_t v = fallback;
  const bool found = PE_parse_boot_argn(name, &v, sizeof v);
  if (given) *given = found;
  return found ? v : fallback;
}
uint32_t clampArg(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void readArgs()
{
  bool given = false;
  gArgs.level = bootArg("piano-backlight", 0);
  gArgs.percent = clampArg(bootArg("piano-backlight-level", 45, &given), 0, 100);
  gArgs.percentGiven = given;
  gArgs.floor = clampArg(bootArg("piano-backlight-min", 128), 32, 1024);
  gArgs.lid = bootArg("piano-backlight-lid", 0);
  gArgs.keys = bootArg("piano-backlight-keys", 0);
  gArgs.flags = bootArg("piano-backlight-flags", 0);
  gArgs.halfUs = clampArg(bootArg("piano-backlight-halfbit", 5), 2, 50);
  gArgs.swap = bootArg("piano-backlight-swap", 0);
  gArgs.autoAllowed = bootArg("piano-backlight-auto", 0) == 1;
  gArgs.native = gArgs.level >= 2 && gArgs.keys == 0 && bootArg("piano-backlight-native", 0) == 1;
  gArgs.android = gArgs.native && bootArg("piano-backlight-android", 0) == 1;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoBacklight: %s\n", line);
}

uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}

// Explicit ldr/str with a barrier after each store, as PianoKeyboard does: a bus line has moved before the half-bit wait starts.
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n dsb sy" ::"r"(v), "r"(a) : "memory"); }

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

// ------------------------------------------------------------------------------------------------------------- the pins (TLMM)
// TLMM (DT pinctrl@f100000, qcom,sm8750-tlmm), pin n at n * 0x1000 (PianoKeyboard.cpp's facts, run on piano): CTL (pull 1:0, 3 up;
// function 5:2; drive 8:6; output enable 9; eGPIO present 11 / enable 12), IO (bit 0 the pad, bit 1 the output value). None of the
// five pins is in the DT's gpio-reserved-ranges (36-39, 48-51, 74). Pin order in the DT's pinctrl states: SDA first, then SCL.
constexpr uint64_t kTlmm = 0x0f100000;
constexpr uint32_t kCtl = 0x0, kIo = 0x4;
constexpr uint32_t kPullUp = 3, kFunction = 0xfu << 2, kOe = 1u << 9, kEgpioPresent = 1u << 11, kEgpioEnable = 1u << 12;

struct Pin {
  unsigned gpio;
  const char *what;
  Region page;
  uint32_t ctl, io;   // as found
};
enum : unsigned { kPrimarySda, kPrimaryScl, kSecondarySda, kSecondaryScl, kHwen, kPinCount };
Pin gPin[kPinCount] = {
  {70, "primary SDA (i2c@98c000, I2C hub SE3)", {}, 0, 0}, {71, "primary SCL", {}, 0, 0},
  {0, "secondary SDA (i2c@880000, QUP2 SE0)", {}, 0, 0}, {1, "secondary SCL", {}, 0, 0},
  {2, "KTZ8866 HWEN (read only, never written)", {}, 0, 0},
};

struct Bus {
  unsigned sda, scl;   // gPin indexes
  bool usable, muxed;
  BitI2c i2c;
  uint32_t reads, writes, errors, restores, restoreMismatches;
};
Bus gBus[kIcs] = {{kPrimarySda, kPrimaryScl, false, false, {}, 0, 0, 0, 0, 0}, {kSecondarySda, kSecondaryScl, false, false, {}, 0, 0, 0, 0, 0}};

// An eGPIO-capable pin whose eGPIO enable is clear belongs to the other (LPI) TLMM: not driven from here.
bool foreign(const Pin &p) { return (p.ctl & kEgpioPresent) && !(p.ctl & kEgpioEnable); }
uint32_t gpioCtl(const Pin &p, bool low) { return (p.ctl & ~(kFunction | kOe | 3u)) | kPullUp | (low ? kOe : 0); }
void lineSda(void *ctx, bool release) { const Pin &p = gPin[static_cast<Bus *>(ctx)->sda]; wr32(p.page.va + kCtl, gpioCtl(p, !release)); }
void lineScl(void *ctx, bool release) { const Pin &p = gPin[static_cast<Bus *>(ctx)->scl]; wr32(p.page.va + kCtl, gpioCtl(p, !release)); }
bool readSda(void *ctx) { return rd32(gPin[static_cast<Bus *>(ctx)->sda].page.va + kIo) & 1; }
bool readScl(void *ctx) { return rd32(gPin[static_cast<Bus *>(ctx)->scl].page.va + kIo) & 1; }
void delayUs(void *, unsigned us) { IODelay(us); }

const char *icName(unsigned i) { return kIcName[i]; }

// The bus's pins to GPIO, both released (output value 0 first, then CTL with the function 0 and the output disabled).
bool muxToGpio(Bus &b)
{
  const unsigned pins[2] = {b.sda, b.scl};
  for (unsigned idx : pins) {
    const Pin &p = gPin[idx];
    wr32(p.page.va + kIo, 0);
    wr32(p.page.va + kCtl, gpioCtl(p, false));
  }
  b.muxed = true;
  IODelay(2 * gArgs.halfUs);
  return true;
}
// Back exactly as found: IO's output value, then CTL (the QUP function, pull, drive); both read back.
void restorePins(Bus &b)
{
  bool same = true;
  const unsigned pins[2] = {b.sda, b.scl};
  for (unsigned idx : pins) {
    const Pin &p = gPin[idx];
    wr32(p.page.va + kIo, p.io & 2);
    wr32(p.page.va + kCtl, p.ctl);
    same = same && rd32(p.page.va + kCtl) == p.ctl;
  }
  b.muxed = false;
  ++b.restores;
  if (!same && b.restoreMismatches++ < 4)
    note("pins of the %s bus: CTL read back differs after the restore (gpio%u %08x, gpio%u %08x; found %08x, %08x)",
         icName(unsigned(&b - gBus)), gPin[b.sda].gpio, rd32(gPin[b.sda].page.va + kCtl), gPin[b.scl].gpio,
         rd32(gPin[b.scl].page.va + kCtl), gPin[b.sda].ctl, gPin[b.scl].ctl);
}

// The two ICs on the two software buses. write() refuses everything Ktz8866.h does not need, whatever the caller asks.
class KextIcs final : public Ics {
 public:
  bool read(unsigned ic, uint8_t reg, uint8_t *value) override
  {
    if (ic >= kIcs || !gBus[ic].usable) return false;
    Bus &b = gBus[ic];
    if (!b.muxed) muxToGpio(b);
    const I2c r = b.i2c.writeRead(kAddress, &reg, 1, value, 1);
    ++b.reads;
    if (r != I2c::ok) failed(ic, "read", reg, r);
    return r == I2c::ok;
  }
  bool write(unsigned ic, uint8_t reg, uint8_t value) override
  {
    const char *refusal = gArgs.level < 2 ? "level 1 writes nothing"
                        : !writable(reg) ? "not one of 0x04, 0x05, 0x08"
                        : reg == kRegEn && (value & kEnSoftReset) ? "BL_EN with the software reset bit"
                        : reg == kRegBrtMsb && value < (gArgs.floor >> 3) ? "a code under the floor"
                        : nullptr;
    if (refusal || ic >= kIcs) {
      note("REFUSED write %s 0x%02x = 0x%02x: %s", ic < kIcs ? icName(ic) : "?", reg, value, refusal ? refusal : "no such IC");
      return false;
    }
    Bus &b = gBus[ic];
    if (!b.usable) return false;
    if (!b.muxed) muxToGpio(b);
    const uint8_t bytes[2] = {reg, value};
    const I2c r = b.i2c.write(kAddress, bytes, 2);
    ++b.writes;
    if (r != I2c::ok) failed(ic, "write", reg, r);
    return r == I2c::ok;
  }
  void done() override
  {
    for (Bus &b : gBus)
      if (b.muxed) restorePins(b);
  }
  void log(const char *line) override { IOLog("PianoBacklight: %s\n", line); }
  uint64_t nowMs() override { return ::nowMs(); }

 private:
  void failed(unsigned ic, const char *what, uint8_t reg, I2c r)
  {
    ++gBus[ic].errors;
    uint32_t dropped = 0;
    if (limit_.allow(::nowMs(), &dropped))
      note("%s %s 0x%02x: %s (errors on this bus %u)%s", what, icName(ic), reg, i2cName(r), gBus[ic].errors, dropped ? " (some not logged)" : "");
  }
  LogLimit limit_{12, 10000};
};

// Requests from callPlatformFunction and setProperties, taken by the thread.
struct Pending {
  bool any, display, on;
  uint32_t system, api;
  int32_t steps;
  int32_t percent;   // -1: none
  const char *source;
  uint32_t displayCalls, stepCalls;
  bool ambient;              // piano-backlight-auto=1 only: the newest target (permille, -1 leaves automatic mode)
  int32_t target;
  uint32_t lux;
  const char *ambientSource;
  int32_t automatic;         // the property piano-backlight-auto: -1 none, 0 off, 1 on
  uint32_t ambientCalls;
  int32_t sleep;             // setPowerState: -1 none, 1 off (a system sleep), 0 on (the wake)
  int32_t native = -1;       // newest absolute request, encoded as signed 16.16 nits
  int32_t reference = -1;    // newest root A/B selection
  pianoauto::AmbientVersion ambientVersion{};
  bool ambientFault = false;
  uint64_t nativeOrder = 0, autoOrder = 0;
  int32_t sync = -1;         // 2026-11-21: piano-brightness-sync's announced level (signed 16.16 nits), -1 none
  int32_t managed = -1;      // its lease in seconds (0 ends it), -1 none
  int32_t moved = -1;        // the user's move it saw in CoreBrightness's level (16.16), -1 none
};
IOLock *gLock;
uint32_t gNativeDark;   // WindowServer's requests <= 0 (dark ramps), taken and dropped
uint32_t gSwapNits;     // 2026-11-21: requests from the swap record (NativeGopDisplay)
Pending gPending = {false, false, false, 0, 0, 0, -1, "?", 0, 0, false, -1, 0, "?", -1, 0, -1};
// The thread's answer to a sleep request: the requests taken, and whether the ICs were dark after the last one.
uint32_t gSleepAsked, gSleepTaken;
bool gServing, gDarkForSleep;
uint32_t gBlankAsked, gBlankTaken;
bool gDarkForPanel;
bool gNativeReady;
uint32_t gNativeMin, gNativeMax;  // fixed schema, initialized by the worker under gLock
uint64_t gControlOrder;
LogLimit gNativeRejectionLog{8, 10000};  // protected by gLock

class NativeProperties {
 public:
  explicit NativeProperties(IOService &service) : service_(service), fields_(OSDictionary::withCapacity(6)) {}
  ~NativeProperties() { OSSafeReleaseNULL(fields_); }

  bool range(const char *key, uint32_t min, uint32_t max, uint32_t value)
  {
    OSDictionary *range = OSDictionary::withCapacity(3);
    const bool ok = fields_ && range && put(range, "min", min) && put(range, "max", max)
        && put(range, "value", value) && fields_->setObject(key, range);
    OSSafeReleaseNULL(range);
    return ok;
  }
  bool number(const char *key, uint32_t value) { return put(fields_, key, value); }
  bool scalar(const char *key, uint32_t value) { return service_.setProperty(key, uint64_t(value), 32); }
  bool data(const char *key, const void *bytes, unsigned size)
  {
    OSData *data = OSData::withBytes(bytes, size);
    const bool ok = data && service_.setProperty(key, data);
    OSSafeReleaseNULL(data);
    return ok;
  }
  bool commit(const char *key) { return fields_ && service_.setProperty(key, fields_); }

 private:
  static bool put(OSDictionary *dict, const char *key, uint32_t value)
  {
    OSNumber *number = OSNumber::withNumber(uint64_t(value), 32);
    const bool ok = dict && number && dict->setObject(key, number);
    OSSafeReleaseNULL(number);
    return ok;
  }
  IOService &service_;
  OSDictionary *fields_;
};

}  // namespace

// --------------------------------------------------------------------------------------------------------------------- the service
class PianoBacklight : public IOService {
  OSDeclareDefaultStructors(PianoBacklight)

  KextIcs fIcs;
  Controller fCtl{fIcs};
  IOService *fDisplay = nullptr, *fKeyboard = nullptr, *fSensors = nullptr;   // fSensors: piano-backlight-auto=1 only
  uint64_t fSensorsSeq = ~0ull;   // PianoSensors' piano-sensors-seq as last taken
  int64_t fSensorsTarget = -2;    // its piano-sensors-light-target as last taken (-2: none yet)
  uint64_t fPublished[25] = {};
  bool fHavePublished = false;
  uint64_t fNextLookupMs = 0;
  uint64_t fNextNativeMs = 0;
  uint16_t fNativeCode = 0;
  int32_t fNativeRequest = -1;
  uint32_t fNativeErrors = 0;
  bool fNativePublished = false;
  NativeIntent fIntent;                     // 2026-11-20: the user's moves among WindowServer's stream
  uint32_t fSyncPublished = ~0u;            // 2026-11-21: fIntent's sync counters as last published (syncs + echoes + missed)
  Lease fManaged;                           // piano-brightness-sync's lease: the stream does not end automatic mode
  Handover fHandover;                       // automatic mode the stream ended before a lease: given back when one arrives
  int fManagedPublished = -1;
  int32_t fLastNative = -1;                 // the newest request seen, taken or dropped (16.16), and when
  uint64_t fLastNativeMs = 0;
  int32_t fRequestedPublished = -1;
  uint64_t fRequestedPublishedMs = 0;
  uint32_t fManagedDrops = 0, fDaemonMoves = 0;
  bool fNativeTookAuto = false;             // a move ended automatic mode (a power-on or a wake gives it back)
  uint32_t fNativeFollowed = 0, fNativeMoves = 0, fNativeDropped = 0;
  bool fReference = true;
  uint32_t fRevision = 0, fNativePublishedRevision = 0;
  struct AndroidApply {
    const char *source = nullptr;
    uint32_t revision = 0, milliNits = 0, errors = 0;
    uint16_t dbv = 0, code = 0;
    bool pendingLogged = false;
  } fAndroidApply;
  LogLimit fNativeApplyLog{24, 10000};
  int fDisplayProperty = -1;   // NativeGopDisplay's piano-display-power as last read (-1: none yet)
  bool fDisplayKnown = false, fDisplayOn = true;
  uint32_t fSystem = 0, fApi = 0;

  static void threadMain(void *self, wait_result_t);
  void run();
  bool surveyPins();
  void serve();
  void poll(uint64_t now, bool lidOnly);
  IOService *sibling(const char *className);
  void publishState();
  void publishNativeState();
  void publishAndroidModel();
  void publishAndroidApply(const NativeBrightness &adapter, bool applied, uint16_t code);
  void restartAmbient();
  void setAutomatic(bool on);
  void nativeSettle(const char *why);
  IOReturn enqueueSwapNits(int64_t asked);
  void applyAmbient(int32_t permille, uint32_t lux, const char *source, const pianoauto::AmbientVersion &version);
  IOReturn enqueueNative(OSDictionary &properties, OSObject *value);

 public:
  using IOService::callPlatformFunction;
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
  IOReturn setProperties(OSObject *properties) override;
  IOReturn setPowerState(unsigned long state, IOService *device) override;
};

OSDefineMetaClassAndStructors(PianoBacklight, IOService)

IOService *PianoBacklight::probe(IOService *provider, SInt32 *score)
{
  readArgs();
  if (!gArgs.level || !OSDynamicCast(IOPlatformExpertDevice, provider)) return nullptr;
  return IOService::probe(provider, score);
}

bool PianoBacklight::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  note("start: piano-backlight=%u (%s); boot level %u %%%s, floor %u, lid %u, keys %u, flags %u, half-bit %u us, swap %u%s",
       gArgs.level, gArgs.level >= 2 ? "control" : "read-only survey", gArgs.percent,
       gArgs.percentGiven ? " (given: set whatever was found)" : " (default: only lowers a higher found level)", gArgs.floor, gArgs.lid,
       gArgs.keys, gArgs.flags, gArgs.halfUs, gArgs.swap,
       gArgs.autoAllowed ? "; automatic brightness allowed (piano-backlight-auto=1: PianoBacklightAmbient, PianoSensors' "
                           "piano-sensors-light-target)" : "");
  setProperty("piano-backlight", gArgs.level, 32);
  Config c;
  c.floor = uint16_t(gArgs.floor);
  c.bootPercent = gArgs.percent;
  c.bootForce = gArgs.percentGiven && gArgs.percent;
  c.lidWatch = gArgs.lid == 1;
  c.readFlags = gArgs.flags == 1;
  c.autoAllowed = gArgs.level >= 2 && gArgs.autoAllowed;
  c.android = gArgs.android;
  fCtl.configure(c);
  gLock = IOLockAlloc();
  thread_t thread = nullptr;
  if (!gLock || kernel_thread_start(threadMain, this, &thread) != KERN_SUCCESS) {
    piano_backlight_stage = -1;
    note("FAILED: no lock or no thread; nothing done");
    return true;
  }
  thread_deallocate(thread);
  if (!gArgs.native) registerService();   // Native mode first prepares its initial properties on the worker.
  if (gArgs.level >= 2) note("system sleep: in the power tree (%#x): off clears BL_EN on both ICs, on restores it", pianopower::join(this));
  return true;
}

void PianoBacklight::threadMain(void *self, wait_result_t) { static_cast<PianoBacklight *>(self)->run(); }

bool PianoBacklight::surveyPins()
{
  for (Pin &p : gPin) {
    if (!mapRegion(p.page, kTlmm + uint64_t(p.gpio) * 0x1000, 0x10)) {
      note("pins: gpio%u (%s) does not map", p.gpio, p.what);
      continue;
    }
    p.ctl = rd32(p.page.va + kCtl);
    p.io = rd32(p.page.va + kIo);
    note("pins: gpio%-2u %-40s CTL %08x (function %u, pull %u, drive %u mA, %s%s) IO %08x (pad %u, out %u)", p.gpio, p.what, p.ctl,
         (p.ctl >> 2) & 15, p.ctl & 3, 2 * (((p.ctl >> 6) & 7) + 1), p.ctl & kOe ? "output" : "input",
         p.ctl & kEgpioPresent ? (p.ctl & kEgpioEnable ? ", eGPIO here" : ", eGPIO ELSEWHERE") : "", p.io, p.io & 1, (p.io >> 1) & 1);
  }
  bool any = false;
  for (unsigned i = 0; i < kIcs; ++i) {
    Bus &b = gBus[i];
    if (gArgs.swap & (1u << i)) {
      const unsigned t = b.sda;
      b.sda = b.scl;
      b.scl = t;
    }
    const Pin &sda = gPin[b.sda], &scl = gPin[b.scl];
    const char *why = !sda.page.va || !scl.page.va ? "a pin does not map"
                    : foreign(sda) || foreign(scl) ? "a pin is routed to the other TLMM (eGPIO)"
                    : !(sda.io & scl.io & 1) ? "a line is low as found (bus busy or no pull-up)"
                    : nullptr;
    b.usable = !why;
    b.i2c.configure({&b, lineSda, lineScl, readSda, readScl, delayUs}, gArgs.halfUs, 2000);
    note("bus %s: SDA gpio%u, SCL gpio%u: %s%s", icName(i), sda.gpio, scl.gpio, why ? "NOT used: " : "used (software I2C, pins given back after each burst)",
         why ? why : "");
    any = any || b.usable;
  }
  return any;
}

void PianoBacklight::run()
{
  piano_backlight_stage = 1;
  note("step 1: survey (read only): pins, then both ICs");
  if (!surveyPins()) {
    piano_backlight_stage = -1;
    note("FAILED at step 1: no usable bus; nothing written");
    if (gArgs.native) registerService();
    return;
  }
  const bool both = fCtl.survey();
  const IcState &p = fCtl.ic(0), &q = fCtl.ic(1);
  note("found: primary %s code %u (%u %%) %s, secondary %s code %u (%u %%) %s; %s", p.answered ? "answered," : "SILENT,", p.foundCode,
       percentOf(p.foundCode), p.foundLit ? "lit" : "DARK", q.answered ? "answered," : "SILENT,", q.foundCode, percentOf(q.foundCode),
       q.foundLit ? "lit" : "DARK", both ? "both KTZ8866" : "NOT both KTZ8866");
  setProperty("piano-backlight-found-code", p.foundCode, 32);
  setProperty("piano-backlight-found", both, 32);
  if (gArgs.level < 2) {
    piano_backlight_stage = 3;
    note("level 1: survey done, nothing written (bus accesses: %u + %u reads, %u + %u errors, pins restored %u + %u times)",
         gBus[0].reads, gBus[1].reads, gBus[0].errors, gBus[1].errors, gBus[0].restores, gBus[1].restores);
    return;
  }
  piano_backlight_stage = 2;
  note("step 2: control");
  if (!fCtl.start()) {
    piano_backlight_stage = -2;
    publishState();
    if (gArgs.native) registerService();
    return;
  }
  publishState();
  serve();
}

// The display power (NativeGopDisplay's property, a fallback for a call missed while this service was not yet published) and the
// cover's hall sensor (PianoKeyboard's property): looked up every 2 s until found, read on each poll.
void PianoBacklight::poll(uint64_t now, bool lidOnly)
{
  if ((!fDisplay || !fKeyboard || (gArgs.autoAllowed && !fSensors)) && now >= fNextLookupMs) {
    fNextLookupMs = now + 2000;
    const struct {
      IOService **slot;
      const char *name;
    } wanted[3] = {{&fDisplay, "NativeGopDisplay"}, {&fKeyboard, "PianoKeyboard"}, {&fSensors, "PianoSensors"}};
    for (const auto &w : wanted) {
      IOService **slot = w.slot;
      const char *name = w.name;
      if (*slot || (slot == &fSensors && !gArgs.autoAllowed)) continue;
      OSDictionary *matching = IOService::serviceMatching(name);
      *slot = matching ? IOService::copyMatchingService(matching) : nullptr;
      OSSafeReleaseNULL(matching);
      if (!*slot && slot == &fSensors) *slot = sibling(name);   // PianoSensors (levels 1-3) does not register itself
      if (*slot)
        note("found %s (%s)", name, slot == &fKeyboard ? "its hall sensor property is watched"
             : slot == &fSensors ? "its piano-sensors-light-target is taken when it publishes one" : "its display power property is watched");
    }
  }
  if (fKeyboard) {
    OSNumber *hall = OSDynamicCast(OSNumber, fKeyboard->getProperty("piano-keyboard-hall"));
    const bool wasLit = fCtl.wantLit();
    if (hall) fCtl.lid(hall->unsigned32BitValue() == 0 ? 0 : hall->unsigned32BitValue() == 1 ? 1 : -1);
    if (wasLit != fCtl.wantLit()) restartAmbient();
  }
  // PianoSensors' light target (its level 4; the property is absent before): a new one when its sequence or the value moved.
  if (fSensors) {
    OSObject *snapshot = fSensors->copyProperty("piano-sensors-light");
    OSDictionary *d = OSDynamicCast(OSDictionary, snapshot);
    OSNumber *target = d ? OSDynamicCast(OSNumber, d->getObject("target")) : nullptr;
    OSNumber *seq = d ? OSDynamicCast(OSNumber, d->getObject("seq")) : nullptr;
    OSNumber *lux = d ? OSDynamicCast(OSNumber, d->getObject("lux")) : nullptr;
    OSBoolean *valid = d ? OSDynamicCast(OSBoolean, d->getObject("valid")) : nullptr;
    OSBoolean *fault = d ? OSDynamicCast(OSBoolean, d->getObject("fault")) : nullptr;
    const uint64_t t = target ? target->unsigned64BitValue() : ~0ull, q = seq ? seq->unsigned64BitValue() : 0;
    if (fRevision) {
      OSNumber *revision = d ? OSDynamicCast(OSNumber, d->getObject("revision")) : nullptr;
      OSNumber *curve = d ? OSDynamicCast(OSNumber, d->getObject("curve")) : nullptr;
      if (fault && fault->isTrue()) fCtl.invalidateAmbient(true); // boot-latched, independent of curve
      if (revision && curve && seq && valid && (!valid->isTrue() || t <= 1000))
        applyAmbient(valid->isTrue() ? int32_t(t) : -1, lux ? lux->unsigned32BitValue() : 0, "PianoSensors",
                     {q, revision->unsigned32BitValue(), curve->unsigned32BitValue()});
    } else if (!valid || !valid->isTrue() || (fault && fault->isTrue())) {
      fCtl.invalidateAmbient(fault && fault->isTrue());
      fSensorsTarget = -2;
    } else if (target && seq && t <= 1000 && (int64_t(t) != fSensorsTarget || q != fSensorsSeq)) {
      fSensorsTarget = int64_t(t);
      fSensorsSeq = q;
      fCtl.ambient(int32_t(t), lux ? lux->unsigned32BitValue() : 0, "PianoSensors");
    }
    OSSafeReleaseNULL(snapshot);
  }
  if (lidOnly || !fDisplay) return;
  OSBoolean *power = OSDynamicCast(OSBoolean, fDisplay->getProperty(kDisplayPowerProperty));
  const int value = power ? power->isTrue() : -1;
  if (value >= 0 && value != fDisplayProperty && (!fDisplayKnown || bool(value) != fDisplayOn)) {
    note("display power from NativeGopDisplay's property: %s", value ? "on" : "off");
    fDisplayKnown = true;
    fDisplayOn = value;
    const bool wasLit = fCtl.wantLit();
    fCtl.display(value, fSystem, fApi);
    if (wasLit != fCtl.wantLit()) restartAmbient();
  }
  fDisplayProperty = value;
}

// A service beside this one under IOPlatformExpertDevice, by class, registered or not (copyMatchingService finds registered ones
// only). Retained.
IOService *PianoBacklight::sibling(const char *className)
{
  IOService *provider = getProvider();
  OSIterator *it = provider ? provider->getChildIterator(gIOServicePlane) : nullptr;
  IOService *found = nullptr;
  if (it) {
    while (OSObject *o = it->getNextObject())
      if (o->metaCast(className) && (found = OSDynamicCast(IOService, o))) {
        found->retain();
        break;
      }
    it->release();
  }
  return found;
}

void PianoBacklight::serve()
{
  const NativeBrightness schema(fCtl.floor(), fCtl.ceiling());
  IOLockLock(gLock);
  gNativeMin = schema.minFixed();
  gNativeMax = schema.maxFixed();
  gServing = true;
  IOLockUnlock(gLock);
  fRevision = schema.initialRevision();
  if (fRevision) {
    fSensorsSeq = 0;
    publishAndroidModel();
  }
  if (gArgs.native) {
    publishNativeState();
    if (!fNativePublished) registerService(kIOServiceAsynchronous);
  }
  uint64_t nextPoll = 0, nextSlow = 0, nextAlive = nowMs() + 60000;
  for (;;) {
    Pending p;
    IOLockLock(gLock);
    if (!gPending.any) {
      const uint64_t now = nowMs();
      const uint64_t wait = gPending.native < 0 ? 250 : fNextNativeMs > now ? fNextNativeMs - now : 0;
      uint64_t deadline = 0;
      if (wait) {
        clock_interval_to_deadline(uint32_t(wait), kMillisecondScale, &deadline);
        IOLockSleepDeadline(gLock, &gPending, deadline, THREAD_UNINT);
      }
    }
    p = gPending;
    if (p.native >= 0) {
      const uint64_t now = nowMs();
      if (now < fNextNativeMs) {
        p.native = -1;
        if (fRevision && p.automatic >= 0 && p.autoOrder > p.nativeOrder) p.automatic = -1;
      } else {
        gPending.native = -1;
        fNextNativeMs = now + kNativeApplyIntervalMs;
      }
    }
    gPending.any = gPending.display = gPending.ambient = false;
    gPending.steps = 0;
    gPending.percent = gPending.sleep = -1;
    if (p.automatic >= 0) gPending.automatic = -1;
    gPending.reference = -1;
    gPending.sync = gPending.managed = gPending.moved = -1;
    gPending.ambientFault = false;
    const uint32_t asked = gSleepAsked;
    const uint32_t blank = gBlankAsked;
    IOLockUnlock(gLock);

    if (p.sleep == 0 && fCtl.asleep()) nativeSettle("a wake");
    if (p.sleep >= 0) {
      const bool wasAsleep = fCtl.asleep();
      fCtl.sleep(p.sleep == 1);
      if (wasAsleep != fCtl.asleep()) restartAmbient();
      for (unsigned retry = 0; p.sleep == 1 && fCtl.pending() && retry < 2; ++retry) {   // the off must take before the freeze
        IOSleep(20);
        fCtl.retry();
      }
      IOLockLock(gLock);
      gDarkForSleep = !fCtl.lit(0) && !fCtl.lit(1);
      gSleepTaken = asked;
      IOLockWakeup(gLock, &gSleepTaken, false);
      IOLockUnlock(gLock);
    }

    if (p.display) {
      fDisplayKnown = true;
      if (p.on && !fDisplayOn) nativeSettle("a display power-on");
      fDisplayOn = p.on;
      fSystem = p.system;
      fApi = p.api;
      const bool wasLit = fCtl.wantLit();
      fCtl.display(p.on, p.system, p.api);
      if (wasLit != fCtl.wantLit()) restartAmbient();
    }
    if (p.display && blank != gBlankTaken) {
      for (unsigned retry = 0; !p.on && fCtl.pending() && retry < 2; ++retry) {
        IOSleep(20);
        fCtl.retry();
      }
      IOLockLock(gLock);
      gDarkForPanel = !p.on && fCtl.controlling() && !fCtl.lit(0) && !fCtl.lit(1);
      gBlankTaken = blank;
      IOLockWakeup(gLock, &gBlankTaken, false);
      IOLockUnlock(gLock);
    }
    if (p.reference >= 0 && fReference != bool(p.reference)) {
      fReference = bool(p.reference);
      fAndroidApply = {};  // the pending target was encoded with the old shape
      restartAmbient();
    }
    const bool autoAfterNative = !fRevision || (p.native >= 0 && p.autoOrder > p.nativeOrder);
    if (p.automatic >= 0) fHandover.cleared();   // an explicit piano-backlight-auto is the last word
    if (p.automatic >= 0 && !autoAfterNative) setAutomatic(p.automatic == 1);
    if (p.steps) fCtl.step(p.steps, p.source);
    if (p.percent >= 0) fCtl.percent(uint32_t(p.percent), "setProperties");
    if (p.sync >= 0) {   // before this round's request: the echo may already be in it
      fIntent.expect(p.sync, nowMs());
      if (fIntent.syncs() <= 8 || fIntent.syncs() % 25 == 0)
        note("PianoBacklightSyncV1 CoreBrightness is given requested16=%d (panel code %u, %s; %u syncs, %u echoes followed, %u missed)",
             p.sync, fCtl.code(), fCtl.autoActive() ? "automatic" : "not automatic", fIntent.syncs(), fIntent.echoes(), fIntent.missed());
    }
    if (p.managed >= 0) fManaged.set(uint32_t(p.managed), nowMs());
    const bool managed = fManaged.active(nowMs());
    if (p.native >= 0) {
      fLastNative = p.native;
      fLastNativeMs = nowMs();
    }
    const NativeIntent::Kind intent = p.native >= 0 ? fIntent.classify(p.native, nowMs()) : NativeIntent::kFollow;
    if (gArgs.native && fIntent.syncs() + fIntent.echoes() + fIntent.missed() != fSyncPublished) {
      fSyncPublished = fIntent.syncs() + fIntent.echoes() + fIntent.missed();
      setProperty("piano-backlight-sync-count", uint64_t(fIntent.syncs()), 32);
      setProperty("piano-backlight-sync-echoes", uint64_t(fIntent.echoes()), 32);
      setProperty("piano-backlight-sync-missed", uint64_t(fIntent.missed()), 32);
    }
    const bool autoOwns = gArgs.autoAllowed && fCtl.ambientState().requested();
    if (p.moved >= 0) {   // the user moved CoreBrightness's level (piano-brightness-sync saw DisplayNitsKey change)
      ++fDaemonMoves;
      ++fNativeMoves;
      fHandover.cleared();
      if (autoOwns) {
        // 2026-11-22 (AUTO-BRIGHT.md): automatic mode stays; CoreBrightness's own SDR level becomes the curve's trim (the panel shows it
        // now and follows the room from there). The stream's EDR-scaled value is not used: automatic mode is SDR.
        uint16_t code = 0;
        const bool decoded = NativeBrightness(fCtl.floor(), fCtl.ceiling(), fRevision && fReference).decode(uint32_t(p.moved), code);
        note("PianoBacklightNativeIntentV1 a move to requested16=%d (reported by piano-brightness-sync: CoreBrightness's level %d; %u reported, "
             "%u moves): automatic mode stays, the move is the curve's trim", p.moved, p.moved, fDaemonMoves, fNativeMoves);
        if (decoded) fCtl.userLevel(code, "piano-brightness-sync");
        fIntent.moved(p.moved);
        p.native = -1;
      } else {
        // manual: what WindowServer asked last (the level with its EDR headroom) when it is fresh, else the reported level
        const int32_t level = fLastNative >= 0 && nowMs() - fLastNativeMs < 3000 ? fLastNative : p.moved;
        note("PianoBacklightNativeIntentV1 a move to requested16=%d (reported by piano-brightness-sync: CoreBrightness's level %d; %u reported, "
             "%u moves): manual", level, p.moved, fDaemonMoves, fNativeMoves);
        fIntent.moved(level);
        p.native = level;
      }
    } else if (p.native >= 0) {
      const StreamChoice c = streamChoice(intent, autoOwns, managed, false);
      if (!c.apply) {
        ++fNativeFollowed;   // CoreBrightness's own value while automatic mode owns the panel: kept as the settled value only
        if (intent == NativeIntent::kUserMove) ++fManagedDrops;   // the restore, an EDR ramp or the user: the daemon tells them apart
        p.native = -1;
      } else if (intent == NativeIntent::kUserMove) {
        ++fNativeMoves;
        if (c.endsAuto) {
          fNativeTookAuto = true;
          fHandover.streamEndedAuto(managed);
        }
        if (c.endsAuto || fNativeMoves % 25 == 1)
          note("PianoBacklightNativeIntentV1 a move to requested16=%d (%u moves, %u followed, %u dark, %u from the swap): %s", p.native,
               fNativeMoves, fNativeFollowed, gNativeDark, gSwapNits, c.endsAuto ? "automatic mode ends until a display power-on or a wake" : "manual");
      }
    }
    if (p.native >= 0) {
      uint16_t code = 0;
      uint16_t dbv = 0;
      if (NativeBrightness(fCtl.floor(), fCtl.ceiling(), fRevision && fReference).decode(uint32_t(p.native), code, &dbv)) {
        fNativeRequest = p.native;
        fNativeErrors = fCtl.errors();
        if (fRevision) fAndroidApply = {"native", fRevision, pa_sdr_millinits(uint32_t(p.native)), fNativeErrors, dbv, code};
        fCtl.manualCode(code, kNativeRequest);
      }
    }
    if (gArgs.native) {   // what the daemon watches: its lease and the newest request (at most every 100 ms, the last one always)
      const int on = managed ? 1 : 0;
      bool changed = false;
      if (on != fManagedPublished) {
        if (on || fManagedPublished == 1)
          note("PianoBacklightManagedV1 %s (%u moves reported, %u stream changes left to it while automatic)",
               on ? "piano-brightness-sync watches CoreBrightness's level: the stream no longer ends automatic mode"
                  : "the lease ended: WindowServer's stream decides again", fDaemonMoves, fManagedDrops);
        fManagedPublished = on;
        setProperty(kNativeManaged, bool(on));
        changed = true;
        if (on && fHandover.leaseArrived() && fNativeTookAuto && gArgs.autoAllowed) {
          fNativeTookAuto = false;
          setAutomatic(true);
          note("PianoBacklightNativeIntentV1 automatic mode again: WindowServer's stream ended it before piano-brightness-sync "
               "watched (CoreBrightness's restore or an EDR ramp)");
        }
      }
      if (fLastNative != fRequestedPublished && nowMs() - fRequestedPublishedMs >= 100) {
        fRequestedPublished = fLastNative;
        fRequestedPublishedMs = nowMs();
        setProperty(kNativeRequested, uint64_t(uint32_t(fLastNative)), 32);
        changed = true;
      }
      if (changed) messageClients(kIOMessageServicePropertyChange);
    }
    if (p.automatic >= 0 && autoAfterNative) setAutomatic(p.automatic == 1);
    if (p.ambientFault) fCtl.invalidateAmbient(true);
    if (p.ambient) applyAmbient(p.target, p.lux, p.ambientSource, p.ambientVersion);
    const uint64_t now = nowMs();
    if (now >= nextPoll) {
      nextPoll = now + 250;
      const bool slow = now >= nextSlow;
      if (slow) nextSlow = now + 1000;
      poll(now, !slow);
      if (slow) fCtl.retry();
    }
    publishState();
    if (gArgs.native) publishNativeState();
    if (now >= nextAlive) {
      nextAlive = now + 600000;
      note("alive: %s, code %u (%u %%), %s; %u writes, %u failed accesses; calls: display %u, steps %u", fCtl.controlling() ? "controlling" : "NOT controlling",
           fCtl.code(), percentOf(fCtl.code()), fCtl.lit(0) && fCtl.lit(1) ? "lit" : fCtl.lit(0) || fCtl.lit(1) ? "HALF lit" : "dark",
           fCtl.writes(), fCtl.errors(), p.displayCalls, p.stepCalls);
    }
  }
}

void PianoBacklight::publishNativeState()
{
  const NativeBrightness adapter(fCtl.floor(), fCtl.ceiling(), fRevision && fReference);
  uint16_t code = 0;
  const bool applied = adapter.applied(fCtl, code);
  if (applied && (!fNativePublished || code != fNativeCode || fRevision != fNativePublishedRevision)) {
    NativeProperties params(*this);
    if (adapter.parameters(params, code)) {
      fNativeCode = code;
      fNativePublishedRevision = fRevision;
      if (!fNativePublished) {
        NativeProperties calibration(*this);
        if (adapter.calibration(calibration) && setProperty("piano-backlight-native-implementation", kNativeMarker)) {
          IOLockLock(gLock);
          gNativeReady = true;
          IOLockUnlock(gLock);
          static const uint8_t kOne = 1;
          if (OSData *force = OSData::withBytes(&kOne, 1)) {   // BCAppleBacklightBrtControl's _override (read at its init)
            setProperty(kNativeOverride, force);
            force->release();
          }
          fNativePublished = setProperty("backlight-control", true);  // matching flag comes last
          if (fNativePublished) {
            if (adapter.bounded())
              note("%s ready: Android-reference SDR %u..600000 millinits, floor=%u code=%u",
                   kNativeMarker, adapter.milliNits(fCtl.floor()), fCtl.floor(), code);
            else
              note("%s ready: nominal SDR %u..500 nits, floor=%u code=%u", kNativeMarker, adapter.minNits(), fCtl.floor(), code);
            registerService(kIOServiceAsynchronous);
          } else {
            IOLockLock(gLock);
            gNativeReady = false;
            IOLockUnlock(gLock);
          }
        }
      } else {
        messageClients(kIOMessageServicePropertyChange);
      }
    }
  }
  publishAndroidApply(adapter, applied, code);
  if (fNativeRequest >= 0) {
    uint32_t dropped;
    if (fNativeApplyLog.allow(nowMs(), &dropped)) {
      if (applied && !fCtl.pwmQualified())
        note("%s source=%s requested16=%d result=pwm-unqualified primary=%u secondary=%u errors=%u skipped=%u",
             kNativeMarker, kNativeRequest, fNativeRequest, fCtl.ic(0).code, fCtl.ic(1).code, fCtl.errors() - fNativeErrors, dropped);
      else
        note("%s source=%s requested16=%d result=%s applied_millinits=%u primary=%u secondary=%u errors=%u skipped=%u",
             kNativeMarker, kNativeRequest, fNativeRequest, applied && fNativeCode == code ? "applied" : "pending",
             adapter.milliNits(fNativeCode), fCtl.ic(0).code, fCtl.ic(1).code, fCtl.errors() - fNativeErrors, dropped);
    }
    fNativeRequest = -1;
  }
  if (fNativePublished && !fCtl.controlling()) {
    IOLockLock(gLock);
    gNativeReady = false;
    IOLockUnlock(gLock);
  }
}

void PianoBacklight::publishAndroidModel()
{
  const NativeBrightness adapter(fCtl.floor(), fCtl.ceiling(), fReference);
  setProperty(kReferenceProperty, uint64_t(fReference), 32);
  setProperty("piano-backlight-map-crc", uint64_t(PA_SDR_MAP_CRC), 32);
  setProperty("piano-backlight-sdr-revision", uint64_t(fRevision), 32);
  note("%s model=%s schema=600 min_millinits=%u max_millinits=600000 code_min=%u code_max=1952 map_crc=%08x revision=%u",
       kAndroidMarker, fReference ? "reference" : "linear", adapter.milliNits(fCtl.floor()), fCtl.floor(), PA_SDR_MAP_CRC, fRevision);
}

void PianoBacklight::restartAmbient()
{
  if (!fRevision) return;
  ++fRevision;
  fSensorsSeq = 0;
  fCtl.invalidateAmbient(false);
  if (fAndroidApply.source && !strcmp(fAndroidApply.source, "ambient")) fAndroidApply = {};
  publishAndroidModel();
}

// A display power-on or a wake: WindowServer's next values settle again (its power-on ramp is not a move), and automatic mode
// comes back when the stream's own rule (no lease) had ended it. A reported move never ends it (2026-11-22: the curve's trim).
void PianoBacklight::nativeSettle(const char *why)
{
  if (!gArgs.native) return;
  fIntent.reset(nowMs());
  fHandover.cleared();
  if (fNativeTookAuto && gArgs.autoAllowed) {
    fNativeTookAuto = false;
    setAutomatic(true);
    note("PianoBacklightNativeIntentV1 automatic mode again after %s (the slider's level held until then)", why);
  }
}

void PianoBacklight::setAutomatic(bool on)
{
  if (on && !fCtl.ambientState().requested()) restartAmbient();
  fCtl.automatic(on, "setProperties");
}

void PianoBacklight::applyAmbient(int32_t permille, uint32_t lux, const char *source, const pianoauto::AmbientVersion &version)
{
  if (!fRevision) {
    fCtl.ambient(permille, lux, source);
    return;
  }
  if (!version.newer(fRevision, fReference, fSensorsSeq)) return;
  fSensorsSeq = version.seq;
  if (permille < 0) {
    fCtl.invalidateAmbient(false);
    return;
  }
  if (!fCtl.ambientState().requested() || fCtl.autoFault() || !fCtl.wantLit()) return;
  const uint32_t errors = fCtl.errors();
  fCtl.ambient(permille, lux, source);
  if (!fCtl.ambientState().automatic()) return;
  const NativeBrightness adapter(fCtl.floor(), fCtl.ceiling(), fReference);
  const uint16_t raw = pianoauto::Ambient::codeForPermille(uint32_t(permille), fCtl.floor());
  const uint16_t requested = raw > fCtl.ceiling() ? fCtl.ceiling() : raw;
  const uint32_t milli = adapter.milliNits(requested);
  const uint32_t fixed = fReference ? pa_sdr_fixed_for_code(requested) : pa_sdr_linear_fixed(fCtl.floor(), requested);
  fAndroidApply = {"ambient", fRevision, milli, errors, pa_sdr_dbv(fixed), fCtl.code()};
}

void PianoBacklight::publishAndroidApply(const NativeBrightness &adapter, bool applied, uint16_t code)
{
  auto &request = fAndroidApply;
  if (!request.source) return;
  if (request.code != fCtl.code()) {
    request = {};  // superseded by another control request
    return;
  }
  const unsigned errors = fCtl.errors() - request.errors;
  if (applied && code == request.code) {
    if (fCtl.pwmQualified())
      note("%s source=%s revision=%u requested_millinits=%u dbv=%u target_code=%u primary=%u secondary=%u applied_millinits=%u errors=%u",
           kAndroidMarker, request.source, request.revision, request.milliNits, request.dbv, request.code, fCtl.ic(0).code, fCtl.ic(1).code,
           adapter.milliNits(code), errors);
    else
      note("%s source=%s revision=%u requested_millinits=%u dbv=%u target_code=%u primary=%u secondary=%u result=pwm-unqualified errors=%u",
           kAndroidMarker, request.source, request.revision, request.milliNits, request.dbv, request.code, fCtl.ic(0).code, fCtl.ic(1).code,
           errors);
    request = {};
  } else if (!request.pendingLogged || !fCtl.controlling()) {
    note("%s source=%s revision=%u requested_millinits=%u dbv=%u target_code=%u primary=%u secondary=%u result=%s errors=%u",
         kAndroidMarker, request.source, request.revision, request.milliNits, request.dbv, request.code, fCtl.ic(0).code, fCtl.ic(1).code,
         fCtl.controlling() ? "pending" : "failed", errors);
    request.pendingLogged = true;
    if (!fCtl.controlling()) request = {};
  }
}

void PianoBacklight::publishState()
{
  const pianoauto::Ambient &a = fCtl.ambientState();
  const IcState &p = fCtl.ic(0), &q = fCtl.ic(1);
  const uint64_t state[] = {fCtl.code(), fCtl.controlling(), fCtl.wantLit(), fCtl.autoActive(), fCtl.autoFault(),
      a.requested(), a.valid(), uint64_t(uint32_t(a.lastTarget())), a.lastLux(), uint64_t(uint32_t(a.offset())), a.targets(),
      p.code, q.code, p.lit, q.lit, p.codeReadbacks, q.codeReadbacks, p.enableReadbacks, q.enableReadbacks,
      p.errors, q.errors, fCtl.errors(), fCtl.writes(), fRevision, uint64_t(uint32_t(a.trim()))};
  static_assert(sizeof(state) == sizeof(fPublished), "backlight publication fields");
  if (fHavePublished && !memcmp(state, fPublished, sizeof(state))) return;
  memcpy(fPublished, state, sizeof(state));
  fHavePublished = true;
  setProperty("piano-backlight-code", fCtl.code(), 32);
  setProperty("piano-backlight-floor", uint64_t(fCtl.floor()), 32);
  setProperty("piano-backlight-exponential", fCtl.exponential());
  setProperty("piano-backlight-percent", percentOf(fCtl.code()), 32);
  setProperty("piano-backlight-lit", fCtl.lit(0) && fCtl.lit(1));
  setProperty("piano-backlight-control", fCtl.controlling());
  setProperty("piano-backlight-want-lit", fCtl.wantLit());
  setProperty("piano-backlight-errors", uint64_t(fCtl.errors()), 32);
  setProperty("piano-backlight-writes", uint64_t(fCtl.writes()), 32);
  setProperty("piano-backlight-primary-code", uint64_t(p.code), 32);
  setProperty("piano-backlight-secondary-code", uint64_t(q.code), 32);
  setProperty("piano-backlight-primary-lit", p.lit);
  setProperty("piano-backlight-secondary-lit", q.lit);
  setProperty("piano-backlight-primary-enable", uint64_t(p.reg[kRegEn]), 32);
  setProperty("piano-backlight-secondary-enable", uint64_t(q.reg[kRegEn]), 32);
  setProperty("piano-backlight-primary-readbacks", uint64_t(p.codeReadbacks), 32);
  setProperty("piano-backlight-secondary-readbacks", uint64_t(q.codeReadbacks), 32);
  setProperty("piano-backlight-primary-enable-readbacks", uint64_t(p.enableReadbacks), 32);
  setProperty("piano-backlight-secondary-enable-readbacks", uint64_t(q.enableReadbacks), 32);
  setProperty("piano-backlight-primary-errors", uint64_t(p.errors), 32);
  setProperty("piano-backlight-secondary-errors", uint64_t(q.errors), 32);
  if (!gArgs.autoAllowed) return;
  if (fRevision) {
    NativeProperties snapshot(*this);
    publishAmbientSnapshot(snapshot, fCtl, fRevision, fReference);
  }
  setProperty("piano-backlight-implementation", "sens-ab1:session-valid:dual-readback");
  setProperty(pianoauto::kPropertyAuto, fCtl.autoActive());
  setProperty(pianoauto::kPropertyRequested, a.requested());
  setProperty(pianoauto::kPropertyValid, a.valid());
  setProperty("piano-backlight-auto-active", fCtl.autoActive());
  setProperty("piano-backlight-auto-fault", fCtl.autoFault());
  setProperty(pianoauto::kPropertyOffset, uint64_t(uint32_t(int32_t(a.offset()))), 32);   // signed 32-bit
  setProperty(pianoauto::kPropertyTrim, uint64_t(uint32_t(int32_t(a.trim()))), 32);       // signed 32-bit (2026-11-22)
  setProperty(pianoauto::kPropertyTarget, uint64_t(uint32_t(a.lastTarget())), 32);
  if (a.lastTarget() >= 0) {
    setProperty(pianoauto::kPropertyLux, uint64_t(a.lastLux()), 32);
  }
  messageClients(kIOMessageServicePropertyChange);
}

IOReturn PianoBacklight::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  const bool display = name && name->isEqualTo(kFunctionDisplay), step = name && name->isEqualTo(kFunctionStep);
  const bool ambient = name && name->isEqualTo(kFunctionAmbient);
  if (name && name->isEqualTo(kFunctionNits)) return enqueueSwapNits(int64_t(int32_t(uintptr_t(p1))));
  if (!display && !step && !ambient) return IOService::callPlatformFunction(name, wait, p1, p2, p3, p4);
  if (step && gArgs.native) return kIOReturnUnsupported;
  if (ambient && (gArgs.level < 2 || !gArgs.autoAllowed)) return kIOReturnUnsupported;
  if (!gLock) return kIOReturnNotReady;
  IOLockLock(gLock);
  const bool waitBlank = display && !p1 && p4;
  if (waitBlank && !gServing) {
    IOLockUnlock(gLock);
    return kIOReturnNotReady;
  }
  if (ambient) {
    const intptr_t t = intptr_t(p1);
    const bool versioned = gNativeMax == PA_SDR_MAX_FIXED;
    if (versioned && t >= 0 && !p4) {
      IOLockUnlock(gLock);
      return kIOReturnBadArgument;
    }
    if (versioned && t < 0 && !p4) {
      gPending.ambientFault = true;
    } else {
      gPending.ambient = true;
      gPending.target = t < 0 ? -1 : t > 1000 ? 1000 : int32_t(t);
      gPending.lux = uint32_t(uintptr_t(p3) > 0xffffffffu ? 0xffffffffu : uintptr_t(p3));
      gPending.ambientSource = p2 ? static_cast<const char *>(p2) : "?";
      if (versioned) gPending.ambientVersion = *static_cast<const pianoauto::AmbientVersion *>(p4);
    }
    ++gPending.ambientCalls;
  } else if (display) {
    gPending.display = true;
    gPending.on = p1 != nullptr;
    gPending.system = uint32_t(uintptr_t(p2));
    gPending.api = uint32_t(uintptr_t(p3));
    ++gPending.displayCalls;
  } else {
    const int32_t delta = int32_t(intptr_t(p1));
    gPending.steps = int32_t(clampArg(uint32_t(gPending.steps + delta + 20), 0, 40)) - 20;
    gPending.source = p2 ? static_cast<const char *>(p2) : "?";
    ++gPending.stepCalls;
  }
  gPending.any = true;
  const uint32_t blank = waitBlank ? ++gBlankAsked : 0;
  IOLockWakeup(gLock, &gPending, false);
  IOReturn result = kIOReturnSuccess;
  if (waitBlank) {
    uint64_t deadline = 0;
    clock_interval_to_deadline(800, kMillisecondScale, &deadline);
    while (gBlankTaken != blank && IOLockSleepDeadline(gLock, &gBlankTaken, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
    result = gBlankTaken != blank ? kIOReturnTimeout : gDarkForPanel ? kIOReturnSuccess : kIOReturnIOError;
  }
  IOLockUnlock(gLock);
  return result;
}

// docs/power.md 4: off = the backlight dark before the system freezes (the thread clears BL_EN; this waits for it, at most 800 ms, so
// the answer is synchronous), on = the thread lights it again if the display and the lid want it (asynchronous). Not controlling
// (level 1, control off): nothing.
IOReturn PianoBacklight::setPowerState(unsigned long state, IOService *)
{
  if (!gLock) return pianopower::kAckNow;
  IOLockLock(gLock);
  if (!gServing) {
    IOLockUnlock(gLock);
    return pianopower::kAckNow;
  }
  gPending.sleep = state == pianopower::kOff ? 1 : 0;
  gPending.any = true;
  const uint32_t ask = ++gSleepAsked;
  IOLockWakeup(gLock, &gPending, false);
  bool taken = true, dark = true;
  if (state == pianopower::kOff) {
    uint64_t deadline = 0;
    clock_interval_to_deadline(800, kMillisecondScale, &deadline);
    while (gSleepTaken != ask && IOLockSleepDeadline(gLock, &gSleepTaken, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    taken = gSleepTaken == ask;
    dark = gDarkForSleep;
  }
  IOLockUnlock(gLock);
  if (state == pianopower::kOff && (!taken || !dark))
    note("system sleep: %s", !taken ? "the thread did not answer within 800 ms (the backlight may stay lit while suspended)"
                                    : "an IC did not take BL_EN off (the backlight may stay lit while suspended)");
  return pianopower::kAckNow;
}

// An administrator's request (IORegistryEntrySetCFProperties): "piano-backlight-percent" = 0..100 of the range, "piano-backlight-step"
// = a signed number of steps. Level 2 only.
IOReturn PianoBacklight::enqueueNative(OSDictionary &properties, OSObject *value)
{
  if (!gArgs.native) return kIOReturnUnsupported;
  const bool admin = IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
  proc_t process = current_proc();
  const bool platform = process && csproc_get_platform_binary(process);
  const bool authorized = nativeWriter(admin, platform, platform ? cs_identity_get(process) : nullptr);
  OSNumber *number = OSDynamicCast(OSNumber, value);
  // signed 16.16 (BrightnessControl's setNits); WindowServer's stream is clamped to the published range, its <= 0 (a dark ramp)
  // taken and dropped (2026-11-20, BRIGHTNESS-SLIDER.md)
  const unsigned bits = number ? number->numberOfBits() : 0;
  const int64_t asked = !number ? 0 : bits == 32 ? int64_t(int32_t(uint32_t(number->unsigned64BitValue())))
                                                 : int64_t(number->unsigned64BitValue());
  IOLockLock(gLock);
  const uint32_t min = gServing ? gNativeMin : NativeBrightness(uint16_t(gArgs.floor)).minFixed();
  const uint32_t max = gServing ? gNativeMax : NativeBrightness(uint16_t(gArgs.floor)).maxFixed();
  const bool valid = properties.getCount() == 1 && number && (bits == 32 || bits == 64);
  const uint64_t fixed = asked <= 0 ? 0 : asked < int64_t(min) ? min : asked > int64_t(max) ? max : uint64_t(asked);
  const IOReturn result = !authorized ? kIOReturnNotPrivileged : !valid ? kIOReturnBadArgument
      : !gNativeReady ? kIOReturnNotReady : kIOReturnSuccess;
  uint32_t dropped;
  const bool logRefusal = result != kIOReturnSuccess && gNativeRejectionLog.allow(nowMs(), &dropped);
  if (result == kIOReturnSuccess && !fixed) {
    ++gNativeDark;   // a dark ramp's value: nothing to apply (the display's power takes the backlight down)
  } else if (result == kIOReturnSuccess) {
    gPending.native = int32_t(fixed);
    if (gNativeMax == PA_SDR_MAX_FIXED) gPending.nativeOrder = ++gControlOrder;
    gPending.any = true;
    IOLockWakeup(gLock, &gPending, false);
  }
  IOLockUnlock(gLock);
  if (logRefusal) note("%s source=%s refused=%#x pid=%d skipped=%u", kNativeMarker, kNativeRequest, result, proc_selfpid(), dropped);
  return result;
}

// 2026-11-21: WindowServer's brightness as the swap carries it (NativeGopDisplay, kernel caller): the stream brightness-nits gets.
IOReturn PianoBacklight::enqueueSwapNits(int64_t asked)
{
  if (!gArgs.native || !gLock) return kIOReturnUnsupported;
  IOLockLock(gLock);
  const uint32_t min = gServing ? gNativeMin : NativeBrightness(uint16_t(gArgs.floor)).minFixed();
  const uint32_t max = gServing ? gNativeMax : NativeBrightness(uint16_t(gArgs.floor)).maxFixed();
  const uint64_t fixed = asked <= 0 ? 0 : asked < int64_t(min) ? min : asked > int64_t(max) ? max : uint64_t(asked);
  IOReturn result = kIOReturnSuccess;
  if (!gNativeReady) {
    result = kIOReturnNotReady;
  } else if (!fixed) {
    ++gNativeDark;
  } else {
    gPending.native = int32_t(fixed);
    if (gNativeMax == PA_SDR_MAX_FIXED) gPending.nativeOrder = ++gControlOrder;
    gPending.any = true;
    ++gSwapNits;
    IOLockWakeup(gLock, &gPending, false);
  }
  IOLockUnlock(gLock);
  return result;
}

IOReturn PianoBacklight::setProperties(OSObject *properties)
{
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d || gArgs.level < 2 || !gLock) return kIOReturnUnsupported;
  if (OSObject *native = d->getObject(kNativeRequest)) return enqueueNative(*d, native);
  if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return kIOReturnNotPrivileged;
  if (OSObject *sync = d->getObject(kNativeSync)) {   // 2026-11-21: piano-brightness-sync announces the level it gives CoreBrightness
    OSNumber *number = OSDynamicCast(OSNumber, sync);
    const unsigned bits = number ? number->numberOfBits() : 0;
    const int64_t asked = !number ? -1 : bits == 32 ? int64_t(int32_t(uint32_t(number->unsigned64BitValue())))
                                                    : int64_t(number->unsigned64BitValue());
    if (!gArgs.native) return kIOReturnUnsupported;
    if (d->getCount() != 1 || !number || (bits != 32 && bits != 64)) return kIOReturnBadArgument;
    IOLockLock(gLock);
    const bool ready = gServing && gNativeReady;
    const bool inRange = asked >= int64_t(gNativeMin) && asked <= int64_t(gNativeMax);
    if (ready && inRange) {
      gPending.sync = int32_t(asked);
      gPending.any = true;
      IOLockWakeup(gLock, &gPending, false);
    }
    IOLockUnlock(gLock);
    return !ready ? kIOReturnNotReady : !inRange ? kIOReturnBadArgument : kIOReturnSuccess;
  }
  OSObject *managedObject = d->getObject(kNativeManaged), *movedObject = d->getObject(kNativeMoved);
  if (managedObject || movedObject) {   // round 2: the daemon's lease (seconds) or the user's move it saw (16.16)
    OSNumber *number = OSDynamicCast(OSNumber, managedObject ? managedObject : movedObject);
    const unsigned bits = number ? number->numberOfBits() : 0;
    const int64_t asked = !number ? -1 : bits == 32 ? int64_t(int32_t(uint32_t(number->unsigned64BitValue())))
                                                    : int64_t(number->unsigned64BitValue());
    if (!gArgs.native) return kIOReturnUnsupported;
    if (d->getCount() != 1 || !number || (bits != 32 && bits != 64)) return kIOReturnBadArgument;
    IOLockLock(gLock);
    const bool ready = gServing && gNativeReady;
    const bool inRange = managedObject ? asked >= 0 && asked <= int64_t(kManagedMaxS)
                                       : asked >= int64_t(gNativeMin) && asked <= int64_t(gNativeMax);
    if (ready && inRange) {
      if (managedObject) gPending.managed = int32_t(asked);
      else gPending.moved = int32_t(asked);
      gPending.any = true;
      IOLockWakeup(gLock, &gPending, false);
    }
    IOLockUnlock(gLock);
    return !ready ? kIOReturnNotReady : !inRange ? kIOReturnBadArgument : kIOReturnSuccess;
  }
  if (OSObject *reference = d->getObject(kReferenceProperty)) {
    OSNumber *number = OSDynamicCast(OSNumber, reference);
    if (d->getCount() != 1 || !number || number->unsigned64BitValue() > 1) return kIOReturnBadArgument;
    IOLockLock(gLock);
    const bool supported = gServing && gNativeMax == PA_SDR_MAX_FIXED;
    const bool ready = gNativeReady;
    if (supported && ready) {
      gPending.reference = int32_t(number->unsigned32BitValue());
      gPending.any = true;
      IOLockWakeup(gLock, &gPending, false);
    }
    IOLockUnlock(gLock);
    return !supported ? kIOReturnUnsupported : !ready ? kIOReturnNotReady : kIOReturnSuccess;
  }
  OSNumber *percent = OSDynamicCast(OSNumber, d->getObject("piano-backlight-percent"));
  OSNumber *steps = OSDynamicCast(OSNumber, d->getObject("piano-backlight-step"));
  OSObject *automatic = gArgs.autoAllowed ? d->getObject(pianoauto::kArgAuto) : nullptr;   // OSNumber 0/1 or OSBoolean
  OSNumber *autoNumber = OSDynamicCast(OSNumber, automatic);
  OSBoolean *autoBoolean = OSDynamicCast(OSBoolean, automatic);
  if (!percent && !steps && !autoNumber && !autoBoolean) return kIOReturnBadArgument;
  IOLockLock(gLock);
  if (gNativeMax == PA_SDR_MAX_FIXED && automatic &&
      (d->getCount() != 1 || (!autoNumber && !autoBoolean) || (autoNumber && autoNumber->unsigned64BitValue() > 1))) {
    IOLockUnlock(gLock);
    return kIOReturnBadArgument;
  }
  if (percent) gPending.percent = int32_t(clampArg(percent->unsigned32BitValue(), 0, 100));
  if (steps) {
    gPending.steps = int32_t(clampArg(uint32_t(gPending.steps + int32_t(steps->unsigned32BitValue()) + 20), 0, 40)) - 20;
    gPending.source = "setProperties";
  }
  if (autoNumber || autoBoolean) {
    gPending.automatic = autoBoolean ? autoBoolean->isTrue() : autoNumber->unsigned32BitValue() ? 1 : 0;
    if (gNativeMax == PA_SDR_MAX_FIXED) gPending.autoOrder = ++gControlOrder;
  }
  gPending.any = true;
  IOLockWakeup(gLock, &gPending, false);
  IOLockUnlock(gLock);
  return kIOReturnSuccess;
}
