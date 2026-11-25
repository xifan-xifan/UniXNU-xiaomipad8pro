// SPDX-License-Identifier: BSD-2-Clause-Patent
// The panel backlight of piano: two Kinetic KTZ8866 ICs, the same brightness on both. Portable (no kernel headers): built into the
// kext (PianoBacklight.cpp, which brings the buses, the clock and the log) and into tests/backlight_host.cpp (a register model).
//
// Register facts, from the KTZ8866 datasheet (Kinetic Technologies, revision 04b, January 2022, public on kinet-ic.com): 7-bit
// address 0x11, 100/400 kHz, register address auto-incremented. 0x01 device ID (bits 7:6 0, bits 5:3 = 100, bits 2:0 reserved);
// 0x02 BL_CFG1 (7:5 OVP, 4 OVP mode, 3 BLED_MAP 1 linear / 0 exponential, 2 PWM polarity, 1 ramp shape, 0 PWM enable); 0x03 BL_CFG2
// (6:3 LED current ramp); 0x04 BL_BRT_LSB (2:0 the code's 3 LSBs), 0x05 BL_BRT_MSB (the 8 MSBs): an 11-bit code, LSB written first,
// the MSB write makes it take effect, code 0 turns the boost and the sinks off; 0x08 BL_EN (7 SOFTWARE_RESET: all registers to their
// defaults, 6 BL_EN, 5:0 the six current sinks); 0x09..0x0e the LCD bias (LCD_EN, OUTP/OUTN enables, the panel's +/- supplies: never
// written here); 0x0f FLAG (latched faults, cleared by reading it); 0x10/0x11 options (0x11 1:0 current limit); 0x12/0x13 the PWM
// input as a code; 0x14 turn-on/off ramps (7:4 on, 3:0 off: 0 = 512 us, n = 2^(n-1) ms); 0x15 full-scale current (7:3: 5.2 mA +
// 0.8 mA per step) and PWM ramp. HWEN low is shutdown (registers reset, no I2C): HWEN is TLMM gpio2, never touched here.
// Behaviour, from Linux's ktz8866 backlight driver as Project-SunUEFI patches it for piano (GPL-2.0: read as the specification only,
// nothing copied): every write goes to the primary, then the same write to the secondary; brightness = the 11-bit code; on = the code
// first, then BL_EN set; off = BL_EN cleared (the IC fades out over its 0x14 off ramp); the level found at start is kept when BL_EN is
// set and the code non-zero. Piano's DT: primary backlight@11 on i2c@98c000 (I2C hub SE3, gpio70/71, enable-gpios TLMM 2,
// current-num-sinks 6, kinetic,secondary-backlight), secondary backlight@11 on i2c@880000 (QUP2 SE0, gpio0/1, kinetic,secondary).
//
// This driver writes three registers only, 0x04, 0x05 and 0x08, and in 0x08 only bit 6: bits 5:0 are written back as read, bit 7
// (the software reset) always 0. The code is never below the floor (piano-backlight-min), so a lit panel never goes dark by a step.
//
// Automatic brightness (Config::autoAllowed, piano-backlight-auto=1; off by default, and then nothing below changes): targets of the
// ambient light policy (permille of the code range) set the code through package/PianoAuto/AmbientBacklight.h; the brightness keys
// then shift the curve by this ladder's steps (the offset, kept for the boot); an explicit level (percent) switches to manual until
// automatic(true); the user's slider level (userLevel(), 2026-11-22) keeps automatic mode and becomes the curve's trim.
// docs/drivers/sensors.md 5a.4, backlight.md.
#ifndef PIANO_BACKLIGHT_KTZ8866_H
#define PIANO_BACKLIGHT_KTZ8866_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../PianoAuto/AmbientBacklight.h"
#include "../PianoAuto/AndroidBrightness.h"
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace pianobacklight {

constexpr uint8_t kAddress = 0x11;
enum : uint8_t {
  kRegId = 0x01, kRegCfg1 = 0x02, kRegCfg2 = 0x03, kRegBrtLsb = 0x04, kRegBrtMsb = 0x05, kRegEn = 0x08, kRegLcd1 = 0x09,
  kRegLcd2 = 0x0a, kRegLcd3 = 0x0b, kRegBoost = 0x0c, kRegOutp = 0x0d, kRegOutn = 0x0e, kRegFlag = 0x0f, kRegOpt1 = 0x10,
  kRegOpt2 = 0x11, kRegPwmLsb = 0x12, kRegPwmMsb = 0x13, kRegRamp = 0x14, kRegIfs = 0x15, kRegCount = 0x16,
};
// The survey's reads (no side effect). FLAG (0x0f) is read only with piano-backlight-flags=1: reading it clears its latched bits.
constexpr uint8_t kSurveyRegs[] = {kRegId, kRegCfg1, kRegCfg2, kRegBrtLsb, kRegBrtMsb, kRegEn, kRegLcd1, kRegLcd2, kRegLcd3,
                                   kRegBoost, kRegOutp, kRegOutn, kRegOpt1, kRegOpt2, kRegPwmLsb, kRegPwmMsb, kRegRamp, kRegIfs};
constexpr uint8_t kEnSoftReset = 0x80, kEnBacklight = 0x40, kEnSinks = 0x3f, kCfg1LinearMap = 0x08, kCfg1PwmEnable = 0x01;
constexpr uint8_t kQualifiedPwmCfg1 = 0x53, kQualifiedPwmIfs = 0xd9;
constexpr uint16_t kFullScale = 2047;
constexpr unsigned kIcs = 2;
constexpr const char *kIcName[kIcs] = {"primary", "secondary"};

inline bool idIsKtz8866(uint8_t id) { return (id & 0xf8) == 0x20; }
inline uint16_t codeOf(uint8_t lsb, uint8_t msb) { return uint16_t(msb << 3 | (lsb & 7)); }
inline uint32_t percentOf(uint16_t code) { return (uint32_t(code) * 100 + kFullScale / 2) / kFullScale; }
// The only registers a write may name, and the only BL_EN values: bits 5:0 as read, bit 6 on/off, bit 7 (the reset) never.
inline bool writable(uint8_t reg) { return reg == kRegBrtLsb || reg == kRegBrtMsb || reg == kRegEn; }
inline uint8_t enableValue(uint8_t found, bool on) { return uint8_t((found & kEnSinks) | (on ? kEnBacklight : 0)); }
// A code for a percentage of the 11-bit range (0..100), never below the floor.
inline uint16_t codeForPercent(uint32_t percent, uint16_t floor)
{
  const uint32_t p = percent > 100 ? 100 : percent;
  const uint32_t code = (p * kFullScale + 50) / 100;
  return uint16_t(code < floor ? floor : code > kFullScale ? kFullScale : code);
}
// The off/on ramps of register 0x14 (one nibble) in microseconds.
inline uint32_t rampUs(uint8_t nibble) { return nibble ? 1000u << ((nibble & 15) - 1) : 512u; }

// ------------------------------------------------------------------------------------------------------------- brightness steps
// Ascending codes, the floor first. Linear mapping (BL_CFG1 bit 3, the reset value): full scale divided by 2^(k/3), k = 0, 1, ...
// while above the floor, then the floor: three steps per doubling of the LED current, about 26 % each, which the eye sees as even.
// Exponential mapping: the IC's codes are already perceptual, so 16 equal steps from the floor to full scale.
class Steps {
 public:
  void build(uint16_t floor, bool exponential, uint16_t ceiling = kFullScale)
  {
    n_ = 0;
    floor_ = floor;
    ceiling_ = ceiling;
    if (exponential) {
      for (unsigned i = 0; i <= 16; ++i) code_[n_++] = uint16_t(floor + (uint32_t(ceiling - floor) * i + 8) / 16);
      return;
    }
    static constexpr uint64_t kCubeRoots[3] = {1000000, 1259921, 1587401};   // 2^(0/3), 2^(1/3), 2^(2/3) x 10^6
    uint16_t down[kMax];
    unsigned m = 0;
    for (unsigned k = 0; m + 1 < kMax; ++k) {
      const uint64_t divisor = kCubeRoots[k % 3] << (k / 3);
      const uint16_t v = uint16_t((uint64_t(ceiling) * 1000000 + divisor / 2) / divisor);
      if (v <= floor) break;
      down[m++] = v;
    }
    code_[n_++] = floor;
    while (m) code_[n_++] = down[--m];
  }
  // The next step above code (full scale stays), the next one below (the floor stays; a code under the floor is not raised by "down").
  uint16_t up(uint16_t code) const
  {
    for (unsigned i = 0; i < n_; ++i)
      if (code_[i] > code) return code_[i];
    return code > ceiling_ ? ceiling_ : code;
  }
  uint16_t down(uint16_t code) const
  {
    if (code <= floor_) return code;
    for (unsigned i = n_; i-- > 0;)
      if (code_[i] < code) return code_[i];
    return floor_;
  }
  unsigned count() const { return n_; }
  uint16_t at(unsigned i) const { return code_[i]; }

 private:
  static constexpr unsigned kMax = 40;
  uint16_t code_[kMax] = {};
  unsigned n_ = 0;
  uint16_t floor_ = 0;
  uint16_t ceiling_ = kFullScale;
};

// ------------------------------------------------------------------------------------------------------------- log rate limit
// At most perWindow lines per window; the first line allowed after a window that dropped some reports how many through *dropped.
class LogLimit {
 public:
  LogLimit(unsigned perWindow, uint64_t windowMs) : per_(perWindow), window_(windowMs) {}
  bool allow(uint64_t nowMs, uint32_t *dropped)
  {
    *dropped = 0;
    if (!started_ || nowMs - start_ >= window_) {
      started_ = true;
      start_ = nowMs;
      used_ = 0;
    }
    if (used_ >= per_) {
      ++dropped_;
      return false;
    }
    ++used_;
    *dropped = dropped_;
    dropped_ = 0;
    return true;
  }

 private:
  unsigned per_;
  uint64_t window_, start_ = 0;
  unsigned used_ = 0;
  uint32_t dropped_ = 0;
  bool started_ = false;
};

// ------------------------------------------------------------------------------------------------------------- the controller
// The two ICs as the controller sees them: register reads and writes by IC index (0 primary, 1 secondary), each one I2C
// transaction; done() ends a burst (the kext gives the bus pins back to their QUP function there); the log and a millisecond clock.
class Ics {
 public:
  virtual bool read(unsigned ic, uint8_t reg, uint8_t *value) = 0;
  virtual bool write(unsigned ic, uint8_t reg, uint8_t value) = 0;
  virtual void done() = 0;
  virtual void log(const char *line) = 0;
  virtual uint64_t nowMs() = 0;

 protected:
  ~Ics() = default;
};

struct Config {
  uint16_t floor = 128;        // piano-backlight-min: the lowest code a step or a level may set (a lit panel stays visible)
  uint32_t bootPercent = 45;   // piano-backlight-level: a found level above this share of the range is lowered to it; 0: as found
  bool bootForce = false;      // piano-backlight-level given explicitly: set even when the found level is lower
  bool lidWatch = false;       // piano-backlight-lid=1: the cover's hall sensor closed -> backlight off
  bool readFlags = false;      // piano-backlight-flags=1: the survey reads FLAG (0x0f), which clears its latched faults
  bool autoAllowed = false;    // piano-backlight-auto=1: ambient targets taken (automatic brightness)
  bool android = false;        // use the bounded SDR range if both ICs support the reference mapping
  unsigned errorLimit = 8;     // consecutive failed accesses before control stops (the ICs stay as they are)
};

struct IcState {
  bool answered = false;              // every required survey read was acknowledged
  uint8_t reg[kRegCount] = {};
  bool have[kRegCount] = {};
  bool lit = false;                   // BL_EN as last read or written
  uint16_t code = 0;                  // the code as last read or written
  uint16_t foundCode = 0;
  bool foundLit = false;
  uint32_t codeReadbacks = 0, enableReadbacks = 0, errors = 0;
};

class Controller {
 public:
  explicit Controller(Ics &ics) : ics_(ics) {}
  void configure(const Config &config) { cfg_ = config; }

  // Level 1 and 2: both ICs read, nothing written. True when both answered as KTZ8866s.
  bool survey()
  {
    for (unsigned i = 0; i < kIcs; ++i) {
      IcState &s = ic_[i];
      s.answered = true;
      for (uint8_t reg : kSurveyRegs) readSurvey(i, reg);
      if (cfg_.readFlags) readSurvey(i, kRegFlag);
      s.foundCode = s.code = codeOf(s.reg[kRegBrtLsb], s.reg[kRegBrtMsb]);
      s.foundLit = s.lit = s.reg[kRegEn] & kEnBacklight;
    }
    ics_.done();
    for (unsigned i = 0; i < kIcs; ++i) describe(i);
    return ok(0) && ok(1);
  }

  // Level 2, after survey(): take over. False (logged) when the ICs are not both there: then nothing is ever written.
  bool start()
  {
    if (!ok(0) || !ok(1)) {
      if (cfg_.android) pwmContract("unqualified", "survey", "boot", ++pwmChecks_);
      note("control: OFF, %s did not answer as a KTZ8866 (both ICs are needed: one alone would light half the panel); read only",
           ok(0) ? "the secondary" : ok(1) ? "the primary" : "neither IC");
      return false;
    }
    const IcState &p = ic_[0], &q = ic_[1];
    exponential_ = !(p.reg[kRegCfg1] & kCfg1LinearMap);
    if ((p.reg[kRegCfg1] ^ q.reg[kRegCfg1]) & kCfg1LinearMap) note("the ICs map codes differently (BL_CFG1 0x%02x, 0x%02x): steps follow the primary's", p.reg[kRegCfg1], q.reg[kRegCfg1]);
    if (cfg_.android) {
      const bool compatible = androidCompatible();
      note("Android SDR %s: primary/secondary BL_CFG1 %#x/%#x sinks %#x/%#x IFS %#x/%#x current-limit %#x/%#x",
           compatible ? "supported" : "unsupported", p.reg[kRegCfg1], q.reg[kRegCfg1], p.reg[kRegEn] & kEnSinks,
           q.reg[kRegEn] & kEnSinks, p.reg[kRegIfs], q.reg[kRegIfs], p.reg[kRegOpt2] & 3, q.reg[kRegOpt2] & 3);
      if (compatible) {
        ceiling_ = PA_SDR_MAX_CODE;
        if (cfg_.floor < PA_SDR_MIN_CODE) cfg_.floor = PA_SDR_MIN_CODE;
      }
    }
    steps_.build(cfg_.floor, exponential_, ceiling_);
    ambient_.configure(cfg_.autoAllowed, cfg_.floor, exponential_, int(steps_.count()) - 1, ceiling_);
    code_ = p.foundCode;
    if (p.foundCode != q.foundCode) note("the ICs hold different codes (%u, %u): both get the primary's", p.foundCode, q.foundCode);
    const uint16_t target = cfg_.bootPercent ? levelCode(cfg_.bootPercent) : 0;
    if (!target)
      note("boot level: as found, code %u (%u %%)", code_, percentOf(code_));
    else if (cfg_.bootForce || code_ > target || code_ < cfg_.floor) {
      note("boot level: found code %u (%u %%), set %u (%u %%, piano-backlight-level%s)", code_, percentOf(code_), target,
           percentOf(uint16_t(target)), cfg_.bootForce ? "" : " default: a found level above it is lowered");
      code_ = uint16_t(target);
    } else {
      note("boot level: found code %u (%u %%), not above %u (%u %%): kept", code_, percentOf(code_), target, percentOf(uint16_t(target)));
    }
    if (ceiling_ != kFullScale) code_ = code_ < cfg_.floor ? cfg_.floor : code_ > ceiling_ ? ceiling_ : code_;
    if (!p.foundLit || !q.foundLit) note("found the backlight OFF (BL_EN 0x%02x, 0x%02x): it is turned on with code %u", p.reg[kRegEn], q.reg[kRegEn], code_);
    note("control: ON, %s mapping, %u steps from %u to %u, floor %u; display off -> BL_EN cleared, on -> code then BL_EN; lid %s",
         exponential_ ? "exponential" : "linear", steps_.count(), steps_.at(0), steps_.at(steps_.count() - 1), cfg_.floor,
         cfg_.lidWatch ? "closed -> off (piano-backlight-lid=1)" : "not acted on");
    control_ = true;
    apply();
    return true;
  }

  // NativeGopDisplay's display power (the system's and WindowServer's power states, on = both). An "off" before the first "on" is the
  // display not yet taken by WindowServer during boot: ignored, so the boot screen stays lit.
  void display(bool on, uint32_t system, uint32_t api)
  {
    if (!rawSeen_ || on != rawOn_ || system != rawSystem_ || api != rawApi_)
      note("display power: system %u, WindowServer %u: %s%s", system, api, on ? "on" : "off",
           !on && !armed_ ? " (before the first on: ignored, the boot screen stays lit)" : "");
    rawSeen_ = true;
    rawOn_ = on;
    rawSystem_ = system;
    rawApi_ = api;
    if (!on && !armed_) return;
    if (on) armed_ = true;
    displayOn_ = on;
    if (!on) ambient_.invalidate();
    apply();
  }
  // A system sleep (docs/power.md 4; setPowerState off / on): asleep -> BL_EN cleared on both ICs whatever the display says (the
  // code stays); awake -> back to what the display and the lid want (the newest code first, then BL_EN). HWEN is never touched.
  void sleep(bool asleep)
  {
    if (asleep == asleep_) return;
    asleep_ = asleep;
    if (asleep) ambient_.invalidate();
    else if (pwmFull_) pwmWakePending_ = true;
    note("system %s: %s", asleep ? "sleep" : "wake",
         asleep ? "the backlight off on both ICs (BL_EN cleared, the code kept; HWEN untouched)"
         : wantLit() ? "the backlight as before the sleep" : "the backlight stays off (the display or the lid wants it so)");
    apply();
  }
  bool asleep() const { return asleep_; }
  // The cover's hall sensor: 1 open, 0 closed, anything else unknown (treated as open).
  void lid(int state)
  {
    if (state == lid_) return;
    lid_ = state;
    if (cfg_.lidWatch && state == 0) ambient_.invalidate();
    note("lid %s%s", state == 0 ? "closed" : state == 1 ? "open" : "unknown", cfg_.lidWatch ? "" : " (not acted on: piano-backlight-lid=0)");
    if (cfg_.lidWatch) apply();
  }
  // Brightness keys: delta steps up (+) or down (-). In automatic mode they shift the curve instead (the offset, kept for the boot).
  void step(int delta, const char *source)
  {
    if (!control_) {
      limited("step %+d from %s: not controlling", delta, source);
      return;
    }
    if (ambient_.automatic()) {
      const int before = ambient_.offset();
      ambient_.step(delta);
      const uint16_t next = ambient_.code();
      limited("step %+d from %s in automatic mode: offset %d -> %d, code %u -> %u (%u %%)%s", delta, source, before, ambient_.offset(),
              code_, next, percentOf(next), next == code_ ? (delta > 0 ? " (full scale)" : " (the floor)")
              : wantLit() ? "" : " (applied when the backlight is on again)");
      code_ = next;
      apply();
      return;
    }
    uint16_t next = code_;
    for (int i = 0; i < delta; ++i) next = steps_.up(next);
    for (int i = 0; i > delta; --i) next = steps_.down(next);
    limited("step %+d from %s: code %u -> %u (%u %%)%s", delta, source, code_, next, percentOf(next),
            next == code_ ? (delta > 0 ? " (full scale)" : " (the floor)") : wantLit() ? "" : " (applied when the backlight is on again)");
    code_ = next;
    apply();
  }
  void percent(uint32_t p, const char *source)
  {
    const uint16_t next = levelCode(p);
    if (control_) limited("level %u %% from %s: code %u -> %u", p, source, code_, next);
    manualCode(next, source);
  }
  void manualCode(uint16_t next, const char *source)
  {
    if (!control_ || next < cfg_.floor || next > ceiling_) return;
    if (ambient_.automatic()) note("automatic brightness: off, a level was set by %s (piano-backlight-auto=1 as a property: on again)", source);
    ambient_.manual();
    code_ = next;
    apply();
  }
  // 2026-11-22 (AUTO-BRIGHT.md): the user's own level (the slider, reported by piano-brightness-sync). While automatic mode is
  // requested it stays on and the level becomes the curve's trim (AmbientBacklight.h adjust()); otherwise it is a manual level, as
  // manualCode(). True when automatic mode took it.
  bool userLevel(uint16_t next, const char *source)
  {
    if (!control_ || next < cfg_.floor || next > ceiling_) return false;
    if (!cfg_.autoAllowed || !ambient_.requested() || ambient_.faulted()) {
      manualCode(next, source);
      return false;
    }
    const int before = ambient_.trim();
    if (ambient_.adjust(next)) {
      const uint16_t code = ambient_.code();
      note("automatic brightness: the user's level from %s at %u lux: trim %d -> %d, code %u -> %u (%u %%; the curve's %u); "
           "automatic mode stays", source, ambient_.lastLux(), before, ambient_.trim(), code_, code, percentOf(code), ambient_.curveCode());
      code_ = code;
    } else {   // no target of this session yet: shown now, the trim is taken at the next target
      note("automatic brightness: the user's level from %s: code %u -> %u, its trim at the next target; automatic mode stays", source,
           code_, next);
      code_ = next;
    }
    apply();
    return true;
  }
  // A state the ICs did not take yet (an access failed): tried again (the kext calls this periodically, every second).
  void retry()
  {
    if (control_ && pending()) apply();
    if (summary_.targets && ics_.nowMs() - summary_.since >= kSummaryMs) summarize();
  }

  // A target of the ambient light policy (PianoBacklightAmbient, or PianoSensors' piano-sensors-light-target): permille of the code
  // range; -1 leaves automatic mode with the code as it is. False when not taken (piano-backlight-auto not set, or not controlling).
  // Its writes are not logged one by one: one line per kSummaryMs (failures still are).
  bool ambient(int32_t permille, uint32_t lux, const char *source)
  {
    if (!control_ || !cfg_.autoAllowed) return false;
    const bool was = ambient_.automatic();
    if (permille < 0) ambient_.invalidate(true);   // stream/ADSP fault, never a user request to disable auto
    if (permille >= 0 && !wantLit()) return false;
    if (!ambient_.target(permille, lux)) {
      if (was && !ambient_.automatic()) note("automatic brightness: off (%s left it): the code stays %u", source, code_);
      return true;
    }
    const uint16_t next = ambient_.code();
    if (!was)
      note("automatic brightness: on (%s): %u lux, target %d permille, offset %d, trim %d: code %u -> %u (%u %%)", source, lux, permille,
           ambient_.offset(), ambient_.trim(), code_, next, percentOf(next));
    if (!summary_.targets) {
      summary_ = Summary{};
      summary_.since = ics_.nowMs();
      summary_.from = code_;
      summary_.luxMin = summary_.luxMax = lux;
      summary_.minPermille = summary_.maxPermille = uint32_t(ambient_.lastTarget());
    }
    ++summary_.targets;
    summary_.luxMin = lux < summary_.luxMin ? lux : summary_.luxMin;
    summary_.luxMax = lux > summary_.luxMax ? lux : summary_.luxMax;
    const uint32_t t = uint32_t(ambient_.lastTarget());
    summary_.minPermille = t < summary_.minPermille ? t : summary_.minPermille;
    summary_.maxPermille = t > summary_.maxPermille ? t : summary_.maxPermille;
    if (next != code_ || pwmFull_) {
      code_ = next;
      quiet_ = true;
      apply();
      quiet_ = false;
    }
    if (ics_.nowMs() - summary_.since >= kSummaryMs) summarize();
    return true;
  }
  // piano-backlight-auto as a property: 0 leaves automatic mode, 1 requests it again (a fresh target, the offset kept).
  bool automatic(bool on, const char *source)
  {
    if (!control_ || !cfg_.autoAllowed) return false;
    const bool now = ambient_.setAutomatic(on);
    note("automatic brightness: %s by %s%s", on ? "on" : "off", source, on && !now ? " (from the next target)" : "");
    if (now) {
      code_ = ambient_.code();
      apply();
    }
    return true;
  }
  const pianoauto::Ambient &ambientState() const { return ambient_; }
  void invalidateAmbient(bool fault) { ambient_.invalidate(fault); }
  bool autoActive() const { return ambient_.automatic() && control_ && wantLit() && !pending(); }
  bool autoFault() const { return ambient_.faulted() || !control_; }

  bool controlling() const { return control_; }
  bool wantLit() const { return displayOn_ && !asleep_ && !(cfg_.lidWatch && lid_ == 0); }
  bool lit(unsigned i) const { return ic_[i].lit; }
  uint16_t code() const { return code_; }
  uint16_t floor() const { return cfg_.floor; }
  uint16_t ceiling() const { return ceiling_; }
  // Boot selects the schema. A later failed PWM witness only disqualifies the luminance receipt.
  bool pwmQualified() const { return pwmQualified_; }
  bool exponential() const { return exponential_; }
  const IcState &ic(unsigned i) const { return ic_[i]; }
  bool pending() const
  {
    for (const IcState &s : ic_)
      if (wantLit() ? (!s.lit || s.code != code_) : s.lit) return true;
    return false;
  }
  uint32_t errors() const { return errors_; }
  uint32_t writes() const { return writes_; }

 private:
  uint16_t levelCode(uint32_t percent) const
  {
    const uint16_t code = codeForPercent(percent, cfg_.floor);
    return code > ceiling_ ? ceiling_ : code;
  }
  bool ok(unsigned i) const { return ic_[i].answered && idIsKtz8866(ic_[i].reg[kRegId]); }
  void readSurvey(unsigned i, uint8_t reg)
  {
    IcState &s = ic_[i];
    uint8_t v = 0;
    s.have[reg] = s.answered && ics_.read(i, reg, &v);   // after a required read fails the IC is not asked again
    s.reg[reg] = v;
    // In the candidate mode this survey value is diagnostic. A fresh duty check at start gates SDR, not legacy control.
    const bool pwmDiagnostic = cfg_.android && s.reg[kRegCfg1] == kQualifiedPwmCfg1
        && (reg == kRegPwmLsb || reg == kRegPwmMsb);
    if (!s.have[reg] && !pwmDiagnostic) s.answered = false;
  }
  bool pwmContract(const char *contract, const char *reason, const char *context, unsigned check)
  {
    const bool supported = !reason;
    note("PianoBacklightPWMV1 contract=%s supported=%u reason=%s context=%s check=%u",
         contract, unsigned(supported), supported ? "ok" : reason, context, check);
    return supported;
  }
  bool androidCompatible()
  {
    const uint8_t *p = ic_[0].reg, *q = ic_[1].reg;
    if ((p[kRegCfg1] | q[kRegCfg1]) & kCfg1PwmEnable) {
      pwmFull_ = checkFullPwm("boot");
      ics_.done();
      return pwmFull_;
    }
    const bool exponential = !((p[kRegCfg1] | q[kRegCfg1]) & kCfg1LinearMap);
    const bool matchingCurrent = (p[kRegEn] & kEnSinks) == (q[kRegEn] & kEnSinks)
        && !((p[kRegIfs] ^ q[kRegIfs]) & 0xf8) && (p[kRegOpt2] & 3) == (q[kRegOpt2] & 3);
    return pwmContract("exp-pwm-off", !exponential ? "mode" : !matchingCurrent ? "current" : nullptr, "boot", ++pwmChecks_);
  }
  static uint16_t pwmCode(uint8_t lsb, uint8_t msb) { return uint16_t(lsb | ((msb & 7) << 8)); }
  bool readPwm(unsigned i, uint16_t &raw)
  {
    uint8_t lsb = 0, msb = 0;
    if (!ics_.read(i, kRegPwmLsb, &lsb) || !ics_.read(i, kRegPwmMsb, &msb)) return false;
    raw = pwmCode(lsb, msb);
    return true;
  }
  bool checkFullPwm(const char *context)
  {
    const unsigned check = ++pwmChecks_;
    bool valid = true, stable = true, full = true, mode = true, current = true;
    for (unsigned i = 0; i < kIcs; ++i) {
      const IcState &s = ic_[i];
      uint8_t cfg1 = 0, en = 0, ifs = 0, opt2 = 0;
      const bool configRead = ics_.read(i, kRegCfg1, &cfg1) && ics_.read(i, kRegEn, &en)
          && ics_.read(i, kRegIfs, &ifs) && ics_.read(i, kRegOpt2, &opt2);
      uint16_t raw = 0, again = 0;
      const bool first = readPwm(i, raw);
      const bool second = readPwm(i, again);
      const bool dutyRead = first && second;
      const bool same = dutyRead && raw == again;
      valid &= configRead && dutyRead;
      stable &= same;
      full &= raw == kFullScale;
      mode &= cfg1 == kQualifiedPwmCfg1;
      current &= (en & kEnSinks) == kEnSinks && ifs == kQualifiedPwmIfs && (opt2 & 3) == 3;
      note("PianoBacklightPWMV1 ic=%s cfg1=%02x pwm_raw=%04x duty_permille=%u stable=%u valid=%u context=%s check=%u pwm_again=%04x code=%u sinks=%02x ifs=%02x limit=%u",
           kIcName[i], cfg1, raw, unsigned(raw) * 1000 / kFullScale, unsigned(same), unsigned(configRead && dutyRead),
           context, check, again, s.code, en & kEnSinks, ifs, opt2 & 3);
    }
    const char *reason = !valid ? "pwm-read" : !mode ? "mode" : !current ? "current"
                       : !stable ? "pwm-unstable" : !full ? "pwm-not-full" : nullptr;
    return pwmContract("exp-pwm-full", reason, context, check);
  }
  bool access(unsigned i, bool okay)
  {
    if (!okay) { ++errors_; ++ic_[i].errors; }
    return okay;
  }
  // The code into one IC: LSB, MSB (which makes it take effect), both read back.
  bool writeCode(unsigned i, uint16_t code)
  {
    if (code < cfg_.floor || code > ceiling_) {
      note("refused: code %u outside %u..%u", code, cfg_.floor, ceiling_);
      return false;
    }
    const uint8_t lsb = uint8_t(code & 7), msb = uint8_t(code >> 3);
    uint8_t rl = 0, rm = 0;
    const bool wrote = access(i, ics_.write(i, kRegBrtLsb, lsb)) && access(i, ics_.write(i, kRegBrtMsb, msb));
    writes_ += wrote ? 2 : 1;
    const bool readBack = wrote && access(i, ics_.read(i, kRegBrtLsb, &rl)) && access(i, ics_.read(i, kRegBrtMsb, &rm));
    const bool good = readBack && codeOf(rl, rm) == code;
    if (!quiet_ || !good) {   // an ambient change that took is in the summary line instead
      limited("write %s: BL_BRT_LSB 0x04 = 0x%02x, BL_BRT_MSB 0x05 = 0x%02x: code %u (%u %%), was %u; %s", kIcName[i], lsb, msb, code,
              percentOf(code), ic_[i].code, !wrote ? "WRITE FAILED" : !readBack ? "read back failed" : good ? "read back ok" : "READ BACK DIFFERS");
    }
    if (readBack && !good) access(i, false);
    if (good) ++ic_[i].codeReadbacks;
    return good;
  }
  // BL_EN's bit 6 in one IC: read, bits 5:0 kept, bit 7 never set, written only when it changes, read back.
  bool writeEnable(unsigned i, bool on)
  {
    uint8_t found = 0, back = 0;
    if (!access(i, ics_.read(i, kRegEn, &found))) {
      limited("read %s: BL_EN failed (backlight %s not written)", kIcName[i], on ? "on" : "off");
      return false;
    }
    const uint8_t value = enableValue(found, on);
    if (value == found) { ic_[i].reg[kRegEn] = found; ++ic_[i].enableReadbacks; return true; }
    const bool wrote = access(i, ics_.write(i, kRegEn, value));
    ++writes_;
    const bool readBack = wrote && access(i, ics_.read(i, kRegEn, &back));
    const bool good = readBack && (back & kEnBacklight) == (value & kEnBacklight);
    limited("write %s: BL_EN 0x08 = 0x%02x (was 0x%02x): backlight %s, sinks 0x%02x kept; %s", kIcName[i], value, found, on ? "ON" : "OFF",
            found & kEnSinks, !wrote ? "WRITE FAILED" : !readBack ? "read back failed" : good ? "read back ok" : "READ BACK DIFFERS");
    if (readBack && !good) access(i, false);
    if (good) { ic_[i].reg[kRegEn] = back; ++ic_[i].enableReadbacks; }
    return good;
  }
  // The ICs toward the wanted state: lit -> the code first, then BL_EN; dark -> BL_EN cleared (the code stays for the next on).
  // An attempt that leaves an IC short of it counts; errorLimit such attempts in a row (retry() makes one a second) end control.
  void apply()
  {
    if (!control_) return;
    const bool want = wantLit();
    bool short_ = false;
    for (unsigned i = 0; i < kIcs; ++i) {
      IcState &s = ic_[i];
      if (want) {
        if (s.code != code_) {
          if (!writeCode(i, code_)) {
            short_ = true;
            continue;
          }
          s.code = code_;
        }
        if (!s.lit) {
          if (writeEnable(i, true)) s.lit = true;
          else short_ = true;
        }
      } else if (s.lit) {
        if (writeEnable(i, false)) s.lit = false;
        else short_ = true;
      }
    }
    if (pwmFull_ && want) {
      pwmQualified_ = !short_ && checkFullPwm(pwmWakePending_ ? "wake" : "apply");
      if (!short_) pwmWakePending_ = false;
    }
    ics_.done();
    if (!short_) {
      failedAttempts_ = 0;
    } else if (++failedAttempts_ >= cfg_.errorLimit) {
      control_ = false;
      note("control: OFF after %u failed attempts in a row: the ICs stay as they are", failedAttempts_);
    }
  }

  void describe(unsigned i)
  {
    const IcState &s = ic_[i];
    const uint8_t *r = s.reg;
    char line[400];
    if (!s.have[kRegId]) {
      note("survey %s: no answer at 0x11 (ID read failed)", kIcName[i]);
      return;
    }
    static constexpr uint16_t kOvp[8] = {214, 264, 315, 365, 239, 290, 340, 400};
    static constexpr uint16_t kCurrentRampMs[16] = {0, 2, 4, 8, 16, 32, 64, 128, 192, 256, 320, 384, 448, 512, 576, 640};
    static constexpr uint16_t kLimitA[4] = {12, 16, 20, 25};
    const uint8_t c1 = r[kRegCfg1], ramp = uint8_t((r[kRegCfg2] >> 3) & 15), ifs = uint8_t(r[kRegIfs] >> 3), f = r[kRegFlag];
    const unsigned duty = pwmCode(r[kRegPwmLsb], r[kRegPwmMsb]);
    note("survey %s: ID 0x%02x (%s, device field %u); BL_CFG1 0x%02x: OVP %u.%u V, %s, %s mapping, PWM input %s%s; BL_CFG2 0x%02x: "
         "current ramp %u %s; code %u (%u %%, LSB 0x%02x MSB 0x%02x); BL_EN 0x%02x: backlight %s, sinks 0x%02x%s",
         kIcName[i], r[kRegId], idIsKtz8866(r[kRegId]) ? "KTZ8866" : "NOT a KTZ8866", (r[kRegId] >> 3) & 7, c1, kOvp[c1 >> 5] / 10,
         kOvp[c1 >> 5] % 10, c1 & 0x10 ? "OVP turns the string off" : "OVP reported only", c1 & kCfg1LinearMap ? "linear" : "exponential",
         c1 & kCfg1PwmEnable ? "ON (active " : "off", c1 & kCfg1PwmEnable ? (c1 & 4 ? "low)" : "high)") : "", r[kRegCfg2],
         ramp ? kCurrentRampMs[ramp] : 1, ramp ? "ms" : "us", s.foundCode, percentOf(s.foundCode), r[kRegBrtLsb], r[kRegBrtMsb], r[kRegEn],
         r[kRegEn] & kEnBacklight ? "ON" : "OFF", r[kRegEn] & kEnSinks, s.answered ? "" : " (a later read failed)");
    line[0] = 0;
    if (s.have[kRegFlag])
      snprintf(line, sizeof line, "; FLAG 0x%02x%s%s%s%s%s%s (now cleared)", f, f & 0x80 ? " LED short" : "", f & 0x40 ? " thermal" : "",
               f & 0x08 ? " OUTP short" : "", f & 0x04 ? " OUTN short" : "", f & 0x02 ? " BL OVP" : "", f & 0x01 ? " BL OCP" : "");
    note("survey %s: LCD bias 0x09 0x%02x (LCD_EN %u, OUTP %u, OUTN %u, external enables %u), 0x0a 0x%02x, 0x0b 0x%02x, REG 0x0c 0x%02x, "
         "OUTP 0x0d 0x%02x, OUTN 0x0e 0x%02x (read only, never written); BL_OPTION1 0x%02x, BL_OPTION2 0x%02x (current limit %u.%u A); "
         "ramps 0x14 0x%02x (on %u us, off %u us); 0x15 0x%02x (full scale %u.%u mA per sink); PWM input code %u%s%s",
         kIcName[i], r[kRegLcd1], r[kRegLcd1] >> 7, (r[kRegLcd1] >> 2) & 1, (r[kRegLcd1] >> 1) & 1, r[kRegLcd1] & 1, r[kRegLcd2],
         r[kRegLcd3], r[kRegBoost], r[kRegOutp], r[kRegOutn], r[kRegOpt1], r[kRegOpt2], kLimitA[r[kRegOpt2] & 3] / 10,
         kLimitA[r[kRegOpt2] & 3] % 10, r[kRegRamp], rampUs(r[kRegRamp] >> 4), rampUs(r[kRegRamp] & 15), r[kRegIfs], (52 + 8 * ifs) / 10,
         (52 + 8 * ifs) % 10, duty, s.have[kRegPwmLsb] && s.have[kRegPwmMsb] ? "" : " (read incomplete)", line);
  }

  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ics_.log(line);
  }
  // Writes and steps: rate limited (keys held down or a fault must not flood the console).
  void limited(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    uint32_t dropped = 0;
    if (!limit_.allow(ics_.nowMs(), &dropped)) return;
    if (dropped) note("(%u write/step lines not logged)", dropped);
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ics_.log(line);
  }

  Ics &ics_;
  Config cfg_;
  IcState ic_[kIcs];
  Steps steps_;
  // Automatic brightness (autoAllowed only) and its log: one line per kSummaryMs while targets come.
  struct Summary {
    uint32_t targets = 0, luxMin = 0, luxMax = 0, minPermille = 0, maxPermille = 0;
    uint64_t since = 0;
    uint16_t from = 0;
  };
  static constexpr uint64_t kSummaryMs = 10000;
  void summarize()
  {
    note("ambient: %u targets in %llu s, lux %u..%u, target %u..%u permille, offset %d, trim %d, code %u -> %u (%u %%)%s", summary_.targets,
         (unsigned long long)((ics_.nowMs() - summary_.since + 500) / 1000), summary_.luxMin, summary_.luxMax, summary_.minPermille,
         summary_.maxPermille, ambient_.offset(), ambient_.trim(), summary_.from, code_, percentOf(code_),
         wantLit() ? "" : " (dark: applied at the next on)");
    summary_ = Summary{};
  }
  pianoauto::Ambient ambient_;
  Summary summary_;
  bool quiet_ = false;
  LogLimit limit_{24, 10000};
  bool control_ = false, exponential_ = false, armed_ = false, displayOn_ = true, asleep_ = false;
  bool rawSeen_ = false, rawOn_ = false;
  uint32_t rawSystem_ = 0, rawApi_ = 0;
  int lid_ = -1;
  uint16_t code_ = 0, ceiling_ = kFullScale;
  uint32_t errors_ = 0, failedAttempts_ = 0, writes_ = 0;
  bool pwmFull_ = false, pwmQualified_ = true, pwmWakePending_ = false;
  unsigned pwmChecks_ = 0;
};

}  // namespace pianobacklight
#endif
