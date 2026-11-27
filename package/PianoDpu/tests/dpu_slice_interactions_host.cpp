// SPDX-License-Identifier: BSD-2-Clause-Patent
// Startup policy and serialized M2/M3/M4/M5 transactions on the timed DPU model.
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. dpu_slice_interactions_host.cpp -o /tmp/dpu_slice_interactions_host && /tmp/dpu_slice_interactions_host
#include "DpuClockRead.h"
#include "DpuFlip.h"
#include "DpuModeSwitch.h"
#include "DpuNightShift.h"
#include "DpuPanel.h"
#include "DpuSliceArgs.h"
#include "DpuSnapshot.h"
#include "../../NativeGopDisplay/GopRefresh.h"
#include "dpu_model.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianodpu;
using namespace pianodpu::slices;

namespace {

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

struct BootArgs {
  std::map<std::string, uint32_t> numbers;
  const char *refresh = nullptr;

  uint32_t number(const char *name, uint32_t fallback) const
  {
    const auto it = numbers.find(name);
    return it == numbers.end() ? fallback : it->second;
  }

  SliceArgs slices() const
  {
    return readStartupArgs(
        [&](const char *name, uint32_t *value, size_t size) {
          CHECK(size == sizeof *value);
          const auto it = numbers.find(name);
          if (it == numbers.end()) return false;
          *value = it->second;
          return true;
        },
        [&](const char *name, char *value, size_t size) {
          if (!refresh || std::strcmp(name, "piano-dpu-refresh") != 0) return false;
          CHECK(std::strlen(refresh) < size);
          std::strcpy(value, refresh);
          return true;
        });
  }

  bool cleanSkip(uint32_t flipLevel) const
  {
    return flip::readCleanSkip(flipLevel, [&](const char *name, uint32_t fallback) { return number(name, fallback); });
  }
};

void testStartup()
{
  const struct {
    bool panel, pcc, modes;
    unsigned ahbVote;
  } cases[] = {
    {false, false, false, 0},
    {true,  false, false, 1},
    {false, true,  false, 0},
    {false, false, true,  1},
    {true,  true,  false, 1},
    {true,  false, true,  1},
    {false, true,  true,  1},
    {true,  true,  true,  1},
  };
  for (unsigned level : {1u, 2u, 3u}) {
    for (const auto &row : cases) {
      BootArgs boot{{{"piano-dpu", level}}};
      if (row.panel) boot.numbers["piano-dpu-panel"] = 1;
      if (row.pcc) boot.numbers["piano-dpu-pcc"] = 1;
      if (row.modes) boot.numbers["piano-dpu-modes"] = 1;
      for (int explicitVote : {-1, 0, 1, 2}) {
        if (explicitVote >= 0) boot.numbers["piano-dpu-ahb-vote"] = unsigned(explicitVote);
        const auto args = boot.slices();
        CHECK(args.level == level && args.refresh == kRefreshOff && !args.nightShift);
        CHECK(args.panel == row.panel && args.panelTrace == row.panel && args.pcc == row.pcc);
        CHECK(!args.panelVideoOnly);
        CHECK(args.modeHz == (row.modes ? 60u : 0u) && args.modesEnabled() == row.modes);
        CHECK(args.wanted() == (row.panel || row.pcc || row.modes));
        const unsigned expectedVote = explicitVote >= 0 ? unsigned(explicitVote) : level >= 2 ? row.ahbVote : 0;
        CHECK(args.ahbVote == expectedVote);
        CHECK(!boot.cleanSkip(1) && !boot.cleanSkip(4));
      }
    }
  }

  BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-pcc", 1}, {"native-gop-display-night-shift", 1}}};
  auto args = boot.slices();
  CHECK(args.nightShift && args.wanted() && args.pcc == 0 && args.ahbVote == 1);
  boot.numbers["native-gop-display-night-shift"] = 2;
  args = boot.slices();
  CHECK(!args.nightShift && args.pcc == 1 && args.ahbVote == 0);

  boot = BootArgs{{{"piano-dpu", 2}, {"piano-dpu-modes", 1}}};
  boot.refresh = "plan";
  args = boot.slices();
  CHECK(args.modeHz == 60 && !args.modesEnabled() && args.refresh == kRefreshPlan && args.ahbVote == 0);
  boot.refresh = nullptr;
  boot.numbers["piano-dpu-refresh-db"] = 0;
  args = boot.slices();
  CHECK(!args.modesEnabled() && !args.wanted() && args.ahbVote == 0);
  boot.numbers.erase("piano-dpu-refresh-db");
  for (unsigned hz : {0u, 60u, 90u, 120u, 144u, UINT32_MAX}) {
    boot.numbers["piano-dpu-modes"] = hz;
    args = boot.slices();
    const bool qualified = hz == 120 || hz == 144;
    CHECK(args.modeHz == (qualified ? hz : 0));
    CHECK(args.modesEnabled() == qualified && args.ahbVote == unsigned(qualified));
  }

  boot = BootArgs{{{"piano-dpu", 2}, {"piano-dpu-clean-skip", 1}}};
  for (unsigned level : {1u, 2u, 3u, 4u}) CHECK(boot.cleanSkip(level) == (level == 4));
  for (unsigned invalid : {0u, 2u, UINT32_MAX}) {
    boot.numbers["piano-dpu-clean-skip"] = invalid;
    CHECK(!boot.cleanSkip(4));
  }
}

struct Output {
  std::string text;

  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char row[480];
    va_list args;
    va_start(args, fmt);
    const int len = std::vsnprintf(row, sizeof row, fmt, args);
    va_end(args);
    CHECK(len >= 0 && size_t(len) < sizeof row);
    text.append(row, size_t(len)).append("\n");
  }
};

struct Capture {
  snapshot::Snapshot state;
  std::vector<uint64_t> loads;
};

Capture takeReadOnlySnapshot(model::Dpu &io, bool hwGatedAhb, bool clockRead = true)
{
  const auto stores = io.stores.size(), refused = io.refused.size(), votes = io.bandwidthVotes.size();
  Capture capture;
  const auto load = [&](uint64_t pa) {
    capture.loads.push_back(pa);
    return io.rd(pa);
  };
  snapshot::take(capture.state, load, hwGatedAhb, true);
  CHECK(capture.state.loads == capture.loads.size());
  Output out;
  snapshot::format(capture.state, 1, io.nowUs(), out);
  CHECK(out.text.find("loads, 0 stores") != std::string::npos);
  if (clockRead) {
    const auto clocks = clkread::capture(capture.state.path.clocks, load);
    clkread::formatReadback(clocks, true, out, [](Output &) { CHECK(false); });
    CHECK(out.text.find("clkread_mdp status=") != std::string::npos);
    CHECK(out.text.find("clkread_rpmh status=unavailable (ACTIVE transport requires stores)") != std::string::npos);
  }
  CHECK(io.stores.size() == stores && io.refused.size() == refused && io.bandwidthVotes.size() == votes);
  return capture;
}

void testSnapshotGates()
{
  constexpr uint32_t off = 1u << 31;
  const struct {
    uint64_t pa;
    uint32_t value;
    bool hwGatedAhb, readable;
    int unclockedLink;
  } cases[] = {
    {kGcc + kGccDispAhb, 1, false, true, -1},
    {kGcc + kGccDispAhb, off | 3, true, true, -1},
    {kGcc + kGccDispAhb, off | 3, false, false, -1},
    {kGcc + kGccDispAhb, off, true, false, -1},
    {kDispCc + kCcMdssGdsc, 0, true, false, -1},
    {kDispCc + kCcAhb, off, true, false, -1},
    {kDispCc + kCcMdp, off, true, false, -1},
    {kDispCc + kCcByte0, off, true, true, 0},
    {kDispCc + kCcPclk0, off, true, true, 0},
    {kDispCc + kCcByte1, off, true, true, 1},
    {kDispCc + kCcPclk1, off, true, true, 1},
  };
  for (bool parked : {false, true}) {
    for (const auto &row : cases) {
      auto io = model::modeSwitchFixture();
      for (unsigned l = 0; l < 2; ++l) {
        io.regs[kDispCc + (l ? kCcByte1 : kCcByte0)] = 1;
        io.regs[kDispCc + (l ? kCcPclk1 : kCcPclk0)] = 1;
      }
      if (parked) {
        io.regs[ctlReg(0, kCtlIntfActive)] = kDsiIntfSlave;
        io.regs[ctlReg(0, kCtlIntfMaster)] = 0;
        io.regs[intfReg(1, kIntfEn)] = 0;
        io.intf[1].on = false;
        io.latchedMaster = 0;
      }
      io.regs[row.pa] = row.value;
      for (bool clockRead : {false, true}) {
        const auto capture = takeReadOnlySnapshot(io, row.hwGatedAhb, clockRead);
        const auto &s = capture.state;
        CHECK(s.readable == row.readable);
        const bool routed = row.readable && row.unclockedLink < 0;
        CHECK(s.path.ok == (routed && !parked));
        if (routed) {
          CHECK(s.path.ctl == 0 && s.path.intfActive == (parked ? kDsiIntfSlave : kIntfFlushDsi));
          CHECK(s.mode.hz == 120);
        }
        for (unsigned l = 0; l < 2; ++l) {
          const bool readable = row.readable && int(l) != row.unclockedLink;
          CHECK(s.dsi[l].read == readable && s.m5Link[l].read == readable);
          if (!readable) {
            for (uint64_t pa : capture.loads) {
              CHECK(pa < kDsi[l] || pa >= kDsi[l] + kDsiBytes);
              CHECK(pa < kDsiPhy[l] || pa >= kDsiPhy[l] + kDsiPhyBytes);
              CHECK(pa < kDsiPll[l] || pa >= kDsiPll[l] + kDsiPllBytes);
            }
          }
        }
        if (!row.readable)
          for (uint64_t pa : capture.loads) CHECK(pa < kMdss || pa >= kMdp + kMdpBytes);
      }
    }
  }
  // Clock readback alone still allows the ACTIVE read, so the suppression check is observable.
  unsigned rpmhReads = 0;
  Output out;
  clkread::formatReadback(clkread::Clocks{}, false, out, [&](Output &) { ++rpmhReads; });
  CHECK(rpmhReads == 1 && out.text.find("clkread_rpmh status=unavailable") == std::string::npos);
}

void checkTiming(const snapshot::Snapshot &s, unsigned hz, unsigned htotal, unsigned vtotal)
{
  // The fixture's inherited start (0x0013b6c1) is on line 2243 of 2310.
  constexpr unsigned fetchLead = 67;
  const auto *target = modes::modeByHz(hz);
  CHECK(target);
  CHECK(s.path.ok && s.mode.hz == hz && s.ctlFlush == 0);
  for (unsigned l = 0; l < 2; ++l) {
    const auto plan = modes::plan(modes::geometry(s.live[l]), *target);
    CHECK(plan.hz == hz && plan.htotal == htotal && plan.vtotal == vtotal && plan.vfp == vtotal - 2242);
    CHECK(plan.pixelClockHz == 159667200 && plan.period == htotal * vtotal);
    CHECK(plan.progFetch == (vtotal - fetchLead) * htotal + 1);
    CHECK(plan.progFetch > plan.dispVEnd && plan.progFetch < plan.period);
    CHECK(s.dsi[l].total == ((vtotal - 1) << 16 | (htotal - 1)));
    CHECK(!modes::firstDifference(plan, s.live[l]));
  }
}

void testDailyMode()
{
  auto io = model::modeSwitchFixture();
  CHECK(!modes::switchTiming(io, 60, {}).why);  // Model the 60 Hz scanout already in use.
  const auto before = io.stores.size();
  const auto unexpected = [] { CHECK(false); };
  const auto checkDaily = [&](const BootArgs &boot, bool videoOnly) {
    const auto args = boot.slices();
    CHECK(!args.wanted() && !args.panel && !args.panelTrace && args.panelVideoOnly == videoOnly && !args.nightShift);
    CHECK(args.ahbVote == 0 && args.refresh == kRefreshOff && !boot.cleanSkip(4));
    for (unsigned hz : {60u, 90u, 120u, 144u}) {
      const auto result = modes::switchTo(io, hz, {.fixedHz = args.modeHz}, unexpected, unexpected);
      CHECK(result.why && !result.timingTouched);
    }
  };
  checkDaily(BootArgs{{{"piano-dpu", 2}}}, false);
  for (unsigned value : {0u, 1u, 2u, UINT32_MAX}) {
    checkDaily(BootArgs{{{"piano-dpu", 2}, {"piano-dpu-panel", 0}, {"piano-dpu-panel-video-only", value}}}, value == 1);
  }
  checkTiming(takeReadOnlySnapshot(io, false).state, 60, 576, 4620);
  CHECK(io.stores.size() == before && io.dcs.empty());
}

void testPanelVideoOnlyCycles()
{
  for (unsigned value : {0u, 1u, 2u, UINT32_MAX}) {
    const BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-panel", 1}, {"piano-dpu-panel-video-only", value}}};
    const auto args = boot.slices();
    CHECK(args.wanted() && args.panel && args.panelTrace && args.panelVideoOnly == (value == 1));
    const panel::Options options{.engines = args.panelEngines != 0, .videoOnly = args.panelVideoOnly};
    auto io = model::modeSwitchFixture();
    CHECK(!modes::switchTiming(io, 60, {}).why);
    for (unsigned l = 0; l < 2; ++l) io.regs[dsiReg(l, kDsiCtrl)] = 0x1f7;
    auto path = readPath(io);
    CHECK(path.ok);
    const uint32_t videoCtrl = value == 1 ? 0x1f3 : 0x1f7;
    panel::TraceRing ring;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
      const size_t begin = io.stores.size();
      const auto off = panel::panelOff(io, path, options);
      CHECK(off.ok && off.enginesStopped && off.dcs[0].dmaDoneMask() == 3 && off.dcs[1].dmaDoneMask() == 3);
      for (unsigned l = 0; l < 2; ++l) {
        const uint32_t ctrl = cycle ? videoCtrl : 0x1f7;
        std::vector<uint32_t> writes;
        for (size_t i = begin; i < io.stores.size(); ++i)
          if (io.stores[i].pa == dsiReg(l, kDsiCtrl)) writes.push_back(io.stores[i].v);
        const std::vector<uint32_t> expected{ctrl | 4u, ctrl, ctrl | 4u, ctrl};
        CHECK(writes == expected && io.rd(dsiReg(l, kDsiCtrl)) == ctrl);
      }
      ring.reset();
      const auto on = panel::panelOn(io, path, options, off.enginesStopped, off.fetch, &ring);
      CHECK(on.ok && on.engines.rate.lockstep && on.dcs[0].dmaDoneMask() == 3 && on.dcs[1].dmaDoneMask() == 3);
      CHECK(ring.total == ring.size() && ring.size() > 4);
      path = readPath(io);   // panelWake rereads the path for the next cycle
      CHECK(path.ok && path.link[0].dsiCtrl == videoCtrl && path.link[1].dsiCtrl == videoCtrl);
      CHECK(io.half[0].displayOn && io.half[1].displayOn);
    }
    checkTiming(takeReadOnlySnapshot(io, false).state, 60, 576, 4620);
    CHECK(io.refused.empty() && io.glitches == 0 && io.desync == 0 && io.commandsInFrame == 0);
  }
}

void testInterleavedSlices()
{
  const BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-clean-skip", 1},
                       {"piano-dpu-modes", 1}, {"native-gop-display-night-shift", 1}}};
  const auto args = boot.slices();
  CHECK(args.nightShift && args.modesEnabled() && args.ahbVote == 1 && !args.panel);

  auto io = model::modeSwitchFixture();
  io.resetPccOnTimingChange = true;
  const uint32_t baseDiagonal[2][3] = {{0x8000, 0x6000, 0x4000}, {0x6000, 0x8000, 0x8000}};
  for (unsigned d = 0; d < 2; ++d) {
    io.pccShadow[d][0] = io.pccActive[d][0] = 1;
    for (unsigned c = 0; c < 3; ++c) {
      const unsigned word = kPccCoef[c][c] / sizeof(uint32_t);
      io.pccShadow[d][word] = io.pccActive[d][word] = baseDiagonal[d][c];
    }
  }
  const bool hwGatedAhb = args.ahbVote == 1;
  pcc::PanelPcc base;
  CHECK(pcc::capturePanelPcc(io, readPath(io, hwGatedAhb), base));
  auto last = base;
  pcc::NightShift nightShift;
  const uint64_t off[9] = {1ull << 32, 0, 0, 0, 1ull << 31, 0, 0, 0, 1ull << 31};
  const uint64_t warm[9] = {1ull << 32, 0, 0, 0, 1ull << 30, 0, 0, 0, 1ull << 29};
  CHECK(nightShift.setMatrix(off) == pcc::NightShift::Input::accepted && nightShift.neutral());
  const auto applyColor = [&](bool replay) {
    return pcc::updateNightShift(io, readPath(io, hwGatedAhb), pcc::nightShiftPrograms(base, nightShift), last, replay);
  };
  CHECK(!applyColor(false).attempted && io.stores.empty());

  struct FlipHardware {} hw;
  flip::Flipper flipper(hw);
  flipper.cfg.level = 4;
  flipper.cfg.cleanSkip = boot.cleanSkip(flipper.cfg.level);
  CHECK(flipper.cfg.cleanSkip);
  using Action = flip::CleanEpoch::Action;
  unsigned cleans = 0, barriers = 0;
  const auto prepare = [&](unsigned slot) {
    const auto verifiedWc = [] { return flip::cleanEligibility(flip::kSurfaceWriteCombined, {4, 0, 0}); };
    return flipper.cleanEpochs[slot].prepare(verifiedWc(), [&](bool whole) {
      CHECK(whole);
      ++cleans;
      return true;
    }, verifiedWc, [&] { ++barriers; }).action;
  };
  CHECK(prepare(0) == Action::initialized && prepare(1) == Action::initialized);

  const struct {
    unsigned hz, htotal, vtotal;
    bool warm;
    unsigned correctionDivisor;
  } steps[] = {
    {60,  576, 4620, true,  1},
    {90,  768, 2310, false, 1},
    {144, 480, 2310, true,  2},
    {120, 576, 2310, false, 1},
  };
  constexpr unsigned rounds = 8;
  unsigned invalidations = 0, replays = 0, transitions = 0;
  for (unsigned round = 0; round < rounds; ++round) {
    for (const auto &step : steps) {
      CHECK(nightShift.setMatrix(step.warm ? warm : off) == pcc::NightShift::Input::accepted);
      nightShift.setCorrection(pcc::kOne / step.correctionDivisor);
      CHECK(nightShift.neutral() == !step.warm);
      const auto update = applyColor(false);
      CHECK(update.attempted && update.result.ok);
      const auto before = io.stores.size();
      CHECK(!applyColor(false).attempted && io.stores.size() == before);
      CHECK(prepare(0) == Action::skipped && prepare(1) == Action::skipped);

      // The timed model exercises all four plans. Production fixes the requested rate at boot.
      const modes::Options modelOptions{.hwGatedAhb = hwGatedAhb, .allowUnqualified = true};
      const auto unexpected = [] { CHECK(false); };
      const auto time = io.ns;
      for (unsigned bad : {0u, 1u, 59u, 61u, 89u, 91u, 119u, 121u, 143u, 145u, UINT32_MAX}) {
        CHECK(!modes::modeByHz(bad));
        const auto refused = modes::switchTo(io, bad, modelOptions, unexpected, unexpected);
        CHECK(refused.why && !refused.timingTouched && !refused.reverted);
      }
      CHECK(io.stores.size() == before && io.ns == time);

      ++transitions;
      const auto switched = modes::switchTo(io, step.hz, modelOptions, [&] {
        ++invalidations;
        flipper.invalidateCleanEpochs();
      }, [&] {
        ++replays;
        CHECK(invalidations == transitions && replays == transitions);
        CHECK(!flipper.cleanEpochs[0].initialized && !flipper.cleanEpochs[1].initialized);
        CHECK(!io.pccActive[0][0] && !io.pccActive[1][0]);
        CHECK(applyColor(true).result.ok);
      });
      if (switched.why) std::fprintf(stderr, "switch %u -> %u Hz: %s\n", switched.from, step.hz, switched.why);
      CHECK(!switched.why && switched.timingTouched && !switched.reverted && switched.rate.lockstep);
      CHECK(switched.rate.milliHz >= step.hz * 980 && switched.rate.milliHz <= step.hz * 1020);

      const auto s = takeReadOnlySnapshot(io, hwGatedAhb).state;
      checkTiming(s, step.hz, step.htotal, step.vtotal);
      for (unsigned d = 0; d < 2; ++d) {
        CHECK(io.pccActive[d][0] == 1 && s.dspp[d].pcc[0] == 1);
        for (unsigned o = 0; o < 3; ++o) {
          for (unsigned i = 0; i < 3; ++i) {
            const unsigned index = kPccCoef[o][i] / sizeof(uint32_t);
            const uint32_t expected = o != i ? 0 : baseDiagonal[d][o] / ((step.warm ? 1u << o : 1u) * step.correctionDivisor);
            CHECK(io.pccActive[d][index] == expected && s.dspp[d].pcc[index] == expected);
          }
        }
      }
      CHECK(prepare(0) == Action::initialized && prepare(1) == Action::initialized);
    }
  }
  CHECK(transitions == rounds * 4 && replays == transitions && invalidations == transitions);
  CHECK(cleans == 2 * (transitions + 1) && barriers == 2 * transitions);
  CHECK(io.glitches == 0 && io.desync == 0 && io.commandsInFrame == 0 && !io.modeUnderruns());
  CHECK(io.refused.empty() && io.irqDepth == 0);
}

// Flips use the same timed CTL as the slices. Model a wired backing and the
// VIG shadows here. The flip suite covers physical fetch and SMMU fault handling.
enum class FlipEvent { tableClean, backingClean, scanoutClean, barrier, flush };

struct DailyFlipIo {
  model::Dpu &io;
  std::map<uint64_t, uint32_t> shadow, active;
  std::vector<FlipEvent> events;
  unsigned tlbSyncs = 0;
  static constexpr uint64_t kBackingBytes = 28 * 1024 * 1024;

  explicit DailyFlipIo(model::Dpu &model) : io(model) {}

  uint64_t nowUs() { return io.nowUs(); }
  uint32_t rd(uint64_t pa) { return io.rd(pa); }
  void wr(uint64_t pa, uint32_t value)
  {
    if (pa == ctlReg(0, kCtlFlush)) {
      CHECK(!io.ctlFlush && io.wr(pa, value));
      events.push_back(FlipEvent::flush);
    } else if (pa != kMdp + kTopIntrEn && pa != kMdp + kTopIntrClear) {
      shadow[pa] = value;
    }
  }
  void barrier() { events.push_back(FlipEvent::barrier); }
  void vsyncArm() {}
  bool irqLive() { return false; }
  uint64_t irqCount() { return 0; }
  uint32_t takeIrqBits() { return 0; }
  void irqDelivered() { CHECK(false); }
  void irqMiss() { CHECK(false); }
  bool waitVsync(uint32_t us)
  {
    io.delayUs(us < 1000 ? us : 1000);
    if (!io.ctlFlush) active = shadow;
    return false;
  }
  void delayUs(uint32_t us) { io.delayUs(us); }
  void clean(const void *, size_t) { events.push_back(FlipEvent::tableClean); }
  void smmuWr(uint32_t off, uint32_t) { if (off == smmu::kTlbSync) ++tlbSyncs; }
  uint32_t smmuRd(uint32_t) { return 0; }
  uint64_t surfaceLength(const PianoDpuFlipRequest &) { return kBackingBytes; }
  bool surfaceMap(const PianoDpuFlipRequest &r, uint64_t *pages, unsigned max, unsigned *count, void **cookie, const char **)
  {
    *count = kBackingBytes / 4096;
    CHECK(*count <= max);
    const auto base = *static_cast<const uint64_t *>(r.surface);
    for (unsigned i = 0; i < *count; ++i) pages[i] = base + i * 4096ull;
    *cookie = r.surface;
    return true;
  }
  void surfaceRelease(void *) {}
  uint32_t surfaceCacheMode(void *) { return flip::kSurfaceWriteCombined; }
  flip::CleanPages surfaceAttributes(const uint64_t *, unsigned count) { return {count, 0, 0}; }
  bool surfaceClean(void *, uint64_t, uint64_t) { CHECK(false); return false; }
  bool surfaceCleanPhysical(const uint64_t *, uint64_t offset, uint64_t bytes)
  {
    CHECK(offset + bytes <= kBackingBytes);
    events.push_back(bytes == kBackingBytes ? FlipEvent::backingClean : FlipEvent::scanoutClean);
    barrier();
    return true;
  }
  void note(const char *, ...) {}
};

void testDailyCombo(unsigned hz, bool switchFrom60, bool failReadback)
{
  const BootArgs boot{{{"piano-dpu", 2}, {"piano-dpu-flip", 4}, {"piano-dpu-panel", 0}, {"piano-dpu-clean-skip", 1},
                       {"piano-dpu-modes", hz}, {"native-gop-display-night-shift", 1}}};
  const auto args = boot.slices();
  CHECK(args.nightShift && args.modesEnabled() && args.ahbVote == 1 && !args.panel && !args.pcc && !args.refresh);
  auto io = model::modeSwitchFixture();
  if (switchFrom60) CHECK(!modes::switchTiming(io, 60, {}).why);
  io.resetPccOnTimingChange = true;
  DailyFlipIo hw(io);
  flip::Flipper flipper(hw);
  flipper.cfg.level = boot.number("piano-dpu-flip", 1);
  flipper.cfg.cleanSkip = boot.cleanSkip(flipper.cfg.level);
  CHECK(flipper.cfg.cleanSkip);
  flipper.path.ctl = 0;
  flipper.path.flushMask = 3;  // VIG0 and VIG1 on the two halves
  flipper.path.count = 2;
  flipper.path.pipe[1].pipe = 1;
  flipper.path.pipe[1].offset[0] = kPanelWidth * 2;
  std::vector<uint64_t> tables(smmu::kTablePages * 512), pages(smmu::kSlotPages);
  flipper.tables.cpu = tables.data();
  flipper.tables.pa = 0x8c1000000;
  flipper.guardPa = 0x8c2000000;
  flipper.pages = pages.data();
  smmu::buildTables(flipper.tables, [&](const void *p, size_t n) { hw.clean(p, n); });
  flipper.state = flip::State::ready;

  bool displayOff = false;
  pcc::PanelPcc base;
  const bool ahb = args.ahbVote == 1;
  CHECK(pcc::capturePanelPcc(io, readPath(io, ahb), base));
  auto last = base;
  pcc::NightShift state;
  constexpr uint64_t neutral[9] = {1ull << 32, 0, 0, 0, 1ull << 31, 0, 0, 0, 1ull << 31};
  CHECK(state.setMatrix(neutral) == pcc::NightShift::Input::accepted && state.neutral());
  const auto apply = [&](bool replay) {
    if (displayOff) return;
    auto update = pcc::updateNightShift(io, readPath(io, ahb), pcc::nightShiftPrograms(base, state), last, replay);
    CHECK(update.attempted && update.result.ok && update.result.dspps == 3);
    for (unsigned d = 0; d < 2; ++d) {
      CHECK(io.pccActive[d][0] == unsigned(last.dspp[d].enabled));
      for (unsigned o = 0; o < 3; ++o)
        for (unsigned i = 0; i < 3; ++i)
          CHECK(io.pccActive[d][kPccCoef[o][i] / 4] == last.dspp[d].words.w[o][i]);
    }
  };
  const auto timingFor = [&](unsigned rate) {
    checkTiming(takeReadOnlySnapshot(io, ahb, false).state, rate, rate == 144 ? 480 : 576, rate == 60 ? 4620 : 2310);
  };

  uint64_t backing[] = {0x8d0000000, 0x8e0000000};
  const auto show = [&](unsigned index, uint32_t row, uint32_t offset, bool producerComplete) {
    PianoDpuFlipRequest r{.size = sizeof r, .version = kPianoDpuFlipVersion, .surface = &backing[index], .memory = &backing[index],
                          .offset = offset, .rowBytes = row, .width = kPanelWidth, .height = kPanelHeight, .format = flip::kFourccBgra};
    hw.events.clear();
    CHECK(flipper.request(r, producerComplete) == kPianoDpuFlipShown && !io.ctlFlush);
    const uint32_t baseIova = smmu::slotIova(unsigned(flipper.slots.shown)) + offset;
    CHECK(flipper.current.base == baseIova && flipper.current.stride == row);
    for (unsigned p = 0; p < flipper.path.count; ++p) {
      const auto &pipe = flipper.path.pipe[p];
      const auto reg = kMdp + kPipes[pipe.pipe].base;
      CHECK(hw.active.at(reg + kSrc0Addr) == baseIova + pipe.offset[0]);
      CHECK(hw.active.at(reg + kYStride0) == row);
    }
  };
  show(0, kSplashStride, 0, true);  // Eligible epoch on the source timing.
  CHECK(flipper.st.cleanInits == 1 && hw.tlbSyncs == 1);
  CHECK(!hw.events.empty() && hw.events.front() == FlipEvent::tableClean);
  const auto originalTarget = flipper.current;
  const auto originalPipes = hw.active;
  const auto beforeDcs = io.dcs.size();
  if (failReadback) {
    state.setCorrection(pcc::kOne / 2);
    apply(false);
    io.dropStore = intfReg(2, kIntfProgFetch);
    io.dropValue = (2310 - 67) * (hz == 144 ? 480 : 576) + 1;
    io.dropCount = 1;
  }
  unsigned invalidations = 0, replays = 0;
  const auto switched = modes::switchTo(io, hz, {.hwGatedAhb = ahb, .fixedHz = args.modeHz}, [&] {
    ++invalidations;
    flipper.invalidateCleanEpochs();
  }, [&] {
    ++replays;
    CHECK(invalidations == 1 && !flipper.cleanEpochs[flipper.slots.shown].initialized);
    apply(true);
  });
  CHECK(switched.timingTouched == (switchFrom60 || hz == 144));
  CHECK(invalidations == unsigned(switched.timingTouched) && replays == invalidations);
  CHECK(hz == 144 ? io.bandwidthVotes.size() == 1 : io.bandwidthVotes.empty());
  CHECK(flip::sameTarget(flipper.current, originalTarget) && hw.active == originalPipes);
  CHECK(bool(switched.why) == failReadback && switched.reverted == failReadback && !switched.restoreWhy);
  const unsigned verifiedHz = failReadback ? switched.from : hz;
  timingFor(verifiedHz);
  uint64_t attributes[goprefresh::kWords];
  const auto period = 1000000000ull / verifiedHz;
  CHECK(goprefresh::fixedAttributes(verifiedHz, [](uint64_t ns) { return ns; }, attributes));
  CHECK(attributes[goprefresh::kMinAbs] == period && attributes[goprefresh::kMaxAbs] == period && !attributes[goprefresh::kStepAbs]);
  CHECK(goprefresh::nextDeadline(period, 2 * period + 10, period) == 3 * period);
  show(0, kSplashStride, 0, true);  // A re-show after timing writes must initialize again.
  using Event = FlipEvent;
  const std::vector<Event> cleanedBacking{Event::backingClean, Event::barrier}, skippedReshow{Event::barrier};
  CHECK(hw.events == (invalidations ? cleanedBacking : skippedReshow) && hw.tlbSyncs == 1);
  if (failReadback) {
    CHECK(io.pccActive[0][kPccCoef[0][0] / 4] == 0x4000);
    CHECK(io.pccActive[1][kPccCoef[0][0] / 4] == 0x4000);
    CHECK(io.refused.empty() && !io.desync && !io.modeUnderruns());
    return;
  }

  show(1, kSplashStride, 0, true);
  const std::vector<Event> skippedLatch{Event::barrier, Event::barrier, Event::barrier, Event::flush,
                                       Event::barrier, Event::barrier, Event::barrier};
  for (unsigned divisor : {2u, 4u, 2u, 1u}) {
    const uint64_t matrix[9] = {1ull << 32, 0, 0, 0, (1ull << 31) / divisor, 0, 0, 0, (1ull << 31) / divisor};
    CHECK(state.setMatrix(matrix) == pcc::NightShift::Input::accepted);
    apply(false);
    const auto blue = io.pccActive[0][kPccCoef[2][2] / 4];
    if (divisor == 1) {
      CHECK(!io.pccActive[0][0] && blue == base.dspp[0].words.w[2][2]);
    } else {
      CHECK(io.pccActive[0][0] && blue == 0x8000 / divisor);
    }
    CHECK(pcc::same(last, base) == (divisor == 1));
    show(0, kSplashStride, 0, true);
    CHECK(hw.events == skippedLatch);
    show(1, kSplashStride, 0, true);
    CHECK(hw.events == skippedLatch);
    CHECK(io.pccActive[0][kPccCoef[2][2] / 4] == blue && io.pccActive[1][kPccCoef[2][2] / 4] == blue);
  }
  // WC eligibility covers the allocation. A new stride/offset on the same slot still latches.
  show(1, kSplashStride + 64, 64, true);
  CHECK(hw.events == skippedLatch && flipper.st.cleanSkips >= 9);
  show(1, kSplashStride + 64, 64, true);
  CHECK(hw.events == skippedReshow);
  show(1, kSplashStride + 64, 64, false);
  CHECK((hw.events == std::vector<Event>{Event::scanoutClean, Event::barrier}) && flipper.st.cleanFull == 1);
  show(1, kSplashStride + 64, 64, true);
  CHECK(hw.events == cleanedBacking);

  state.setCorrection(pcc::kOne / 2);
  apply(false);
  displayOff = true;
  constexpr uint64_t blank[9] = {};
  CHECK(state.setMatrix(blank) == pcc::NightShift::Input::blank);
  const auto beforeOff = io.stores.size(), dcsBeforeWake = io.dcs.size();
  apply(true);
  CHECK(io.stores.size() == beforeOff && io.intf[1].on && io.intf[2].on);
  CHECK(flipper.cleanEpochs[flipper.slots.shown].initialized);
  for (unsigned d = 0; d < 2; ++d) io.pccActive[d][0] = io.pccShadow[d][0] = 0;
  displayOff = false;
  apply(true);
  CHECK(io.pccActive[0][kPccCoef[0][0] / 4] == 0x4000);
  show(1, kSplashStride + 64, 64, true);
  CHECK(hw.events == skippedReshow);
  CHECK(io.dcs.size() == dcsBeforeWake && (hz == 144 || io.dcs.size() == beforeDcs));
  CHECK(hw.tlbSyncs == 2 && !flipper.st.toBoot && !flipper.st.cleanFailures && !flipper.st.cleanUnderruns);
  CHECK(flipper.st.cleanInits + flipper.st.cleanSkips + flipper.st.cleanFull == flipper.st.flips + flipper.st.reshows);
  timingFor(hz);
  CHECK(io.refused.empty() && !io.glitches && !io.desync && !io.modeUnderruns());
}

}  // namespace

int main()
{
  testStartup();
  testSnapshotGates();
  testDailyMode();
  testPanelVideoOnlyCycles();
  testInterleavedSlices();
  testDailyCombo(120, false, false);
  for (unsigned hz : {120u, 144u}) {
    testDailyCombo(hz, true, false);
    testDailyCombo(hz, true, true);
  }
  std::puts("PianoDpu startup and slice interactions: all passed");
}
