// SPDX-License-Identifier: BSD-2-Clause-Patent
// c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. coex_host.cpp -o /tmp/coex_host && /tmp/coex_host
#include "WifiBtCoex.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianowifi::coex;

static unsigned checks, failures;
#define CHECK(expr) do { \
  ++checks; \
  if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; } \
} while (0)

static const PriorityTable kInquiryFixture{{{1, 2}, {3, 4}, {5, 6}, {7, 8}}};
static const PriorityTable kLeFixture{{{9, 10}, {11, 12}, {13, 14}, {15, UINT32_MAX}}};

static bool sameTable(const PriorityTable &a, const PriorityTable &b)
{
  for (unsigned i = 0; i < kWifiWorkCount; ++i)
    if (a.byWifiWork[i].wifi != b.byWifiWork[i].wifi || a.byWifiWork[i].bt != b.byWifiWork[i].bt) return false;
  return true;
}

static bool sameFixture(const ConfigFixture &a, const ConfigFixture &b)
{
  return a.parameterCount == b.parameterCount && std::equal(a.parameters, a.parameters + kMaxParameterWords, b.parameters) &&
         sameTable(a.weights.inquiry, b.weights.inquiry) && sameTable(a.weights.leScan, b.weights.leScan);
}

static void testGoldenFrame()
{
  constexpr uint8_t expected[] = {
    0x03, 0x90, 0x02, 0x00,  // WMI command ID 0x29003
    0x4c, 0x00, 0x0a, 0x02,  // TLV tag 0x20a, 76 value bytes
    0x48, 0x4f, 0x53, 0x54,  // fixture signature
    0x01, 0x00, 0x00, 0x00,
    0xef, 0xcd, 0xab, 0x89,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00,
    0x0f, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
  };
  const ConfigFixture config{1, {0x89abcdef}, {kInquiryFixture, kLeFixture}};
  uint8_t frame[sizeof expected] = {};
  CHECK(encodeConfigFixture(frame, sizeof frame, config) == sizeof expected);
  CHECK(std::memcmp(frame, expected, sizeof expected) == 0);

  ConfigFixture decoded;
  CHECK(decodeConfigFixture(decoded, expected, sizeof expected));
  CHECK(sameFixture(config, decoded));
}

static void testCodecBounds()
{
  // Start at an odd offset and check both buffer guards, as well as the whole
  // buffer after any refused encode. Reuse the decoder destination across sizes.
  constexpr uint8_t canary = 0xa5;
  std::array<uint8_t, kFixtureMaxBytes + 2> buffer;
  ConfigFixture decoded;
  std::vector<uint8_t> full;
  for (unsigned count = kMaxParameterWords + 1; count-- > 0;) {
    ConfigFixture config{count, {}, {kInquiryFixture, kLeFixture}};
    for (unsigned i = 0; i < count; ++i) config.parameters[i] = UINT32_MAX - i;
    const uint32_t bytes = kFixtureMinBytes + 4 * count;
    buffer.fill(canary);
    const auto before = buffer;
    CHECK(encodeConfigFixture(buffer.data() + 1, bytes - 1, config) == 0);
    CHECK(buffer == before);
    CHECK(encodeConfigFixture(buffer.data() + 1, bytes, config) == bytes);
    CHECK(buffer.front() == canary && std::equal(buffer.begin() + bytes + 1, buffer.end(), before.begin() + bytes + 1));
    CHECK(decodeConfigFixture(decoded, buffer.data() + 1, bytes));
    CHECK(sameFixture(config, decoded));
    if (count == kMaxParameterWords) full.assign(buffer.begin() + 1, buffer.begin() + bytes + 1);
  }

  buffer.fill(canary);
  const auto before = buffer;
  for (uint32_t count : {kMaxParameterWords + 1, UINT32_MAX}) {
    ConfigFixture oversized;
    oversized.parameterCount = count;
    CHECK(encodeConfigFixture(buffer.data(), buffer.size(), oversized) == 0);
    CHECK(buffer == before);
  }
  CHECK(encodeConfigFixture(nullptr, kFixtureMaxBytes, {}) == 0);
  CHECK(encodeConfigFixture(buffer.data(), 0, {}) == 0);
  CHECK(buffer == before);

  const ConfigFixture sentinel{1, {0x12345678}, {kInquiryFixture, kLeFixture}};
  auto reject = [&](const uint8_t *data, uint32_t length) {
    decoded = sentinel;
    CHECK(!decodeConfigFixture(decoded, data, length));
    CHECK(sameFixture(decoded, sentinel));
  };
  CHECK(full.size() == kFixtureMaxBytes);
  reject(nullptr, kFixtureMinBytes);
  reject(full.data(), UINT32_MAX);
  for (size_t bytes = 0; bytes < full.size(); ++bytes) {
    const std::vector<uint8_t> truncated(full.begin(), full.begin() + bytes);
    reject(truncated.data(), truncated.size());
  }
  full.push_back(0);
  reject(full.data(), full.size());

  const std::vector<uint8_t> empty(buffer.begin(), buffer.begin() + kFixtureMinBytes);
  auto malformed = empty;
  CHECK(encodeConfigFixture(malformed.data(), malformed.size(), {}) == kFixtureMinBytes);
  const auto valid = malformed;
  for (const auto &edit : std::array<std::array<uint8_t, 2>, 10>{{
         {0, 0x04}, {3, 0x01}, {6, 0x0b}, {7, 0x00}, {4, 0x00}, {4, 0x49}, {4, 0x44},
         {8, 0x00}, {12, 0x01}, {12, kMaxParameterWords + 1},
       }}) {
    malformed = valid;
    malformed[edit[0]] = edit[1];
    reject(malformed.data(), malformed.size());
  }
  malformed = valid;
  malformed[4] = malformed[5] = 0xff;  // advertised 65535-byte value
  reject(malformed.data(), malformed.size());
  malformed = valid;
  std::fill(malformed.begin() + 12, malformed.begin() + 16, 0xff);
  reject(malformed.data(), malformed.size());
  malformed = valid;
  malformed.insert(malformed.end(), {0x00, 0x00, 0x0a, 0x02});  // second TLV
  reject(malformed.data(), malformed.size());
  malformed = valid;
  ++malformed[4];  // real but unaligned value length
  malformed.push_back(0);
  reject(malformed.data(), malformed.size());
}

static constexpr Client kClients[] = {Client::Wifi, Client::Bluetooth};
static constexpr LastPut kKeep = LastPut::RetainPower, kWithdraw = LastPut::WithdrawPower;

static void expectRails(const SharedRails &rails, RadioState wifi, RadioState bt, unsigned votes, bool power)
{
  CHECK(rails.state(Client::Wifi) == wifi);
  CHECK(rails.state(Client::Bluetooth) == bt);
  CHECK(rails.votes() == votes);
  CHECK(rails.powered() == power);
  CHECK(!rails.votes() || rails.powered());
}

static void testAcquisitionAndRelease()
{
  using enum RadioState;
  for (Client first : kClients) {
    const Client second = first == Client::Wifi ? Client::Bluetooth : Client::Wifi;
    for (Client firstOff : kClients) {
      const Client lastOff = firstOff == Client::Wifi ? Client::Bluetooth : Client::Wifi;
      SharedRails rails;
      expectRails(rails, Off, Off, 0, false);
      CHECK(!rails.release(first, kWithdraw));
      CHECK(!rails.activate(first));
      CHECK(!rails.quiesce(first));
      CHECK(!rails.sleep(first, SleepVote::Drop, kWithdraw));
      CHECK(!rails.wake(first));
      CHECK(rails.acquire(first));
      CHECK(rails.acquire(first));
      CHECK(rails.state(first) == Quiesced && rails.hasVote(first));
      CHECK(rails.state(second) == Off && !rails.hasVote(second));
      CHECK(rails.votes() == 1 && rails.powered());
      CHECK(!rails.release(second, kWithdraw));
      CHECK(rails.activate(first));
      CHECK(rails.acquire(first));
      CHECK(rails.votes() == 1);
      CHECK(rails.acquire(second));
      CHECK(rails.activate(second));
      expectRails(rails, Active, Active, 2, true);
      CHECK(!rails.release(firstOff, kWithdraw));
      CHECK(rails.quiesce(firstOff));
      CHECK(rails.release(firstOff, kWithdraw));
      CHECK(!rails.release(firstOff, kWithdraw));
      CHECK(rails.state(firstOff) == Off && rails.state(lastOff) == Active);
      CHECK(rails.votes() == 1 && rails.powered());
      CHECK(rails.quiesce(lastOff));
      CHECK(rails.release(lastOff, kWithdraw));
      expectRails(rails, Off, Off, 0, false);
    }
  }

  SharedRails retained;
  CHECK(retained.acquire(Client::Wifi));
  CHECK(retained.release(Client::Wifi, kKeep));
  CHECK(!retained.release(Client::Wifi, kWithdraw));
  CHECK(!retained.release(Client::Bluetooth, kWithdraw));
  expectRails(retained, Off, Off, 0, true);
  CHECK(retained.acquire(Client::Bluetooth));
  CHECK(retained.release(Client::Bluetooth, kWithdraw));
  expectRails(retained, Off, Off, 0, false);

  SharedRails preexisting(true);
  CHECK(preexisting.acquire(Client::Bluetooth));
  CHECK(preexisting.release(Client::Bluetooth, kWithdraw));
  expectRails(preexisting, Off, Off, 0, true);
}

static void testSleepAndWake()
{
  using enum RadioState;
  for (unsigned sleepFirst = 0; sleepFirst < 2; ++sleepFirst) {
    for (unsigned wakeFirst = 0; wakeFirst < 2; ++wakeFirst) {
      for (LastPut lastPut : {kKeep, kWithdraw}) {
        SharedRails rails;
        for (Client client : kClients) {
          CHECK(rails.acquire(client));
          CHECK(rails.activate(client));
        }
        for (unsigned i = 0; i < 2; ++i) {
          const Client client = kClients[sleepFirst ^ i];
          CHECK(!rails.sleep(client, SleepVote::Drop, lastPut));
          CHECK(rails.quiesce(client));
          CHECK(rails.sleep(client, SleepVote::Drop, lastPut));
          CHECK(!rails.sleep(client, SleepVote::Drop, lastPut));
          CHECK(!rails.release(client, kWithdraw));
          CHECK(!rails.activate(client));
          CHECK(!rails.acquire(client));
          CHECK(rails.state(client) == AsleepWithoutVote && !rails.hasVote(client));
          CHECK(rails.votes() == 1 - i);
          CHECK(rails.powered() == (i == 0 || lastPut == kKeep));
        }
        expectRails(rails, AsleepWithoutVote, AsleepWithoutVote, 0, lastPut == kKeep);
        for (unsigned i = 0; i < 2; ++i) {
          const Client client = kClients[wakeFirst ^ i];
          CHECK(rails.wake(client));
          CHECK(!rails.wake(client));
          CHECK(rails.state(client) == Quiesced && rails.hasVote(client));
          CHECK(rails.votes() == i + 1 && rails.powered());
          CHECK(rails.activate(client));
        }
        expectRails(rails, Active, Active, 2, true);
      }
    }
  }

  for (unsigned retained = 0; retained < 2; ++retained) {
    const Client holder = kClients[retained], peer = kClients[retained ^ 1];
    SharedRails rails;
    CHECK(rails.acquire(holder));
    CHECK(rails.activate(holder));
    CHECK(rails.acquire(peer));
    CHECK(rails.activate(peer));
    CHECK(rails.quiesce(holder));
    CHECK(rails.sleep(holder, SleepVote::Retain, kWithdraw));
    CHECK(rails.state(holder) == AsleepWithVote && rails.hasVote(holder));
    CHECK(rails.state(peer) == Active && rails.votes() == 2);
    CHECK(!rails.release(holder, kWithdraw));
    CHECK(rails.quiesce(peer));
    CHECK(rails.sleep(peer, SleepVote::Drop, kWithdraw));
    CHECK(rails.votes() == 1 && rails.powered());
    CHECK(rails.wake(holder));
    CHECK(rails.state(holder) == Quiesced && rails.votes() == 1);
    CHECK(rails.state(peer) == AsleepWithoutVote);
    CHECK(rails.release(holder, kWithdraw));
    CHECK(rails.votes() == 0 && !rails.powered());
    CHECK(rails.wake(peer));
    CHECK(rails.activate(peer));
    CHECK(rails.votes() == 1 && rails.powered());
  }
}

static void testPolicy()
{
  constexpr WifiBand bands[] = {WifiBand::Unknown, WifiBand::GHz2_4, WifiBand::GHz5, WifiBand::GHz6};
  constexpr WifiWork work[] = {WifiWork::Idle, WifiWork::Traffic, WifiWork::Beacon, WifiWork::Rejoin};
  constexpr BtScan scans[] = {BtScan::None, BtScan::Inquiry, BtScan::LeScan};
  for (WifiBand band : bands) {
    for (WifiWork wifi : work) {
      for (BtScan scan : scans) {
        Grant expected;
        if (wifi == WifiWork::Idle) expected = scan == BtScan::None ? Grant::Idle : Grant::Bluetooth;
        else if (scan == BtScan::None) expected = Grant::Wifi;
        else if (band == WifiBand::GHz5 || band == WifiBand::GHz6) expected = Grant::Both;
        else expected = wifi == WifiWork::Traffic ? Grant::Bluetooth : Grant::Wifi;

        for (bool authorized : {false, true}) {
          for (bool requested : {false, true}) {
            for (bool idleElapsed : {false, true}) {
              const auto decision = arbitrate({band, wifi, scan, authorized, requested, idleElapsed});
              CHECK(decision.grant == expected);
              CHECK(decision.stationDozeEligible == (wifi == WifiWork::Idle && authorized && requested && idleElapsed));
            }
          }
        }
      }
    }
  }

  // The fixture's table for each discovery mode can drive arbitration after
  // decoding. Exercise the equal-weight Wi-Fi tie and a distinct LE scan row.
  ConfigFixture config;
  config.weights.inquiry.byWifiWork[1] = {4, 4};
  config.weights.leScan.byWifiWork[1] = {4, 5};
  uint8_t buffer[kFixtureMaxBytes];
  const auto size = encodeConfigFixture(buffer, sizeof buffer, config);
  ConfigFixture parsed;
  CHECK(decodeConfigFixture(parsed, buffer, size));
  PolicyInput operation{WifiBand::GHz5, WifiWork::Traffic, BtScan::LeScan};
  CHECK(arbitrate(operation, parsed.weights).grant == Grant::Both);
  operation.operationBand = WifiBand::GHz2_4;  // Wi-Fi goes off-channel to the discovery band
  CHECK(arbitrate(operation, parsed.weights).grant == Grant::Bluetooth);
  operation.bt = BtScan::Inquiry;
  CHECK(arbitrate(operation, parsed.weights).grant == Grant::Wifi);
}

int main()
{
  testGoldenFrame();
  testCodecBounds();
  testAcquisitionAndRelease();
  testSleepAndWake();
  testPolicy();
  std::printf("coex_host: %u passed, %u failed\n", checks - failures, failures);
  return failures ? 1 : 0;
}
