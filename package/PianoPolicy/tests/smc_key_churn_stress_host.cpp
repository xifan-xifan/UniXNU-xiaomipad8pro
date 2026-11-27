// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. smc_key_churn_stress_host.cpp -o /tmp/smc_key_churn_stress_host && /tmp/smc_key_churn_stress_host
// Exercise the selector-2 wire contract and interleave calls with real battery mapping and policy steps.
#include "Smc.h"
#include "Policy.h"
#include "../../PianoPower/BatteryMap.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

using namespace pianosmc;
namespace policy = pianopolicy;

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// MSHP is the only published key. These requests must stay absent even when the corresponding policy inputs change.
constexpr uint32_t absentKeys[] = {
  key4('B', '0', 'A', 'V'), key4('B', '0', 'A', 'C'), key4('B', 'R', 'S', 'C'),
  key4('C', 'H', '0', 'R'), key4('M', 'S', 'L', 'D'), key4('#', 'K', 'E', 'Y'),
  key4('T', 'C', '0', 'P'), key4('T', 'B', '0', 'T'),
  key4('z', 'E', 'T', 'M'), key4('z', 'E', 'C', 'm'), key4('z', 'E', 'A', 'R'),
  key4('M', 'S', 'H', 'p'), key4('Z', 'Z', 'Z', 'Z'), 0, UINT32_MAX,
};
constexpr uint8_t keyCommands[] = {kCmdKeyInfo, kCmdRead, kCmdWrite};
constexpr size_t kIndexAt = 0x2c;
static_assert(kKeyMSHP == 0x4d534850);

// Keep the wire fixtures independent of the encoder/decoder in Smc.h.
static void wireU32(uint8_t *field, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) field[i] = uint8_t(value >> (i * 8));
}

struct Request {
  uint32_t key;
  uint8_t command;
  uint32_t size;
  uint32_t index = 0;

  void encode(uint8_t *buf) const
  {
    wireU32(buf + kKeyAt, key);
    wireU32(buf + kDataSizeAt, size);
    wireU32(buf + kIndexAt, index);
    buf[kCommandAt] = command;
  }
};

static void checkReply(const Call &call, const Request &req, bool present, const uint8_t *bytes, size_t size)
{
  const uint8_t successInfo[] = {1, 0, 0, 0, ' ', '8', 'i', 'u', 0x80};
  const uint8_t missingInfo[sizeof successInfo] = {};
  const uint8_t *info = present ? successInfo : missingInfo;
  const uint8_t result = present ? 0 : 0x84;
  if (call.ret != 0 || call.key != req.key || call.command != req.command || call.mshp != present || call.result != result) {
    std::fprintf(stderr, "key %08x command %u length %u index %u output size %zu\n",
                 req.key, unsigned(req.command), req.size, req.index, size);
    CHECK(false);
  }
  uint8_t key[4];
  wireU32(key, req.key);
  CHECK(std::equal(key, key + sizeof key, bytes + kKeyAt));
  CHECK(bytes[kCommandAt] == req.command && bytes[kStatusAt] == 0 && bytes[kResultAt] == result);
  CHECK(std::equal(info, info + sizeof successInfo, bytes + kDataSizeAt));
  CHECK(bytes[kBytesAt] == (present && req.command == kCmdRead ? 1 : 0));
  CHECK(std::all_of(bytes + kBytesAt + 1, bytes + size, [](uint8_t b) { return b == 0; }));
}

static void checkFrame(Config config, const Request &req, size_t inSize, size_t outSize, bool present)
{
  // Exact-size separate buffers put ASan redzones at both ends of the declared frames.
  std::vector<uint8_t> in(inSize, 0xa5), out(outSize, 0xee);
  req.encode(in.data());
  auto staged = in;
  staged.resize(std::max(inSize, outSize), 0xee);
  const Call separate = answer(config, in.data(), inSize, out.data(), outSize);
  const Call inplace = answer(config, staged.data(), inSize, staged.data(), outSize);
  checkReply(separate, req, present, out.data(), outSize);
  checkReply(inplace, req, present, staged.data(), outSize);
  CHECK(std::equal(out.begin(), out.end(), staged.begin()));
  if (inSize > outSize) CHECK(std::equal(in.begin() + outSize, in.end(), staged.begin() + outSize));
}

static void matrix()
{
  const size_t frameSizes[] = {kMinBytes, kMinBytes + 1, kSmallBytes, kLargeBytes, kMaxBytes};
  const uint32_t payloadSizes[] = {0, 1, 2, 31, 32, 33, 119, 120, 121, 464, 465, UINT32_MAX};
  for (bool enabled : {true, false}) {
    const Config config{enabled};
    for (size_t inSize : frameSizes) {
      for (size_t outSize : frameSizes) {
        for (uint32_t length : payloadSizes) {
          for (uint8_t command : keyCommands) {
            checkFrame(config, {kKeyMSHP, command, length}, inSize, outSize, enabled && command != kCmdWrite);
            for (uint32_t key : absentKeys) checkFrame(config, {key, command, length}, inSize, outSize, false);
          }
        }
        // The directly addressable key has no index, even at 0. Sweep past the table boundary and through extremes.
        for (uint32_t index = 0; index <= 64; ++index)
          checkFrame(config, {0, kCmdKeyByIndex, 0, index}, inSize, outSize, false);
        for (uint32_t index : {0u, 1u, 0xffffu, 0x80000000u, UINT32_MAX})
          checkFrame(config, {kKeyMSHP, kCmdKeyByIndex, 0, index}, inSize, outSize, false);
      }
    }
  }

  std::array<uint8_t, kMaxBytes + 1> buf;
  buf.fill(0xa5);
  for (uint8_t command : {kCmdRead, kCmdKeyInfo, kCmdWrite, kCmdKeyByIndex}) {
    Request{kKeyMSHP, command, UINT32_MAX}.encode(buf.data());
    const auto before = buf;
    for (size_t bad : {size_t(0), kBytesAt - 1, kMinBytes - 1, kMaxBytes + 1, SIZE_MAX}) {
      CHECK(answer({}, buf.data(), bad, buf.data(), kSmallBytes).ret == kBadSize);
      CHECK(answer({}, buf.data(), kSmallBytes, buf.data(), bad).ret == kBadSize);
      CHECK(buf == before);
    }
    CHECK(answer({}, nullptr, kSmallBytes, buf.data(), kSmallBytes).ret == kBadSize);
    CHECK(answer({}, buf.data(), kSmallBytes, nullptr, kSmallBytes).ret == kBadSize);
    CHECK(buf == before);
  }
}

static void churn()
{
  using namespace policy;
  const pianosmc::Config smc{};
  CHECK(typeMaskForLevel(1) == 0x2);
  policy::Config config;
  config.act = true;               // piano-policy=2
  config.lid = true;
  config.gpuNeeded = true;
  config.thermalOwnsGpu = true;    // exercise thermal protection on a shared GPU slot
  Engine engine(config);
  Inputs in;
  in.lidSleepAC = 0;
  in.lidSleepArmed = true;
  in.sleepOn = true;
  in.wakeMask = kWakePowerKey | kWakeLid;
  in.attached = true;
  in.thermalProfiles = kThermalPerformance;

  struct Phase {
    bool ac, closed, charging, lpm, hpm;
    Thermal band;
    Profile profile;
    uint32_t thermalMode;
    std::vector<uint32_t> root;
  };
  const uint32_t acOn = kSetAcAdaptorConnected | kSetValue;
  const uint32_t acOff = kSetAcAdaptorConnected;
  const Phase phases[] = {
    {true,  false, false, false, false, Thermal::normal,   Profile::balanced,    kThermalAuto,        {kDisableClamshell, acOn}},
    {true,  true,  true,  true,  false, Thermal::warm,     Profile::saver,       kThermalSaver,       {kClamshellClosed}},
    {false, true,  false, true,  true,  Thermal::passive,  Profile::saver,       kThermalSaver,       {acOff, kEnableClamshell}},
    {false, false, false, false, true,  Thermal::critical, Profile::performance, kThermalPerformance, {kClamshellOpened}},
    {false, true,  false, false, false, Thermal::normal,   Profile::saver,       kThermalAuto,        {kClamshellClosed}},
    {true,  true,  true,  false, true,  Thermal::passive,  Profile::performance, kThermalPerformance, {kDisableClamshell, acOn}},
    {true,  false, false, true,  true,  Thermal::warm,     Profile::saver,       kThermalSaver,       {kClamshellOpened}},
    {false, false, false, false, false, Thermal::unknown,  Profile::saver,       kThermalAuto,        {acOff, kEnableClamshell}},
  };
  std::array<uint8_t, kLargeBytes> frame;
  frame.fill(0xcc);
  uint32_t mode = kThermalAuto;
  constexpr unsigned updates = 1280;  // 640 transitions each for clamshell, source and charging state
  for (unsigned i = 0; i < updates; ++i) {
    const Phase &p = phases[i % std::size(phases)];
    pianopower::PackReading pack;
    pack.ok = true;
    pack.fullMah = 9000;
    pack.socPct = int(35 + i % 41);
    pack.remainingMah = pack.fullMah * pack.socPct / 100;
    pack.voltageMv = int(7400 + i % 600);
    pack.tempDeciC = int(260 + i % 120);
    pack.charging = p.charging;
    pack.discharging = !p.ac;
    pack.currentMa = p.charging ? 1700 : p.ac ? 0 : -900;
    pianopower::PortState port;
    port.ok = true;
    port.sourceSeen = p.ac;
    const auto battery = pianopower::mapBattery(nullptr, 0, pack, &port, 0);
    CHECK(battery.known && battery.external == p.ac && battery.charging == p.charging);
    CHECK(battery.percent == unsigned(pack.socPct) && battery.voltageMv == unsigned(pack.voltageMv));
    CHECK(battery.instantMa == pack.currentMa && battery.temperatureCenti == pack.tempDeciC * 10);

    in.power = battery.external ? Power::external : Power::battery;
    in.lid = p.closed ? Lid::closed : Lid::open;
    in.thermal = p.band;
    in.lpm = p.lpm;
    in.hpm = p.hpm;
    auto expectedRoot = p.root;
    if (i == 0) expectedRoot.push_back(kClamshellOpened);
    const auto actions = engine.step(in, i);
    const View state = view(engine.config(), in, engine.seenOpen());
    CHECK(state.close == (p.ac ? Close::lock : Close::sleep) && state.profile == p.profile);
    const bool held = p.band >= Thermal::passive;
    std::vector<uint32_t> root;
    unsigned userActive = 0;
    for (unsigned j = 0; j < actions.n; ++j) {
      const Action &a = actions.a[j];
      if (a.kind == Kind::root) root.push_back(a.value);
      if (a.kind == Kind::userActive) ++userActive;
      if (a.kind == Kind::thermalProfile) {
        mode = a.value;
        engine.profileDone(mode, Result::taken);
      }
      if (a.kind == Kind::gpuCeiling) {
        CHECK(!held && a.client == kGpuClientShared);
        CHECK(engine.gpuDone(a.value, Result::taken, in.thermal, a.client).n == 0);
      }
    }
    CHECK(root == expectedRoot && mode == p.thermalMode);
    const bool reopened = i != 0 && expectedRoot.back() == kClamshellOpened;
    CHECK(userActive == unsigned(reopened));
    CHECK(engine.gpuHeld() == held);
    if (!held) CHECK(engine.gpuWritten() == (p.profile == Profile::saver ? 342000u : 0u));
    CHECK(engine.step(in, i).n == 0);

    // Reuse the previous reply as the next request, including an attempted write with a poisoned payload.
    const size_t bytes = i % 2 ? kSmallBytes : kLargeBytes;
    const auto call = [&](const Request &req, bool present) {
      req.encode(frame.data());
      const Call c = answer(smc, frame.data(), bytes, frame.data(), bytes);
      checkReply(c, req, present, frame.data(), bytes);
    };
    call({kKeyMSHP, kCmdKeyInfo, 0}, true);
    frame[kBytesAt] = 0x7e;
    call({kKeyMSHP, kCmdWrite, 1}, false);
    call({kKeyMSHP, kCmdRead, 1}, true);
    for (uint32_t key : absentKeys) {
      call({key, kCmdRead, UINT32_MAX}, false);
      call({kKeyMSHP, kCmdRead, 1}, true);
    }
  }
  std::printf("smc_key_churn_stress_host: %u updates\n", updates);
}

int main()
{
  matrix();
  churn();
  std::printf("smc_key_churn_stress_host: %u checks\n", checks);
}
