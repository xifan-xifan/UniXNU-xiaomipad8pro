// SPDX-License-Identifier: BSD-2-Clause-Patent
// D0 single-sink, 8b/10b DisplayPort link training and stream arithmetic.
#pragma once
#include "DpEdid.h"

namespace pianodp {

enum class DpAuxStatus : uint8_t { Ack, Nack, Timeout, Defer, ShortTransfer, IoError };
enum class DpTrainingPattern : uint8_t { Disabled, Tps1, Tps2, Tps3, Tps4 };
enum class DpDownstreamPort : uint8_t { None, DisplayPort, Analog, Tmds, Other };

struct DpReceiverCaps {
  uint8_t revision = 0;
  uint32_t maxRateKhz = 0;
  unsigned maxLanes = 0;
  unsigned crDelayUs = 0, eqDelayUs = 0;
  bool enhancedFraming = false, downspread = false;
  DpTrainingPattern eqPattern = DpTrainingPattern::Tps2;
  DpDownstreamPort downstream = DpDownstreamPort::None;
  uint8_t downstreamPorts = 0;
  bool detailedPortCaps = false;
  bool edpAlternateScrambler = false, edpFramingChange = false, edpDisplayControl = false, edpMsaIgnored = false;
  bool extendedCapsPresent = false;

  constexpr bool hasSinkCount() const { return revision >= 0x11 && downstream != DpDownstreamPort::None; }
};

constexpr size_t kDpReceiverCapBytes = 16;

inline bool parseDpReceiverCaps(const uint8_t *dpcd, size_t length, DpReceiverCaps &caps)
{
  caps = {};
  if (!dpcd || length < kDpReceiverCapBytes) return false;
  const uint8_t revision = dpcd[0];
  if (revision < 0x10 || revision >= 0x30 || (revision & 15) > 9) return false;
  if ((dpcd[6] & 3) == 2) return false; // receiver advertises only 128b/132b coding
  uint32_t rate;
  switch (dpcd[1]) {
    case 0x06: rate = kDpRbrKhz; break;
    case 0x0a: rate = kDpHbrKhz; break;
    case 0x14: rate = kDpHbr2Khz; break;
    case 0x1e: rate = kDpHbr3Khz; break;
    default: return false;
  }
  const unsigned lanes = dpcd[2] & 0x1f;
  const unsigned interval = dpcd[14] & 0x7f;
  if (!validDpLaneCount(lanes) || interval > 4) return false;
  caps.revision = revision;
  caps.maxRateKhz = rate;
  caps.maxLanes = lanes;
  caps.crDelayUs = caps.revision >= 0x14 || !interval ? 100 : interval * 4000;
  caps.eqDelayUs = interval ? interval * 4000 : 400;
  caps.enhancedFraming = dpcd[2] & 0x80;
  caps.downspread = dpcd[3] & 1;
  if (caps.revision >= 0x14 && (dpcd[3] & 0x80)) caps.eqPattern = DpTrainingPattern::Tps4;
  else if (caps.revision >= 0x12 && (dpcd[2] & 0x40)) caps.eqPattern = DpTrainingPattern::Tps3;
  if (dpcd[5] & 1) {
    caps.downstream = DpDownstreamPort(1 + ((dpcd[5] >> 1) & 3));
    caps.downstreamPorts = dpcd[7] & 15;
    caps.detailedPortCaps = dpcd[5] & 0x10;
  }
  caps.edpAlternateScrambler = dpcd[13] & 1;
  caps.edpFramingChange = dpcd[13] & 2;
  caps.edpDisplayControl = dpcd[13] & 8;
  caps.edpMsaIgnored = dpcd[7] & 0x40;
  caps.extendedCapsPresent = dpcd[14] & 0x80;
  return true;
}

struct DpDrive {
  unsigned swing = 0, preemphasis = 0;
  constexpr bool operator==(const DpDrive &) const = default;
};

// QMP PHY table indices: row = swing, column = pre-emphasis.
constexpr DpDrive clampDpDrive(unsigned swing, unsigned preemphasis)
{
  if (swing > 3) swing = 3;
  if (preemphasis > 3 - swing) preemphasis = 3 - swing;
  return {swing, preemphasis};
}

enum class DpTrainStatus : uint8_t {
  Trained, InvalidPinLanes, InvalidCaps, AuxError, PhyError, ClockRecoveryFailed, ChannelEqualizationFailed,
};

struct DpTrainResult {
  DpTrainStatus status;
  DpAuxStatus auxStatus = DpAuxStatus::Ack;
  DpLink link;   // last attempted configuration
  unsigned attempts = 0;
  bool enhancedFraming = false;
  bool downspread = false;
};

namespace dp_train_detail {

constexpr uint32_t kReceiverCaps = 0x000, kExtendedReceiverCaps = 0x2200;
constexpr uint32_t kLinkConfig = 0x100, kPatternSet = 0x102, kLaneSet = 0x103;
constexpr uint32_t kDownspreadCtrl = 0x107, kLinkStatus = 0x202;
constexpr unsigned kCrDone = 1, kEqDone = 7, kAlignDone = 1;
constexpr unsigned kCrTriesPerSwing = 4, kEqTries = 6;

template <typename Io>
bool readReceiverCaps(Io &io, DpReceiverCaps &caps, DpAuxStatus &aux)
{
  caps = {};
  uint8_t base[kDpReceiverCapBytes]{};
  aux = io.auxRead(kReceiverCaps, base, sizeof base);
  if (aux != DpAuxStatus::Ack) return false;
  // The base revision can be older than the actual receiver revision.
  if (base[14] & 0x80) {
    uint8_t extended[kDpReceiverCapBytes]{};
    aux = io.auxRead(kExtendedReceiverCaps, extended, sizeof extended);
    if (aux != DpAuxStatus::Ack) return false;
    if (extended[0] >= base[0]) {
      const bool valid = parseDpReceiverCaps(extended, sizeof extended, caps);
      caps.extendedCapsPresent = valid;
      return valid;
    }
  }
  return parseDpReceiverCaps(base, sizeof base, caps);
}

constexpr uint8_t dpcdPattern(DpTrainingPattern pattern)
{
  if (pattern == DpTrainingPattern::Disabled) return 0;
  if (pattern == DpTrainingPattern::Tps4) return 7; // DPCD code, scrambling enabled
  return uint8_t(0x20 | uint8_t(pattern));          // TPS1–3 disable scrambling
}

constexpr uint8_t dpcdLaneSet(DpDrive drive)
{
  return uint8_t(drive.swing | (drive.swing == 3 ? 0x04 : 0) |
                 (drive.preemphasis << 3) | (drive.preemphasis == 3 - drive.swing ? 0x20 : 0));
}

inline bool allLanesHaveStatus(const uint8_t *status, unsigned lanes, unsigned bits)
{
  if (!validDpLaneCount(lanes)) return false;
  for (unsigned lane = 0; lane < lanes; ++lane)
    if (((status[lane / 2] >> (4 * (lane % 2))) & bits) != bits) return false;
  return true;
}

inline bool linkEqualized(const uint8_t *status, unsigned lanes)
{
  return (status[2] & kAlignDone) && allLanesHaveStatus(status, lanes, kEqDone);
}

template <typename Io>
struct TrainAttempt {
  Io &io;
  DpTrainResult &result;
  const DpReceiverCaps &caps;
  DpDrive drive{};

  bool write(uint32_t addr, const uint8_t *data, size_t size)
  {
    result.auxStatus = io.auxWrite(addr, data, size);
    return result.auxStatus == DpAuxStatus::Ack;
  }

  bool setDrive()
  {
    return io.setPhy(result.link.rateKhz, result.link.lanes, drive.swing, drive.preemphasis);
  }

  DpTrainStatus adjustDrive(DpDrive next)
  {
    if (next == drive) return DpTrainStatus::Trained;
    drive = next;
    if (!setDrive()) return DpTrainStatus::PhyError;
    const uint8_t lane = dpcdLaneSet(drive);
    const uint8_t settings[] = {lane, lane, lane, lane};
    return write(kLaneSet, settings, result.link.lanes) ? DpTrainStatus::Trained : DpTrainStatus::AuxError;
  }

  DpTrainStatus phase(DpTrainingPattern pattern)
  {
    const bool cr = pattern == DpTrainingPattern::Tps1;
    io.setPattern(pattern);
    const uint8_t lane = dpcdLaneSet(drive);
    const uint8_t settings[] = {dpcdPattern(pattern), lane, lane, lane, lane};
    if (!write(kPatternSet, settings, 1 + result.link.lanes)) return DpTrainStatus::AuxError;

    // The total CR poll bound also handles a sink that oscillates between swing requests.
    const unsigned tries = cr ? 4 * kCrTriesPerSwing : kEqTries;
    unsigned sameSwingTries = 0;
    for (unsigned i = 0; i < tries; ++i) {
      uint8_t status[6]{};
      io.delayUs(cr ? caps.crDelayUs : caps.eqDelayUs);
      result.auxStatus = io.auxRead(kLinkStatus, status, sizeof status);
      if (result.auxStatus != DpAuxStatus::Ack) return DpTrainStatus::AuxError;

      const bool crDone = allLanesHaveStatus(status, result.link.lanes, kCrDone);
      if (cr && crDone) return DpTrainStatus::Trained;
      if (!cr && !crDone) return DpTrainStatus::ClockRecoveryFailed;
      if (!cr && linkEqualized(status, result.link.lanes)) return DpTrainStatus::Trained;

      // As on piano's reference path, apply lane 0's drive request to every active lane.
      const DpDrive next = clampDpDrive(status[4] & 3, (status[4] >> 2) & 3);
      if (cr) {
        if (drive.swing == 3) break;
        if (next.swing != drive.swing) sameSwingTries = 0;
        else if (++sameSwingTries == kCrTriesPerSwing) break;
      }
      if (i + 1 == tries) break;
      const DpTrainStatus adjusted = adjustDrive(next);
      if (adjusted != DpTrainStatus::Trained) return adjusted;
    }
    return cr ? DpTrainStatus::ClockRecoveryFailed : DpTrainStatus::ChannelEqualizationFailed;
  }

  DpTrainStatus run()
  {
    if (!setDrive()) return DpTrainStatus::PhyError;
    const uint8_t linkConfig[] = {uint8_t(result.link.rateKhz / 27000),
                                  uint8_t(result.link.lanes | (caps.enhancedFraming ? 0x80 : 0))};
    const uint8_t coding[] = {uint8_t(caps.downspread ? 0x10 : 0), 1}; // 0.5% spread, 8b/10b coding
    if (!write(kLinkConfig, linkConfig, sizeof linkConfig) || !write(kDownspreadCtrl, coding, sizeof coding))
      return DpTrainStatus::AuxError;

    DpTrainStatus status = phase(DpTrainingPattern::Tps1);
    if (status == DpTrainStatus::Trained) status = phase(caps.eqPattern);

    // A failed AUX transaction may already have reached the sink. Always retire
    // the source pattern and try to retire the sink pattern, preserving the first AUX error.
    const uint8_t disabled = 0;
    const DpAuxStatus stopped = io.auxWrite(kPatternSet, &disabled, 1);
    io.setPattern(DpTrainingPattern::Disabled);
    if (status != DpTrainStatus::AuxError && stopped != DpAuxStatus::Ack) {
      result.auxStatus = stopped;
      return DpTrainStatus::AuxError;
    }
    return status;
  }
};

template <typename Io>
DpTrainResult trainWithCaps(Io &io, unsigned pinLanes, const DpReceiverCaps &caps)
{
  DpTrainResult result{.status = DpTrainStatus::InvalidCaps, .link = {},
                       .enhancedFraming = caps.enhancedFraming, .downspread = caps.downspread};
  constexpr uint32_t rates[] = {kDpHbr2Khz, kDpHbrKhz, kDpRbrKhz};
  static_assert(rates[0] == kPianoDpMaxRateKhz);
  const unsigned maxLanes = pinLanes < caps.maxLanes ? pinLanes : caps.maxLanes;
  // Check lower rates before dropping lanes, including when CR passes but EQ fails.
  for (unsigned lanes = maxLanes; lanes; lanes /= 2) {
    for (const uint32_t rate : rates) {
      if (rate > caps.maxRateKhz) continue;
      result.link = {rate, lanes};
      ++result.attempts;
      result.status = TrainAttempt<Io>{io, result, caps}.run();
      if (result.status != DpTrainStatus::ClockRecoveryFailed &&
          result.status != DpTrainStatus::ChannelEqualizationFailed) return result;
    }
  }
  return result;
}

} // namespace dp_train_detail

// Io supplies auxRead/auxWrite(addr, buf, len) returning DpAuxStatus (Ack means
// the full length), setPhy(rateKhz, lanes, swing, preemphasis) returning bool,
// setPattern(DpTrainingPattern) and delayUs(us). The adapter owns power and timing
// prerequisites. The model trains one upstream link within piano's HBR2 cap.
template <typename Io>
DpTrainResult trainDpLink(Io &io, unsigned pinLanes)
{
  DpTrainResult result{.status = DpTrainStatus::InvalidPinLanes, .link = {}};
  if (!validDpLaneCount(pinLanes)) return result;
  DpReceiverCaps caps;
  if (!dp_train_detail::readReceiverCaps(io, caps, result.auxStatus)) {
    result.status = result.auxStatus == DpAuxStatus::Ack ? DpTrainStatus::InvalidCaps : DpTrainStatus::AuxError;
    return result;
  }
  return dp_train_detail::trainWithCaps(io, pinLanes, caps);
}

struct DpMsaTu {
  uint16_t hTotal = 0, hStart = 0, hActive = 0, hBlank = 0, hSyncWidth = 0;
  uint16_t vTotal = 0, vStart = 0, vActive = 0, vBlank = 0, vSyncWidth = 0;
  bool hSyncPositive = false, vSyncPositive = false;
  uint8_t misc0 = 0, misc1 = 0;
  uint32_t mvid = 0, nvid = 0;
  uint8_t tuSize = 0, validSymbols = 0;
  // Exact average per lane = validSymbolsFloor + numerator / denominator.
  uint8_t validSymbolsFloor = 0;
  uint32_t validSymbolsNumerator = 0, validSymbolsDenominator = 0;
};

// Progressive RGB 8 bpc SST, asynchronous stream clock. The exact TU ratio
// lets a later controller adapter choose a fractional stuffing schedule.
constexpr bool computeDpMsaTu(const DpTiming &mode, DpLink link, DpMsaTu &out)
{
  out = {};
  if (!dpStreamFits(mode, link)) return false;

  const uint64_t mvid = (uint64_t(mode.pixelKhz) * kDpNvid + link.rateKhz / 2) / link.rateKhz;
  const uint64_t tuNumerator = uint64_t(mode.pixelKhz) * kDpRgb8BitsPerPixel * kDpTuSize;
  const uint32_t tuDenominator = dpLinkPayloadKbps(link);
  const uint64_t validSymbols = (tuNumerator + tuDenominator - 1) / tuDenominator;

  out.hTotal = uint16_t(mode.hActive + mode.hBlank);
  out.vTotal = uint16_t(mode.vActive + mode.vBlank);
  out.hStart = uint16_t(mode.hBlank - mode.hSyncOffset);
  out.vStart = uint16_t(mode.vBlank - mode.vSyncOffset);
  out.hActive = mode.hActive;
  out.vActive = mode.vActive;
  out.hBlank = mode.hBlank;
  out.vBlank = mode.vBlank;
  out.hSyncWidth = mode.hSyncWidth;
  out.vSyncWidth = mode.vSyncWidth;
  out.hSyncPositive = mode.hSyncPositive;
  out.vSyncPositive = mode.vSyncPositive;
  out.misc0 = 0x20;  // 8 bpc, RGB, asynchronous clock
  out.mvid = uint32_t(mvid);
  out.nvid = kDpNvid;
  out.tuSize = kDpTuSize;
  out.validSymbols = uint8_t(validSymbols);
  out.validSymbolsFloor = uint8_t(tuNumerator / tuDenominator);
  out.validSymbolsNumerator = uint32_t(tuNumerator % tuDenominator);
  out.validSymbolsDenominator = tuDenominator;
  return true;
}

} // namespace pianodp
