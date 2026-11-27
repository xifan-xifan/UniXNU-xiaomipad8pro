// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline model for one LE central link with an explicitly addressed peer. HCI wire values are
// from Bluetooth Core 5.4, Vol 4 Parts A/E and Vol 1 Part F. See docs/drivers/bluetooth.md §8.7
// for the WCN7860 reference and the synthetic policy (deadlines, retry budget and ACL capacity).
#pragma once

#include "HciH4.h"
#include <algorithm>
#include <array>
#include <bit>
#include <mutex>
#include <optional>
#include <span>

namespace pianobt::host {

enum : uint16_t {
  kReset = 0x0c03, kSetLeEventMask = H4LeMaskPatcher::kOpcode, kCreate = 0x200d, kExtendedCreate = 0x2043,
  kReadRemoteVersion = 0x041d, kReadRemoteFeatures = 0x2016, kDisconnect = 0x0406,
  kNoHandle = 0xffff,
};
enum : uint8_t { kSuccess = 0, kConnectionTimeout = 0x08, kFailedToEstablish = 0x3e };

inline size_t h4FrameSize(std::span<const uint8_t> frame)
{
  if (frame.empty() || frame.size() > H4Decoder::kMax + 1 ||
      frame[0] < kH4Command || frame[0] > kH4Event) return 0;

  // The stream decoder can recover from garbage or emit multiple packets. Require exactly one
  // packet consuming all the bytes for this fixture's frame-at-a-time interface.
  H4Decoder decoder;
  size_t bytes = 0;
  decoder.feed(frame.data(), frame.size(), [&](uint8_t type, const uint8_t *, size_t size) {
    if (type == frame[0]) bytes = size + 1;
  });
  return decoder.idle() && decoder.stats().packets == 1 && bytes == frame.size() ? bytes : 0;
}

inline bool patchLeEventMask(std::span<uint8_t> frame, bool enabled)
{
  if (!h4FrameSize(frame)) return false;
  if (frame[0] == kH4Command) {
    H4LeMaskPatcher patcher;
    patcher.patch(frame.data(), frame.size(), enabled, [](const H4LeMaskPatcher::Seen &) {});
  }
  return true;
}

struct LeFeatures {
  std::array<uint8_t, 8> bytes{};
  // Core 5.4, Vol 6 Part B §4.6, feature bits 5 and 8.
  bool dataLengthExtension() const { return (bytes[0] & 0x20) != 0; }
  bool phy2M() const { return (bytes[1] & 0x01) != 0; }
};

class HciLinkModel {
 public:
  using Key = std::array<uint8_t, 16>;   // opaque synthetic SMP result
  enum class Power { off, wakeReset, wakeMask, ready, draining, suspendReset, powerDownReady };
  enum class Security { idle, pairing, encrypted, hogp };
  static constexpr unsigned kAclCapacity = 3, kMaxAttempts = 3;
  static constexpr uint64_t kResponseMs = 100, kDrainMs = 50, kBackoffMs = 20;

  struct Snapshot {
    Power power = Power::off;
    Security security = Security::idle;
    uint64_t epoch = 0, linkId = 0, retryMs = 0, eventMask = 0;
    uint16_t handle = kNoHandle, pending = 0, procedure = 0;
    unsigned attempts = 0, aclInFlight = 0;
    bool commandCredit = false, waitingForLinkLoss = false, forcedPowerCycle = false;
    bool versionRead = false, featuresRead = false, bonded = false;
    LeFeatures features;

    unsigned aclCredits() const { return kAclCapacity - aclInFlight; }
  };

  Snapshot snapshot() const
  {
    const std::lock_guard lock(mutex_);
    Snapshot snapshot = state_;
    snapshot.bonded = bond_.has_value();
    return snapshot;
  }

  // The epoch belongs to the transport, outside the H4 bytes. Tag queued work when it is
  // produced. Bytes within an epoch are delivered in order, as on a single UART stream.
  bool transmit(uint64_t epoch, std::span<uint8_t> frame)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || (state_.power != Power::ready && state_.power != Power::wakeMask) ||
        !h4FrameSize(frame)) return false;
    if (frame[0] == kH4Acl) {
      if (state_.power != Power::ready || state_.handle == kNoHandle || state_.waitingForLinkLoss ||
          state_.procedure == kDisconnect || state_.pending == kDisconnect || !state_.aclCredits()) return false;
      const uint16_t handleAndFlags = le16(frame.data() + 1);
      // LE host TX allows point-to-point first fragments (PB=0) and continuations (PB=1).
      if ((handleAndFlags & 0x0fff) != state_.handle || (handleAndFlags >> 12) > 1) return false;
      ++state_.aclInFlight;
      return true;
    }
    if (frame[0] != kH4Command || state_.pending || state_.procedure || state_.waitingForLinkLoss ||
        !state_.commandCredit) return false;

    const uint16_t opcode = le16(frame.data() + 1);
    const auto p = frame.subspan(4);
    if (state_.power == Power::wakeMask && opcode != kSetLeEventMask) return false;
    switch (opcode) {
    case kSetLeEventMask: {
      if (p.size() != 8 || (state_.power == Power::wakeMask && !(p[0] & 1))) return false;
      H4LeMaskPatcher patcher;
      patcher.patch(frame.data(), frame.size(), true, [](const H4LeMaskPatcher::Seen &) {});
      pendingMask_ = uint64_t(le32(p.data())) | (uint64_t(le32(p.data() + 4)) << 32);
      break;
    }
    case kCreate:
    case kExtendedCreate:
      if (!startCreate(opcode, p)) return false;
      break;
    case kReadRemoteVersion:
    case kReadRemoteFeatures:
      if (p.size() != 2 || !matchesHandle(p.data()) ||
          (opcode == kReadRemoteVersion ? state_.versionRead : state_.featuresRead)) return false;
      break;
    case kDisconnect:
      if (p.size() != 3 || !matchesHandle(p.data())) return false;
      break;
    default:
      return false;
    }
    issue(opcode);
    return true;
  }

  bool receive(uint64_t epoch, std::span<const uint8_t> frame)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power == Power::off || state_.power == Power::powerDownReady ||
        !h4FrameSize(frame) || frame[0] != kH4Event) return false;
    const auto p = frame.subspan(3);
    switch (frame[1]) {
    case kEvCommandComplete: return commandComplete(frame.subspan(1));
    case kEvCommandStatus: return commandStatus(frame.subspan(1));
    case 0x05: return disconnected(p);   // Disconnection Complete
    case 0x0c: return remoteComplete(kReadRemoteVersion, p);
    case 0x13: return completedPackets(p);
    case 0x3e: return leEvent(p);
    default: return false;
    }
  }

  // Only a new host request can replenish an exhausted budget.
  bool renewRetryBudget(uint64_t epoch)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power != Power::ready || state_.handle != kNoHandle ||
        state_.pending || state_.procedure || state_.attempts != kMaxAttempts) return false;
    state_.attempts = 0;
    return true;
  }

  // Fixture time never comes from the wall clock. A lost response, a stuck command credit,
  // or an unresponsive drain requires a power cycle before accepting any new work.
  void advance(uint64_t epoch, uint64_t elapsedMs)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch) return;
    state_.retryMs -= std::min(state_.retryMs, elapsedMs);
    if (!deadlineMs_) return;
    deadlineMs_ -= std::min(deadlineMs_, elapsedMs);
    if (!deadlineMs_) preparePowerDown(true);
  }

  bool beginSuspend(uint64_t epoch)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power != Power::ready) return false;
    state_.power = Power::draining;
    deadlineMs_ = kDrainMs;
    return true;
  }

  // The lifecycle owns Reset. External HCI/ACL writes are closed from beginSuspend onward.
  bool resetAfterDrain(uint64_t epoch)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power != Power::draining || state_.pending || state_.procedure ||
        state_.aclInFlight || state_.waitingForLinkLoss || !state_.commandCredit) return false;
    state_.power = Power::suspendReset;
    issue(kReset);
    return true;
  }

  bool powerOff(uint64_t epoch)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power != Power::powerDownReady) return false;
    state_.power = Power::off;
    return true;
  }

  bool beginWake(uint64_t epoch)
  {
    const std::lock_guard lock(mutex_);
    if (epoch != state_.epoch || state_.power != Power::off) return false;
    ++state_.epoch;
    state_.forcedPowerCycle = false;
    state_.power = Power::wakeReset;
    issue(kReset);
    return true;
  }

  // These host callbacks model SMP outcomes following encryption and key distribution,
  // and HOGP enrollment after encryption. The link identity fences late callbacks even
  // when a new connection reuses the same controller handle.
  bool beginPairing(uint64_t linkId)
  {
    const std::lock_guard lock(mutex_);
    if (!linkReady(linkId) || state_.security != Security::idle) return false;
    state_.security = Security::pairing;
    return true;
  }

  bool finishPairing(uint64_t linkId, std::optional<Key> key)
  {
    const std::lock_guard lock(mutex_);
    if (!linkReady(linkId) || state_.security != Security::pairing) return false;
    state_.security = key ? Security::encrypted : Security::idle;
    if (key) bond_ = Bond{peer_, *key};
    return true;
  }

  bool restoreEncryption(uint64_t linkId, const Key &key)
  {
    const std::lock_guard lock(mutex_);
    if (!linkReady(linkId) || state_.security != Security::idle ||
        !bond_ || bond_->peer != peer_ || bond_->key != key) return false;
    state_.security = Security::encrypted;
    return true;
  }

  bool enableHogp(uint64_t linkId)
  {
    const std::lock_guard lock(mutex_);
    if (!linkReady(linkId) || state_.security != Security::encrypted) return false;
    state_.security = Security::hogp;
    return true;
  }

  bool acceptsHid(uint64_t linkId) const
  {
    const std::lock_guard lock(mutex_);
    return linkReady(linkId) && state_.security == Security::hogp;
  }

 private:
  using Bytes = std::span<const uint8_t>;
  // Address type plus six wire-order bytes. This central-only model does not resolve RPAs.
  using Peer = std::array<uint8_t, 7>;
  struct Bond { Peer peer; Key key; };

  static bool isCreate(uint16_t opcode) { return opcode == kCreate || opcode == kExtendedCreate; }

  bool startCreate(uint16_t opcode, Bytes p)
  {
    if (state_.handle != kNoHandle || state_.retryMs || state_.attempts >= kMaxAttempts) return false;
    if (opcode == kCreate) {
      if (p.size() != 25 || p[4] != 0) return false;
      p = p.subspan(5);
    } else {
      if (p.size() < 10 || p[0] != 0) return false;
      const unsigned phys = p[9];
      if ((phys & ~7u) || !(phys & 5u) || p.size() != 10 + 16 * size_t(std::popcount(phys))) return false;
      p = p.subspan(2);
    }
    Peer peer;
    std::copy_n(p.begin(), peer.size(), peer.begin());
    if (peer[0] > 1 || (state_.attempts && peer != peer_)) return false;
    peer_ = peer;
    ++state_.attempts;
    return true;
  }

  void issue(uint16_t opcode)
  {
    state_.pending = opcode;
    state_.commandCredit = false;
    deadlineMs_ = kResponseMs;
  }

  void updateDeadline()
  {
    if (state_.power != Power::ready && state_.power != Power::wakeMask) return;
    const bool waiting = state_.pending || state_.procedure || state_.waitingForLinkLoss ||
                         !state_.commandCredit || state_.power == Power::wakeMask;
    deadlineMs_ = waiting ? kResponseMs : 0;
  }

  // A credit-only event may change idle flow control. It cannot extend the response
  // deadline of an existing command, procedure or lifecycle transition.
  bool creditOnly(uint8_t ncmd)
  {
    if (state_.power != Power::ready && state_.power != Power::draining && state_.power != Power::wakeMask) return false;
    if (state_.commandCredit != (ncmd != 0)) {
      state_.commandCredit = ncmd != 0;
      if (state_.power == Power::ready && !state_.pending && !state_.procedure && !state_.waitingForLinkLoss)
        updateDeadline();
    }
    return true;
  }

  bool commandComplete(Bytes event)
  {
    CommandComplete cc;
    if (!parseCommandComplete(event.data(), event.size(), cc)) return false;
    if (!cc.opcode) return cc.retLen == 0 && creditOnly(cc.ncmd);
    if (cc.opcode != state_.pending || cc.retLen != 1 ||
        (cc.opcode != kReset && cc.opcode != kSetLeEventMask)) return false;
    state_.pending = 0;
    state_.commandCredit = cc.ncmd != 0;
    if (cc.opcode == kReset) {
      if (state_.power == Power::suspendReset || cc.ret[0] != kSuccess) {
        preparePowerDown(cc.ret[0] != kSuccess);
        return true;
      }
      state_.power = Power::wakeMask;
    } else if (cc.ret[0] == kSuccess) {
      state_.eventMask = pendingMask_;
      if (state_.power == Power::wakeMask) state_.power = Power::ready;
    } else if (state_.power == Power::wakeMask) {
      preparePowerDown(true);
      return true;
    }
    pendingMask_ = 0;
    updateDeadline();
    return true;
  }

  bool commandStatus(Bytes event)
  {
    CommandStatus cs;
    if (!parseCommandStatus(event.data(), event.size(), cs)) return false;
    if (!cs.opcode) return creditOnly(cs.ncmd);
    if (cs.opcode != state_.pending || cs.opcode == kReset || cs.opcode == kSetLeEventMask) return false;
    state_.pending = 0;
    state_.commandCredit = cs.ncmd != 0;
    if (isCreate(cs.opcode)) {
      if (cs.status == kSuccess) state_.procedure = cs.opcode;
      else createFailed(cs.status);
    } else if (state_.handle != kNoHandle) {
      // If a disconnect came first, this reply only returns the command credit.
      if (cs.status == kSuccess) state_.procedure = cs.opcode;
      else if (cs.status == kConnectionTimeout) state_.waitingForLinkLoss = true;
    }
    updateDeadline();
    return true;
  }

  bool leEvent(Bytes p)
  {
    if (p.empty()) return false;
    switch (p[0]) {
    case 0x01: return state_.procedure == kCreate && connected(p, 19);
    case 0x0a: return connected(p, 31);
    case 0x29: return connected(p, 34);
    case 0x04: return remoteComplete(kReadRemoteFeatures, p.subspan(1));
    default: return false;
    }
  }

  bool connected(Bytes p, size_t expected)
  {
    if (p.size() != expected || !isCreate(state_.procedure)) return false;
    if (p[1] == kSuccess) {
      const uint16_t handle = le16(p.data() + 2);
      const uint8_t addressType = p[5];
      if (handle > 0x0eff || p[4] != 0 || addressType > 3 ||
          (addressType & 1) != peer_[0] || !std::equal(peer_.begin() + 1, peer_.end(), p.begin() + 6)) return false;
      state_.handle = handle;
      ++state_.linkId;
    } else {
      createFailed(p[1]);   // handle and peer fields are undefined on failure
    }
    state_.procedure = 0;
    updateDeadline();
    return true;
  }

  bool remoteComplete(uint16_t opcode, Bytes p)
  {
    const bool version = opcode == kReadRemoteVersion;
    if (p.size() != (version ? 8u : 11u) || state_.procedure != opcode || !matchesHandle(p.data() + 1)) return false;
    state_.procedure = 0;
    if (p[0] == kSuccess) {
      if (version) {
        state_.versionRead = true;
      } else {
        std::copy_n(p.begin() + 3, state_.features.bytes.size(), state_.features.bytes.begin());
        state_.featuresRead = true;
      }
    } else if (p[0] == kConnectionTimeout) {
      state_.waitingForLinkLoss = true;
    }
    updateDeadline();
    return true;
  }

  bool disconnected(Bytes p)
  {
    if (p.size() != 4 || !matchesHandle(p.data() + 1)) return false;
    if (p[0] != kSuccess) {
      if (state_.procedure != kDisconnect) return false;
      state_.procedure = 0;
      state_.waitingForLinkLoss = p[0] == kConnectionTimeout;
    } else {
      releaseLink();   // Core Vol 4 Part E §4.3: all outstanding ACL buffers for this handle are freed
      if (p[3] == kConnectionTimeout || p[3] == kFailedToEstablish) scheduleRetry();
      else state_.attempts = 0;
    }
    updateDeadline();
    return true;
  }

  bool completedPackets(Bytes p)
  {
    if (p.size() != 5 || p[0] != 1 || !matchesHandle(p.data() + 1)) return false;
    const unsigned count = le16(p.data() + 3);
    if (count > state_.aclInFlight) return false;
    state_.aclInFlight -= count;
    return true;
  }

  bool matchesHandle(const uint8_t *wire) const
  {
    return state_.handle != kNoHandle && le16(wire) == state_.handle;
  }

  bool linkReady(uint64_t linkId) const
  {
    return state_.power == Power::ready && state_.linkId == linkId && state_.handle != kNoHandle &&
           state_.versionRead && state_.featuresRead && !state_.waitingForLinkLoss &&
           state_.pending != kDisconnect && state_.procedure != kDisconnect;
  }

  void createFailed(uint8_t status)
  {
    if (state_.power != Power::ready) return;
    if (status == kConnectionTimeout || status == kFailedToEstablish) scheduleRetry();
    else state_.attempts = kMaxAttempts;
  }

  void scheduleRetry()
  {
    if (state_.power == Power::ready && state_.attempts < kMaxAttempts)
      state_.retryMs = kBackoffMs * state_.attempts;
  }

  void releaseLink()
  {
    state_.handle = kNoHandle;
    state_.procedure = 0;
    state_.aclInFlight = 0;
    state_.security = Security::idle;
    state_.waitingForLinkLoss = false;
    state_.versionRead = state_.featuresRead = false;
    state_.features = {};
  }

  void preparePowerDown(bool forced)
  {
    // On expiry these are host-side discards. Only the ensuing power cycle can
    // reset the controller where an HCI Reset did not complete.
    releaseLink();
    state_.pending = 0;
    state_.commandCredit = false;
    state_.attempts = 0;
    state_.retryMs = 0;
    state_.eventMask = pendingMask_ = 0;
    deadlineMs_ = 0;
    state_.forcedPowerCycle = forced;
    state_.power = Power::powerDownReady;
  }

  mutable std::mutex mutex_;
  Snapshot state_;
  uint64_t deadlineMs_ = 0, pendingMask_ = 0;
  Peer peer_{};
  std::optional<Bond> bond_;
};

}   // namespace pianobt::host
