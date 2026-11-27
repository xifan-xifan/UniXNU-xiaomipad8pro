// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline hotplug model for one routed SST sink. Events are serialized.
#pragma once
#include "DpExternalModes.h"

namespace pianodp {

enum class DpHotplugStatus : uint8_t {
  Disconnected, Active, InvalidPinLanes, EdidAuxError, BadEdid, TrainFailed, NoMode, StreamFailed,
  NoSink, UnsupportedTopology,
};

constexpr uint32_t kDpSinkCount = 0x200;
constexpr size_t kDpHpdIrqBytes = 8; // DPCD 0x200–0x207
constexpr size_t kDpEdidAuxChunk = 16;
static_assert(kEdidBlockBytes % kDpEdidAuxChunk == 0);
constexpr unsigned kDpEdidAuxAttempts = 4, kDpEdidAuxRetryDelayUs = 400;

constexpr unsigned dpSinkCount(uint8_t byte, uint8_t revision)
{
  return (byte & 0x3f) | (revision >= 0x12 ? (byte & 0x80) >> 1 : 0);
}

struct DpHpdIrq {
  unsigned sinkCount = 0;
  uint8_t serviceVector = 0;
  bool contentProtectionReady = false, downstreamChanged = false, linkStatusUpdated = false, linkHealthy = false;
};

inline bool parseDpHpdIrq(const uint8_t *bytes, size_t length, uint8_t revision, unsigned lanes, DpHpdIrq &irq)
{
  irq = {};
  if (!bytes || length < kDpHpdIrqBytes || !validDpLaneCount(lanes)) return false;
  irq.sinkCount = dpSinkCount(bytes[0], revision);
  irq.serviceVector = bytes[1] & (revision >= 0x12 ? 0x7f : 0x4f);
  irq.contentProtectionReady = bytes[0] & 0x40;
  irq.downstreamChanged = bytes[4] & 0x40;
  irq.linkStatusUpdated = bytes[4] & 0x80;
  irq.linkHealthy = dp_train_detail::linkEqualized(bytes + 2, lanes);
  return true;
}

// Io supplies the trainDpLink callbacks plus readEdid(offset, bytes, size),
// startStream(link, timing) and teardown(). readEdid maps a linear EDID offset
// to a complete I2C-over-AUX read; Ack promises the full chunk. teardown clears
// adapter state after any attempted plug, including failed AUX probes and partially started streams.
class DpHotplug {
 public:
  DpHotplug() = default;
  DpHotplug(const DpHotplug &) = delete;
  DpHotplug &operator=(const DpHotplug &) = delete;

  DpHotplugStatus status() const { return status_; }
  DpLink link() const { return link_; }
  const DpExternalModes &modes() const { return modes_; }
  // The service vector is diagnostic. This model handles topology and link status.
  const DpHpdIrq &lastIrq() const { return lastIrq_; }

  template <typename Io>
  DpHotplugStatus hpd(Io &io, bool high, bool irq, unsigned pinLanes)
  {
    if (!high) {
      disconnect(io);
      lastIrq_ = {};
      return status_;
    }
    if (pinLanes_ != pinLanes) lastIrq_ = {};
    if (status_ != DpHotplugStatus::Disconnected && pinLanes_ == pinLanes) {
      if (!irq) return status_;
      if (status_ == DpHotplugStatus::Active && readIrq(io)) {
        if (caps_.hasSinkCount() && lastIrq_.sinkCount != 1) {
          disconnect(io);
          pinLanes_ = pinLanes;
          return status_ = branchStatus(lastIrq_.sinkCount);
        }
        if (lastIrq_.linkHealthy && !(caps_.hasSinkCount() && lastIrq_.downstreamChanged)) return status_;
      }
    }

    disconnect(io);
    pinLanes_ = pinLanes;
    if (!validDpLaneCount(pinLanes)) return status_ = DpHotplugStatus::InvalidPinLanes;

    status_ = activate(io);
    if (status_ != DpHotplugStatus::Active) io.teardown();
    return status_;
  }

 private:
  DpHotplugStatus status_ = DpHotplugStatus::Disconnected;
  unsigned pinLanes_ = 0;
  DpLink link_{};
  DpExternalModes modes_{};
  DpReceiverCaps caps_{};
  DpHpdIrq lastIrq_{};

  static constexpr DpHotplugStatus branchStatus(unsigned sinks)
  {
    if (!sinks) return DpHotplugStatus::NoSink;
    return sinks == 1 ? DpHotplugStatus::Active : DpHotplugStatus::UnsupportedTopology;
  }

  template <typename Io>
  void disconnect(Io &io)
  {
    const bool wasActive = status_ == DpHotplugStatus::Active;
    link_ = {};
    modes_ = {};
    caps_ = {};
    status_ = DpHotplugStatus::Disconnected;
    pinLanes_ = 0;
    if (wasActive) io.teardown();
  }

  template <typename Io>
  bool readIrq(Io &io)
  {
    lastIrq_ = {};
    uint8_t bytes[kDpHpdIrqBytes]{};
    return io.auxRead(kDpSinkCount, bytes, sizeof bytes) == DpAuxStatus::Ack &&
           parseDpHpdIrq(bytes, sizeof bytes, caps_.revision, link_.lanes, lastIrq_);
  }

  template <typename Io>
  DpHotplugStatus activate(Io &io)
  {
    DpReceiverCaps caps;
    DpAuxStatus aux;
    if (!dp_train_detail::readReceiverCaps(io, caps, aux)) return DpHotplugStatus::TrainFailed;
    if (caps.hasSinkCount()) {
      uint8_t count = 0;
      if (io.auxRead(kDpSinkCount, &count, 1) != DpAuxStatus::Ack) return DpHotplugStatus::TrainFailed;
      const DpHotplugStatus topology = branchStatus(dpSinkCount(count, caps.revision));
      if (topology != DpHotplugStatus::Active) return topology;
    }

    uint8_t bytes[kEdidMaxBytes]{};
    size_t length = kEdidBlockBytes;
    for (size_t offset = 0; offset < length; offset += kDpEdidAuxChunk) {
      for (unsigned attempt = 1;; ++attempt) {
        const DpAuxStatus reply = io.readEdid(offset, bytes + offset, kDpEdidAuxChunk);
        if (reply == DpAuxStatus::Ack) break;
        if (attempt == kDpEdidAuxAttempts || (reply != DpAuxStatus::Defer && reply != DpAuxStatus::Timeout))
          return DpHotplugStatus::EdidAuxError;
        io.delayUs(kDpEdidAuxRetryDelayUs);
      }
      if (offset + kDpEdidAuxChunk == kEdidBlockBytes) {
        if (!edid_detail::edidHeaderOk(bytes) || !edid_detail::edidChecksumOk(bytes) || bytes[126] >= kEdidMaxBlocks)
          return DpHotplugStatus::BadEdid;
        length = (size_t(bytes[126]) + 1) * kEdidBlockBytes;
      }
    }

    EdidInfo edid;
    if (parseEdid(bytes, length, edid) != EdidStatus::Ok) return DpHotplugStatus::BadEdid;
    const DpTrainResult training = dp_train_detail::trainWithCaps(io, pinLanes_, caps);
    if (training.status != DpTrainStatus::Trained) return DpHotplugStatus::TrainFailed;

    const DpExternalModes modes = buildDpExternalModes(edid, training.link, DpPixelFormat::Rgb8);
    if (!modes.count) return DpHotplugStatus::NoMode;
    if (!io.startStream(training.link, modes.modes[0])) return DpHotplugStatus::StreamFailed;
    link_ = training.link;
    modes_ = modes;
    caps_ = caps;
    return DpHotplugStatus::Active;
  }
};

} // namespace pianodp
