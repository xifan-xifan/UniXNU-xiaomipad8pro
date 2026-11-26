// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of DpuSnapshot.h (B1 milestone 1's run-time snapshot) on the timed register model (dpu_model.h), seeded with the
// installed v13.1y12k survey's words: the snapshot stores nothing, keeps the survey's gate, classifies the live mode (120, then 60
// after DpuRefresh.h's own switch on the model), reads both DSI controllers' shadow and command-path words, and its text is bounded.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_snapshot_host.cpp -o /tmp/dpu_snapshot_host && /tmp/dpu_snapshot_host
#include "DpuSnapshot.h"
#include "DpuPanel.h"
#include "dpu_model.h"
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
}  // namespace

int main()
{
  testIdle120();
  testProgFetchOn();
  testAfterVfpSwitch();
  testGateClosed();
  testDsiClockOff();
  testHalfSwitched();
  testPanelAsleep();
  if (failures) {
    std::printf("PianoDpu snapshot host tests: %d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("PianoDpu snapshot host tests: all passed (%d checks)\n", checks);
  return 0;
}
