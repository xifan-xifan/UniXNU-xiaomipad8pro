// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// PianoBacklight's automatic mode, for PianoBacklight's owner to take in (docs/drivers/sensors.md section 5a, "PianoBacklight").
// Header only, portable C++ as Ktz8866.h (no kernel header, no allocation, integer only); host-tested against PianoBacklight's own
// step ladder (tests/ambient_host.cpp). Nothing here touches an IC: it only says which 11-bit code the backlight should have.
//
// The targets come from PianoSensors' kext (AutoLight.h: SensorsPolicy's curve, smoothing and hysteresis; at most one per 250 ms,
// none at rest) through callPlatformFunction(kFunctionAmbient, false, (void *)(intptr_t)permille, (void *)source, (void *)(uintptr_t)
// lux, metadata). A target is permille of the code range (PsCurvePoint's unit: 1000 = 2047). The user's brightness keys keep working
// in automatic mode: each press shifts the curve by one of PianoBacklight's own steps (linear mapping: x 2^(1/3), three per doubling,
// the Steps ladder's factor; exponential mapping: 1/16 of the range), and the shift (the offset) stays for the rest of the boot, so
// "a little darker than automatic" holds as the room changes. A press always changes what is shown unless the code is at an end
// (the floor, full scale): an offset that the clamp at the floor would hide is skipped over.
//
// Modes: off (piano-backlight-auto not 1: targets are refused, PianoBacklight behaves as today); manual (allowed, but no target yet,
// the sensor left with -1, or the user chose a level: piano-backlight-percent, or piano-backlight-auto=0 as a property); automatic.
// A target enters automatic mode unless the user chose manual; the property piano-backlight-auto=1 requests it again (a fresh session target,
// the offset kept).
// 2026-11-22 (AUTO-BRIGHT.md): the user's own level while automatic mode is requested (the Control Center slider, which
// piano-brightness-sync reports) does not leave automatic mode: adjust() makes it the curve's trim at the current target, as macOS's
// CBAutoBrightnessModuleSKL userBrightnessCommitHandler: puts the user's nits at the current lux into its curve and stays enabled, and
// as Android's slider shifts its automatic curve. The trim is kept for the boot like the offset (display off/on, a sleep, a manual
// level and re-entry keep it), so "darker than automatic" holds as the room changes. Exponential mapping: codes added (the codes are
// perceptual: about a factor 3 in luminance per 350 codes on the bounded SDR range); linear mapping: the selected code / curve code.
//
// Integration in Ktz8866.h's Controller (three calls, PianoBacklight's owner):
//   pianoauto::Ambient ambient_;                         // start(): ambient_.configure(cfg_.autoAllowed, cfg_.floor, exponential_,
//                                                        //                            int(steps_.count()) - 1);
//   void ambient(int32_t permille, uint32_t lux) { if (control_ && ambient_.target(permille, lux)) { code_ = ambient_.code(); apply(); } }
//   step(delta, source): if (ambient_.step(delta)) { code_ = ambient_.code(); ...log...; apply(); return; }   // else as today
//   percent(p, source):  ambient_.manual(); ...as today
// apply() already writes only a changed code, and nothing while the backlight is off (display asleep, lid closed): the newest code
// is the one the wake lights.
#ifndef PIANO_AUTO_AMBIENT_BACKLIGHT_H
#define PIANO_AUTO_AMBIENT_BACKLIGHT_H
#include <stdint.h>

namespace pianoauto {

// The interface PianoBacklight adds (its owner mirrors these names in PianoBacklightClient.h).
constexpr const char *kFunctionAmbient = "PianoBacklightAmbient";   // p1 permille (intptr_t, -1 leaves), p2 source, p3 lux (uintptr_t)
constexpr const char *kArgAuto = "piano-backlight-auto";            // boot argument (1: targets taken) and setProperties key (0/1)
constexpr const char *kPropertyAuto = "piano-backlight-auto";       // published: automatic mode now (boolean)
constexpr const char *kPropertyRequested = "piano-backlight-auto-requested";
constexpr const char *kPropertyValid = "piano-backlight-ambient-valid";
constexpr const char *kPropertyOffset = "piano-backlight-offset";   // published: the user's shift in steps (signed)
constexpr const char *kPropertyTrim = "piano-backlight-trim";       // published: the user's slider trim (signed; codes or 1/1024)
constexpr const char *kPropertyTarget = "piano-backlight-ambient-permille";
constexpr const char *kPropertyLux = "piano-backlight-ambient-lux";
constexpr const char *kPropertyAmbientSnapshot = "piano-backlight-ambient";
constexpr uint16_t kAmbientFullScale = 2047;

// The synchronous platform-function entry point copies this metadata before it queues an update.
struct AmbientVersion {
  uint64_t seq;
  uint32_t revision, curve;

  bool newer(uint32_t expectedRevision, uint32_t expectedCurve, uint64_t taken) const
  {
    return revision == expectedRevision && curve == expectedCurve && seq > taken;
  }
};

enum class Mode : uint8_t { off, manual, automatic };

class Ambient {
 public:
  // allowed: piano-backlight-auto=1; floor: piano-backlight-min; exponential: BL_CFG1's mapping; maxOffset: the ladder's length - 1.
  void configure(bool allowed, uint16_t floor, bool exponential, int maxOffset, uint16_t ceiling = kAmbientFullScale)
  {
    allowed_ = allowed;
    floor_ = floor < 1 ? 1 : floor > kAmbientFullScale ? kAmbientFullScale : floor;
    ceiling_ = clampCode(ceiling, floor_);
    exponential_ = exponential;
    maxOffset_ = maxOffset < 0 ? 0 : maxOffset > 40 ? 40 : maxOffset;
    mode_ = allowed ? Mode::manual : Mode::off;
    offset_ = trim_ = queuedSteps_ = 0;
    ratioLevel_ = ratioCurve_ = 1;
    pending_ = 0;
    have_ = userManual_ = false;
    fault_ = false;
  }

  // PianoBacklightAmbient. True when the code should be applied now (code()); -1 leaves automatic mode with the code as it is.
  bool target(int32_t permille, uint32_t lux)
  {
    if (!allowed_ || fault_) return false;
    if (permille < 0) {
      have_ = false;
      mode_ = Mode::manual;
      return false;
    }
    permille_ = permille > 1000 ? 1000 : uint32_t(permille);
    lux_ = lux;
    ++targets_;
    if (userManual_) return false;
    have_ = true;
    mode_ = Mode::automatic;
    if (pending_) adjust(pending_);   // take the slider's trim from this session's first target
    step(queuedSteps_);
    queuedSteps_ = 0;
    return true;
  }

  // The user's level (a code) while automatic mode is requested: automatic mode stays and the level becomes the trim, so code() is
  // that level now (unless clamped) and follows the curve from there. True when code() is to be applied now; false when not requested
  // (the caller sets the level as a manual one) or when no target of this session is there yet (the level is kept and the trim is
  // taken at the next target; the caller shows the level meanwhile).
  bool adjust(uint16_t user)
  {
    if (!requested() || fault_) return false;
    const uint16_t level = clampCode(user > ceiling_ ? ceiling_ : user, floor_);
    if (!have_) {
      pending_ = level;
      queuedSteps_ = 0;
      return false;
    }
    pending_ = 0;
    const uint16_t curve = curveCode();
    if (exponential_) {
      trim_ = int(level) - int(curve);
    } else {
      ratioLevel_ = level;
      ratioCurve_ = curve;
      trim_ = int((uint32_t(level) * 1024 + curve / 2) / curve) - 1024;
    }
    return true;
  }
  int trim() const { return trim_; }
  uint16_t pendingLevel() const { return pending_; }
  void deferSteps(int delta)
  {
    if (!requested() || fault_ || have_) return;
    const int64_t steps = int64_t(queuedSteps_) + delta;
    queuedSteps_ = steps < -maxOffset_ ? -maxOffset_ : steps > maxOffset_ ? maxOffset_ : int(steps);
  }

  // A brightness step (keys, piano-backlight-step) in automatic mode: true when taken here (code() is the new code; it may be the old
  // one at an end); false when not automatic (the caller steps as it does today).
  bool step(int delta)
  {
    if (mode_ != Mode::automatic) return false;
    for (; delta > 0; --delta)
      if (!nudge(+1)) break;
    for (; delta < 0; ++delta)
      if (!nudge(-1)) break;
    return true;
  }

  // An explicit level from an administrator (piano-backlight-percent): manual until piano-backlight-auto=1 is set again.
  void manual()
  {
    if (!allowed_) return;
    userManual_ = true;
    pending_ = 0;
    queuedSteps_ = 0;
    invalidate();
  }

  // The property piano-backlight-auto: 0 leaves (manual), 1 requests it again. A manual transition invalidates cached targets.
  bool setAutomatic(bool on)
  {
    if (!allowed_) return false;
    userManual_ = !on;
    if (!on) {
      pending_ = 0;
      queuedSteps_ = 0;
      invalidate();
      return false;
    }
    if (!have_) return false;                // the next target enters it
    mode_ = Mode::automatic;
    return true;
  }

  Mode mode() const { return mode_; }
  bool requested() const { return allowed_ && !userManual_; }
  bool valid() const { return have_; }
  bool faulted() const { return fault_; }
  // Session invalidation preserves the owner's request and offset. A stream/guard fault cannot be cleared by auto=1.
  void invalidate(bool fault = false)
  {
    have_ = false;
    fault_ = fault_ || fault;
    mode_ = allowed_ ? Mode::manual : Mode::off;
  }
  bool automatic() const { return mode_ == Mode::automatic; }
  int offset() const { return offset_; }
  int32_t lastTarget() const { return have_ ? int32_t(permille_) : -1; }
  uint32_t lastLux() const { return lux_; }
  uint32_t targets() const { return targets_; }
  // The curve's code for the last target, shifted by the offset (the keys): floor..ceiling.
  uint16_t curveCode() const
  {
    const uint16_t target = codeForPermille(have_ ? permille_ : 0, floor_);
    return shift(target > ceiling_ ? ceiling_ : target, offset_, floor_, exponential_, ceiling_);
  }
  // What is applied: the curve's code with the user's trim: floor..ceiling.
  uint16_t code() const
  {
    const uint16_t curve = curveCode();
    if (exponential_) return trimmed(curve, trim_, floor_, true, ceiling_);
    const uint32_t level = (uint32_t(curve) * ratioLevel_ + ratioCurve_ / 2) / ratioCurve_;
    return clampCode(level > ceiling_ ? ceiling_ : level, floor_);
  }

  static uint16_t trimmed(uint16_t code, int trim, uint16_t floor, bool exponential, uint16_t ceiling = kAmbientFullScale)
  {
    const int64_t v = exponential ? int64_t(code) + trim : (int64_t(code) * (1024 + int64_t(trim)) + 512) / 1024;
    return clampCode(v > ceiling ? ceiling : v, floor);
  }

  static uint16_t clampCode(int64_t v, uint16_t floor)
  {
    return uint16_t(v >= kAmbientFullScale || floor >= kAmbientFullScale ? kAmbientFullScale : v < floor ? floor : v);
  }
  // permille of the range (1000 = full scale), never below the floor.
  static uint16_t codeForPermille(uint32_t permille, uint16_t floor)
  {
    const uint32_t p = permille > 1000 ? 1000 : permille;
    return clampCode((int64_t(p) * kAmbientFullScale + 500) / 1000, floor);
  }
  // code shifted by offset steps: linear mapping x 2^(offset/3) (Ktz8866.h Steps' factors, the same rounding), exponential mapping
  // + offset/16 of the range above the floor; clamped to floor..2047.
  static uint16_t shift(uint16_t code, int offset, uint16_t floor, bool exponential, uint16_t ceiling = kAmbientFullScale)
  {
    offset = offset < -40 ? -40 : offset > 40 ? 40 : offset;
    const uint32_t k = uint32_t(offset < 0 ? -offset : offset);
    if (exponential) {
      const int64_t step = (int64_t(k) * (ceiling - floor) + 8) / 16;
      const int64_t next = offset < 0 ? int64_t(code) - step : int64_t(code) + step;
      return clampCode(next > ceiling ? ceiling : next, floor);
    }
    static constexpr uint64_t kCubeRoots[3] = {1000000, 1259921, 1587401};   // 2^(0/3), 2^(1/3), 2^(2/3) x 10^6
    const uint64_t factor = kCubeRoots[k % 3] << (k / 3);                      // k <= 40: below 2^35
    const uint64_t v = offset < 0 ? (uint64_t(code) * 1000000 + factor / 2) / factor : (uint64_t(code) * factor + 500000) / 1000000;
    return clampCode(int64_t(v > ceiling ? ceiling : v), floor);
  }

 private:
  // One press: the next offset that shows a different code; false (the offset unchanged) at an end.
  bool nudge(int direction)
  {
    const uint16_t before = code();
    const int start = offset_;
    while (direction > 0 ? offset_ < maxOffset_ : offset_ > -maxOffset_) {
      offset_ += direction;
      if (code() != before) return true;
    }
    offset_ = start;
    return false;
  }

  bool allowed_ = false, exponential_ = false, have_ = false, userManual_ = false, fault_ = false;
  Mode mode_ = Mode::off;
  uint16_t floor_ = 128, ceiling_ = kAmbientFullScale;
  uint16_t pending_ = 0;   // the user's level waiting for this session's first target (0: none)
  uint16_t ratioLevel_ = 1, ratioCurve_ = 1;
  int maxOffset_ = 12, offset_ = 0, trim_ = 0, queuedSteps_ = 0;
  uint32_t permille_ = 0, lux_ = 0, targets_ = 0;
};

}  // namespace pianoauto
#endif
