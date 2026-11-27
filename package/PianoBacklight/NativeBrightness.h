// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H
#define PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H

#include "Ktz8866.h"
#include "../PianoAuto/AndroidBrightness.h"

namespace pianobacklight {

constexpr char kNativeMarker[] = "PianoBacklightNativeV1";
constexpr char kNativeIdentity[] = "com.apple.WindowServer";
constexpr char kNativeRequest[] = "brightness-nits";
constexpr char kAndroidMarker[] = "PianoBacklightAndroidV1";
constexpr char kReferenceProperty[] = "piano-backlight-reference";
constexpr uint32_t kNativeApplyIntervalMs = 50;
// 2026-11-20 (BRIGHTNESS-SLIDER.md): BrightnessControl's -[BCAppleBacklightBrtControl setNits:error:] works for a non-DFR panel
// only when this registry property (OSData, first byte non-zero) is on the backlight service or a parent.
constexpr char kNativeOverride[] = "force-brightness-control";

inline bool nativeWriter(bool administrator, bool platform, const char *identity)
{
  if (administrator) return true;
  if (!platform || !identity) return false;
  for (unsigned i = 0; ; ++i) {
    if (identity[i] != kNativeIdentity[i]) return false;
    if (!kNativeIdentity[i]) return true;
  }
}

// 2026-11-21 (SLIDER-INIT.md): CoreBrightness never reads the panel back (its level starts from its BrightnessLevelNits preference
// and changes only by its own commits), so piano-brightness-sync gives it the automatic level through DisplayServicesClient's
// DisplayNitsKey. The daemon announces each such sync first (root, this property, signed 16.16 nits); the commit that follows is
// CoreBrightness taking the panel's own level, not the user's move.
constexpr char kNativeSync[] = "piano-backlight-sync";
// 2026-11-21 (SLIDER-INIT.md round 2): WindowServer's swap brightness is CoreBrightness's SDR level times the EDR headroom
// (installed 14:07:31: sdr 431.949 x headroom 1.04185 = 450.025), and CoreBrightness's restore and EDR ramps arrive there like a
// slider move: both runs ended automatic mode that way. While piano-brightness-sync holds this lease (root, seconds, 0 ends it), the
// stream cannot adjust the automatic curve; the daemon watches CoreBrightness's own SDR level (DisplayNitsKey, untouched by EDR) and
// reports the user's move with kNativeMoved (16.16: that level; since 2026-11-22 the curve's trim while automatic, see streamChoice).
// The newest request is published (kNativeRequested) so the daemon wakes on every commit that reaches the panel.
constexpr char kNativeManaged[] = "piano-backlight-managed";
constexpr char kNativeMoved[] = "piano-backlight-moved";
constexpr char kNativeRequested[] = "piano-backlight-native-requested";
constexpr uint32_t kManagedMaxS = 600;

class Lease {
 public:
  void set(uint32_t seconds, uint64_t nowMs) { until_ = seconds ? nowMs + uint64_t(seconds > kManagedMaxS ? kManagedMaxS : seconds) * 1000 : 0; }
  bool active(uint64_t nowMs) const { return nowMs < until_; }

 private:
  uint64_t until_ = 0;
};

// 2026-11-20 (BRIGHTNESS-SLIDER.md): WindowServer sends the panel's brightness with every swap that changes it, CoreBrightness's
// own ramps (the power-on ramp, a dim) included. A request is the user's move only when it leaves the settled value by more
// than 2 % after the settle time (from the first request, a display power-on or a wake); the rest follow.
// 2026-11-21: after an announced sync (expect()), requests follow until one within 3 % of the announced level arrives (the echo:
// the window closes there) or kExpectMs pass (no echo: counted as missed).
class NativeIntent {
 public:
  static constexpr uint64_t kSettleMs = 4000;
  static constexpr uint64_t kExpectMs = 600;
  enum Kind : uint8_t { kFollow = 0, kUserMove = 1 };
  void reset(uint64_t nowMs)
  {
    anchor_ = -1;
    settleUntil_ = nowMs + kSettleMs;
    armed_ = true;
  }
  void expect(int64_t fixed, uint64_t nowMs)
  {
    if (expect_ >= 0) ++missed_;   // a newer sync replaces one whose echo never came
    expect_ = fixed;
    expectUntil_ = nowMs + kExpectMs;
    ++syncs_;
  }
  Kind classify(int64_t fixed, uint64_t nowMs)
  {
    if (!armed_) reset(nowMs);   // the first request ever starts the settle time
    if (expect_ >= 0 && nowMs < expectUntil_) {
      anchor_ = fixed;
      const int64_t e = fixed > expect_ ? fixed - expect_ : expect_ - fixed;
      if (e * 100 <= expect_ * 3) {
        expect_ = -1;
        ++echoes_;
      }
      return kFollow;
    }
    if (expect_ >= 0) {
      expect_ = -1;
      ++missed_;
    }
    if (anchor_ < 0 || nowMs < settleUntil_) {
      anchor_ = fixed;
      return kFollow;
    }
    const int64_t d = fixed > anchor_ ? fixed - anchor_ : anchor_ - fixed;
    if (d * 50 <= anchor_) return kFollow;
    anchor_ = fixed;
    return kUserMove;
  }
  // piano-brightness-sync reported the user's move to this level: the stream goes on from there.
  void moved(int64_t fixed)
  {
    anchor_ = fixed;
    expect_ = -1;
  }
  uint32_t syncs() const { return syncs_; }
  uint32_t echoes() const { return echoes_; }
  uint32_t missed() const { return missed_; }

 private:
  int64_t anchor_ = -1, expect_ = -1;
  uint64_t settleUntil_ = 0, expectUntil_ = 0;
  uint32_t syncs_ = 0, echoes_ = 0, missed_ = 0;
  bool armed_ = false;
};

// Stream changes classified as user moves adjust the curve when the daemon has no lease.
enum class StreamChoice { follow, manualLevel, automaticTrim };
inline StreamChoice streamChoice(NativeIntent::Kind intent, bool owns, bool managed)
{
  if (!owns) return StreamChoice::manualLevel;
  if (intent == NativeIntent::kUserMove && !managed) return StreamChoice::automaticTrim;
  return StreamChoice::follow;
}

// 2026-11-21: an IEEE-754 double (the swap record's nits) as signed 16.16 without floating point (kernel code): 0 for a negative,
// zero, NaN or infinite value, saturated at INT32_MAX.
inline int32_t nitsFixedFromDouble(uint64_t bits)
{
  const uint32_t exponent = uint32_t(bits >> 52) & 0x7ffu;
  if ((bits >> 63) || exponent == 0 || exponent == 0x7ffu) return 0;
  const uint64_t mantissa = (bits & ((uint64_t(1) << 52) - 1)) | (uint64_t(1) << 52);
  const int shift = int(exponent) - 1075 + 16;   // value * 65536 = mantissa * 2^(exponent - 1075 + 16)
  if (shift >= 0) return shift > 10 ? INT32_MAX : (mantissa << shift) > uint64_t(INT32_MAX) ? INT32_MAX : int32_t(mantissa << shift);
  return shift <= -64 ? 0 : (mantissa >> -shift) > uint64_t(INT32_MAX) ? INT32_MAX : int32_t(mantissa >> -shift);
}

// Nominal SDR slider scale. The endpoints model the safe code range, not a panel luminance measurement.
class NativeBrightness {
 public:
  explicit NativeBrightness(uint16_t floor, uint16_t max = kFullScale, bool reference = false)
    : floor_(floor), max_(max), reference_(reference) {}

  bool bounded() const { return max_ == PA_SDR_MAX_CODE; }
  uint32_t initialRevision() const { return bounded() ? 1 : 0; }
  uint32_t minFixed() const
  {
    return bounded() ? pa_sdr_floor_fixed(floor_) : ((500u * floor_ + kFullScale / 2) / kFullScale) << 16;
  }
  uint32_t maxFixed() const { return (bounded() ? 600u : 500u) << 16; }
  uint32_t midFixed() const { return bounded() ? (minFixed() + maxFixed()) / 2 : ((minNits() + 500u) / 2) << 16; }
  uint32_t minNits() const { return (minFixed() + 32768) >> 16; }
  uint32_t midNits() const { return (midFixed() + 32768) >> 16; }

  bool accepts(uint64_t fixed) const { return fixed >= minFixed() && fixed <= maxFixed(); }

  bool decode(uint64_t fixed, uint16_t &code, uint16_t *dbv = nullptr) const
  {
    if (!accepts(fixed)) return false;  // includes negative signed 16.16 values
    if (dbv) *dbv = bounded() ? pa_sdr_dbv(uint32_t(fixed)) : 0;
    if (reference_) {
      const uint16_t target = pa_sdr_code(uint32_t(fixed));
      code = fixed == minFixed() || target < floor_ ? floor_ : target;
    } else {
      const uint32_t lo = minFixed(), span = maxFixed() - lo;
      code = uint16_t(floor_ + ((fixed - lo) * (max_ - floor_) + span / 2) / span);
    }
    return true;
  }

  uint32_t milliNits(uint16_t code) const
  {
    if (bounded()) {
      if (code <= floor_) return pa_sdr_millinits(minFixed());
      if (code >= max_) return 600000;
      if (reference_) return pa_sdr_millinits(pa_sdr_fixed_for_code(code));
      return pa_sdr_millinits(pa_sdr_linear_fixed(floor_, code));
    }
    const uint32_t span = max_ - floor_;
    const uint32_t delta = code > floor_ ? code - floor_ : 0;
    return 1000 * minNits() + (1000 * (500u - minNits()) * delta + span / 2) / span;
  }

  bool applied(const Controller &ctl, uint16_t &code) const
  {
    if (!ctl.controlling() || !ctl.wantLit() || ctl.pending() || ctl.code() < floor_ || ctl.code() > max_) return false;
    code = ctl.code();  // pending() checks the last verified code and BL_EN on both ICs
    return true;
  }

  // The sink supplies typed properties. The same schema is exercised with a recording sink on the host.
  template <typename Sink> bool parameters(Sink &sink, uint16_t code) const
  {
    return sink.range("brightness", floor_, max_, code)
        && sink.range("BrightnessMilliNits", milliNits(floor_), maxFixed() / 65536 * 1000, milliNits(code))
        && sink.commit("IODisplayParameters");
  }

  template <typename Sink> bool calibration(Sink &sink) const
  {
    const uint32_t table[] = {minFixed(), midFixed(), maxFixed()};
    return sink.number("lMinProduct", table[0]) && sink.number("lMidProduct", table[1])
        && sink.number("lMaxProduct", table[2]) && sink.number("lMinPanel", table[0])
        && sink.number("lMidPanel", table[1]) && sink.number("lMaxPanel", table[2])
        && sink.commit("backlight-calibration-parameters")
        && sink.data("backlight-marketing-table", table, sizeof(table))
        && sink.data("LmaxProduct", &table[2], sizeof(table[2]))
        && sink.scalar("LminProduct", minNits()) && sink.scalar("LmidProduct", midNits());
  }

 private:
  uint16_t floor_, max_;
  bool reference_;
};

// The ALS consumer takes these fields together when selecting a curve and starting a fresh session.
template <typename Sink> bool publishAmbientSnapshot(Sink &sink, const Controller &ctl, uint32_t revision, bool reference)
{
  const pianoauto::Ambient &a = ctl.ambientState();
  return revision && sink.number("curve", reference) && sink.number("revision", revision)
      && sink.number("floor", ctl.floor()) && sink.number("target", uint32_t(a.lastTarget()))
      && sink.number("lux", a.lastLux()) && sink.number("valid", a.valid())
      && sink.number("fault", ctl.autoFault()) && sink.commit(pianoauto::kPropertyAmbientSnapshot);
}

}  // namespace pianobacklight
#endif
