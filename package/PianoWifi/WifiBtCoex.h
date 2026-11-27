// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "WifiWmi.h"

namespace pianowifi::coex {

// Host fixtures for docs/drivers/wifi-bt-coex.md. The pinned ath12k declares the
// command ID and TLV tag. The payload signature, layout, and weights below are
// local model data. WCN7860 firmware parameter IDs and weights remain unknown.
constexpr uint32_t kWmiCoexConfigCmd = 0x29003;
constexpr uint32_t kWmiTagCoexConfig = 0x20a;

enum class WifiBand { Unknown, GHz2_4, GHz5, GHz6 };
enum class WifiWork { Idle, Traffic, Beacon, Rejoin };
enum class BtScan { None, Inquiry, LeScan };
enum class Grant { Idle, Wifi, Bluetooth, Both };

struct PriorityWeights {
  uint32_t wifi, bt;
};

constexpr unsigned kWifiWorkCount = static_cast<unsigned>(WifiWork::Rejoin) + 1;
struct PriorityTable {
  PriorityWeights byWifiWork[kWifiWorkCount];
};

// Rows follow WifiWork. Discovery gets preference over ordinary Wi-Fi traffic,
// with beacon service and rejoin work taking precedence under contention.
constexpr PriorityTable kDiscoveryWeights{{{0, 2}, {1, 2}, {3, 2}, {3, 2}}};
struct BtActiveWeights {
  PriorityTable inquiry = kDiscoveryWeights;
  PriorityTable leScan = kDiscoveryWeights;
};

struct PolicyInput {
  WifiBand operationBand = WifiBand::Unknown;  // actual work band, including off-channel work
  WifiWork wifi = WifiWork::Idle;
  BtScan bt = BtScan::None;
  bool stationAuthorized = false;
  bool psRequested = false;
  bool inactivityElapsed = false;
};

struct Decision {
  Grant grant;
  bool stationDozeEligible;
};

// Rejoin denotes pending WLAN work. Native join authorization is handled by
// the caller. The band rule covers direct spectral overlap only.
inline Decision arbitrate(const PolicyInput &input, const BtActiveWeights &weights = {})
{
  if (input.wifi == WifiWork::Idle) {
    const bool doze = input.stationAuthorized && input.psRequested && input.inactivityElapsed;
    return {input.bt == BtScan::None ? Grant::Idle : Grant::Bluetooth, doze};
  }
  if (input.bt == BtScan::None) return {Grant::Wifi, false};
  if (input.operationBand == WifiBand::GHz5 || input.operationBand == WifiBand::GHz6) return {Grant::Both, false};

  const PriorityTable &table = input.bt == BtScan::Inquiry ? weights.inquiry : weights.leScan;
  const PriorityWeights &row = table.byWifiWork[static_cast<unsigned>(input.wifi)];
  return {row.wifi >= row.bt ? Grant::Wifi : Grant::Bluetooth, false};
}

constexpr unsigned kMaxParameterWords = 16;  // fixture storage limit
struct ConfigFixture {
  uint32_t parameterCount = 0;
  uint32_t parameters[kMaxParameterWords] = {};
  BtActiveWeights weights;
};

// One coex TLV, with little-endian words: "HOST", parameter count, opaque
// parameter words, then fixed inquiry and LE scan tables of (WLAN, BT) pairs.
constexpr uint32_t kFixtureSignature = 0x54534f48;
constexpr uint32_t kFixtureFixedWords = 2 + 2 * kWifiWorkCount * 2;
constexpr uint32_t kFixtureMinBytes = kWmiCmdHeaderBytes + kWmiTlvHeaderBytes + 4 * kFixtureFixedWords;
constexpr uint32_t kFixtureMaxBytes = kFixtureMinBytes + 4 * kMaxParameterWords;
static_assert(4 * (kFixtureFixedWords + kMaxParameterWords) <= kWmiTlvLenMask);

// Return the encoded length, or zero on failure. Validate before writing.
inline uint32_t encodeConfigFixture(uint8_t *buffer, uint32_t capacity, const ConfigFixture &config)
{
  if (config.parameterCount > kMaxParameterWords || !buffer || capacity < kFixtureMinBytes + 4 * config.parameterCount)
    return 0;

  WmiWriter writer;
  wmiBegin(writer, buffer, capacity, kWmiCoexConfigCmd);
  uint8_t *value = wmiStruct(writer, kWmiTagCoexConfig, kFixtureFixedWords + config.parameterCount);
  wmiPut(value, 0, kFixtureSignature);
  wmiPut(value, 1, config.parameterCount);
  uint32_t word = 2;
  for (uint32_t i = 0; i < config.parameterCount; ++i) wmiPut(value, word++, config.parameters[i]);
  const PriorityTable *tables[] = {&config.weights.inquiry, &config.weights.leScan};
  for (const auto *table : tables)
    for (const auto &row : table->byWifiWork) {
      wmiPut(value, word++, row.wifi);
      wmiPut(value, word++, row.bt);
    }
  return wmiEnd(writer);
}

// Accept only a complete fixture, leaving the destination intact on failure.
inline bool decodeConfigFixture(ConfigFixture &out, const uint8_t *buffer, uint32_t length)
{
  if (!buffer || length < kFixtureMinBytes || length > kFixtureMaxBytes || wmiGet(buffer, 0) != kWmiCoexConfigCmd)
    return false;

  const uint8_t *cursor = buffer + kWmiCmdHeaderBytes;
  uint32_t remaining = length - kWmiCmdHeaderBytes;
  WmiTlvRef tlv;
  if (!wmiNext(cursor, remaining, tlv) || remaining || tlv.tag != kWmiTagCoexConfig || tlv.length < 4 * kFixtureFixedWords ||
      wmiGet(tlv.value, 0) != kFixtureSignature)
    return false;

  const uint32_t count = wmiGet(tlv.value, 1);
  if (count > kMaxParameterWords || tlv.length != 4 * (kFixtureFixedWords + count)) return false;

  ConfigFixture decoded;
  decoded.parameterCount = count;
  uint32_t word = 2;
  for (uint32_t i = 0; i < count; ++i) decoded.parameters[i] = wmiGet(tlv.value, word++);
  PriorityTable *tables[] = {&decoded.weights.inquiry, &decoded.weights.leScan};
  for (auto *table : tables)
    for (auto &row : table->byWifiWork) {
      row.wifi = wmiGet(tlv.value, word++);
      row.bt = wmiGet(tlv.value, word++);
    }
  out = decoded;
  return true;
}

enum class Client { Wifi, Bluetooth };
enum class RadioState { Off, Quiesced, Active, AsleepWithVote, AsleepWithoutVote };
enum class LastPut { RetainPower, WithdrawPower };
enum class SleepVote { Retain, Drop };

// Serialized model of the shared owner bits in PianoWcnRails.h. One vote per
// radio. Each radio's transport and enable must be quiesced before a put.
// Power represents the common rail group, with optional preexisting power.
class SharedRails {
public:
  explicit SharedRails(bool preexistingPower = false) : preexistingPower_(preexistingPower), powered_(preexistingPower) {}

  RadioState state(Client client) const
  {
    const unsigned index = static_cast<unsigned>(client);
    return index < kClientCount ? states_[index] : RadioState::Off;
  }

  bool hasVote(Client client) const
  {
    const auto current = state(client);
    return current == RadioState::Quiesced || current == RadioState::Active || current == RadioState::AsleepWithVote;
  }

  unsigned votes() const { return unsigned(hasVote(Client::Wifi)) + unsigned(hasVote(Client::Bluetooth)); }
  bool powered() const { return powered_; }

  bool acquire(Client client)
  {
    const auto current = state(client);
    if (current == RadioState::Quiesced || current == RadioState::Active) return true;
    if (!transition(client, RadioState::Off, RadioState::Quiesced)) return false;
    powered_ = true;
    return true;
  }

  bool activate(Client client) { return transition(client, RadioState::Quiesced, RadioState::Active); }
  bool quiesce(Client client) { return transition(client, RadioState::Active, RadioState::Quiesced); }

  bool release(Client client, LastPut lastPut)
  {
    if (!transition(client, RadioState::Quiesced, RadioState::Off)) return false;
    withdrawIfUnowned(lastPut);
    return true;
  }

  bool sleep(Client client, SleepVote vote, LastPut lastPut)
  {
    const bool retain = vote == SleepVote::Retain;
    if (!transition(client, RadioState::Quiesced, retain ? RadioState::AsleepWithVote : RadioState::AsleepWithoutVote))
      return false;
    if (!retain) withdrawIfUnowned(lastPut);
    return true;
  }

  bool wake(Client client)
  {
    const auto current = state(client);
    if (current != RadioState::AsleepWithVote && current != RadioState::AsleepWithoutVote) return false;
    if (!transition(client, current, RadioState::Quiesced)) return false;
    powered_ = true;
    return true;
  }

private:
  bool transition(Client client, RadioState from, RadioState to)
  {
    const unsigned index = static_cast<unsigned>(client);
    if (index >= kClientCount || states_[index] != from) return false;
    states_[index] = to;
    return true;
  }

  void withdrawIfUnowned(LastPut lastPut)
  {
    if (!votes() && lastPut == LastPut::WithdrawPower && !preexistingPower_) powered_ = false;
  }

  static constexpr unsigned kClientCount = 2;
  RadioState states_[kClientCount] = {};
  bool preexistingPower_;
  bool powered_;
};

}  // namespace pianowifi::coex
