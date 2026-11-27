// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu M3 (DpuModeSwitch.h): every directed switch, 100 back-to-back switches, refusals and rollback.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_mode_switch_host.cpp -o /tmp/dpu_mode_switch_host && /tmp/dpu_mode_switch_host
#include "DpuModeSwitch.h"
#include "DpuBandwidth.h"
#include "DpuFlip.h"
#include "DpuNightShift.h"
#include "dpu_model.h"

#include <cstdlib>
#include <cstring>

using namespace pianodpu;
using namespace pianodpu::slices;
namespace m = pianodpu::modes;

namespace {

constexpr m::Options kAllTimings{.allowUnqualified = true};
unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

bool switchOk(model::Dpu &io, unsigned hz, const m::Options &options = kAllTimings)
{
  const auto r = m::switchTiming(io, hz, options);
  if (r.why) {
    std::printf("switch %u -> %u: %s (restore: %s)\n", r.from, hz, r.why, r.restoreWhy ? r.restoreWhy : "no error");
    return false;
  }
  CHECK(!r.reverted);
  if (r.from != hz) {
    CHECK(r.rate.lockstep);
    CHECK(std::abs(int(r.rate.milliHz) - int(hz * 1000)) < int(hz * 20));
  }
  return true;
}

void checkMode(model::Dpu &io, unsigned hz)
{
  const uint32_t fetch = hz == 144 ? 0x106da1 : hz == 120 ? 0x13b6c1 : hz == 90 ? 0x1a4901 : 0x280441;
  const auto mode = m::modeByHz(hz);
  const auto path = readPath(io);
  const m::LinkLive live[] = {m::readTiming(io, path.link[0], 0), m::readTiming(io, path.link[1], 1)};
  CHECK(m::classify(live).hz == hz);
  for (unsigned l = 0; l < 2; ++l) {
    CHECK(live[l].progFetch == fetch);
    CHECK(io.intf[l + 1].fetch == fetch);
    CHECK(io.half[l].b2 == mode->b2 && io.half[l].b3 == mode->b3);
    CHECK(!io.dsi[l].db && !io.dsi[l].armed);
    CHECK(io.rd(dsiReg(l, kDsiTpgCtrl)) == 0);
    CHECK(io.rd(dsiReg(l, kDsiCmdDmaCtrl)) == 0x14000000);
  }
  CHECK(io.irqDepth == 0);
}

void allPairs()
{
  for (bool boe : {true, false}) {
    m::Options options = kAllTimings;
    options.boe = boe;
    options.db = boe ? refresh::Db::sunuefi : refresh::Db::downstream;
    for (const auto &from : kModes) {
      for (const auto &to : kModes) {
        auto io = model::modeSwitchFixture();
        if (!boe) io.dbHw = model::DbHw::downstream;
        CHECK(switchOk(io, from.hz, options));
        io.dcs.clear();
        const size_t fetchWrites = io.storesTo(intfReg(1, kIntfProgFetch));
        const size_t priorStores = io.stores.size();
        CHECK(switchOk(io, to.hz, options));
        checkMode(io, to.hz);
        const bool anyChange = from.hz != to.hz;
        CHECK(io.storesTo(intfReg(1, kIntfProgFetch)) == fetchWrites + anyChange);
        if (!anyChange) CHECK(io.stores.size() == priorStores);
        const bool group = !m::sameGroup(from, to);
        CHECK(io.dcs.size() == (group ? 8u : 0u));
        if (group && io.dcs.size() == 8) {
          for (unsigned l = 0; l < 2; ++l) {
            const auto *cmd = &io.dcs[l * 4];
            CHECK(cmd[0].command == 0xff && cmd[0].parameter == 0x10);
            CHECK(cmd[1].command == 0xfb && cmd[1].parameter == 1);
            CHECK(cmd[2].command == (boe ? 0xb2 : 0xb3) && cmd[2].parameter == (boe ? to.b2 : to.b3));
            CHECK(cmd[3].command == (boe ? 0xb3 : 0xb2) && cmd[3].parameter == (boe ? to.b3 : to.b2));
            for (unsigned j = 0; j < 4; ++j) CHECK(!cmd[j].lowPower && cmd[j].link == l && cmd[j].ns == cmd[0].ns);
          }
        }
        CHECK(io.glitches == 0 && io.commandsInFrame == 0 && io.refused.empty());
      }
    }
  }
}

void repeatedSwitches()
{
  auto io = model::modeSwitchFixture();
  unsigned hz = 120;
  const unsigned sequence[] = {60, 90, 144, 120};
  for (unsigned i = 0; i < 100; ++i) {
    hz = sequence[i % 4];
    CHECK(switchOk(io, hz));
  }
  checkMode(io, hz);
  CHECK(io.glitches == 0 && io.commandsInFrame == 0 && io.refused.empty());
}

void refusals()
{
  auto io = model::modeSwitchFixture();
  bool transmitted = false;
  const auto timing = m::plan(m::geometry(m::readTiming(io, readPath(io).link[0], 0)), *m::modeByHz(120));
  CHECK(m::sendRateGroup(io, m::rateGroup(*m::modeByHz(144), true), {}, transmitted));
  CHECK(m::sendRateGroup(io, {}, timing, transmitted));
  CHECK(!transmitted && io.stores.empty());
  CHECK(m::switchTiming(io, 75, kAllTimings).why);
  CHECK(m::switchTiming(io, 60, {refresh::Db::direct}).why);
  io.regs[kGcc + kGccDispAhb] = 0x80000000;
  CHECK(m::switchTiming(io, 90, kAllTimings).why);
  io = model::modeSwitchFixture();
  io.regs[kDispCc + kCcPclk1] = 0x80000000;
  CHECK(m::switchTiming(io, 90, kAllTimings).why);
  io = model::modeSwitchFixture();
  io.regs[intfReg(1, kIntfProgFetch)] = 0;
  CHECK(m::switchTiming(io, 90, kAllTimings).why);
  io = model::modeSwitchFixture();
  io.dsi[0].total ^= 1;
  CHECK(m::switchTiming(io, 90, kAllTimings).why);
  io = model::modeSwitchFixture();
  io.regs[dsiReg(0, kDsiTpgCtrl)] = 1;
  CHECK(m::switchTiming(io, 90, kAllTimings).why);
  CHECK(io.stores.empty() && io.dcs.empty());
  CHECK(!storeAllowed(intfReg(1, kIntfHsyncCtl), 1));
  CHECK(storeAllowed(intfReg(1, kIntfHsyncCtl), 1, true));
  CHECK(storeAllowed(intfReg(1, kIntfProgFetch), 1));   // M5 already owns this register
  CHECK(!storeAllowed(intfReg(0, kIntfProgFetch), 1, true));
  CHECK(!storeAllowed(dsiReg(0, kDsiActiveV), 1, true));
  CHECK(!storeAllowed(kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride, 1, true));
}

void counterAndFlushGate()
{
  auto io = model::modeSwitchFixture();
  for (unsigned l = 0; l < 2; ++l) {
    io.countEnableIgnored[l] = true;
    io.intf[l + 1].counting = true;
  }
  CHECK(switchOk(io, 60, {}));  // NativeGopDisplay's initial request on a 120 Hz boot
  checkMode(io, 60);
  CHECK(switchOk(io, 144));
  checkMode(io, 144);
  CHECK(io.rd(intfReg(1, kIntfCountEn)) == 0 && io.rd(intfReg(2, kIntfCountEn)) == 0);
  CHECK(io.glitches == 0 && io.commandsInFrame == 0 && io.refused.empty());

  auto wrapped = model::modeSwitchFixture();
  for (unsigned i = 1; i <= 2; ++i) wrapped.intf[i].frames = UINT32_MAX - 1;
  const auto acrossWrap = m::switchTiming(wrapped, 60, {});
  CHECK(!acrossWrap.why && acrossWrap.counters.moving && acrossWrap.rate.lockstep);
  CHECK(wrapped.intf[1].frames < 10 && wrapped.intf[2].frames < 10);

  auto disabled = model::modeSwitchFixture();
  const m::Options off{.fixedHz = m::bootModeHz(0)};
  const auto unexpected = [] { CHECK(false); };
  for (unsigned hz : {0u, 60u, 120u, 144u}) {
    const auto r = m::switchTo(disabled, hz, off, unexpected, unexpected);
    CHECK(r.why && !r.timingTouched && !r.counters.sampled);
  }
  CHECK(disabled.stores.empty() && disabled.dcs.empty() && disabled.bandwidthVotes.empty() && disabled.ns == 0);

  auto checkRefusal = [](model::Dpu &blocked, const char *expected) {
    const auto unexpected = [] { CHECK(false); };
    const auto r = m::switchTo(blocked, 60, {}, unexpected, unexpected);
    CHECK(r.from == 120 && r.why && !r.timingTouched && !r.reverted);
    CHECK(blocked.stores.empty() && blocked.dcs.empty());
    if (!r.why) return;
    char line[256] = {};
    m::logFailure(r, 60, [&](const char *fmt, auto... args) {
      std::snprintf(line, sizeof(line), fmt, args...);
    });
    CHECK(std::strcmp(line, expected) == 0);
    checkMode(blocked, 120);
  };

  io = model::modeSwitchFixture();
  for (unsigned i = 1; i <= 2; ++i)
    io.regs[intfReg(i, kIntfCountEn)] = 3;  // enable bits read back, both counters still frozen
  checkRefusal(io, "mode: 120 -> 60 Hz: INTF counters not advancing in lockstep "
                   "(count-en 3/3, frame deltas 0/0, lines 0/0 -> 0/0, line moved 0/0); refused");

  io = model::modeSwitchFixture();
  io.intf[1].counting = io.intf[2].counting = true;
  io.ctlFlush = kFlushIntf;
  io.flushStuck = true;
  checkRefusal(io, "mode: 120 -> 60 Hz: CTL flush is pending (CTL0, CTL_FLUSH 0x80000000); refused");
}

void commandClockWrap()
{
  struct WrappedDpu : model::Dpu {
    uint64_t nowUs() { return (UINT64_MAX - 8250) + model::Dpu::nowUs(); }
  } io;
  static_cast<model::Dpu &>(io) = model::modeSwitchFixture();
  io.intf[1].counting = io.intf[2].counting = true;
  const auto &mode = *m::modeByHz(120);
  const auto path = readPath(io);
  const auto timing = m::plan(m::geometry(m::readTiming(io, path.link[0], 0)), mode);
  bool transmitted = false;
  CHECK(!m::sendRateGroup(io, m::rateGroup(mode, true), timing, transmitted));
  CHECK(transmitted && io.dcs.size() == 8 && io.commandsInFrame == 0);
}

void rollback()
{
  auto io = model::modeSwitchFixture();
  io.dropStore = intfReg(2, kIntfProgFetch);
  io.dropValue = 0x106da1;
  io.dropCount = 1;
  auto r = m::switchTiming(io, 144, kAllTimings);
  CHECK(r.why && r.reverted && !r.restoreWhy);
  CHECK(io.dcs.empty());  // timing read-back failed before the command
  checkMode(io, 120);

  io = model::modeSwitchFixture();
  io.dropStore = dsiReg(1, kDsiTotal);
  io.dropValue = 0x120b023f;
  io.dropCount = 1;
  r = m::switchTiming(io, 60, {});
  CHECK(r.why && r.reverted && !r.restoreWhy);
  checkMode(io, 120);

  io = model::modeSwitchFixture();
  CHECK(switchOk(io, 144));
  io.underrunAfterDma = 1;   // both halves accepted the new group before the timing check failed
  io.handleUnderrunIrq = true;
  r = m::switchTiming(io, 90, kAllTimings);
  CHECK(r.why && r.reverted && !r.restoreWhy);
  checkMode(io, 144);

  io = model::modeSwitchFixture();
  io.dmaHang[1] = true;
  r = m::switchTiming(io, 144, kAllTimings);
  CHECK(r.why && r.reverted && r.restoreWhy);
  CHECK(io.dsi[0].total == 0x0905023f && io.dsi[1].total == 0x0905023f);
  CHECK(io.half[0].b2 == 0x91 && io.half[0].b3 == 0x40);  // the idle link got the old group
  CHECK(io.dsi[1].dmaPending);  // still owned by hardware
  CHECK(io.glitches == 0 && io.commandsInFrame == 0);

  io = model::modeSwitchFixture();
  io.lateDmas = 2;
  r = m::switchTiming(io, 144, kAllTimings);
  CHECK(r.why && std::strstr(r.why, "front porch") && r.reverted && !r.restoreWhy);
  checkMode(io, 120);

  io = model::modeSwitchFixture();
  io.flushStuck = true;
  r = m::switchTiming(io, 90, kAllTimings);
  CHECK(r.why && r.reverted && r.restoreWhy);
  CHECK(io.dcs.empty());
}

void qual1Underrun()
{
  auto io = model::modeSwitchFixture();
  for (unsigned l = 0; l < 2; ++l) {
    io.countEnableIgnored[l] = true;
    io.intf[l + 1].counting = true;
  }
  io.minFetchLineNs = 3300;  // between the 144 and 120/60 Hz line times
  io.handleUnderrunIrq = true;
  CHECK(switchOk(io, m::kBootModeHz, {}));
  checkMode(io, m::kBootModeHz);

  const auto unexpected = [] { CHECK(false); };
  const auto stores = io.stores.size();
  // A stale WindowServer choice (144 Hz in qual1) cannot reach the timing or command path.
  for (unsigned hz : {144u, 90u, 120u}) {
    const auto refused = m::switchTo(io, hz, {}, unexpected, unexpected);
    CHECK(refused.why && !refused.timingTouched && !refused.reverted);
  }
  CHECK(io.stores.size() == stores && io.dcs.empty() && io.bandwidthVotes.empty() && !io.modeUnderruns());
  CHECK(switchOk(io, m::kBootModeHz, {}));  // an already running 60 Hz mode must verify without staging
  for (size_t n = stores; n < io.stores.size(); ++n)
    CHECK(io.stores[n].pa == intfReg(1, kIntfCountEn) || io.stores[n].pa == intfReg(2, kIntfCountEn));
  checkMode(io, m::kBootModeHz);

  // Exercise the old failing request in the timed model only. The same starvation continues
  // until the recovery latch. Those IRQ-sticky bits cannot fail the restored 60 Hz interval.
  const auto r = m::switchTiming(io, 144, kAllTimings);
  CHECK(r.from == 60 && r.reverted && r.timingTouched && r.rate.lockstep);
  CHECK(r.why && std::strcmp(r.why, "an INTF underran") == 0);
  CHECK(r.restoreTransitionUnderruns == kUnderrunBits && !r.restoreWhy && !io.modeUnderruns());
  CHECK(io.dcs.size() == 16 && io.glitches == 0);
  checkMode(io, 60);

  // Also exercise an accepted VFP-only boot switch with a missing latch on one link.
  io = model::modeSwitchFixture();
  io.dropStore = intfReg(2, kIntfProgFetch);
  io.dropValue = 0x280441;
  io.dropCount = 1;
  const auto restoredBoot = m::switchTiming(io, m::kBootModeHz, {});
  CHECK(restoredBoot.why && restoredBoot.reverted && !restoredBoot.restoreWhy);
  CHECK(restoredBoot.from == m::kInheritedModeHz && io.dcs.empty());
  checkMode(io, m::kInheritedModeHz);

  io = model::modeSwitchFixture();
  io.minFetchLineNs = 3700;  // a fault that continues after restoring the old timing
  const auto brokenBoot = m::switchTiming(io, m::kBootModeHz, {});
  CHECK(brokenBoot.why && brokenBoot.reverted && brokenBoot.restoreTransitionUnderruns);
  CHECK(brokenBoot.restoreWhy && std::strcmp(brokenBoot.restoreWhy, "an INTF underran") == 0);
  CHECK(io.modeUnderruns() && io.dcs.empty());

  io = model::modeSwitchFixture();
  CHECK(switchOk(io, 90));
  const auto inheritedStores = io.stores.size();
  const auto wrongBootMode = m::switchTiming(io, m::kBootModeHz, {});
  CHECK(wrongBootMode.why && wrongBootMode.from == 90 && !wrongBootMode.timingTouched);
  CHECK(io.stores.size() == inheritedStores);
}

void fixed120Boot()
{
  static_assert(m::bootModeHz(1) == 60 && m::bootModeHz(120) == 120);
  static_assert(!m::bootModeHz(0) && !m::bootModeHz(60) && !m::bootModeHz(90));
  const m::Options options{.fixedHz = m::bootModeHz(120)};
  auto io = model::modeSwitchFixture();
  for (unsigned l = 0; l < 2; ++l) {
    io.countEnableIgnored[l] = true;
    io.intf[l + 1].counting = true;
  }
  io.minFetchLineNs = 3300;  // the inherited resources can feed 120 but would underrun at 144
  const auto unexpected = [] { CHECK(false); };
  const auto inherited = m::switchTo(io, 120, options, unexpected, unexpected);
  CHECK(!inherited.why && inherited.from == 120 && !inherited.timingTouched && !inherited.reverted);
  CHECK(inherited.rate.lockstep && inherited.rate.milliHz >= 117600 && inherited.rate.milliHz <= 122400);
  for (const auto &s : io.stores)
    CHECK(s.pa == intfReg(1, kIntfCountEn) || s.pa == intfReg(2, kIntfCountEn));
  checkMode(io, 120);

  const auto stores = io.stores.size();
  for (unsigned hz : {60u, 90u, 144u}) {
    const auto refused = m::switchTo(io, hz, options, unexpected, unexpected);
    CHECK(refused.why && !refused.timingTouched && !refused.reverted);
  }
  CHECK(io.stores.size() == stores && io.dcs.empty() && io.bandwidthVotes.empty());

  // The measurement must catch starvation even when all inherited timing words already match.
  io.minFetchLineNs = 3700;
  const auto starved = m::switchTo(io, 120, options, unexpected, unexpected);
  CHECK(starved.why && std::strcmp(starved.why, "an INTF underran") == 0);
  CHECK(!starved.timingTouched && !starved.reverted && starved.rate.lockstep);

  io = model::modeSwitchFixture();
  io.minFetchLineNs = 3300;
  CHECK(switchOk(io, 60, {}));
  auto fail = io;
  const auto up = m::switchTiming(io, 120, options);
  CHECK(!up.why && up.from == 60 && up.timingTouched && !up.reverted && up.rate.lockstep);
  CHECK(io.dcs.empty() && !io.glitches && !io.modeUnderruns() && io.refused.empty());
  checkMode(io, 120);

  fail.dropStore = intfReg(2, kIntfProgFetch);
  fail.dropValue = 0x13b6c1;
  fail.dropCount = 1;
  const auto restored = m::switchTiming(fail, 120, options);
  CHECK(restored.why && restored.from == 60 && restored.reverted && !restored.restoreWhy);
  CHECK(fail.dcs.empty() && !fail.modeUnderruns());
  checkMode(fail, 60);
}

void fixed144Boot()
{
  static_assert(m::bootModeHz(144) == 144);
  static_assert(m::scanoutAvgKBps(kModes[0]) == 5109350 && m::scanoutAvgKBps(kModes[1]) == 4257792);
  static_assert(m::scanoutAvgKBps(kModes[3]) == 4257792);  // 60 Hz doubles the total lines
  static_assert(m::kMm0PeakKBps == 4257792);
  constexpr auto mm0At144 = m::mm0Vote(kModes[0], 2000000, 32);
  constexpr auto mm0At120 = m::mm0Vote(kModes[1], 2000000, 32);
  static_assert(mm0At144.average == 1277 && mm0At120.average == 1064);
  static_assert(mm0At144.peak == 2128 && mm0At120.peak == mm0At144.peak);
  static_assert(m::mm0Vote(kModes[0], 0, 32).average == 0 && m::mm0Vote(kModes[0], 1, 0).peak == 0);
  static_assert(m::mm0Vote(kModes[0], 1, 32).average == Sm8750RpmhBcmVoteMask);

  const m::Options options{.fixedHz = m::kHighModeHz};
  for (unsigned from : {60u, 120u}) {
    auto io = model::modeSwitchFixture();
    io.minFetchLineNs = 3300;
    if (from == 60) CHECK(switchOk(io, from, {}));
    const size_t start = io.stores.size();

    auto refused = io;
    refused.bandwidthVoteFails = true;
    const auto blocked = m::switchTiming(refused, 144, options);
    CHECK(blocked.why && !blocked.timingTouched && !blocked.reverted);
    CHECK(refused.bandwidthVotes.size() == 1 && refused.dcs.empty());
    for (size_t n = start; n < refused.stores.size(); ++n)
      CHECK(refused.stores[n].pa == intfReg(1, kIntfCountEn) || refused.stores[n].pa == intfReg(2, kIntfCountEn));
    checkMode(refused, from);

    auto starved = io;
    const auto restored = m::switchTiming(starved, 144, options);
    CHECK(restored.why && restored.reverted && restored.from == from && !restored.restoreWhy);
    CHECK(restored.underruns == kUnderrunBits && starved.bandwidthVotes.size() == 1);
    checkMode(starved, from);

    io.votedMinFetchLineNs = 2900;
    const auto result = m::switchTiming(io, 144, options);
    CHECK(!result.why && result.timingTouched && !result.reverted && result.rate.lockstep && !result.underruns);
    CHECK(io.bandwidthVotes.size() == 1 && io.bandwidthVotes.front() < io.stores.size());
    if (!io.bandwidthVotes.empty())
      for (size_t n = start; n < io.bandwidthVotes.front(); ++n)
        CHECK(io.stores[n].pa == intfReg(1, kIntfCountEn) || io.stores[n].pa == intfReg(2, kIntfCountEn));
    CHECK(io.dcs.size() == 8 && !io.glitches && io.refused.empty());
    checkMode(io, 144);

    const auto path = readPath(io);
    const panel::Options sleep;
    const auto off = panel::panelOff(io, path, sleep);
    CHECK(off.ok && off.enginesStopped);
    CHECK(panel::panelOn(io, path, sleep, off.enginesStopped, off.fetch).ok);
    checkMode(io, 144);
    CHECK(io.bandwidthVotes.size() == 1 && !io.modeUnderruns());
  }

  auto io = model::modeSwitchFixture();
  io.ctlFlush = 0x10;
  const auto pending = m::switchTiming(io, 144, options);
  CHECK(pending.why && !pending.timingTouched && io.bandwidthVotes.empty());
}

void sleepAndFetch()
{
  for (bool stopEngines : {true, false}) {
    auto io = model::modeSwitchFixture();
    CHECK(switchOk(io, 90));
    const auto path = readPath(io);
    panel::Options options;
    options.engines = stopEngines;
    const auto off = panel::panelOff(io, path, options);
    CHECK(off.ok && off.enginesStopped == stopEngines);
    const auto stores = io.stores.size(), commands = io.dcs.size();
    m::Options modeOptions;
    modeOptions.panelUnavailable = off.ok;
    const auto unexpected = [] { CHECK(false); };
    const auto blocked = m::switchTo(io, 60, modeOptions, unexpected, unexpected);
    CHECK(blocked.why && !blocked.timingTouched && !blocked.reverted);
    CHECK(io.stores.size() == stores && io.dcs.size() == commands);
    for (unsigned i = 1; i <= 2; ++i) {
      CHECK(io.half[i - 1].sleeping && !io.half[i - 1].displayOn);
      CHECK(bool(io.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch) == !stopEngines);
      CHECK(io.intf[i].on == !stopEngines);
    }
    CHECK(panel::panelOn(io, path, options, off.enginesStopped, off.fetch).ok);
    checkMode(io, 90);
    for (unsigned i = 1; i <= 2; ++i) CHECK(io.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch);
    CHECK(switchOk(io, 144));
    checkMode(io, 144);
    CHECK(!io.glitches && !io.desync && !io.commandsInFrame && io.refused.empty());
  }
}

void nightShiftAndCleanEpochs()
{
  auto io = model::modeSwitchFixture();
  io.resetPccOnTimingChange = true;
  pcc::PanelPcc base;
  CHECK(pcc::capturePanelPcc(io, readPath(io), base));
  pcc::PanelPcc last = base;
  pcc::NightShift state;
  // ns-base: the first location-9 matrix is the boot reference (cancelled); send an identity reference first so the
  // white point below is applied as a relative change, as with Night Shift turned on after boot.
  const uint64_t identityRef[9] = {1ull << 32, 0, 0, 0, 1ull << 32, 0, 0, 0, 1ull << 32};
  CHECK(state.setMatrix(identityRef) == pcc::NightShift::Input::accepted);
  const uint64_t whitePoint[9] = {1ull << 32, 1ull << 29, 0, 0, 3ull << 30, 0, 0, 0, 1ull << 31};
  CHECK(state.setMatrix(whitePoint) == pcc::NightShift::Input::accepted);
  state.setCorrection(pcc::kOne / 2);
  const auto wanted = pcc::nightShiftPrograms(base, state);
  CHECK(pcc::updateNightShift(io, readPath(io), wanted, last, false).result.ok);

  struct TestFlipHardware {} hw;
  flip::Flipper flipper(hw);
  using Action = flip::CleanEpoch::Action;
  unsigned cleans = 0, verifications = 0, barriers = 0;
  auto prepare = [&](unsigned slot) {
    return flipper.cleanEpochs[slot].prepare(flip::CleanReason::eligible, [&](bool whole) {
      CHECK(whole);
      ++cleans;
      return true;
    }, [&] {
      ++verifications;
      return flip::CleanReason::eligible;
    }, [&] { ++barriers; }).action;
  };
  for (unsigned i = 0; i < smmu::kSlots; ++i) CHECK(prepare(i) == Action::initialized);

  unsigned invalidations = 0, replays = 0;
  auto invalidate = [&] {
    ++invalidations;
    flipper.invalidateCleanEpochs();
  };
  auto replay = [&] {
    ++replays;
    CHECK(io.pccActive[0][0] == 0 && io.pccActive[1][0] == 0);
    CHECK(pcc::updateNightShift(io, readPath(io), wanted, last, true).result.ok);
  };
  auto checkColor = [&] {
    for (unsigned d = 0; d < 2; ++d) {
      CHECK(io.pccActive[d][0] == 1 && io.pccActive[d][4] == 0x4000);
      CHECK(io.pccActive[d][7] == 0x0800 && io.pccActive[d][12] == 0x2000);
    }
  };
  checkColor();
  CHECK(!m::switchTo(io, 120, kAllTimings, invalidate, replay).why);
  CHECK(m::switchTo(io, 75, kAllTimings, invalidate, replay).why);
  CHECK(!invalidations && !replays && prepare(0) == Action::skipped);

  CHECK(!m::switchTo(io, 144, kAllTimings, invalidate, replay).why);
  CHECK(invalidations == 1 && replays == 1);
  checkColor();
  for (unsigned i = 0; i < smmu::kSlots; ++i) CHECK(prepare(i) == Action::initialized);
  CHECK(prepare(0) == Action::skipped);

  io.underrunAfterDma = 1;
  const auto failed = m::switchTo(io, 90, kAllTimings, invalidate, replay);
  CHECK(failed.why && failed.reverted && !failed.restoreWhy);
  CHECK(invalidations == 2 && replays == 2);
  checkColor();
  for (unsigned i = 0; i < smmu::kSlots; ++i) CHECK(prepare(i) == Action::initialized);
  CHECK(cleans == 3 * smmu::kSlots && verifications == cleans && barriers == 2);

  io.flushStuck = true;
  const auto unrestored = m::switchTo(io, 90, kAllTimings, invalidate, replay);
  CHECK(unrestored.why && unrestored.restoreWhy);
  CHECK(invalidations == 3 && replays == 2 && !flipper.cleanEpochs[0].initialized);
}

}  // namespace

int main()
{
  allPairs();
  repeatedSwitches();
  refusals();
  counterAndFlushGate();
  commandClockWrap();
  rollback();
  qual1Underrun();
  fixed120Boot();
  fixed144Boot();
  sleepAndFetch();
  nightShiftAndCleanEpochs();
  std::printf("PianoDpu M3 mode switch: %u checks, %u failures\n", checks, failures);
  return failures != 0;
}
