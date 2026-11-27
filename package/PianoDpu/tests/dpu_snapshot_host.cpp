// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of DpuSnapshot.h (B1 milestone 1's run-time snapshot) on the timed register model (dpu_model.h), seeded with the
// installed v13.1y12k survey's words: the snapshot stores nothing, keeps the survey's gate, classifies the live mode (120, then 60
// after DpuRefresh.h's own switch on the model), reads both DSI controllers' shadow and command-path words, and its text is bounded.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. dpu_snapshot_host.cpp -o /tmp/dpu_snapshot_host && /tmp/dpu_snapshot_host
#include "DpuSnapshot.h"
#include "DpuPanel.h"
#include "dpu_model.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pianodpu;
using namespace pianodpu::snapshot;
static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
struct Text {
  std::vector<std::string> lines;
  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    lines.emplace_back(b);
  }
  bool has(const char *s) const
  {
    for (const std::string &l : lines)
      if (l.find(s) != std::string::npos) return true;
    return false;
  }
  bool hasLine(const char *s) const { return std::find(lines.begin(), lines.end(), s) != lines.end(); }
};

// The words the model does not simulate, as installed y12k's survey read them (both links alike).
// The y12k console cuts the survey's line before INTF_PROG_FETCH_START's value: a start msm's formula would give for 24 lines
// ((2310 - 24) x 576 + 1) stands in; milestone 1's snapshot reads the real one.
constexpr uint32_t kFetchStart = (2310 - 24) * 576 + 1;
void seedY12k(model::Dpu &m, bool fetch = false)
{
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[slices::intfReg(i, kIntfVsyncWidth)] = 0x00000480;
    m.regs[slices::intfReg(i, kIntfDispVStart)] = 0x0000ee80;
    m.regs[slices::intfReg(i, kIntfDispVEnd)] = 0x0013b47f;
    m.regs[slices::intfReg(i, kIntfDispHctl)] = 0x01ca0020;
    m.regs[slices::intfReg(i, kIntfConfig)] = fetch ? 0x80800000u : 0x00800000u;   // y12k: 80800000 = programmable fetch on (bit 31)
    m.regs[slices::intfReg(i, kIntfProgFetch)] = fetch ? kFetchStart : 0;
  }
  for (unsigned l = 0; l < 2; ++l) {
    m.regs[slices::dsiReg(l, kDsiActiveH)] = 0x01cb0020;
    m.regs[slices::dsiReg(l, kDsiActiveV)] = 0x08c2006a;
    m.regs[slices::dsiReg(l, kDsiClkCtrl)] = 0x0000033f;
    m.regs[slices::dsiReg(l, kDsiLaneStatus)] = 0x00001f00;
    m.regs[slices::dsiReg(l, kDsiLaneCtrl)] = 0x01000000;
  }
  m.regs[kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride + 4] = 1;   // ID pin high: p81 35-02-0b (this unit)
}

struct Load {
  model::Dpu &m;
  uint32_t operator()(uint64_t pa) { return m.rd(pa); }
};

void testIdle120()
{
  model::Dpu m;
  seedY12k(m);
  m.sleepMs(3);
  Snapshot s;
  Load ld{m};
  take(s, ld, true);
  CHECK(m.stores.empty() && m.refused.empty());   // the snapshot stores nothing
  CHECK(s.readable && s.path.ok && s.path.ctl == 0);
  CHECK(s.dsi[0].read && s.dsi[1].read);
  CHECK(s.mode.hz == 120);
  CHECK(s.vfpVerdict == nullptr);
  CHECK(s.loads > 60 && s.loads < 200);
  CHECK(s.dsi[0].trigCtrl == (4u | 1u << 12));     // the model's boot state: software trigger, DMA blocked within the frame
  Text t;
  format(s, 1, 123, t);
  CHECK(t.lines.size() <= kMaxLines);
  for (const std::string &l : t.lines) CHECK(l.size() < 480);   // fits the kernel's note()/sysctl line
  CHECK(t.has("0 stores"));
  CHECK(t.has("mode: live 120 Hz"));
  CHECK(t.has("p81 35-02-0b") && t.has("B2 then B3"));
  CHECK(t.has("plan 144 Hz: hsync 01e00010 period 0010eb40"));
  CHECK(t.has("plan 120 Hz: hsync 02400010 period 00144d80") && t.has("words changed from live 000"));
  CHECK(t.has("plan 60 Hz: hsync 02400010 period 00289b00") && t.has("dsi total 120b023f"));
  CHECK(t.has("plan 90 Hz: hsync 03000010"));
  CHECK(t.has("dma trigger software, block-in-frame 1"));
  CHECK(t.has("timing shadow +1e4"));
  CHECK(t.has("vfp switch (DpuRefresh.h): the path takes it"));
  CHECK(t.has("intf clock 159667200 Hz"));
  CHECK(t.has("plan: programmable fetch off"));
}

// The installed state: programmable fetch on (INTF_CONFIG 80800000). B1c's VFP slice refuses that path as built, and every plan
// moves the fetch start with the timing (24 lines before the frame end in each mode).
void testProgFetchOn()
{
  model::Dpu m;
  seedY12k(m, true);
  m.sleepMs(3);
  Snapshot s;
  Load ld{m};
  take(s, ld, true);
  CHECK(m.stores.empty());
  CHECK(s.mode.hz == 120);
  CHECK(s.vfpVerdict && std::strstr(s.vfpVerdict, "programmable fetch is on"));
  Text t;
  format(s, 6, 0, t);
  CHECK(t.has("vfp switch (DpuRefresh.h): programmable fetch is on"));
  CHECK(t.has("plan: programmable fetch on"));
  char want[96];
  std::snprintf(want, sizeof want, "prog-fetch %08x", kFetchStart);
  CHECK(t.has(want));                                                    // 120 Hz: the live start
  std::snprintf(want, sizeof want, "prog-fetch %08x", (4620 - 24) * 576 + 1);
  CHECK(t.has(want));                                                    // 60 Hz: 24 lines before the long frame's end
  std::snprintf(want, sizeof want, "prog-fetch %08x", (2310 - 24) * 480 + 1);
  CHECK(t.has(want));                                                    // 144 Hz: in 480-clock lines
}

void testAfterVfpSwitch()
{
  model::Dpu m;
  seedY12k(m);
  m.sleepMs(5);
  slices::LivePath p = slices::readPath(m);
  const refresh::Switch sw = refresh::switchTo(m, p, slices::kVtotal60, refresh::Options());
  CHECK(sw.ok);
  const size_t stores = m.stores.size();
  m.sleepMs(20);
  Snapshot s;
  Load ld{m};
  take(s, ld, true);
  CHECK(m.stores.size() == stores);   // still nothing stored by the snapshot
  CHECK(s.mode.hz == 60);
  Text t;
  format(s, 2, 456, t);
  CHECK(t.has("mode: live 60 Hz"));
  CHECK(t.has("plan 60 Hz: hsync 02400010 period 00289b00") && t.has("words changed from live 000"));
}

void testGateClosed()
{
  model::Dpu m;
  seedY12k(m);
  m.regs[kDispCc + kCcAhb] = 0x80000000u;   // MDSS AHB off
  Snapshot s;
  std::vector<uint64_t> seen;
  auto spy = [&](uint64_t pa) {
    seen.push_back(pa);
    return m.rd(pa);
  };
  take(s, spy, true);
  CHECK(!s.readable);
  // Nothing of MDSS (0xae00000..), the DPU or DSI was loaded: only GCC, DISPCC and the TLMM ID pin.
  for (uint64_t pa : seen) CHECK(!(pa >= kMdss && pa < kMdss + 0x100000));
  Text t;
  format(s, 3, 0, t);
  CHECK(t.has("nothing of MDSS, the DPU or DSI was read"));
  CHECK(t.lines.size() == 2);
}

void testDsiClockOff()
{
  model::Dpu m;
  seedY12k(m);
  m.regs[kDispCc + kCcByte1] = 0x80000000u;   // DSI1's byte clock off
  Snapshot s;
  std::vector<uint64_t> seen;
  auto spy = [&](uint64_t pa) {
    seen.push_back(pa);
    return m.rd(pa);
  };
  take(s, spy, true);
  for (uint64_t pa : seen) CHECK(!(pa >= kDsi[1] && pa < kDsi[1] + kDsiBytes));   // never a load from the unclocked controller
  CHECK(!s.dsi[1].read && s.mode.hz == 0);
  Text t;
  format(s, 4, 0, t);
  CHECK(t.has("dsi1: not read"));
}

constexpr uint32_t probeHostOffsets[] = {
    0x010, 0x020, 0x030, 0x034, 0x038, 0x00c, 0x068, 0x0b0, 0x0b4, 0x0b8,
    0x0bc, 0x0c0, 0x0c4, 0x0cc, 0x10c, 0x120, 0x180, 0x2a8, 0x2ac, 0x040, 0x044, 0x04c, 0x118, 0x0a4};
constexpr uint32_t probePhyOffsets[] = {
    0x010, 0x014, 0x01c, 0x140, 0x148, 0x14c, 0x028, 0x02c, 0x030, 0x1b0, 0x034, 0x038, 0x03c, 0x0a0, 0x0a4, 0x024};
constexpr uint32_t probeTimingOffsets[] = {
    0x0b4, 0x0b8, 0x0bc, 0x0c0, 0x0c4, 0x0c8, 0x0cc, 0x0d0, 0x0d4, 0x0d8, 0x0dc, 0x0e0, 0x0e4, 0x0e8};
constexpr uint64_t probePllStatus[] = {0x0ae956b0, 0x0ae976b0};

void enableLinkClocks(model::Dpu &m)
{
  for (uint32_t off : {kCcByte0, kCcByte1, kCcPclk0, kCcPclk1}) m.regs[kDispCc + off] = kCbcrEnable;
}

void checkProbeLoads(const std::vector<uint64_t> &seen, unsigned link, bool expected, bool pllExpected = false)
{
  for (uint32_t off : probeHostOffsets)
    CHECK(std::count(seen.begin(), seen.end(), kDsi[link] + off) == int(expected));
  const auto mappedLoads = std::count_if(seen.begin(), seen.end(), [&](uint64_t pa) {
    return pa >= kDsiPhy[link] && pa < kDsiPhy[link] + 0x1000;
  });
  // The entire PHY page is covered, including holes, the lane resource and the separate PLL window.
  CHECK(size_t(mappedLoads) == (expected ? std::size(probePhyOffsets) + std::size(probeTimingOffsets) : 0) + unsigned(pllExpected));
  for (uint32_t off : probePhyOffsets)
    CHECK(std::count(seen.begin(), seen.end(), kDsiPhy[link] + off) == int(expected));
  for (uint32_t off : probeTimingOffsets)
    CHECK(std::count(seen.begin(), seen.end(), kDsiPhy[link] + off) == int(expected));
  CHECK(std::count(seen.begin(), seen.end(), probePllStatus[link]) == int(pllExpected));
}

void testM5Probe()
{
  model::Dpu m;
  seedY12k(m);
  enableLinkClocks(m);
  for (unsigned l = 0; l < 2; ++l) {
    for (uint32_t off : probeHostOffsets) m.regs[kDsi[l] + off] = ((l + 1) << 16) | off;
    for (uint32_t off : probePhyOffsets) m.regs[kDsiPhy[l] + off] = ((l + 3) << 16) | off;
    for (uint32_t off : probeTimingOffsets) m.regs[kDsiPhy[l] + off] = ((l + 5) << 16) | off;
    m.regs[kDsiPhy[l] + 0x024] |= 0x60;
    m.regs[probePllStatus[l]] = 0x80 + (1 - l);   // Only PLL0 locks in this bonded control.
  }
  for (bool enabled : {false, true}) {
    Snapshot s;
    std::vector<uint64_t> seen;
    take(s, [&](uint64_t pa) {
      seen.push_back(pa);
      return m.rd(pa);
    }, true, enabled);
    CHECK(s.loads == seen.size());
    CHECK(m.stores.empty() && m.refused.empty());
    for (unsigned l = 0; l < 2; ++l) {
      checkProbeLoads(seen, l, enabled, enabled);
      const auto &words = s.m5Link[l];
      CHECK(words.read == enabled);
      CHECK(words.pllStatus == (enabled ? 0x81 - l : UINT32_MAX));
      if (!enabled) continue;
      for (unsigned i = 0; i < HostWordCount; ++i)
        CHECK(words.host[i] == (((l + 1) << 16) | probeHostOffsets[i]));
      for (unsigned i = 0; i < PhyWordCount; ++i)
        CHECK(words.phy[i] == (((l + 3) << 16) | probePhyOffsets[i] | (probePhyOffsets[i] == 0x024 ? 0x60 : 0)));
      for (unsigned i = 0; i < std::size(probeTimingOffsets); ++i)
        CHECK(words.timing[i] == (((l + 5) << 16) | probeTimingOffsets[i]));
    }
    Text t;
    format(s, 10, 0, t);
    CHECK(t.has("0 stores"));
    CHECK(t.lines.size() <= kMaxLines);
    for (const auto &line : t.lines) CHECK(line.size() < 480);
    CHECK(t.has("dsi0 m5: vid-cfg0 00010010 vid-cfg1 00010020") == enabled);
    CHECK(t.has("cmd-compression 000202a8/000202ac") == enabled);
    CHECK(t.has("phy-err 000200b4 timeout 000200c0") == enabled);
    CHECK(t.has("dsi0 m5: cmd-cfg0 00010040 cmd-cfg1 00010044 dma-length 0001004c reset 00010118 misr-ctrl 000100a4") == enabled);
    const char *phyAndPllRows[] = {
      "dsi0 phy: clk-cfg0 00030010 clk-cfg1 00030014 rbuf-ctrl 0003001c phy-status 00030140 lane-status0 00030148 lane-status1 0003014c",
      "dsi0 phy: ctrl0 00030064 ctrl1 00030028 ctrl2 0003002c ctrl3 00030030 ctrl5 000301b0 lane-cfg0 00030034 lane-cfg1 00030038 "
      "lane-ctrl0 000300a0 lane-ctrl1 000300a4 pll-ctrl 0003003c",
      "dsi0 phy timing 0..13: 000500b4 000500b8 000500bc 000500c0 000500c4 000500c8 000500cc 000500d0 "
      "000500d4 000500d8 000500dc 000500e0 000500e4 000500e8",
      "dsi0 pll: status 00000081 local-lock 1",
      "dsi1 phy: clk-cfg0 00040010 clk-cfg1 00040014 rbuf-ctrl 0004001c phy-status 00040140 lane-status0 00040148 lane-status1 0004014c",
      "dsi1 phy: ctrl0 00040064 ctrl1 00040028 ctrl2 0004002c ctrl3 00040030 ctrl5 000401b0 lane-cfg0 00040034 lane-cfg1 00040038 "
      "lane-ctrl0 000400a0 lane-ctrl1 000400a4 pll-ctrl 0004003c",
      "dsi1 phy timing 0..13: 000600b4 000600b8 000600bc 000600c0 000600c4 000600c8 000600cc 000600d0 "
      "000600d4 000600d8 000600dc 000600e0 000600e4 000600e8",
      "dsi1 pll: status 00000080 local-lock 0",
    };
    for (const char *line : phyAndPllRows) CHECK(t.hasLine(line) == enabled);
  }
}

void testPllProbePower()
{
  for (uint32_t ctrl0 : {0u, 0x20u, 0x40u, UINT32_MAX, 0x60u}) {
    model::Dpu m;
    seedY12k(m);
    enableLinkClocks(m);
    m.regs[kDsiPhy[0] + 0x024] = ctrl0;
    m.regs[kDsiPhy[1] + 0x024] = 0x60;
    m.regs[probePllStatus[0]] = UINT32_MAX;   // Even a permitted load may fail to map.
    m.regs[probePllStatus[1]] = 0;            // Zero from a successful load is meaningful.
    Snapshot s;
    std::vector<uint64_t> seen;
    take(s, [&](uint64_t pa) {
      seen.push_back(pa);
      return m.rd(pa);
    }, true, true);
    CHECK(s.m5Link[0].read && s.m5Link[1].read);
    checkProbeLoads(seen, 0, true, ctrl0 == 0x60u);
    checkProbeLoads(seen, 1, true, true);
    Text t;
    format(s, 12, 0, t);
    CHECK(t.has("dsi0 pll: status unread"));
    CHECK(t.has("dsi1 pll: status 00000000 local-lock 0"));
    CHECK(m.stores.empty() && m.refused.empty());
  }
}

void testM5ClockGates()
{
  struct Case { uint64_t reg; uint32_t value; int link; };
  const Case cases[] = {
    {kGcc + kGccDispAhb, 0x80000000, -1}, {kGcc + kGccDispAhb, UINT32_MAX, -1}, {kGcc + kGccDispAhb, 0, -1},
    {kDispCc + kCcMdssGdsc, 0, -1}, {kDispCc + kCcMdssGdsc, UINT32_MAX, -1},
    {kDispCc + kCcMdssGdsc, 0x80000001, -1},
    {kDispCc + kCcAhb, 0x80000000, -1}, {kDispCc + kCcAhb, 0, -1},
    {kDispCc + kCcMdp, 0x80000000, -1},
    {kDispCc + kCcByte0, 0x80000001, 0}, {kDispCc + kCcByte1, 0, 1},
    {kDispCc + kCcPclk0, UINT32_MAX, 0}, {kDispCc + kCcPclk1, 0x80000000, 1},
  };
  for (const auto &c : cases) {
    model::Dpu m;
    seedY12k(m);
    enableLinkClocks(m);
    for (uint64_t base : kDsiPhy) m.regs[base + 0x024] = 0x60;
    m.regs[c.reg] = c.value;
    Snapshot s;
    std::vector<uint64_t> seen;
    take(s, [&](uint64_t pa) {
      seen.push_back(pa);
      return m.rd(pa);
    }, true, true);
    for (unsigned l = 0; l < 2; ++l) {
      const bool expected = c.link >= 0 && unsigned(c.link) != l;
      CHECK(s.m5Link[l].read == expected);
      checkProbeLoads(seen, l, expected, expected);
    }
    CHECK(m.stores.empty() && m.refused.empty());
  }

  // Revalidation must catch a gate that went away after the ordinary snapshot's DSI loads.
  model::Dpu m;
  seedY12k(m);
  enableLinkClocks(m);
  for (uint64_t base : kDsiPhy) m.regs[base + 0x024] = 0x60;
  Snapshot s;
  std::vector<uint64_t> seen;
  take(s, [&](uint64_t pa) {
    seen.push_back(pa);
    if (pa == kDsi[0] + kDsiVidCompress) m.regs[kDispCc + kCcPclk0] = 0x80000000;
    return m.rd(pa);
  }, true, true);
  CHECK(s.dsi[0].read && !s.m5Link[0].read && s.m5Link[1].read);
  checkProbeLoads(seen, 0, false);
  checkProbeLoads(seen, 1, true, true);
  CHECK(m.stores.empty() && m.refused.empty());
}

void testHalfSwitched()
{
  model::Dpu m;
  seedY12k(m);
  m.sleepMs(3);
  m.intf[1].shadow = m.intf[1].cur = 4620 * model::kHtotal;   // INTF1 at 60's period, the rest at 120: no mode
  Snapshot s;
  Load ld{m};
  take(s, ld, true);
  CHECK(s.mode.hz == 0);
  Text t;
  format(s, 5, 0, t);
  CHECK(t.has("mode: live is none of 144/120/90/60"));
  CHECK(t.has("vfp switch (DpuRefresh.h): a DSI TOTAL does not match") || t.has("vfp switch (DpuRefresh.h): INTF"));
}

void testPanelAsleep()
{
  model::Dpu m;
  seedY12k(m);
  CHECK(panel::panelOff(m, slices::readPath(m), panel::Options()).ok);
  const size_t stores = m.stores.size();
  Snapshot s;
  take(s, Load{m}, true);
  CHECK(m.stores.size() == stores);
  CHECK(s.readable && !s.path.ok && s.path.ctl == 0 && s.path.intfActive == slices::kDsiIntfSlave && !s.path.intfMaster);
  CHECK(s.ctlFlush == 0 && s.ctlIntfFlush == 0);
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(!s.path.link[i].en && !s.path.link[i].status);
    CHECK(s.path.link[i].frame == m.intf[i + 1].frames && s.path.link[i].frame > 0);
  }
  Text t;
  format(s, 7, 0, t);
  CHECK(t.has("ctl0: top f0000000 intf-active 00000004 master 00000000"));
}

void testPanelWakeScanout()
{
  using namespace slices;
  model::Dpu m;
  seedY12k(m, true);
  m.regs[ctlReg(0, kCtlDscActive)] = 3;
  m.regs[ctlReg(0, kCtlPipeActive)] = 6;
  m.regs[ctlReg(0, kCtlFetchActive)] = 3;
  m.regs[intfReg(1, kIntfConfig2)] = 0x1011;
  m.regs[intfReg(2, kIntfConfig2)] = 0x1011;
  m.regs[intfReg(1, kIntfDataHctl)] = 0x01ca0020;
  m.regs[intfReg(2, kIntfDataHctl)] = 0x01ca0021;
  const uint64_t compression0 = dsiReg(0, kDsiVidCompress);
  const uint64_t compression1 = dsiReg(1, kDsiVidCompress);
  m.regs[compression0] = 0x03200b81;
  m.regs[compression1] = 0x03200b81;
  const LivePath path = readPath(m);
  const auto off = panel::panelOff(m, path, panel::Options());
  CHECK(off.ok && off.enginesStopped);
  CHECK(panel::panelOn(m, path, panel::Options(), off.enginesStopped, off.fetch).ok);
  const size_t stores = m.stores.size();
  Snapshot awake;
  take(awake, Load{m}, true);
  Text text;
  format(awake, 8, 0, text);
  CHECK(awake.path.ok && awake.dsi[0].videoCompression == awake.dsi[1].videoCompression);
  CHECK(text.has("scanout ctl0: dsc-active 00000003 dsc-flush 00000000 pipe-active 00000006 fetch-active 00000003"));
  CHECK(text.has("config2 00001011 data-hctl 01ca0020") && text.has("config2 00001011 data-hctl 01ca0021"));
  CHECK(text.has("video-compression 03200b81"));

  // A live INTF and video-enabled link can still have lost compression setup.
  m.regs[compression1] = 0;
  Snapshot mismatch;
  take(mismatch, Load{m}, true);
  CHECK(mismatch.path.ok && mismatch.path.link[0].engine() && mismatch.path.link[1].engine());
  CHECK(mismatch.dsi[0].videoCompression == 0x03200b81 && mismatch.dsi[1].videoCompression == 0);
  Text mismatchText;
  format(mismatch, 9, 0, mismatchText);
  CHECK(mismatchText.has("video-compression 00000000"));
  CHECK(m.stores.size() == stores && m.refused.empty());
}
}  // namespace

int main()
{
  testIdle120();
  testProgFetchOn();
  testAfterVfpSwitch();
  testGateClosed();
  testDsiClockOff();
  testM5Probe();
  testPllProbePower();
  testM5ClockGates();
  testHalfSwitched();
  testPanelAsleep();
  testPanelWakeScanout();
  if (failures) {
    std::printf("PianoDpu snapshot host tests: %d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("PianoDpu snapshot host tests: all passed (%d checks)\n", checks);
  return 0;
}
