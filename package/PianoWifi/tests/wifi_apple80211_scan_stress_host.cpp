// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. wifi_apple80211_scan_stress_host.cpp -o /tmp/wifi_apple80211_scan_stress_host && /tmp/wifi_apple80211_scan_stress_host
#include "WifiApple80211.h"
#include "WifiUserControl.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace {
using namespace pianowifi;
namespace apple = pianowifi::a80211;
using Bytes = std::span<const uint8_t>;
using Bssid = std::array<uint8_t, 6>;

constexpr uint16_t kSecured = kDot11CapEss | kDot11CapPrivacy | kDot11CapShortSlot;
constexpr uint8_t kName[] = {'s', 'c', 'a', 'n', '-', 'h', 'o', 's', 't'};
constexpr uint8_t kWpa[] = {221, 22, 0, 0x50, 0xf2, 1, 1, 0, 0, 0x50, 0xf2, 2, 1, 0, 0, 0x50, 0xf2, 2,
                           1, 0, 0, 0x50, 0xf2, 2};
constexpr uint8_t kWpa2[] = {48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0};
constexpr uint8_t kTransition[] = {48, 24, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 4,
                                  2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 8, 0x80, 0};
constexpr uint8_t kSae[] = {48, 26, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8,
                            0xc0, 0, 0, 0, 0, 0x0f, 0xac, 6}; // PMF required/capable, no PMKIDs, BIP-CMAC-128
constexpr uint8_t kOweAkm = 18;
constexpr uint8_t kOwe[] = {48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, kOweAkm, 0xc0, 0};

struct Channel {
  uint8_t number;
  uint32_t freq;
};

constexpr Channel kChannels[] = {{1, 2412}, {36, 5180}, {5, 5975}, {11, 2462}, {149, 5745}, {37, 6135}};

Bssid bssid(unsigned id)
{
  return {2, 0, 0, 0, uint8_t(id >> 8), uint8_t(id)};
}

ScanEntry *hear(ScanList &list, unsigned id, Channel channel, int32_t rssi, bool probe, Bytes ssid,
                uint16_t capability = kDot11CapEss, Bytes elements = {}, Bytes tail = {})
{
  assert(ssid.size() <= 255);
  constexpr auto fixed = kDot11HeaderBytes + kDot11BeaconFixedBytes;
  constexpr uint8_t rates[] = {kDot11ElementRates, 1, 0x8c};
  const size_t dsBytes = channel.freq < 2500 ? 3 : 0;
  // A size-constructed buffer puts the final IE boundary next to the ASan redzone.
  std::vector<uint8_t> frame(fixed + 2 + ssid.size() + sizeof rates + dsBytes + elements.size() + tail.size());
  frame[0] = probe ? kDot11SubtypeProbeResponse : kDot11SubtypeBeacon;
  const auto mac = bssid(id);
  std::copy(mac.begin(), mac.end(), frame.begin() + 16);
  frame[fixed - 4] = 100;
  frame[fixed - 2] = uint8_t(capability);
  frame[fixed - 1] = uint8_t(capability >> 8);
  frame[fixed] = kDot11ElementSsid;
  frame[fixed + 1] = uint8_t(ssid.size());
  auto at = std::copy(ssid.begin(), ssid.end(), frame.begin() + fixed + 2);
  at = std::copy(std::begin(rates), std::end(rates), at);
  if (dsBytes) {
    *at++ = kDot11ElementDsParams;
    *at++ = 1;
    *at++ = channel.number;
  }
  at = std::copy(elements.begin(), elements.end(), at);
  std::copy(tail.begin(), tail.end(), at);
  WmiMgmtRx rx{};
  rx.frame = frame.data();
  rx.bytes = uint32_t(frame.size());
  rx.channel = channel.number;
  rx.freq = channel.freq;
  rx.signalDbm = rssi;
  return scanAdd(list, rx);
}

struct FrontEnd {
  apple::State state{};
  alignas(apple::ScanResult) uint8_t scratch[apple::kScratchBytes];

  FrontEnd() { apple::reset(state); }

  apple::Answer ask(bool set, int type, void *buffer = nullptr, uint32_t size = 0, int32_t *value = nullptr)
  {
    // Model copyin/copyout with the caller's actual buffer extent.
    const apple::UserIo io{
        &size,
        [](void *ctx, uint64_t from, void *to, uint32_t n) {
          if (!from || n > *static_cast<uint32_t *>(ctx)) return apple::kErrFault;
          std::memcpy(to, reinterpret_cast<const void *>(from), n);
          return 0;
        },
        [](void *ctx, const void *from, uint64_t to, uint32_t n) {
          if (!to || n > *static_cast<uint32_t *>(ctx)) return apple::kErrFault;
          std::memcpy(reinterpret_cast<void *>(to), from, n);
          return 0;
        }};
    apple::Req rq{};
    rq.type = type;
    rq.length = size;
    rq.data = reinterpret_cast<uintptr_t>(buffer);
    const auto answer = apple::handle(state, {"host", "model", "en0"}, set, rq, io, scratch);
    if (value) *value = rq.value;
    return answer;
  }

  void publish(const ScanList &list)
  {
    // In the kexts this copy crosses the IOKit registry in list order.
    state.scan.count = list.count;
    assert(list.count <= kScanEntriesMax);
    for (uint32_t i = 0; i < list.count; ++i) {
      const auto &entry = list.entries[i];
      auto &bss = state.scan.bss[i];
      bss = {};
      std::memcpy(bss.bssid, entry.bssid, sizeof bss.bssid);
      bss.ssidLength = entry.ssidLength;
      std::memcpy(bss.ssid, entry.ssidRaw, bss.ssidLength);
      bss.hidden = entry.hidden;
      bss.channel = entry.channel;
      bss.freq = entry.freq;
      bss.rssi = entry.rssi;
      bss.capability = entry.capability;
      bss.beaconInterval = entry.beaconInterval;
      bss.rateCount = entry.rateCount;
      std::memcpy(bss.rates, entry.rates, bss.rateCount);
      bss.rsnLength = entry.rsnElementLength;
      std::memcpy(bss.rsn, entry.rsnElement, bss.rsnLength);
    }
  }

  apple::ScanResult readBss(int type)
  {
    state.nowNs += 1234567;
    std::array<uint8_t, apple::kScanResultBytes27> wire;
    wire.fill(0xa5);
    const auto answer = ask(false, type, wire.data(), uint32_t(wire.size()));
    assert(!answer.error && answer.wrote == wire.size());
    uint64_t timestamp = 0;
    for (unsigned i = 0; i < sizeof timestamp; ++i) timestamp |= uint64_t(wire[apple::kScanResultTimestampAt + i]) << (8 * i);
    assert(timestamp == state.nowNs);
    assert(wire[apple::kScanResultTimestampAt - 1] == 0 && wire.back() == 0);
    apple::ScanResult result;
    std::memcpy(&result, wire.data(), sizeof result);
    assert(result.version == 1 && result.ageMs == 0);
    return result;
  }
};

bool equal(Bytes a, Bytes b) { return std::ranges::equal(a, b); }

void checkBss(const apple::ScanResult &record, const ScanEntry &entry, Bytes rsn = {})
{
  assert(equal(record.bssid, entry.bssid));
  assert(record.ssidLength == entry.ssidLength && equal({record.ssid, record.ssidLength}, {entry.ssidRaw, entry.ssidLength}));
  assert(record.channel.channel == entry.channel && record.rssi == entry.rssi);
  assert(record.capability == entry.capability);
  assert(record.elementsLength <= sizeof record.elements);
  unsigned ssids = 0, rsns = 0;
  for (unsigned at = 0; at < record.elementsLength;) {
    assert(record.elementsLength - at >= 2);
    const uint8_t *element = record.elements + at;
    const unsigned bytes = 2u + element[1];
    assert(bytes <= record.elementsLength - at);
    if (element[0] == kDot11ElementSsid) {
      ++ssids;
      assert(equal({element + 2, bytes - 2}, {entry.ssidRaw, entry.ssidLength}));
    }
    if (element[0] == kDot11ElementRsn) {
      ++rsns;
      assert(equal({element, bytes}, rsn));
    }
    at += bytes;
  }
  assert(ssids == 1 && rsns == (rsn.empty() ? 0u : 1u));
}

void testDenseScan()
{
  constexpr unsigned bursts = 10;
  constexpr unsigned total = bursts * kScanEntriesMax;
  static_assert(total >= 256);
  ScanList list{};
  FrontEnd front;
  apple::ScanCompletions completions;
  front.state.nowNs = 0x123456789abcdef0ull;
  const auto pinned = bssid(0);
  std::copy(pinned.begin(), pinned.end(), list.keepBssid);
  list.keepSet = true;
  for (unsigned burst = 0; burst < bursts; ++burst) {
    scanRoundBegin(list);
    const unsigned start = burst * kScanEntriesMax;
    for (unsigned i = 0; i < kScanEntriesMax; ++i) {
      const unsigned id = start + i;
      const auto channel = kChannels[id % std::size(kChannels)];
      const int32_t rssi = -90 + int32_t(i);
      const auto *entry = hear(list, id, channel, rssi, id % 2 != 0, kName);
      assert(entry && entry->channel == channel.number && entry->freq == channel.freq && entry->heardFreq == channel.freq);
      assert(!entry->offChannel && entry->heardRound == list.round && entry->rssi == rssi);
    }
    assert(list.count == kScanEntriesMax && list.evicted == start && !list.dropped);
    assert(equal(list.entries[0].bssid, pinned));
    std::array<bool, kScanEntriesMax> seen{};
    for (unsigned i = 1; i < list.count; ++i) {
      const auto &entry = list.entries[i];
      const unsigned id = unsigned(entry.bssid[4]) << 8 | entry.bssid[5];
      assert(id > start && id < start + kScanEntriesMax && !seen[id - start]);
      seen[id - start] = true;
    }

    front.publish(list);
    const auto request = front.ask(true, apple::kScanReq);
    assert(!request.error && request.effects == apple::kDoScanStarted);
    assert(completions.start() == (burst == 0));
    // A missing output buffer must leave the first result available for a retry.
    assert(front.ask(false, apple::kScanResult).error == apple::kErrInval && front.state.cursor == 0);
    for (unsigned i = 0; i < list.count; ++i) checkBss(front.readBss(apple::kScanResult), list.entries[i]);
    assert(front.ask(false, apple::kScanResult).error == apple::kErrEndOfResults);
    assert(front.state.cursor == 0 && front.state.listEnds == burst + 1);
  }
  assert(list.frames == total && list.beacons == total / 2 && list.probes == total / 2);
  assert(front.state.resultsServed == total && front.state.scanRequests == bursts);
  std::vector<uint32_t> events;
  const auto post = [&](uint32_t code) { events.push_back(code); };
  apple::postScanCompletions(completions.take(), front.state.power, post);
  assert(events.size() == 2 * bursts && completions.take() == 0);
  for (unsigned i = 0; i < bursts; ++i) {
    assert(events[2 * i] == 15 && events[2 * i + 1] == 10);
  }
  assert(!hear(list, total, kChannels[0], -95, false, kName));
  assert(list.dropped == 1 && list.count == kScanEntriesMax);

  auto *entry = hear(list, total - 1, kChannels[(total - 1) % std::size(kChannels)], -35, false, kName);
  assert(entry && entry->rssi == -35 && entry->beacons == 1 && entry->probes == 1);
  assert(hear(list, total - 1, kChannels[0], -70, true, kName) == entry && entry->rssi == -35);
  assert(entry->probes == 2 && entry->freq == kChannels[0].freq && list.count == kScanEntriesMax);
  scanRoundBegin(list);
  assert(scanExpire(list, 2, nullptr, pinned.data()) == 0); // A deaf round keeps the cache.
  scanRoundBegin(list);
  assert(hear(list, total - 1, kChannels[0], -80, false, kName) == entry);
  assert(entry->rssi == -80 && entry->heardRound == list.round);
  assert(scanExpire(list, 2, nullptr, pinned.data()) == kScanEntriesMax - 2);
  assert(list.count == 2 && equal(list.entries[0].bssid, pinned) && list.entries[1].rssi == -80);
  front.publish(list);
  assert(front.ask(true, apple::kScanReqMultiple).effects == apple::kDoScanStarted);
  assert(completions.start());
  checkBss(front.readBss(apple::kScanResult), list.entries[0]);
  checkBss(front.readBss(apple::kScanResult), list.entries[1]);
  events.clear();
  apple::postScanCompletions(completions.take(), true, post);
  assert((events == std::vector<uint32_t>{15, 10}));
}

void testSsidAndFraming()
{
  FrontEnd front;
  std::array<uint8_t, 255> name;
  for (unsigned i = 0; i < name.size(); ++i) name[i] = uint8_t(0xfe + i);
  for (unsigned length = 0; length <= 33; ++length) {
    ScanList list{};
    const auto *entry = hear(list, length, kChannels[0], -55, false, Bytes(name).first(length));
    assert(entry && entry->hidden == (length == 0));
    const unsigned accepted = length <= 32 ? length : 0;
    assert(entry->ssidLength == accepted && entry->ssid[accepted] == 0);
    assert(equal({entry->ssidRaw, accepted}, Bytes(name).first(accepted)));
    front.publish(list);
    assert(!front.ask(true, apple::kScanReq).error);
    checkBss(front.readBss(apple::kScanResult), *entry);
  }

  constexpr uint8_t allZero[32] = {};
  for (const Bytes hidden : {Bytes{}, Bytes(allZero)}) {
    ScanList list{};
    const auto *entry = hear(list, 1, kChannels[0], -75, false, hidden, kSecured, kWpa2);
    assert(entry && entry->hidden && entry->ssidLength == hidden.size());
    assert(hear(list, 1, kChannels[0], -60, true, Bytes(name).first(32), kSecured, kWpa2) == entry);
    assert(!entry->hidden && entry->ssidLength == 32 && equal(entry->ssidRaw, Bytes(name).first(32)));
    assert(hear(list, 1, kChannels[0], -59, false, hidden, kSecured) == entry);
    assert(hear(list, 1, kChannels[0], -50, true, name, kSecured) == entry);
    assert(list.count == 1 && entry->beacons == 2 && entry->probes == 2 && entry->rssi == -50);
    assert(!entry->hidden && entry->ssidLength == 32 && equal(entry->ssidRaw, Bytes(name).first(32)));
    front.publish(list);
    assert(!front.ask(true, apple::kScanReq).error);
    checkBss(front.readBss(apple::kScanResult), *entry, kWpa2);
  }

  constexpr uint8_t zeroLength[] = {221, 0, 48, 0};
  constexpr uint8_t shortVendor[] = {221, 3, 0, 0x50, 0xf2};
  constexpr uint8_t cutVendor[] = {221, 4, 0, 0x50, 0xf2};
  constexpr uint8_t cutRsn[] = {48, 20, 1, 0, 0};
  constexpr uint8_t oddTail[] = {48};
  for (const Bytes tail : {Bytes(zeroLength), Bytes(shortVendor), Bytes(cutVendor), Bytes(cutRsn), Bytes(oddTail)}) {
    ScanList list{};
    const auto *entry = hear(list, 1, kChannels[1], -62, false, {}, kSecured, {}, tail);
    assert(entry && entry->hidden && !entry->ssidLength && !entry->security.rsn && !entry->security.wpa1);
    assert(hear(list, 1, kChannels[1], -40, true, kName, kSecured, kWpa2, tail) == entry);
    assert(list.count == 1 && entry->security.rsn && !entry->security.truncated && !entry->security.wpa1);
    front.publish(list);
    assert(!front.ask(true, apple::kScanReq).error);
    checkBss(front.readBss(apple::kScanResult), *entry, kWpa2);
  }

  // Complete IE framing with an incomplete RSN suite is still rejected by the reply builder.
  constexpr uint8_t shortSuite[] = {48, 5, 1, 0, 0, 0x0f, 0xac};
  ScanList list{};
  const auto *entry = hear(list, 1, kChannels[1], -44, false, kName, kSecured, shortSuite);
  assert(entry && entry->security.rsn && entry->security.truncated);
  front.publish(list);
  assert(!front.ask(true, apple::kScanReq).error);
  checkBss(front.readBss(apple::kScanResult), *entry);
}

void testAdvertisedSecurity()
{
  constexpr uint32_t psk = 1u << kRsnAkmPsk, sae = 1u << kRsnAkmSae;
  constexpr uint16_t pmf = kRsnCapMfpRequired | kRsnCapMfpCapable;
  const struct {
    Bytes ies, rsn;
    uint32_t akm;
    uint16_t caps;
    uint8_t group;
    bool wpa;
  } cases[] = {{kWpa, {}, 0, 0, 0, true},
               {kWpa2, kWpa2, psk, 0, kRsnCipherCcmp, false},
               {kTransition, kTransition, psk | sae, kRsnCapMfpCapable, kRsnCipherTkip, false},
               {kSae, kSae, sae, pmf, kRsnCipherCcmp, false},
               {kOwe, kOwe, 1u << kOweAkm, pmf, kRsnCipherCcmp, false}};

  ScanList list{};
  FrontEnd front;
  for (unsigned i = 0; i < std::size(cases); ++i) {
    const auto &test = cases[i];
    const auto *entry = hear(list, i, kChannels[i], -53, i % 2 != 0, kName, kSecured, test.ies);
    assert(entry && !entry->security.truncated && entry->security.wpa1 == test.wpa);
    assert(entry->security.rsn == !test.rsn.empty() && entry->security.akm == test.akm);
    assert(entry->security.rsnCaps == test.caps && entry->security.groupCipher == test.group);
    assert(entry->security.pairwise == (test.rsn.empty() ? 0u : 1u << kRsnCipherCcmp));
    assert(equal({entry->rsnElement, entry->rsnElementLength}, test.rsn));
  }
  // A mixed WPA/WPA2 advertisement carries both the vendor IE and the RSN IE.
  const auto *mixed = hear(list, 5, kChannels[1], -43, false, kName, kSecured, kTransition, kWpa);
  assert(mixed && mixed->security.rsn && mixed->security.wpa1 && mixed->security.akm == (psk | sae));
  front.publish(list);
  assert(!front.ask(true, apple::kScanReq).error);
  for (unsigned i = 0; i < std::size(cases); ++i)
    checkBss(front.readBss(apple::kScanResult), list.entries[i], cases[i].rsn);
  checkBss(front.readBss(apple::kScanResult), *mixed, kTransition);
  assert(front.ask(false, apple::kScanResult).error == apple::kErrEndOfResults);
}

piano_wifi_assoc_snapshot radioSnapshot(uint32_t generation, const ScanEntry *entry = nullptr, uint32_t result = 0,
                                        bool power = true)
{
  piano_wifi_assoc_snapshot raw{};
  raw.version = PIANO_WIFI_ASSOC_SNAPSHOT_VERSION;
  raw.join_generation = generation;
  raw.power = power;
  raw.associate_result = result;
  if (entry) {
    raw.associated = true;
    raw.authorized = result == 1;
    raw.state = raw.authorized ? apple::kStateRun : apple::kStateAssoc;
    raw.auth_type = entry->security.rsn ? 2 : 1;
    raw.ssid_length = entry->ssidLength;
    std::memcpy(raw.ssid, entry->ssidRaw, raw.ssid_length);
    std::memcpy(raw.bssid, entry->bssid, sizeof raw.bssid);
    raw.channel = entry->channel;
    raw.frequency_mhz = entry->freq;
    raw.rssi_dbm = entry->rssi;
    raw.capability = entry->capability;
    raw.rsn_length = entry->rsnElementLength;
    std::memcpy(raw.rsn, entry->rsnElement, raw.rsn_length);
  }
  return raw;
}

void testCurrentNetwork()
{
  ScanList list{};
  FrontEnd front;
  const auto *first = hear(list, 1, kChannels[1], -45, false, kName, kSecured, kWpa2);
  assert(first);
  front.publish(list);
  apple::observeAssociation(front.state, radioSnapshot(1, first, 1), true);
  checkBss(front.readBss(apple::kCurrentNetwork), *first, kWpa2);
  assert(!front.ask(true, apple::kScanCacheClear).error);
  assert(front.ask(false, apple::kScanResult).error == apple::kErrEndOfResults);
  checkBss(front.readBss(apple::kCurrentNetwork), *first, kWpa2);

  // The second AP advertises WPA2-PSK plus SAE and optional PMF. The
  // worker has selected it before the front end has taken another scan.
  const auto *roam = hear(list, 2, kChannels[4], -35, true, kName, kSecured, kTransition);
  assert(roam);
  const auto old = apple::observeAssociation(front.state, radioSnapshot(2, roam, 1), true);
  const auto events = apple::associationEvents(old, front.state.association, false);
  assert(events.count == 4 && events.codes[0] == apple::kEvAssocDone && events.codes[1] == apple::kEvLinkChanged);
  checkBss(front.readBss(apple::kCurrentNetwork), *roam, kTransition);

  // The same BSSID's cached advertisement can also be older than the one
  // selected by the worker at wake. The current BSS owns its security.
  front.publish(list);
  apple::observeAssociation(front.state, radioSnapshot(2, nullptr, 2, false), true);
  assert(front.ask(false, apple::kCurrentNetwork).error == apple::kErrNotAssociated);
  assert(hear(list, 2, kChannels[4], -37, true, kName, kSecured, kWpa2) == roam);
  apple::observeAssociation(front.state, radioSnapshot(3, roam, 1), true);
  checkBss(front.readBss(apple::kCurrentNetwork), *roam, kWpa2);

  ScanList afterWake{};
  const auto *open = hear(afterWake, 2, kChannels[4], -40, false, kName);
  assert(open);
  apple::observeAssociation(front.state, radioSnapshot(4, open, 1), true);
  checkBss(front.readBss(apple::kCurrentNetwork), *open);
}

void checkWait(const piano_wifi_assoc_snapshot &raw, uint32_t generation, uint64_t elapsedNs, const char *expected)
{
  const char *outcome = apple::associateWaitOutcome(raw, generation, elapsedNs);
  assert(outcome && std::strcmp(outcome, expected) == 0);
}

void testAssociateDeadline()
{
  constexpr uint64_t nsPerMs = 1000000;
  static_assert(apple::kAssociateWaitMs == 8000 && apple::kAssociateWaitStepMs == 20);
  auto pending = radioSnapshot(1);
  for (unsigned elapsed = apple::kAssociateWaitStepMs; elapsed < apple::kAssociateWaitMs; elapsed += apple::kAssociateWaitStepMs)
    assert(!apple::associateWaitOutcome(pending, 1, elapsed * nsPerMs));
  checkWait(pending, 1, apple::kAssociateWaitMs * nsPerMs, "timeout");
  pending.associated = true;
  checkWait(pending, 1, apple::kAssociateWaitMs * nsPerMs, "associated, not yet authorized");
  checkWait(radioSnapshot(1, nullptr, 2), 1, nsPerMs, "failed");
  checkWait(radioSnapshot(1, nullptr, 9), 1, nsPerMs, "timed out");
  checkWait(radioSnapshot(1, nullptr, 2, false), 1, nsPerMs, "power off");
}

void checkDefaults(FrontEnd &front)
{
  assert(front.state.power && front.state.sheng == 0);
  assert(front.state.accept == apple::kAcceptInfraEnumerated);
  assert(front.state.gaps == (apple::kGapChannelsInfo | apple::kGapCountryCode));
  assert(front.ask(true, apple::kSuspendInterface).error == apple::kErrUnsupported);
  assert(front.ask(false, apple::kVirtualIfRole).error == apple::kErrUnsupported);
  uint8_t capabilities[apple::kCapBytes]{};
  const auto caps = front.ask(false, apple::kCardCapabilities, capabilities, sizeof capabilities);
  assert(!caps.error && caps.wrote == sizeof capabilities);
  for (const unsigned bit : {1u, 13u, 14u}) assert(capabilities[bit / 8] & (1u << (bit % 8)));
  assert(!(capabilities[46 / 8] & (1u << (46 % 8))));
}

void testRapidRequests()
{
  using namespace apple;
  FrontEnd front;
  checkDefaults(front);
  ScanList list{};
  const auto *ap = hear(list, 1, kChannels[1], -42, false, kName, kSecured, kTransition);
  assert(ap);

  WifiUserControl intent;
  WifiJoinRequest native;
  WifiBootPower bootPower;
  bootPower.enabled = false; // daily native-boot policy
  native.cancel();
  uint32_t restart = 1, generation = 0;
  ScanCompletions scans;
  int available = -1;
  bool adopted = false;
  auto publish = [&](const piano_wifi_assoc_snapshot &raw, bool usable, bool emit = true, bool acceptedPower = false) {
    const bool powerChanged = powerEventRequired(front.state, raw.power, acceptedPower);
    const auto old = observeAssociation(front.state, raw, emit);
    std::vector<uint32_t> events;
    if (!emit) return events;
    if (powerChanged) events.push_back(kEvPowerChanged);
    if (driverAvailabilityEventRequired(adopted, available, usable, false)) {
      available = usable;
      const auto record = driverAvailabilityRecord(usable);
      const auto payload = bsdEventPayload(kEvDriverAvailable, {}, false, nullptr, &record);
      assert(payload.size == 0xf8 && static_cast<const DriverAvailability *>(payload.data)->available == uint32_t(usable));
      events.push_back(kEvDriverAvailable);
    }
    const auto association = associationEvents(old, front.state.association, raw.user_stop);
    events.insert(events.end(), association.codes, association.codes + association.count);
    return events;
  };
  assert(publish(radioSnapshot(0), true).empty());
  adopted = true;
  assert(publish(radioSnapshot(0), true) == std::vector<uint32_t>{55});

  AssociateRequest27 saved{};
  saved.version = 1;
  saved.mode = 2;
  saved.auth_lower = 1;
  saved.auth_upper = 0x08;
  saved.network.ssid_length = sizeof kName;
  std::memcpy(saved.network.ssid, kName, sizeof kName);
  saved.network.key_length = 8;
  saved.network.key_cipher = 10;
  std::memset(saved.network.key, '0', saved.network.key_length);
  auto set20 = [&](uint64_t now) {
    auto answer = front.ask(true, kAssociate, &saved, sizeof saved);
    assert(!answer.error && answer.associationProfile && answer.associationProfileData.security == PianoWifiSecWpa2Psk);
    assert(answer.associationProfileData.ssid_len == sizeof kName);
    assert(equal({answer.associationProfileData.ssid, sizeof kName}, kName));
    piano_wifi_profile_wipe(&answer.associationProfileData);
    assert(intent.join());
    native.accept(++generation, now, restart);
    return generation;
  };
  auto set19 = [&](bool on) {
    auto power = powerRecord(on);
    const auto answer = front.ask(true, kPower, &power, sizeof power);
    assert(!answer.error && answer.effects == kDoPowerChanged && answer.powerOn == on);
    assert(bootPower.setPower(intent, answer.powerOn));
    if (!on) native.cancel();
    return intent.read();
  };

  // Advance the worker and request threads at explicit boundaries. The
  // schedule exercises wait progress and stop acknowledgments without wall-clock sleeps.
  constexpr unsigned rounds = 128;
  for (unsigned i = 0; i < rounds; ++i) {
    const uint64_t now = 100 * i;
    front.publish(list);
    assert(front.ask(true, kScanReq).effects == kDoScanStarted && scans.start());
    const uint32_t stale = set20(now);
    assert(intent.allowed(intent.read()) && native.canStart(now));
    native.consume();
    assert(publish(radioSnapshot(stale), true).empty());
    assert(!associateWaitOutcome(radioSnapshot(stale), stale, 20000000));

    const bool deauth = i % 2 != 0;
    uint32_t leave[4] = {1, 3, 1, 0};
    const uint32_t leaveBytes = deauth ? sizeof leave : sizeof leave - sizeof leave[0];
    const auto left = front.ask(true, deauth ? kDeauth : kDisassociate, leave, leaveBytes);
    assert(!left.error && left.effects == kDoDisassociate);
    assert(front.state.cacheCleared == !deauth);
    native.cancel();
    const uint32_t stop = intent.disconnect();
    assert(intent.pending(stop) && !native.finish(stale, 1, now + 1, restart));

    const uint32_t active = set20(now + 2);
    assert(active == stale + 1 && !intent.allowed(intent.read()));
    assert(WifiUserControl::serial(intent.read()) == WifiUserControl::serial(stop));
    assert(!native.finish(stale, 1, now + 3, restart));
    assert(publish(radioSnapshot(active), true).empty());
    checkWait(radioSnapshot(active), stale, 40000000, "superseded");
    assert(front.ask(false, kCurrentNetwork).error == kErrNotAssociated);
    intent.stopped(stop);
    assert(intent.allowed(intent.read()) && native.canStart(now + 4));
    native.consume();
    assert(native.currentReceipt(now + 4, restart, stale, true, true) == 0);
    assert(native.currentReceipt(now + 4, restart, active, true, false) == active);

    const auto associated = radioSnapshot(active, ap);
    assert(publish(associated, true, false).empty()); // GET poll cannot consume ASSOC_DONE.
    int32_t result = -1;
    assert(!front.ask(false, kAssociateResult, nullptr, 0, &result).error && result == 0);
    assert(publish(associated, true) == std::vector<uint32_t>{kEvAssocDone});
    checkBss(front.readBss(kCurrentNetwork), *ap, kTransition);
    assert(!associateWaitOutcome(associated, active, 20000000));

    assert(native.finish(active, 1, now + 5, restart));
    assert(!native.finish(stale, 1, now + 6, restart));
    const auto authorized = radioSnapshot(active, ap, native.result(now + 6));
    assert((publish(authorized, true) == std::vector<uint32_t>{kEvAssocDone, kEvLinkChanged, kEvSsidChanged, kEvBssidChanged}));
    assert(publish(authorized, true).empty());
    assert(front.state.eventAssociation.generation == active);
    checkWait(authorized, active, 40000000, "authorized");
    assert(!front.ask(false, kAssociateResult, nullptr, 0, &result).error && result == 1);
    checkBss(front.readBss(kCurrentNetwork), *ap, kTransition);

    // Exercise both a physical OFF/ON cycle and system-only suspend. An
    // outstanding scan still completes once, with no cache event while powered off.
    const bool off = i % 4 == 0 || i % 4 == 3;
    if (off) {
      const auto word = set19(false);
      assert(!intent.radioOffConfirmed(word) && WifiUserControl::reportedPower(word));
      intent.radioOff(word);
      assert(intent.radioOffConfirmed(word) && !WifiUserControl::reportedPower(intent.read()));
    }
    assert(!WifiUserControl::canReportJoinReceipt(intent.read(), false, true));
    const auto idle = radioSnapshot(active, nullptr, 2, !off);
    const auto down = publish(idle, false, true, off);
    const std::vector<uint32_t> expectedDown = off ? std::vector<uint32_t>{kEvPowerChanged, 55, kEvLinkChanged, kEvSsidChanged, kEvBssidChanged}
                                                   : std::vector<uint32_t>{55, kEvLinkChanged, kEvSsidChanged, kEvBssidChanged};
    assert(down == expectedDown && publish(idle, false).empty());
    assert(front.ask(false, kCurrentNetwork).error == kErrNotAssociated);
    if (off) {
      assert(front.ask(true, kScanReq).error == kErrPowerOff);
      assert(front.ask(true, kAssociate, &saved, sizeof saved).error == kErrPowerOff);
    }
    std::vector<uint32_t> completed;
    postScanCompletions(scans.take(), front.state.power, [&](uint32_t code) { completed.push_back(code); });
    assert(completed == (off ? std::vector<uint32_t>{10} : std::vector<uint32_t>{15, 10}));

    ++restart;
    if (off) assert(intent.allowed(set19(true)));
    const auto awake = radioSnapshot(active, nullptr, 2);
    const auto availability = publish(awake, true, true, off);
    assert(availability == (off ? std::vector<uint32_t>{kEvPowerChanged, 55} : std::vector<uint32_t>{55}));
    assert(publish(awake, true).empty());
    assert(!native.canStart(now + 7) && !native.currentReceipt(now + 7, restart, active, true, true));
    assert(!native.finish(active, 1, now + 8, restart));
    assert(front.ask(false, kCurrentNetwork).error == kErrNotAssociated);
  }
  assert(generation == 2 * rounds && front.state.scanRequests == rounds && scans.take() == 0);
  checkDefaults(front);
}
} // namespace

int main()
{
  testDenseScan();
  testSsidAndFraming();
  testAdvertisedSecurity();
  testCurrentNetwork();
  testAssociateDeadline();
  testRapidRequests();
  std::puts("wifi_apple80211_scan_stress_host: all passed (320 BSS, 256 SET20 generations)");
}
