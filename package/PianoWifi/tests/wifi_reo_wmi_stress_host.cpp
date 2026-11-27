// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. wifi_reo_wmi_stress_host.cpp -o /tmp/wifi_reo_wmi_stress_host && /tmp/wifi_reo_wmi_stress_host
#include "wifi_station_model.h"
#include "WifiBa.h"
#include "WifiHtc.h"
#include "WifiNet.h"
#include "WifiUserControl.h"

namespace {
using namespace wifi_test;

// The firmware model classifies sequence numbers. dpPoll sees completions
// from the selected ring and owns the host-side buffer lifecycle.
struct SequenceWindow {
  uint32_t next = kReoQueueSsnMask - 16;
  uint32_t wraps = 0;

  uint32_t take()
  {
    const uint32_t seq = next;
    next = (next + 1) & kReoQueueSsnMask;
    if (!next) ++wraps;
    return seq;
  }

  uint32_t dropStatus(uint32_t seq) const
  {
    constexpr uint32_t kDuplicate = 4, kOutsideWindow = 7;
    const uint32_t ahead = (seq - next) & kReoQueueSsnMask;
    CHECK(ahead == kReoQueueSsnMask || ahead >= kBaWindowMax);
    return (ahead == kReoQueueSsnMask ? kDuplicate : kOutsideWindow) << kDpReoErrShift;
  }

};

void testRingExhaustion()
{
  constexpr uint32_t rounds = 64, rxBurst = kDpReoEntries - 1, excBurst = kDpReoExcEntries - 1;
  constexpr uint32_t linksPerBurst = 2, buffersPerLink = 3, poolSize = kRxRefillEntries - 1;
  constexpr uint32_t firstExceptions = excBurst + linksPerBurst * (buffersPerLink - 1);
  constexpr uint32_t heldCount = poolSize - rxBurst - firstExceptions;
  static_assert(heldCount > 0 && heldCount + 1 <= excBurst && buffersPerLink <= kDpMsdusPerLink);
  static_assert(rounds * rxBurst > 10000 && rounds * (poolSize - rxBurst) > 10000);

  ReceiveRings rings;
  SequenceWindow sequence;
  auto &dp = rings.dp;
  auto &rx = rings.rx;
  uint32_t packetsDelivered = 0, burstDrops = 0;
  rings.checkAllBuffersPosted();
  for (uint32_t round = 0; round < rounds; ++round) {
    const auto old = dp.st;
    uint32_t expectedSeq = sequence.next;
    for (uint32_t packet = 0; packet < rxBurst; ++packet)
      rings.postReceive(rings.takeBuffer(), sequence.take());

    const uint32_t duplicate = (sequence.next - 1) & kReoQueueSsnMask;
    const uint32_t outside = (sequence.next + kBaWindowMax) & kReoQueueSsnMask;
    for (uint32_t i = 0; i < linksPerBurst; ++i) {
      const uint32_t index = round * linksPerBurst + i;
      CHECK(index < dp.linkDescs);
      uint8_t *link = dp.linkVa + index * kDpLinkDescBytes;
      for (uint32_t m = 0; m < buffersPerLink; ++m) {
        const uint32_t cookie = rings.takeBuffer();
        httdetail::putBuffer(link + kDpLinkMsduAt + m * kDpLinkMsduBytes, rx.refillBus[cookie], rx.rbm, cookie);
      }
      const uint32_t status = sequence.dropStatus(i == 0 ? duplicate : outside) | kDpReoBufTypeLinkDesc;
      rings.post(dp.reoExc, dp.linkBus + index * kDpLinkDescBytes, kDpRbmIdleDescDev0, index, status);
    }
    for (uint32_t i = linksPerBurst; i < excBurst; ++i) {
      const uint32_t cookie = rings.takeBuffer();
      rings.post(dp.reoExc, rx.refillBus[cookie], rx.rbm, cookie, sequence.dropStatus(i % 2 == 0 ? duplicate : outside));
    }

    // Firmware already owns these buffers when both completion rings fill.
    std::array<uint32_t, heldCount> held{};
    for (auto &cookie : held) cookie = rings.takeBuffer();
    CHECK(httRefillFree(rx) == poolSize);
    const auto &refill = rx.ring[kHttRefill];
    CHECK(*refill.head == *refill.tail);
    const uint32_t excHead = *dp.reoExc.theirs, rxHead = *dp.reo.theirs;
    CHECK(!rings.tryPost(dp.reoExc, rx.refillBus[held[0]], rx.rbm, held[0], sequence.dropStatus(outside)));
    CHECK(!rings.tryPost(dp.reo, rx.refillBus[held[0]], rx.rbm, held[0], 0));
    CHECK(*dp.reoExc.theirs == excHead && *dp.reo.theirs == rxHead);
    for (uint32_t i = 0; i < 32; ++i) {
      uint32_t cookie = poolSize;
      CHECK(!rings.tryTakeBuffer(cookie) && cookie == poolSize);
      ++burstDrops;
    }

    const auto deliver = [&](uint32_t kind, const uint8_t *bytes, uint32_t size) {
      CHECK(kind == kDpRxEthernet && size == ReceiveRings::kFrameBytes);
      CHECK(wmiGet(bytes + 14, 0) == expectedSeq);
      expectedSeq = (expectedSeq + 1) & kReoQueueSsnMask;
      ++packetsDelivered;
    };
    CHECK(dpPoll(dp, rings.io, deliver) == rxBurst + excBurst);
    CHECK(expectedSeq == sequence.next && packetsDelivered == (round + 1) * rxBurst);
    CHECK(dp.st.excBufRecycled == old.excBufRecycled + firstExceptions);
    CHECK(dp.st.excBufOther == old.excBufOther && httRefillFree(rx) == heldCount);
    CHECK(rings.doorbells[0] == excHead && rings.doorbells[1] == rxHead && dp.reo.ours * dp.reo.words == rxHead);

    // The first held buffer completes twice. Space remains on the refill
    // ring after its first completion, so the duplicate check must reject it.
    for (uint32_t i = 0; i < held.size(); ++i) {
      const auto cookie = held[i];
      const uint32_t status = sequence.dropStatus(i % 2 == 0 ? duplicate : outside);
      rings.post(dp.reoExc, rx.refillBus[cookie], rx.rbm, cookie, status);
      if (i == 0) rings.post(dp.reoExc, rx.refillBus[cookie], rx.rbm, cookie, status);
    }
    const auto noDelivery = [](uint32_t, const uint8_t *, uint32_t) { CHECK(false); };
    CHECK(dpPoll(dp, rings.io, noDelivery) == heldCount + 1);
    CHECK(dpPoll(dp, rings.io, noDelivery) == 0);
    CHECK(dp.st.excLinks == old.excLinks + linksPerBurst && !dp.st.excLinkForeign);
    CHECK(dp.st.excEntries == old.excEntries + excBurst + heldCount + 1);
    CHECK(dp.st.excBufRecycled == old.excBufRecycled + poolSize - rxBurst);
    CHECK(dp.st.excBufOther == old.excBufOther + 1);
    CHECK(dp.st.lastErrorCode == (sequence.dropStatus(heldCount % 2 ? duplicate : outside) >> kDpReoErrShift));
    CHECK(dp.st.rxRecycled == packetsDelivered && !dp.st.rxRecycleFull && !dp.st.rxBadCookie);
    CHECK(rings.doorbells[0] == *dp.reoExc.theirs && dp.reoExc.ours * dp.reoExc.words == *dp.reoExc.theirs);
    rings.checkAllBuffersPosted();
  }
  CHECK(rings.consumed == rounds * poolSize && sequence.wraps > 1 && burstDrops == rounds * 32);
  std::printf("REO: %u buffers recycled, %u received, %u exception drops, %u burst drops\n",
              rings.consumed, packetsDelivered, dp.st.excBufRecycled, burstDrops);
}

void testMalformedWmi(Station &station)
{
  auto &sta = station.sta;
  const auto before = sta.events;
  const auto kickouts = sta.kickouts;
  const auto reject = [&](const auto &message) {
    const auto bytes = uint32_t(message.size());
    if (bytes < kWmiCmdHeaderBytes) {
      CHECK(wmiEventId(message.data(), bytes) == 0);
      return;
    }
    CHECK(!staOnEvent(sta, wmiEventId(message.data(), bytes), message.data() + kWmiCmdHeaderBytes,
                      bytes - kWmiCmdHeaderBytes, station.now));
    CHECK(sta.step == kStaAssociated && sta.vdevUp && !sta.disconnect && sta.kickouts == kickouts && !sta.outLength);
  };

  std::array<uint8_t, 20> roam{};
  WmiWriter writer;
  wmiBegin(writer, roam.data(), roam.size(), kWmiEvtRoam);
  auto *fixed = wmiStruct(writer, kWmiTagRoamEvent, 3);
  CHECK(fixed && wmiEnd(writer) == roam.size());
  wmiPut(fixed, 1, kWmiRoamReasonBeaconMiss);
  // Allocate each prefix at its exact size so ASan can see overreads.
  for (size_t n = 0; n < roam.size(); ++n) reject(std::vector<uint8_t>(roam.begin(), roam.begin() + n));
  auto malformed = roam;
  wmiPut(malformed.data() + kWmiCmdHeaderBytes, 0, uint32_t(kWmiTagRoamEvent) << kWmiTlvTagShift);
  reject(malformed);
  wmiPut(malformed.data() + kWmiCmdHeaderBytes, 0, uint32_t(kWmiTagRoamEvent) << kWmiTlvTagShift | kWmiTlvLenMask);
  reject(malformed);
  malformed = roam;
  wmiPut(malformed.data(), 0, kWmiCmdIdMask);
  reject(malformed);

  // A child header at the end of an array may not borrow bytes from the
  // following outer TLV, even though the whole message is in bounds.
  std::array<uint8_t, 16> nested{};
  wmiBegin(writer, nested.data(), nested.size(), kWmiEvtRoam);
  auto *child = wmiTlv(writer, kWmiTagArrayStruct, kWmiTlvHeaderBytes);
  CHECK(child && wmiTlv(writer, kWmiTagArrayByte, 0) && wmiEnd(writer) == nested.size());
  wmiPut(child, 0, uint32_t(kWmiTagRoamEvent) << kWmiTlvTagShift | 4u);
  const uint8_t *outer = nested.data() + kWmiCmdHeaderBytes;
  uint32_t remaining = nested.size() - kWmiCmdHeaderBytes;
  WmiTlvRef array, inner, following;
  CHECK(wmiNext(outer, remaining, array) && array.tag == kWmiTagArrayStruct && array.length == kWmiTlvHeaderBytes);
  const uint8_t *at = array.value;
  uint32_t left = array.length;
  CHECK(!wmiNext(at, left, inner) && at == array.value && left == array.length && !inner.value);
  CHECK(wmiNext(outer, remaining, following) && following.tag == kWmiTagArrayByte && !following.length && !remaining);
  reject(nested);
  CHECK(sta.events == before + roam.size() - kWmiCmdHeaderBytes + 4);
}

void testMalformedHtt(HttRx &rx, uint32_t generation)
{
  constexpr uint32_t statusBytes = 12, endpoint = 1;
  std::array<uint8_t, kHtcHeaderBytes + statusBytes> status{};
  wmiPut(status.data(), 0, statusBytes << kHtcHdrLengthShift | endpoint);
  uint8_t *payload = status.data() + kHtcHeaderBytes;
  wmiPut(payload, 0, generation << 16 | kHttT2hPeerMap);
  std::memcpy(payload + 4, kApMac, sizeof kApMac);

  Htc htc;
  htc.ep[endpoint].used = true;
  for (size_t n = 0; n < status.size(); ++n) {
    const std::vector<uint8_t> truncated(status.begin(), status.begin() + n);
    const auto receive = htcReceive(htc, truncated.data(), uint32_t(truncated.size()));
    CHECK(receive.kind == kHtcBad && !receive.payload && !receive.length);
  }
  CHECK(htc.badLength == status.size() && !htc.rx && !htc.ep[endpoint].received);
  const auto valid = htcReceive(htc, status.data(), status.size());
  CHECK(valid.kind == kHtcPayload && valid.length == statusBytes);
  httReceive(rx, valid.payload, valid.length);
  const auto eventCount = rx.peerEvents, messages = rx.t2hMessages;
  const auto *peer = httPeerMapOf(rx, 0, kApMac);
  CHECK(peer && peer->peerId == generation);

  for (uint32_t n = 0; n < statusBytes; ++n) {
    const std::vector<uint8_t> truncated(payload, payload + n);
    httReceive(rx, truncated.data(), n);
  }
  const std::array<uint8_t, 4> unknown{0xff, 0, 0, 0};
  httReceive(rx, unknown.data(), unknown.size());
  CHECK(rx.t2hMessages == messages + statusBytes - 4 + 1); // complete headers may update telemetry
  CHECK(!rx.versionSeen && !rx.peerUnmaps && rx.peerMaps == generation && rx.peerEvents == eventCount);
  CHECK(httPeerMapOf(rx, 0, kApMac) == peer && peer->peerId == generation);
}

void testRejoin()
{
  constexpr uint32_t losses = 64;
  Station station;
  ReceiveRings rings;
  WifiUserControl intent;
  const auto joinIntent = intent.read();
  WifiJoinRequest profileMode;
  piano_wifi_profile profile{};
  profile.ssid_len = sizeof kSsid;
  profile.security = PianoWifiSecWpa2Psk;
  std::memcpy(profile.ssid, kSsid, sizeof kSsid);
  NetKeys keys;
  NetQueue tx;
  std::vector<uint8_t> queueMemory((losses + 1) * kReoPoolStep, 0xff);
  ReoPool reo;
  reoPoolInit(reo, queueMemory.data(), 0x24000000, uint32_t(queueMemory.size()));
  std::array<uint64_t, kBaTids> agreements{};
  uint64_t lastQueue = 0;
  uint32_t received = 0, closedDrops = 0;
  uint32_t lossCause = kStaDiscNone;
  station.hearBeacon();

  const auto drainClosedPort = [&](uint32_t oldGeneration) {
    const auto drop = [&](uint32_t kind, const uint8_t *data, uint32_t length) {
      CHECK(staAppleAssociateResult(station.sta) != 1);
      CHECK(kind == kDpRxEthernet && length == ReceiveRings::kFrameBytes && wmiGet(data + 14, 0) == oldGeneration);
      ++closedDrops;
    };
    CHECK(dpPoll(rings.dp, rings.io, drop) == 1 && closedDrops == oldGeneration);
    CHECK(dpPoll(rings.dp, rings.io, drop) == 0);
    rings.checkAllBuffersPosted();
  };
  for (uint32_t generation = 1; generation <= losses + 1; ++generation) {
    CHECK(intent.read() == joinIntent && intent.allowed(joinIntent) && profileMode.canStart(station.now));
    CHECK(std::all_of(agreements.begin(), agreements.end(), [](uint64_t bus) { return bus == 0; }));
    const bool kickoutEnds = generation % 2 == 0;
    station.begin(profile, lossCause != kStaDiscNone, kickoutEnds);
    CHECK(!staAppleAssociateResult(station.sta));
    if (lossCause) {
      station.runTo(kStaRescan);
      if (lossCause == kStaDiscBeaconMiss) drainClosedPort(generation - 1);
      station.finishScan(true);
    }
    station.joined();
    CHECK(station.sta.scanFirstDone == (lossCause != kStaDiscNone));
    testMalformedWmi(station);
    testMalformedHtt(rings.rx, generation);

    // Simulate the supplicant completing keys and authorizing this port.
    piano_wifi_8021x_key key{};
    key.generation = generation;
    key.key_len = 16;
    std::fill_n(key.key, key.key_len, uint8_t(generation));
    const auto install = [&](bool group, uint32_t seq) {
      key.group = group;
      CHECK(netKeysOffer(keys, key, seq));
      uint32_t next = 0;
      const auto *pending = netKeysNext(keys, &next);
      CHECK(pending && pending->group == group && pending->generation == generation && next == seq);
      netKeysSent(keys, group, station.now);
    };
    install(false, generation * 4);
    CHECK(netKeysDone(keys, true) == generation * 4);
    install(true, generation * 4 + 1);
    CHECK(netKeysDone(keys, true) == generation * 4 + 1);
    CHECK(!staAppleAssociateResult(station.sta));
    station.sta.authorized = true;
    CHECK(staAppleAssociateResult(station.sta) == 1);

    const uint32_t tid = generation % kBaTids, ssn = (kReoQueueSsnMask - generation) & kReoQueueSsnMask;
    const auto agreement = baDecide({.tid = uint8_t(tid), .immediate = true, .window = kBaWindowMax}, kBaModeAccept);
    CHECK(agreement.accept && agreement.window == kBaWindowMax);
    uint8_t *queue = nullptr;
    bool wrapped = true;
    CHECK(!agreements[tid] && reoPoolTake(reo, reoQueueBytes(tid), &queue, &agreements[tid], &wrapped));
    CHECK(!wrapped && agreements[tid] > lastQueue && queue[kReoPoolStep - 1] == 0);
    lastQueue = agreements[tid];
    reoQueueInit(queue, tid, agreement.window, ssn);
    CHECK(wmiGet(queue, 3) == ssn << kReoQueueSsnShift);

    const auto deliver = [&](uint32_t kind, const uint8_t *data, uint32_t length) {
      CHECK(staAppleAssociateResult(station.sta) == 1);
      CHECK(kind == kDpRxEthernet && length == ReceiveRings::kFrameBytes);
      CHECK(wmiGet(data + 14, 0) == generation);
      ++received;
    };
    rings.postReceive(rings.takeBuffer(), generation);
    CHECK(dpPoll(rings.dp, rings.io, deliver) == 1 && received == generation);
    if (generation > losses) break;

    // Leave an RX completion pending at the loss edge, and a rekey both
    // in flight and queued. Time advances on the model's logical clock.
    rings.postReceive(rings.takeBuffer(), generation);
    install(true, generation * 4 + 2);
    CHECK(netKeysOffer(keys, key, generation * 4 + 3));
    std::array<uint8_t, ReceiveRings::kFrameBytes> outgoing{};
    CHECK(netQueuePut(tx, outgoing.data(), outgoing.size(), false));
    station.now += 10000;
    station.event(kWmiEvtPeerStaKickout, kWmiTagPeerStaKickoutEvent, {0, 0, 1, 0}, 0);
    if (!kickoutEnds) {
      CHECK(station.sta.kickouts == 1 && staAppleAssociateResult(station.sta) == 1);
      station.event(kWmiEvtRoam, kWmiTagRoamEvent, {0, kWmiRoamReasonBeaconMiss, 0});
    }
    lossCause = station.sta.disconnect;
    CHECK(lossCause == (kickoutEnds ? kStaDiscKickout : kStaDiscBeaconMiss));
    CHECK(station.sta.step == kStaTeardown && staAppleAssociateResult(station.sta) == 2);

    netKeysReset(keys);
    netQueueClear(tx);
    CHECK(!keys.pairwiseDone && !keys.inFlight && !keys.groupWaiting && !keys.pairwiseWaiting);
    const auto zero = [](uint8_t byte) { return byte == 0; };
    CHECK(std::all_of(std::begin(keys.group.key), std::end(keys.group.key), zero));
    CHECK(std::all_of(std::begin(keys.pairwise.key), std::end(keys.pairwise.key), zero));
    CHECK(!netKeysNext(keys, nullptr));
    CHECK(!netKeysDone(keys, true) && keys.installed == generation * 2 && !netQueuePeek(tx));
    // Alternate draining the old completion during teardown and during
    // the next directed scan, before the new port opens.
    if (kickoutEnds) drainClosedPort(generation);
    station.runTo(kStaDone);
    station.checkTeardown();
    CHECK(!station.sta.deauthSent);
    // The firmware model discards its BA binding on the peer-delete exchange.
    CHECK(std::count(station.commands.begin(), station.commands.end(), kWmiCmdPeerDelete) == 1);
    agreements[tid] = 0;
    const uint32_t oldFreq = station.airFreq;
    station.airFreq = oldFreq == 5180 ? 5200 : 5180;
    CHECK(station.scan.count == 1 && station.scan.entries[0].freq == oldFreq);
  }
  CHECK(received == losses + 1 && closedDrops == losses && reo.takes == losses + 1 && !reo.wraps);
  rings.checkAllBuffersPosted();
  std::printf("Rejoin: %u recoveries, %u old completions drained with the port closed, WMI/HTT boundary checks passed\n",
              losses, closedDrops);
}
}  // namespace

int main()
{
  testRingExhaustion();
  testRejoin();
}
