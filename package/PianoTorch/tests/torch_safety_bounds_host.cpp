// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all torch_safety_bounds_host.cpp -o /tmp/torch_safety_bounds_host && /tmp/torch_safety_bounds_host

#include "../TorchCore.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>

namespace {
using namespace pianotorch;

constexpr unsigned kSafetyCeilingMa = 400;
constexpr unsigned kSafetyCeilingUa = kSafetyCeilingMa * 1000;
constexpr unsigned kDefaultCurrentUa = kDefaultOnMa * 1000;
static_assert(kDefaultLevel == 0 && kDefaultOnMa == kSafetyCeilingMa && kDefaultMaxMa == kSafetyCeilingMa &&
              kHardMaxMa == kSafetyCeilingMa);

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); ++failures; } } while (0)

// PM8550 register fixture. Observer reads are memory copies. Owner writes use
// the real SPMI transaction and status decoder with a fake channel and no delays.
class FlashPmic {
 public:
  FlashPmic()
  {
    reg[kRegType] = kTypeFlash;
    reg[kRegSubtype] = kSubtype4ch;
  }

  std::array<uint8_t, 256> reg{};
  unsigned writeAttempts = 0, writeFailures = 0, peakUa = 0;
  bool denyWrites = false;
  struct WriteFault {
    unsigned attempt = 0;
    uint32_t status = 0;
    bool applied = false;
  } fault;

  Bus bus()
  {
    return {this,
            [](void *ctx, uint8_t addr, uint8_t *out, unsigned bytes) {
              const auto &pmic = *static_cast<FlashPmic *>(ctx);
              if (!pianospmi::validRange(addr, bytes)) return false;
              std::memcpy(out, pmic.reg.data() + addr, bytes);
              return true;
            },
            [](void *ctx, uint8_t addr, uint8_t value) {
              auto &pmic = *static_cast<FlashPmic *>(ctx);
              const bool ok = spmiWriteByte(pmic.channel_, addr, value) == WriteResult::ok;
              if (!ok) ++pmic.writeFailures;
              return ok;
            }};
  }

  // Derive current from both hardware enables, the resolution and the programmed
  // targets. Ignore the clamp here so a bad ITARGET cannot hide behind it.
  unsigned currentUa() const
  {
    if (!(reg[kRegModuleEn] & kModuleEnBit)) return 0;
    unsigned ua = 0;
    for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
      if (!(reg[kRegChanEn] & (1u << ch))) continue;
      const unsigned stepUa = (reg[kRegIres] & (1u << ch)) ? 5000 : 12500;
      ua += ((reg[kRegItarget0 + ch] & kItargetMask) + 1) * stepUa;
    }
    return ua;
  }

  // Advance the PMIC through a hardware-timed pulse without calling Torch::poll.
  void runTimers(unsigned msSinceActivation)
  {
    if (!(reg[kRegModuleEn] & kModuleEnBit)) return;
    for (unsigned ch = 0; ch < kChannels4ch; ++ch) {
      const uint8_t bit = uint8_t(1u << ch);
      const uint8_t timer = reg[kRegTimer0 + ch];
      if ((reg[kRegChanEn] & bit) && (timer & kTimerEnable) && msSinceActivation >= (timer & kTimerValueMask) * 10u) {
        reg[kRegChanEn] &= uint8_t(~bit);
        reg[kRegStatus3] |= uint8_t(1u << (2 * ch));
      }
    }
  }

 private:
  void writeChannel(uint32_t offset, uint32_t value)
  {
    if (offset == kChnlWdata0) {
      data_ = uint8_t(value);
      return;
    }
    CHECK(offset == kChnlCmd && value == (uint32_t(uint8_t(value >> 4)) << 4));
    const bool failed = ++writeAttempts == fault.attempt;
    status_ = denyWrites ? kStatusDone | pianospmi::kDenied : failed ? fault.status : kStatusDone;
    if (denyWrites || (failed && !fault.applied)) return;
    reg[uint8_t(value >> 4)] = data_;
    peakUa = std::max(peakUa, currentUa());
  }

  uint8_t data_ = 0;
  uint32_t status_ = 0;
  const ChnlIo channel_{this,
                        [](void *ctx, uint32_t offset) {
                          CHECK(offset == kChnlStatus);
                          return static_cast<FlashPmic *>(ctx)->status_;
                        },
                        [](void *ctx, uint32_t offset, uint32_t value) {
                          static_cast<FlashPmic *>(ctx)->writeChannel(offset, value);
                        },
                        [](void *, unsigned) {}};
};

void checkOff(FlashPmic &pmic, Torch &torch)
{
  const auto live = torch.controller().live();
  CHECK(live.ok && !(live.moduleEn & kModuleEnBit) && !(live.chanEn & 0x0f));
  CHECK(!torch.lit() && torch.ma() == 0 && !torch.needsOff());
  CHECK(pmic.currentUa() == 0 && pmic.peakUa <= kSafetyCeilingUa);
}

void checkTeardown(FlashPmic &pmic, Torch &torch, const Torch::Outcome &off, Reason reason)
{
  CHECK(!off.lit && off.ma == 0 && off.reason == reason);
  CHECK(off.steps.ok && off.steps.readbacks >= 4);
  checkOff(pmic, torch);
}

void checkCurrent(uint64_t rawMa, unsigned configuredMaxMa)
{
  FlashPmic pmic;
  Torch torch(pmic.bus(), {}, configuredMaxMa);
  const unsigned expectedMax = std::clamp(configuredMaxMa, 10u, kSafetyCeilingMa);
  const uint64_t clamped = std::clamp<uint64_t>(rawMa, 10, expectedMax);
  const unsigned expectedMa = rawMa ? unsigned(clamped / 10) * 10 : 0;
  // setProperties and PianoTorchSet bound the incoming value before narrowing it.
  const auto out = torch.request(0, boundedRequest(rawMa, kHardMaxMa), 1, {});
  CHECK(torch.maxMa() == expectedMax && out.steps.ok);
  CHECK(out.lit == (expectedMa != 0) && out.ma == expectedMa && torch.ma() == expectedMa);
  CHECK(pmic.currentUa() == expectedMa * 1000 && pmic.peakUa <= expectedMax * 1000);
  if (expectedMa) {
    const uint8_t target = uint8_t(expectedMa / 10 - 1);
    CHECK(pmic.reg[kRegChanEn] == kPianoChannelMask && pmic.reg[kRegIres] == kPianoChannelMask);
    CHECK(out.steps.writes == 12 && out.steps.readbacks == 12);
    CHECK(pmic.reg[kRegItarget0] == target && pmic.reg[kRegItarget0 + 1] == target && target <= pmic.reg[kRegTorchClamp]);
    CHECK(pmic.reg[kRegTorchClamp] == expectedMax / 10 - 1);
  } else {
    checkOff(pmic, torch);
  }
}

void testCurrentBounds()
{
  // Include a non-quantized configured ceiling and an oversized boot argument.
  for (const unsigned limit : {0u, 9u, 10u, 95u, kUbuntuTorchMaxMa, 101u, 199u, 399u, kDefaultMaxMa, 401u, 500u, UINT32_MAX}) {
    for (unsigned ma = 0; ma <= 500; ++ma) checkCurrent(ma, limit);
    for (const uint64_t ma : {uint64_t(UINT32_MAX), uint64_t(uint32_t(INT32_MIN)), uint64_t(uint32_t(-2)),
                              uint64_t(INT64_MIN), uint64_t(-2), UINT64_MAX})
      checkCurrent(ma, limit);
    // Two channels share one total budget. Accumulate at ABI width before
    // clamping, including sums that would wrap to 0 or 1 in a 32-bit carrier.
    checkCurrent(uint64_t(51) + 50, limit);
    checkCurrent(uint64_t(UINT32_MAX) + 1, limit);
    checkCurrent(uint64_t(UINT32_MAX) + 2, limit);
    checkCurrent(uint64_t(UINT32_MAX) + UINT32_MAX, limit);
  }

  FlashPmic pmic;
  Torch torch(pmic.bus(), {}, 500);
  const auto on = torch.request(0, UINT32_MAX, 1, {});
  CHECK(on.lit && on.ma == kSafetyCeilingMa && pmic.currentUa() == kSafetyCeilingUa);
  CHECK(torch.controller().on(60, 60).ok);
  CHECK(pmic.reg[kRegTorchClamp] == 39 && pmic.reg[kRegItarget0] == 39 && pmic.reg[kRegItarget0 + 1] == 39);
  CHECK(pmic.currentUa() == kSafetyCeilingUa && pmic.peakUa == kSafetyCeilingUa);
  checkTeardown(pmic, torch, torch.turnOff(1, Reason::user), Reason::user);
}

void checkSoftwareDeadline(unsigned seconds, unsigned expectedSeconds)
{
  FlashPmic pmic;
  Torch torch(pmic.bus(), {}, kDefaultMaxMa);
  constexpr uint64_t start = uint64_t(1) << 32;
  const uint64_t end = start + expectedSeconds * 1000;
  const auto on = torch.request(start, kDefaultOnMa, seconds, {});
  CHECK(on.lit && torch.deadlineMs() == end);
  CHECK(pmic.reg[kRegTimer0] == 0 && pmic.reg[kRegTimer0 + 1] == 0);
  CHECK(torch.poll(end - 1, {}).lit && pmic.currentUa() == kDefaultCurrentUa);
  pmic.runTimers(expectedSeconds * 1000);
  CHECK(pmic.currentUa() == kDefaultCurrentUa);
  checkTeardown(pmic, torch, torch.poll(end, {}), Reason::timeout);
  const unsigned writes = pmic.writeAttempts;
  CHECK(!torch.poll(end + 1000, {}).lit && pmic.writeAttempts == writes);
}

void testSoftwareDeadline()
{
  const struct { uint64_t request; unsigned expectedSeconds; } cases[] = {
    {0, 300}, {1, 1}, {1799, 1799}, {1800, 1800}, {1801, 1800}, {UINT32_MAX, 1800}, {uint64_t(1) << 32, 1800}, {UINT64_MAX, 1800},
  };
  for (const auto &[seconds, expectedSeconds] : cases)
    checkSoftwareDeadline(boundedRequest(seconds, GuardConfig{}.maxS), expectedSeconds);
  checkSoftwareDeadline(UINT32_MAX, 1800);  // The core also bounds requests that bypass the ABI.
}

void testHardwareWatchdog()
{
  for (bool blockCleanup : {false, true}) {
    FlashPmic pmic;
    Torch torch(pmic.bus(), {}, 400);
    const auto on = torch.requestProbe(0, UINT32_MAX, UINT32_MAX, {}, kPianoChannelMask, true);
    CHECK(on.lit && on.ma == 50 && torch.deadlineMs() == 80 && pmic.currentUa() == 50000);
    CHECK(pmic.reg[kRegTimer0] == (kTimerEnable | 8) && pmic.reg[kRegTimer0 + 1] == (kTimerEnable | 8));
    const unsigned writes = pmic.writeAttempts;
    pmic.runTimers(79);
    CHECK(pmic.currentUa() == 50000 && pmic.reg[kRegStatus3] == 0);
    pmic.runTimers(80);
    CHECK(pmic.currentUa() == 0 && pmic.reg[kRegStatus3] == 0x05 && pmic.writeAttempts == writes);

    pmic.denyWrites = blockCleanup;
    const auto expired = torch.poll(80, {});
    CHECK(expired.sampled && expired.live.ok && expired.live.status3 == 0x05 && expired.live.chanEn == 0);
    if (blockCleanup) {
      CHECK(!expired.steps.ok && torch.needsOff() && pmic.currentUa() == 0);
      pmic.denyWrites = false;
      checkTeardown(pmic, torch, torch.poll(81, {}), Reason::test);
    } else {
      checkTeardown(pmic, torch, expired, Reason::test);
    }
  }
}

uint64_t reachLadderStep(FlashPmic &pmic, Torch &torch, unsigned finalMa)
{
  uint64_t start = 0;
  for (const unsigned ma : kLadderLevelsMa) {
    const auto on = torch.request(start, ma, kLadderWindowSeconds, {});
    CHECK(on.lit && on.ma == ma && on.steps.writes == 12 && on.steps.readbacks == 12);
    CHECK(pmic.currentUa() == ma * 1000 && pmic.reg[kRegTorchClamp] == 39);
    if (ma == finalMa) return start;
    checkTeardown(pmic, torch, torch.poll(start + kLadderWindowMs, {}, /*ladder=*/true), Reason::timeout);
    start += 2 * kLadderWindowMs;
  }
  CHECK(false);
  return start;
}

void testFaultCutoffs()
{
  const struct { uint8_t reg, bits; Reason reason; } cases[] = {
    {kRegStatus2, kSts2Otst1, Reason::pmicThermal}, {kRegStatus2, kSts2Otst2, Reason::pmicThermal},
    {kRegStatus2, kSts2BobThermOverload, Reason::pmicThermal}, {kRegStatus2, kSts2VphLow, Reason::pmicVoltage},
    {kRegStatus2, kSts2BclIbat, Reason::pmicCurrent}, {kRegStatus2, kSts2BobIlimS1, Reason::pmicCurrent},
    {kRegStatus2, kSts2BobIlimS2, Reason::pmicCurrent}, {kRegIntRtSts, kRtFault, Reason::pmicFault},
    {kRegStatus1, 0x01, Reason::shortCircuit}, {kRegStatus1, 0x08, Reason::channelOpen},
  };
  for (const auto &[addr, bits, reason] : cases) {
    FlashPmic pmic;
    Torch torch(pmic.bus(), {}, 400);
    const uint64_t trip = reachLadderStep(pmic, torch, 300) + kProbePollMs;
    pmic.reg[addr] = bits;
    const auto off = torch.poll(trip, {}, /*ladder=*/true);
    CHECK(off.steps.writes == 4 && off.steps.readbacks == 4);
    checkTeardown(pmic, torch, off, reason);
    pmic.reg[addr] = 0;
    const unsigned writes = pmic.writeAttempts;
    const auto blocked = torch.request(trip + 59999, 400, 10, {});
    CHECK(!blocked.lit && blocked.reason == reason && pmic.writeAttempts == writes);
    CHECK(torch.request(trip + 60000, 400, 10, {}).lit);
    checkTeardown(pmic, torch, torch.turnOff(trip + 60001, Reason::user), Reason::user);
  }

  const struct { Temperatures below, at; Reason reason; } temperatures[] = {
    {{.socValid = true, .socDeci = 849}, {.socValid = true, .socDeci = 850}, Reason::socHot},
    {{.batteryValid = true, .batteryCenti = 4499}, {.batteryValid = true, .batteryCenti = 4500}, Reason::batteryHot},
  };
  for (const auto &[below, at, reason] : temperatures) {
    FlashPmic pmic;
    Torch torch(pmic.bus(), {}, 400);
    const auto refused = torch.request(0, 400, 10, at);
    CHECK(!refused.lit && refused.reason == reason && pmic.writeAttempts == 0);
    const uint64_t start = reachLadderStep(pmic, torch, 300);
    CHECK(torch.poll(start + kProbePollMs, below, /*ladder=*/true).lit);
    const auto off = torch.poll(start + 2 * kProbePollMs, at, /*ladder=*/true);
    CHECK(off.steps.writes == 4 && off.steps.readbacks == 4);
    checkTeardown(pmic, torch, off, reason);
  }
}

void testSpmiWriteFaults()
{
  FlashPmic healthy;
  Torch normal(healthy.bus(), {}, 400);
  const auto on = normal.request(0, 400, 1, {});
  CHECK(on.lit && on.steps.writes == 12);
  const auto timeout = normal.poll(1000, {});
  checkTeardown(healthy, normal, timeout, Reason::timeout);

  for (const uint32_t status : {0u, kStatusDone | pianospmi::kDenied}) {
    // A timed-out write may still have reached the PMIC, even the final enable.
    for (unsigned step = 1; step <= on.steps.writes; ++step) {
      FlashPmic pmic;
      Torch torch(pmic.bus(), {}, 400);
      pmic.fault = {step, status, status == 0};
      const auto failed = torch.request(0, 400, 1, {});
      CHECK(!failed.lit && failed.ma == 0 && failed.reason == Reason::writeFailed);
      CHECK(!failed.steps.ok && failed.steps.writes == step - 1 && pmic.writeFailures == 1);
      CHECK(failed.cleanedUp && failed.cleanup.ok && failed.cleanup.readbacks == 4);
      if (step == on.steps.writes && status == 0) CHECK(pmic.peakUa == kSafetyCeilingUa);
      checkOff(pmic, torch);
    }
    for (unsigned step = 1; step <= timeout.steps.writes; ++step) {
      FlashPmic pmic;
      Torch torch(pmic.bus(), {}, 400);
      CHECK(torch.request(0, 400, 1, {}).lit);
      pmic.fault = {pmic.writeAttempts + step, status, status == 0};
      checkTeardown(pmic, torch, torch.poll(1000, {}), Reason::timeout);
      CHECK(pmic.writeFailures == 1);
    }
  }
}

// Host-only event adapter. System sleep maps to PianoTorch::offNow. Display-only
// suspend and client loss exercise the teardown contract through this fixture.
enum class StopEvent { displaySuspend, systemSuspend, clientDisconnect };

class HostLifecycle {
 public:
  explicit HostLifecycle(Torch &torch) : torch_(torch) {}

  bool queue(unsigned ma)
  {
    if (!clientConnected_ || !torch_.operational()) return false;
    pending_ = ma;
    return true;
  }

  Torch::Outcome dispatch(uint64_t now)
  {
    if (!pending_) return torch_.poll(now, {});
    const unsigned ma = *pending_;
    pending_.reset();
    return torch_.request(now, ma, 0, {});
  }

  Torch::Outcome stop(uint64_t now, StopEvent event)
  {
    pending_.reset();
    if (event == StopEvent::clientDisconnect) {
      clientConnected_ = false;
      return torch_.request(now, 0, 0, {});
    }
    return torch_.quiesce(now, Reason::sleep);
  }

  void resume() { torch_.resume(); }
  void connect() { clientConnected_ = true; }

 private:
  Torch &torch_;
  std::optional<unsigned> pending_;
  bool clientConnected_ = true;
};

void testBootOff()
{
  FlashPmic pmic;
  Torch torch(pmic.bus(), {}, kDefaultMaxMa);
  const auto boot = torch.checkBootState(0, survey(pmic.bus()));
  CHECK(!boot.lit && boot.ma == 0 && boot.reason == Reason::none && pmic.writeAttempts == 0);
  checkOff(pmic, torch);

  // Even an enabled controller that inherits a lit pair on startup clears it.
  pmic.reg[kRegModuleEn] = kModuleEnBit;
  pmic.reg[kRegChanEn] = kPianoChannelMask;
  CHECK(pmic.currentUa() > 0);
  checkTeardown(pmic, torch, torch.checkBootState(1, survey(pmic.bus())), Reason::bootFoundOn);
}

void testLifecycleTeardown()
{
  for (StopEvent event : {StopEvent::displaySuspend, StopEvent::systemSuspend, StopEvent::clientDisconnect}) {
    FlashPmic pmic;
    Torch torch(pmic.bus(), {}, 400);
    HostLifecycle host(torch);
    CHECK(host.queue(400) && host.dispatch(2).lit && pmic.currentUa() == kSafetyCeilingUa);
    CHECK(host.queue(50));  // Pending ON must be discarded along with the active one.
    const bool disconnect = event == StopEvent::clientDisconnect;
    const auto off = host.stop(3, event);
    checkTeardown(pmic, torch, off, disconnect ? Reason::user : Reason::sleep);
    CHECK(torch.operational() == disconnect && !host.queue(100));

    if (disconnect) host.connect();
    else host.resume();
    const unsigned writes = pmic.writeAttempts;
    CHECK(torch.operational() && !host.dispatch(4).lit && pmic.writeAttempts == writes);
    checkOff(pmic, torch);
    CHECK(host.queue(50) && host.dispatch(5).lit && pmic.currentUa() == 50000);
    checkTeardown(pmic, torch, host.stop(6, StopEvent::clientDisconnect), Reason::user);
  }
}

}  // namespace

int main()
{
  testCurrentBounds();
  testSoftwareDeadline();
  testHardwareWatchdog();
  testFaultCutoffs();
  testSpmiWriteFaults();
  testBootOff();
  testLifecycleTeardown();
  if (failures) return 1;
  std::puts("torch_safety_bounds_host: all passed");
  return 0;
}
