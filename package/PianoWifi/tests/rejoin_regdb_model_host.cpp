// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. rejoin_regdb_model_host.cpp -o /tmp/rejoin_regdb_model_host && /tmp/rejoin_regdb_model_host
#include "wifi_station_model.h"
#include "WifiUserControl.h"

namespace {
using namespace wifi_test;

struct FrontEnd {
  apple::State state;
  alignas(uint32_t) uint8_t scratch[apple::kScratchBytes];

  apple::Answer ask(bool set, int type, void *data = nullptr, uint32_t size = 0)
  {
    const apple::UserIo io{
        nullptr,
        [](void *, uint64_t from, void *to, uint32_t n) { std::memcpy(to, reinterpret_cast<const void *>(from), n); return 0; },
        [](void *, const void *from, uint64_t to, uint32_t n) { std::memcpy(reinterpret_cast<void *>(to), from, n); return 0; }};
    apple::Req req{};
    req.type = type;
    req.length = size;
    req.data = reinterpret_cast<uintptr_t>(data);
    return apple::handle(state, {"host", "model", "en0"}, set, req, io, scratch);
  }

  apple::ScanResult readBss(int type)
  {
    uint8_t bytes[apple::kScanResultBytes27];
    const auto answer = ask(false, type, bytes, sizeof bytes);
    CHECK(!answer.error && answer.wrote == sizeof bytes);
    apple::ScanResult result;
    std::memcpy(&result, bytes, sizeof result);
    return result;
  }
};

apple::AssociateRequest27 savedNetwork()
{
  apple::AssociateRequest27 request{};
  request.version = 1;
  request.mode = 2;
  request.auth_lower = 1;
  request.auth_upper = 0x08;
  request.network.ssid_length = sizeof kSsid;
  std::memcpy(request.network.ssid, kSsid, sizeof kSsid);
  request.network.key_length = 8;
  request.network.key_cipher = 10;
  std::memset(request.network.key, '0', request.network.key_length);
  return request;
}

void testRegulatory()
{
  FrontEnd front;
  const RegDomain china = firmwareDomain("CN", 156, 1, true, {
      {2402, 2482, 40, 20, 0}, {5735, 5775, 40, 23, 0}, {5170, 5189, 20, 23, 0}});
  const RegDomain usa = firmwareDomain("US", 840, 2, false, {
      {2402, 2472, 40, 20, 0}, {5170, 5210, 40, 23, 0}, {5250, 5270, 20, 23, kRegRuleFlagRadar}});
  const RegDomain japan = firmwareDomain("JP", 392, 3, true, {
      {2402, 2482, 40, 20, 0}, {5170, 5190, 20, 18, kRegRuleFlagNoIr},
      {5190, 5210, 20, 18, kRegRuleFlagDisabled}, {5210, 5230, 10, 18, 0}});

  struct Expected {
    const RegDomain &reg;
    const char *country;
    uint32_t lower, passive, radar;
    std::vector<uint32_t> upper;
  };
  for (const auto &test : {Expected{china, "CN", 13, 0, 0, {5745, 5765}},
                           Expected{usa, "US", 11, 1, 1, {5180, 5200, 5260}},
                           Expected{japan, "JP", 13, 1, 0, {5180}}}) {
    CHECK(std::memcmp(test.reg.alpha2, test.country, 3) == 0);
    apple::setRegulatoryCountry(front.state.country, test.reg.alpha2, 2);
    char country[3];
    const auto get = front.ask(false, apple::kCountryCode, country, sizeof country);
    CHECK(!get.error && get.wrote == 3 && std::memcmp(country, test.country, 3) == 0);
    uint8_t set[8] = {1, 0, 0, 0, uint8_t(test.country[0]), uint8_t(test.country[1]), 0, 0};
    CHECK(!front.ask(true, apple::kCountryCode, set, sizeof set).error);
    set[4] = set[5] = 'Z';
    CHECK(front.ask(true, apple::kCountryCode, set, sizeof set).error == apple::kErrUnsupported);
    CHECK(!front.ask(false, apple::kCountryCode, country, sizeof country).error);
    CHECK(std::memcmp(country, test.country, 3) == 0);

    ChanList channels;
    chanListBuild(channels, test.reg, true);
    CHECK(channels.count == test.lower + test.upper.size() && channels.left24 == 13 - test.lower);
    CHECK(channels.left5 == std::size(kAssocChannels5) - test.upper.size());
    CHECK(channels.passive == test.passive && channels.radar == test.radar);
    for (uint32_t i = 0; i < test.lower; ++i) CHECK(channels.entry[i].freq == 2412 + 5 * i);
    for (size_t i = 0; i < test.upper.size(); ++i) {
      const auto &channel = channels.entry[test.lower + i];
      CHECK(channel.freq == test.upper[i]);
      const bool last = i + 1 == test.upper.size();
      CHECK(bool(channel.info & kWmiChanInfoPassive) == (last && test.passive));
      CHECK(bool(channel.info & kWmiChanInfoDfs) == (last && test.radar));
      const uint32_t halfDbm = test.reg.countryId == 392 ? 36 : 46;
      CHECK(channel.reg1 == (halfDbm << 8 | halfDbm << 16));
    }
  }

  for (const char *invalid : {"cN", "U1", "J?", "0\x80"}) {
    const auto reg = firmwareDomain(invalid, 0, 0, false, {{2402, 2472, 20, 18, 0}});
    apple::setRegulatoryCountry(front.state.country, reg.alpha2, 2);
    char country[3];
    CHECK(!front.ask(false, apple::kCountryCode, country, sizeof country).error);
    CHECK(std::memcmp(country, "XX", 3) == 0);
    uint8_t set[8] = {1, 0, 0, 0, uint8_t(invalid[0]), uint8_t(invalid[1]), 0, 0};
    CHECK(front.ask(true, apple::kCountryCode, set, sizeof set).error == apple::kErrInval);
    ChanList channels;
    chanListBuild(channels, reg, true);
    CHECK(channels.count == 11 && channels.left5 == std::size(kAssocChannels5));
  }
}

void testInvoluntaryRejoin(bool kickoutEnds)
{
  FrontEnd front;
  auto saved = savedNetwork();
  const auto request = front.ask(true, apple::kAssociate, &saved, sizeof saved);
  CHECK(!request.error && request.associationProfile);
  const auto &profile = request.associationProfileData;

  // The retained-profile retry path can start a new station without SET20.
  // The native request path is exercised separately below.
  WifiUserControl intent;
  WifiJoinRequest profileMode;
  Station station;
  ReceiveRings rings;
  auto beginFromProfile = [&](uint32_t lossCause) {
    CHECK(intent.allowed(intent.read()) && profileMode.canStart(station.now));
    station.begin(profile, lossCause != kStaDiscNone, kickoutEnds);
  };
  rings.checkAllBuffersPosted();
  station.hearBeacon();
  beginFromProfile(kStaDiscNone);
  station.joined();

  // Exercise generations 6 through 15 over the same DMA rings for each
  // policy: kickout ends the station, or the default low-ack report followed
  // by beacon miss ends it.
  const uint32_t originalIntent = intent.read();
  for (uint32_t generation = 6; generation <= 15; ++generation) {
    station.event(kWmiEvtPeerStaKickout, kWmiTagPeerStaKickoutEvent, {0, 0, 1, 0}, 0);
    if (!kickoutEnds) {
      CHECK(station.sta.step == kStaAssociated && station.sta.kickouts == 1 && !station.sta.disconnect);
      station.event(kWmiEvtRoam, kWmiTagRoamEvent, {0, kWmiRoamReasonBeaconMiss, 0});
    }
    const uint32_t cause = station.sta.disconnect;
    CHECK(cause == (kickoutEnds ? kStaDiscKickout : kStaDiscBeaconMiss));
    CHECK(station.sta.step == kStaTeardown);

    const uint32_t pendingRx = rings.takeBuffer();
    rings.exceptions(generation);
    station.runTo(kStaDone);
    station.checkTeardown();
    CHECK(!station.sta.deauthSent);
    CHECK(intent.read() == originalIntent);

    const uint32_t oldFreq = station.startFreq;
    station.airFreq = oldFreq == 5180 ? 5200 : 5180;
    CHECK(station.scan.count == 1 && station.scan.entries[0].freq == oldFreq);
    beginFromProfile(cause);
    station.runTo(kStaRescan);
    if (generation == 6) {
      // A scan without a beacon leaves the cached channel deaf. A failure
      // before association retains the involuntary cause for the next try.
      station.finishScan(false);
      station.runTo(kStaStopped);
      CHECK(station.sta.error == kStaErrNoAnswer && !station.sta.associatedAt && !station.sta.disconnect);
      CHECK(station.startFreq == oldFreq && intent.allowed(intent.read()));
      beginFromProfile(cause);
      station.runTo(kStaRescan);
    }
    station.finishScan(true);
    station.joined();
    CHECK(station.sta.scanFirstDone && station.sta.rescansDone == 0);
    rings.receive(pendingRx, uint8_t(generation));
    rings.checkAllBuffersPosted();
  }
  CHECK(rings.delivered == 10 && rings.dp.st.excBufRecycled == 30);
}

void publishBss(FrontEnd &front, const ScanEntry &entry)
{
  // Mirror the scan property's fields at the IOKit boundary.
  auto &bss = front.state.scan.bss[0];
  std::memcpy(bss.bssid, entry.bssid, sizeof bss.bssid);
  std::memcpy(bss.ssid, entry.ssidRaw, entry.ssidLength);
  bss.ssidLength = entry.ssidLength;
  bss.channel = entry.channel;
  bss.freq = entry.freq;
  bss.rssi = entry.rssi;
  bss.capability = entry.capability;
  bss.beaconInterval = entry.beaconInterval;
  bss.rateCount = entry.rateCount;
  std::memcpy(bss.rates, entry.rates, entry.rateCount);
  bss.rsnLength = entry.rsnElementLength;
  std::memcpy(bss.rsn, entry.rsnElement, bss.rsnLength);
  front.state.scan.count = 1;
}

bool hasRsn(const apple::ScanResult &bss)
{
  CHECK(bss.elementsLength <= sizeof bss.elements);
  for (uint32_t i = 0; i < bss.elementsLength;) {
    CHECK(i + 2 <= bss.elementsLength);
    const uint32_t bytes = 2u + bss.elements[i + 1];
    CHECK(i + bytes <= bss.elementsLength);
    if (bss.elements[i] == kDot11ElementRsn) {
      CHECK(bytes == sizeof kRsn && std::memcmp(bss.elements + i, kRsn, bytes) == 0);
      return true;
    }
    i += bytes;
  }
  return false;
}

void checkSecuredBss(const apple::ScanResult &bss)
{
  CHECK(bss.capability == kCapability && hasRsn(bss));
  CHECK(bss.ssidLength == sizeof kSsid && std::memcmp(bss.ssid, kSsid, sizeof kSsid) == 0);
  CHECK(std::memcmp(bss.bssid, kApMac, sizeof kApMac) == 0);
}

void testMenuAndWake()
{
  Station station;
  FrontEnd front;
  WifiUserControl intent;
  WifiBootPower bootPower;
  WifiJoinRequest native;
  auto saved = savedNetwork();
  uint32_t restart = 0, generation = 0;
  bool usable = true, sleeping = false;

  auto receipt = [&] {
    if (!WifiUserControl::canReportJoinReceipt(intent.read(), usable, sleeping)) return 0u;
    return native.currentReceipt(station.now, restart, generation, station.sta.step == kStaAssociated,
                                 station.sta.authorized && intent.allowed(intent.read()));
  };
  auto canStart = [&] {
    return usable && !sleeping && intent.allowed(intent.read()) && native.canStart(station.now);
  };
  auto reportIdle = [&] {
    apple::observeAssociation(front.state, {}, WifiUserControl::reportedPower(intent.read()), true);
    uint8_t bytes[apple::kScanResultBytes27];
    CHECK(front.ask(false, apple::kCurrentNetwork, bytes, sizeof bytes).error == apple::kErrNotAssociated);
  };
  auto setPower = [&](bool on) {
    auto power = apple::powerRecord(on);
    const auto answer = front.ask(true, apple::kPower, &power, sizeof power);
    CHECK(!answer.error && answer.effects == apple::kDoPowerChanged && answer.powerOn == on);
    CHECK(bootPower.setPower(intent, answer.powerOn));
    if (!on) native.cancel();
    return intent.read();
  };
  auto leave = [&] {
    const auto answer = front.ask(true, apple::kDisassociate);
    CHECK(!answer.error && answer.effects == apple::kDoDisassociate);
    native.cancel();
    return intent.disconnect();
  };
  auto stopStation = [&] {
    staStop(station.sta, station.now);
    station.runTo(kStaStopped);
    station.checkTeardown();
    CHECK(station.sta.stopAsked && !station.sta.disconnect);
  };

  auto autoJoin = [&] {
    // A stand-in for the native client's automatic SET20 from a saved
    // network. The old SET20 generation is single-use, including after ON.
    const auto result = front.ask(true, apple::kAssociate, &saved, sizeof saved);
    CHECK(!result.error && result.associationProfile && intent.join());
    const auto &profile = result.associationProfileData;
    CHECK(profile.psk_len == saved.network.key_length &&
          std::memcmp(profile.psk, saved.network.key, profile.psk_len) == 0);
    native.accept(++generation, station.now, restart);
    CHECK(canStart());
    native.consume();
    station.begin(profile);
    station.joined();

    // The host AP ends at 802.11 association. Model the supplicant's
    // successful authorization when publishing the native join result.
    station.sta.authorized = true;
    CHECK(native.finish(generation, 1, station.now, restart) && receipt() == generation);
    apple::Association association{};
    association.associated = association.authorized = true;
    association.authType = 2;
    association.generation = generation;
    association.result = 1;
    association.ssidLength = station.sta.ssidLength;
    std::memcpy(association.ssid, station.sta.ssid, association.ssidLength);
    std::memcpy(association.bssid, station.sta.bssid, sizeof association.bssid);
    association.channel = uint16_t(station.sta.choice.channel);
    association.frequency = station.sta.choice.freq;
    const auto old = apple::observeAssociation(front.state, association, true, true);
    CHECK(apple::associationTransition(old, association).connected);
    const auto bss = front.readBss(apple::kCurrentNetwork);
    checkSecuredBss(bss);
    CHECK(bss.channel.channel == association.channel);
  };
  auto scan = [&] {
    station.hearBeacon();
    publishBss(front, station.scan.entries[0]);
    CHECK(front.ask(true, apple::kScanReq).effects == apple::kDoScanStarted);
    checkSecuredBss(front.readBss(apple::kScanResult));
  };

  scan();
  autoJoin();
  CHECK(generation == 1);
  bootPower.arm();  // explicit menu control is enabled for this host scenario
  const uint32_t left = leave();  // SET22 tears down the station, with power still on
  CHECK(intent.power(left) && !intent.allowed(left) && !receipt());
  const uint32_t off = setPower(false);  // physical OFF is SET19
  CHECK(!intent.radioOffConfirmed(off) && intent.reportedPower(off) && !canStart());
  stopStation();
  intent.stopped(left);
  CHECK(!intent.radioOffConfirmed(off));
  usable = false;
  intent.radioOff(off);
  CHECK(intent.radioOffConfirmed(off) && !intent.reportedPower(intent.read()));
  reportIdle();
  CHECK(front.ask(true, apple::kAssociate, &saved, sizeof saved).error == apple::kErrPowerOff);

  const uint32_t on = setPower(true);
  CHECK(intent.allowed(on) && !canStart() && !receipt());
  usable = true;
  ++restart;
  reportIdle();
  CHECK(!canStart() && !receipt() && native.generation == 1);
  scan();
  autoJoin();

  // Exercise both a system-only park and a park after sleep-preparation
  // SET22, where a wake ON reopens the logical station gate.
  for (bool sleepLeave : {false, true}) {
    uint32_t stopped = sleepLeave ? leave() : intent.read();
    sleeping = true;
    CHECK(!receipt() && !canStart());
    stopStation();
    usable = false;
    intent.stopped(stopped);
    reportIdle();
    sleeping = false;
    ++restart;
    if (sleepLeave) {
      CHECK(!intent.allowed(intent.read()));
      setPower(true);
    }
    CHECK(intent.allowed(intent.read()));
    usable = true;
    CHECK(!canStart() && !receipt() && native.generation == generation);
    scan();
    autoJoin();
  }
  CHECK(generation == 4 && restart == 3);

  CHECK(!front.ask(true, apple::kScanCacheClear).error);
  const auto uncached = front.readBss(apple::kCurrentNetwork);
  CHECK((uncached.capability & kDot11CapPrivacy) && !hasRsn(uncached));
  scan();
  checkSecuredBss(front.readBss(apple::kCurrentNetwork));
}
} // namespace

int main()
{
  testInvoluntaryRejoin(false);
  testInvoluntaryRejoin(true);
  testMenuAndWake();
  testRegulatory();
  std::puts("rejoin_regdb_model_host: all passed (offline; multi-AP roaming and menu-row click HOLD)");
}
