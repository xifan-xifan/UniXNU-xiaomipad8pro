// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline B7 contract. Inputs are observations and outputs are simulated requests. No MMIO, GLINK, or kext entry points.
#pragma once

#include "Ucsi.h"
#include "UsbHostHw.h"
#include "../PianoDwc3/Dwc3Regs.h"
#include "../PianoDwc3/PianoUsbHw.h"
#include "../PianoDwc3/UsbHandoff.h"
#include "../include/PianoUsbcContract.h"

namespace pianousbhost::b7 {

enum class Phase { device, releasing, ramping, waitingPort, resetting, enumerating, online, stopping, reclaiming, held };
enum class Owner { device, host, none };
enum class End { none, detach, role, witness, sleep, powerConflict, powerLost, fault, release, ramp, reset, enumeration,
                 loan, halt, discharge, reclaim };
enum class DataRole { none, device, host };
enum class PowerRole { none, sink, source };
enum class Cc { none, normal, reverse };
enum class Level { unknown, safe0v, unsettled, safe5v };
enum Fault : uint32_t { overCurrent = 1u, thermal = 2u, battery = 4u, dma = 8u, controller = 16u };
enum class Protocol { usb2, usb3 };

struct Connector {
  bool known = false;
  DataRole data = DataRole::none;
  PowerRole power = PowerRole::none;
  Cc cc = Cc::none;

  bool detached() const { return known && data == DataRole::none; }
  bool sourceHost() const { return known && data == DataRole::host && power == PowerRole::source; }
};

// The PAN frame and the committed snapshot are paired by the fixture. Generation/sequence belong to the owner snapshot.
inline Connector decodeConnector(const PianoUsbcObservation &status, const uint8_t *pan, unsigned panBytes, uint64_t now)
{
  using namespace pianoucsi;
  const uint16_t version = status.ucsi_version;
  if (!piano_usbc_observation_fresh(&status, now) || panBytes != 32) return {};
  switch (version) {
    case kV1_0:
    case kV1_1:
    case kV1_2: break;
    case kV2_0: if (status.status_bytes < 11) return {}; break;
    default: return {};
  }
  const auto port = decodePort(pan, panBytes);
  if (!port.full || port.port != 0 || get32(pan + 4) != kTypeRequest) return {};

  const auto s = decodeStatus(status.status, status.status_bytes, version);
  if (!s.connected) {
    constexpr uint8_t noOrientation = 0xff;  // fixture encoding for an absent CC witness
    if (s.provider || s.powerMode != kModeNone || s.partnerType != kPartnerNone || s.partnerFlags ||
        port.mux != kMuxNone || (portOrientation(port.orientation) < 0 && port.orientation != noOrientation)) return {};
    return {true};
  }

  const int cc = portOrientation(port.orientation);
  if (cc < 0 || (s.orientation >= 0 && s.orientation != cc) || port.mux != kMuxUsb3 ||
      s.powerMode == kModeNone || s.powerMode > kModeTypec3_0 || !(s.partnerFlags & kPartnerFlagUsb)) return {};

  DataRole data;
  switch (s.partnerType) {
    case kPartnerDfp: data = DataRole::device; break;
    case kPartnerUfp:
    case kPartnerCableUfp: data = DataRole::host; break;
    default: return {};
  }
  return {true, data, s.provider ? PowerRole::source : PowerRole::sink, cc == 0 ? Cc::normal : Cc::reverse};
}

struct PowerSample {
  Level level = Level::unknown;
  bool charging = false;
  bool sinkVbus = false;
  uint32_t faults = 0;
};

struct Route {
  uint16_t sid;
  unsigned bank;
  bool verified;
};

struct GadgetRelease {
  pianousb::HandoffState state;
  uint32_t dctl;
  uint32_t dsts;
  bool eventsMasked;
};

struct PortSample {
  Protocol protocol;
  uint32_t portsc;
  unsigned resetAttempt = 0;  // fixture correlation, on a reset completion
};

class Model {
 public:
  // Synthetic milliseconds with exclusive deadlines. No arithmetic adds to the supplied monotonic clock.
  static constexpr uint64_t kDebounceMs = 10, kReleaseMs = 50, kRampMs = 20, kResetMs = 30, kEnumerationMs = 50,
                            kHaltMs = 20, kDischargeMs = 40, kReclaimMs = 50, kLoanMs = 500;
  static constexpr unsigned kResetAttempts = 2;
  explicit Model(Route pinned) : pinned_(pinned) {}

  Phase phase() const { return phase_; }
  Owner owner() const { return owner_; }
  const Route &route() const { return pinned_; }
  const Connector &connector() const { return connector_; }
  bool gadgetActive() const { return owner_ == Owner::device && phase_ != Phase::held && !asleep_; }
  bool source5v() const { return source5v_; }
  bool runXhci() const { return phase_ >= Phase::waitingPort && phase_ <= Phase::online; }
  bool portPower() const { return runXhci(); }
  bool detachRequired() const { return needsDetach_; }
  uint32_t latchedFaults() const { return latchedFaults_; }
  unsigned resetAttempts() const { return resetAttempts_; }
  uint32_t resetRequest() const { return phase_ == Phase::resetting ? (protocol_ == Protocol::usb2 ? kPortPr : kPortWpr) : 0; }
  End end() const { return end_; }

  bool advance(uint64_t now)
  {
    if (now < now_) return false;
    now_ = now;
    if (connector_.known && now_ > witnessExpiry_) invalidate();
    if (inLoan() && elapsed(loanStart_, kLoanMs)) stop(End::loan);
    switch (phase_) {
      case Phase::releasing: if (elapsed(phaseStart_, kReleaseMs)) stop(End::release); break;
      case Phase::ramping: if (elapsed(phaseStart_, kRampMs)) stop(End::ramp); break;
      case Phase::resetting: if (elapsed(phaseStart_, kResetMs)) retryReset(); break;
      case Phase::enumerating: if (elapsed(phaseStart_, kEnumerationMs)) stop(End::enumeration); break;
      case Phase::stopping:
        if (!halted_ && elapsed(phaseStart_, kHaltMs)) hold(End::halt);
        else if (!discharged_ && elapsed(phaseStart_, kDischargeMs)) hold(End::discharge);
        break;
      case Phase::reclaiming: if (!asleep_ && elapsed(phaseStart_, kReclaimMs)) hold(End::reclaim); break;
      default: break;
    }
    return true;
  }

  bool observe(const PianoUsbcObservation &status, const uint8_t *pan, unsigned bytes, uint64_t now)
  {
    if (!advance(now) || asleep_ || status.updated_ms < observationsSince_ || !status.generation || !status.seq ||
        status.generation < generation_ || (status.generation == generation_ && status.seq <= sequence_)) return false;
    if (generation_ && generation_ != status.generation) invalidate();
    generation_ = status.generation;
    sequence_ = status.seq;

    const Connector next = decodeConnector(status, pan, bytes, now);
    if (!next.known) {
      invalidate();
      return false;
    }
    const bool changed = connector_.data != next.data || connector_.power != next.power || connector_.cc != next.cc;
    if (!connector_.known || changed) attachSince_ = now;
    connector_ = next;
    observationsSince_ = status.updated_ms;
    witnessExpiry_ = status.expires_ms;
    if (inLoan() && changed) stop(next.detached() ? End::detach : End::role);
    cleanDetach_ = next.detached() && !power_.faults && !externalPower();
    rearm();
    return true;
  }

  void relayLost(uint64_t now)
  {
    if (!advance(now)) return;
    observationsSince_ = now;
    invalidate();
  }

  void suspend(uint64_t now)
  {
    if (!advance(now) || asleep_) return;
    asleep_ = true;
    connector_ = {};
    cleanDetach_ = false;
    stop(End::sleep);
  }

  void wake(uint64_t now)
  {
    if (!advance(now) || !asleep_) return;
    asleep_ = false;
    observationsSince_ = now;
    if (phase_ == Phase::reclaiming) phaseStart_ = now;
  }

  void power(PowerSample sample, uint64_t now)
  {
    if (!advance(now)) return;
    const bool stopping = phase_ == Phase::stopping;
    power_ = sample;
    if (externalPower()) cleanDetach_ = false;

    if (sample.faults) {
      latch(sample.faults);
    } else if (inLoan() && externalPower()) {
      stop(End::powerConflict);
    } else if (phase_ == Phase::releasing && !safe0v()) {
      stop(End::powerConflict);
    } else if (phase_ == Phase::ramping && sample.level == Level::safe5v) {
      enter(Phase::waitingPort);
    } else if ((phase_ == Phase::ramping && sample.level == Level::unknown) || (runXhci() && sample.level != Level::safe5v)) {
      stop(End::powerLost);
    }

    if (stopping && phase_ == Phase::stopping) {
      discharged_ = safe0v();
      finishStop();
    }
    rearm();
  }

  // Returns a synthetic loan ID. Every asynchronous controller response must belong to this request.
  uint64_t requestHost(uint64_t now)
  {
    if (!advance(now) || phase_ != Phase::device || asleep_ || needsDetach_ || !connector_.sourceHost() ||
        !elapsed(attachSince_, kDebounceMs) || !safe0v() || power_.faults || !validRoute(pinned_) || token_ == UINT64_MAX) return 0;
    end_ = End::none;
    needsDetach_ = true;
    loanStart_ = now;
    resetAttempts_ = 0;
    enter(Phase::releasing);
    return ++token_;
  }

  bool release(uint64_t token, GadgetRelease sample, Route route, uint64_t now)
  {
    using namespace pianousb;
    if (!advance(now) || phase_ != Phase::releasing || token != token_) return false;
    if (sample.state == kHoDevice || sample.state == kHoStarting) return false;  // release is pending
    if (sample.state == kHoOff) {
      stop(End::release);
      return false;
    }
    if (!handoffCoreFree(true, sample.state) || (sample.dctl & dwc3::DCTL_RUN_STOP) ||
        sample.dsts == UINT32_MAX || !(sample.dsts & dwc3::DSTS_DEVCTRLHLT) || !sample.eventsMasked) {
      hold(End::release);
      return false;
    }

    owner_ = Owner::none;
    if (!sameRoute(route)) {
      end_ = End::release;
      enter(Phase::reclaiming);
      return false;
    }

    owner_ = Owner::host;
    source5v_ = true;  // releasing already enforces a fresh source/host witness and an unpowered, non-charging bus
    enter(Phase::ramping);
    return true;
  }

  // Returns the sampled W1C change bits to acknowledge. Reset requests are separate from acknowledgements.
  uint32_t port(uint64_t token, PortSample sample, uint64_t now)
  {
    if (!advance(now) || token != token_ || !runXhci() ||
        (sample.protocol != Protocol::usb2 && sample.protocol != Protocol::usb3)) return 0;
    const uint32_t reg = sample.portsc;
    if (reg & (kPortOca | kPortOcc)) {
      latch(overCurrent);
    } else if (phase_ == Phase::waitingPort) {
      if ((reg & (kPortPp | kPortCcs | kPortCsc)) == (kPortPp | kPortCcs | kPortCsc)) {
        protocol_ = sample.protocol;
        if (protocol_ == Protocol::usb3 && enabledAtSpeed(reg)) enter(Phase::enumerating);
        else retryReset();
      }
    } else if (protocol_ == sample.protocol) {
      if (!(reg & kPortCcs) || (reg & kPortCsc)) {
        // The initial CSC was acknowledged. A later CSC can hide a disconnect/reconnect between samples.
        stop(End::detach);
      } else if (!(reg & kPortPp)) {
        stop(End::powerLost);
      } else if (phase_ == Phase::resetting && sample.resetAttempt == resetAttempts_ &&
                 (reg & (protocol_ == Protocol::usb2 ? kPortPrc : kPortWrc)) && !(reg & (kPortPr | kPortWpr))) {
        if (enabledAtSpeed(reg)) enter(Phase::enumerating);
        else retryReset();
      } else if ((phase_ == Phase::enumerating || phase_ == Phase::online) && !enabledAtSpeed(reg)) {
        stop(End::enumeration);
      }
    }
    return reg & kPortChangeBits;
  }

  bool enumerated(uint64_t token, bool addressed, bool descriptors, uint64_t now)
  {
    if (!advance(now) || token != token_ || phase_ != Phase::enumerating) return false;
    if (!addressed || !descriptors) {
      stop(End::enumeration);
      return false;
    }
    enter(Phase::online);
    return true;
  }

  void xhci(uint64_t token, uint32_t cmd, uint32_t sts, uint64_t now)
  {
    const bool stopping = phase_ == Phase::stopping;
    if (!advance(now) || token != token_) return;
    if (runXhci() && (!(cmd & kUsbCmdRun) || (sts & (kUsbStsHalted | kUsbStsCnr | kUsbStsHce | kUsbStsHse)))) {
      latch(controller);
    } else if (stopping && phase_ == Phase::stopping) {
      halted_ = !(cmd & (kUsbCmdRun | kUsbCmdHcrst)) && (sts & (kUsbStsHalted | kUsbStsCnr)) == kUsbStsHalted;
      finishStop();
    }
  }

  bool reclaim(uint64_t token, pianousb::HandoffState state, uint32_t gctl, Route route, uint64_t now)
  {
    using namespace pianousb;
    if (!advance(now) || token != token_ || asleep_ || phase_ != Phase::reclaiming) return false;
    if (state == kHoReclaiming) return false;
    if (state != kHoDevice || dwc3::roleOf(gctl) != dwc3::ROLE_DEVICE || !sameRoute(route)) {
      hold(End::reclaim);
      return false;
    }
    owner_ = Owner::device;
    enter(Phase::device);
    rearm();
    return true;
  }

 private:
  bool elapsed(uint64_t since, uint64_t interval) const { return now_ - since >= interval; }
  bool inLoan() const { return phase_ == Phase::releasing || source5v_; }
  bool externalPower() const { return power_.charging || power_.sinkVbus; }
  bool safe0v() const { return power_.level == Level::safe0v && !externalPower(); }
  static bool validRoute(Route route) { return route.verified && route.sid == pianohw::kUsbStream && route.bank < 128; }
  bool sameRoute(Route route) const { return validRoute(route) && route.bank == pinned_.bank; }
  bool enabledAtSpeed(uint32_t portsc) const
  {
    // Fixture uses the default xHCI speed IDs and a U0 link for USB3.
    return (portsc & kPortPed) && !(portsc & (kPortPr | kPortWpr)) &&
           (protocol_ == Protocol::usb2 ? portSpeed(portsc) == 3 : portSpeed(portsc) == 4 && portLinkState(portsc) == 0);
  }

  void enter(Phase phase)
  {
    phase_ = phase;
    phaseStart_ = now_;
  }
  void stop(End why)
  {
    if (!inLoan()) return;
    end_ = why;
    if (phase_ == Phase::releasing) {
      enter(Phase::device);  // the tokenized release was canceled before transfer
    } else {
      source5v_ = false;
      halted_ = discharged_ = false;
      enter(Phase::stopping);  // root-port power and Run/Stop requests drop with this transition
    }
  }
  void hold(End why)
  {
    end_ = why;
    enter(Phase::held);  // ownership stays as-is until a new model (boot)
  }
  void finishStop()
  {
    if (!halted_ || !discharged_) return;
    owner_ = Owner::none;
    enter(Phase::reclaiming);
  }
  void retryReset()
  {
    if (resetAttempts_ == kResetAttempts) {
      stop(End::reset);
    } else {
      ++resetAttempts_;
      enter(Phase::resetting);
    }
  }
  void invalidate()
  {
    connector_ = {};
    cleanDetach_ = false;
    stop(End::witness);
  }
  void latch(uint32_t faults)
  {
    latchedFaults_ |= faults;
    needsDetach_ = true;
    cleanDetach_ = false;
    stop(End::fault);
  }
  void rearm()
  {
    if (phase_ != Phase::device || asleep_ || !cleanDetach_ || !safe0v() || power_.faults) return;
    needsDetach_ = false;
    latchedFaults_ = 0;
  }

  const Route pinned_;
  Phase phase_ = Phase::device;
  Owner owner_ = Owner::device;
  End end_ = End::none;
  Connector connector_;
  PowerSample power_;
  Protocol protocol_ = Protocol::usb2;
  uint32_t generation_ = 0, sequence_ = 0, latchedFaults_ = 0;
  uint64_t now_ = 0, phaseStart_ = 0, loanStart_ = 0, attachSince_ = 0, witnessExpiry_ = 0, observationsSince_ = 0, token_ = 0;
  unsigned resetAttempts_ = 0;
  bool asleep_ = false, needsDetach_ = false, cleanDetach_ = false, source5v_ = false, halted_ = false, discharged_ = false;
};

}  // namespace pianousbhost::b7
