// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of DpuModes.h: the per-link timing words of 144/120/90/60 Hz.
// The anchor is the installed system's own boot survey (installed v13.1y12k, 2026-11-25, the PianoDpu survey lines):
//   intf1/intf2 raw: hsync 02400010 period 00144d80 vwidth 00000480 dv 0000ee80-0013b47f dhctl 01ca0020
//   dsi0/dsi1 raw:   active-h 01cb0020 active-v 08c2006a total 0905023f
//   refresh:         119.878 Hz (INTF pixel clock 159504 kHz), panel p81 35-02-0b (B2 then B3)
// The planner's 120 Hz words must be those exactly, and its 60 Hz words DpuRefresh.h's (period 00289b00, DSI total 120b023f).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_modes_host.cpp -o /tmp/dpu_modes_host && /tmp/dpu_modes_host
#include "DpuModes.h"
#include "DpuSurvey.h"
#include <cstdio>
#include <cstring>

using namespace pianodpu;
using namespace pianodpu::modes;

namespace {
unsigned gChecks, gFailures;
void check(bool ok, const char *what)
{
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::printf("FAIL: %s\n", what);
  }
}
void checkEq(uint64_t got, uint64_t want, const char *what)
{
  ++gChecks;
  if (got != want) {
    ++gFailures;
    std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
  }
}

LinkLive liveY12k()
{
  LinkLive l;
  l.hsyncCtl = 0x02400010;
  l.period = 0x00144d80;
  l.vsyncWidth = 0x00000480;
  l.dispVStart = 0x0000ee80;
  l.dispVEnd = 0x0013b47f;
  l.dispHctl = 0x01ca0020;
  l.dsiTotal = 0x0905023f;
  l.dsiActiveH = 0x01cb0020;
  l.dsiActiveV = 0x08c2006a;
  return l;
}

void testGeometry()
{
  const Geometry g = geometry(liveY12k());
  check(g.ok, "the live y12k words give a geometry");
  checkEq(g.hsync, 16, "hsync per link");
  checkEq(g.hsyncBp, 32, "hsync + hbp per link (the full panel's 32 + 32 halved)");
  checkEq(g.hactive, 427, "compressed active width per link (1600 px x 8 bpp / 30, rounded up)");
  checkEq(g.vsync, 2, "vsync");
  checkEq(g.vbp, 104, "vbp");
  checkEq(g.dsiHstart, 32, "DSI active start");
  checkEq(g.dsiHend, 459, "DSI active end");
}

void testLive120()
{
  const LinkLive live = liveY12k();
  const Geometry g = geometry(live);
  const LinkWords w = plan(g, *modeByHz(120));
  checkEq(w.htotal, 576, "120 Hz htotal per link");
  checkEq(w.vtotal, 2310, "120 Hz vtotal");
  check(firstDifference(w, live) == nullptr, "the 120 Hz plan is the live y12k registers word for word");
  checkEq(w.pixelClockHz, kPixelClockHz, "120 Hz INTF clock");
  const LinkLive both[2] = {live, live};
  const Classified c = classify(both);
  checkEq(c.hz, 120, "the live y12k registers classify as 120 Hz");
}

void testAllModes()
{
  const Geometry g = geometry(liveY12k());
  struct Want {
    unsigned hz, htotal, vtotal;
    uint32_t hsyncCtl, period, vsyncWidth, vStart, vEnd, dsiTotal;
  };
  // Worked out from the msm formulas (DpuModes.h header) by hand; 60 Hz = DpuRefresh.h's switch (dpu.md §8.1 c0 plan line).
  const Want want[] = {
    {144, 480, 2310, 0x01e00010, 480u * 2310, 2 * 480, 106 * 480, 2242 * 480 - 1, 2309u << 16 | 479},
    {120, 576, 2310, 0x02400010, 0x00144d80, 0x480, 0xee80, 0x13b47f, 0x0905023f},
    {90, 768, 2310, 0x03000010, 768u * 2310, 2 * 768, 106 * 768, 2242 * 768 - 1, 2309u << 16 | 767},
    {60, 576, 4620, 0x02400010, 0x00289b00, 0x480, 0xee80, 0x13b47f, 0x120b023f},
  };
  for (const Want &x : want) {
    char what[96];
    const LinkWords w = plan(g, *modeByHz(x.hz));
    std::snprintf(what, sizeof what, "%u Hz htotal", x.hz);
    checkEq(w.htotal, x.htotal, what);
    std::snprintf(what, sizeof what, "%u Hz vtotal", x.hz);
    checkEq(w.vtotal, x.vtotal, what);
    std::snprintf(what, sizeof what, "%u Hz HSYNC_CTL", x.hz);
    checkEq(w.hsyncCtl, x.hsyncCtl, what);
    std::snprintf(what, sizeof what, "%u Hz VSYNC_PERIOD_F0", x.hz);
    checkEq(w.period, x.period, what);
    std::snprintf(what, sizeof what, "%u Hz VSYNC_PULSE_WIDTH_F0", x.hz);
    checkEq(w.vsyncWidth, x.vsyncWidth, what);
    std::snprintf(what, sizeof what, "%u Hz DISPLAY_V_START_F0", x.hz);
    checkEq(w.dispVStart, x.vStart, what);
    std::snprintf(what, sizeof what, "%u Hz DISPLAY_V_END_F0", x.hz);
    checkEq(w.dispVEnd, x.vEnd, what);
    std::snprintf(what, sizeof what, "%u Hz DSI TOTAL", x.hz);
    checkEq(w.dsiTotal, x.dsiTotal, what);
    std::snprintf(what, sizeof what, "%u Hz DISPLAY_HCTL unchanged", x.hz);
    checkEq(w.dispHctl, 0x01ca0020, what);
    std::snprintf(what, sizeof what, "%u Hz DSI ACTIVE_H unchanged", x.hz);
    checkEq(w.dsiActiveH, 0x01cb0020, what);
    std::snprintf(what, sizeof what, "%u Hz DSI ACTIVE_V unchanged", x.hz);
    checkEq(w.dsiActiveV, 0x08c2006a, what);
    std::snprintf(what, sizeof what, "%u Hz keeps the INTF clock (no clock change)", x.hz);
    checkEq(w.pixelClockHz, kPixelClockHz, what);
  }
}

void testChangedWords()
{
  const Geometry g = geometry(liveY12k());
  const LinkWords w120 = plan(g, *modeByHz(120)), w60 = plan(g, *modeByHz(60)), w144 = plan(g, *modeByHz(144)),
                  w90 = plan(g, *modeByHz(90));
  checkEq(changedWords(w120, w60), kWordPeriod | kWordDsiTotal, "120 -> 60 changes the period and DSI TOTAL only (DpuRefresh.h's plan)");
  const unsigned hfp = kWordHsyncCtl | kWordPeriod | kWordVsyncWidth | kWordVStart | kWordVEnd | kWordDsiTotal;
  checkEq(changedWords(w120, w144), hfp, "120 -> 144 changes the six HFP-dependent words, never HCTL / ACTIVE_H / ACTIVE_V");
  checkEq(changedWords(w120, w90), hfp, "120 -> 90 the same six");
  checkEq(changedWords(w144, w90), hfp, "144 -> 90 the same six");
}

void testClassifyOthers()
{
  const Geometry g = geometry(liveY12k());
  for (const Mode &m : kModes) {
    const LinkWords w = plan(g, m);
    LinkLive l = liveY12k();
    l.hsyncCtl = w.hsyncCtl;
    l.period = w.period;
    l.vsyncWidth = w.vsyncWidth;
    l.dispVStart = w.dispVStart;
    l.dispVEnd = w.dispVEnd;
    l.dsiTotal = w.dsiTotal;
    const LinkLive both[2] = {l, l};
    char what[64];
    std::snprintf(what, sizeof what, "a link set to %u Hz classifies as %u Hz", m.hz, m.hz);
    checkEq(classify(both).hz, m.hz, what);
  }
  // Half switched: INTF2 still at 120 while INTF1 shows 60's period -> no mode, the first difference named.
  LinkLive a = liveY12k(), b = liveY12k();
  a.period = 0x00289b00;
  a.dsiTotal = 0x120b023f;
  const LinkLive both[2] = {a, b};
  const Classified c = classify(both);
  checkEq(c.hz, 0, "a half-switched pair is no mode");
  check(c.why && std::strcmp(c.why, "VSYNC_PERIOD_F0") == 0 && c.link == 0, "the half-switched pair names INTF1's period");
  // A DSI TOTAL that disagrees with its INTF.
  LinkLive d = liveY12k();
  d.dsiTotal = 0x0905023e;
  const LinkLive pair[2] = {d, liveY12k()};
  check(classify(pair).hz == 0 && std::strcmp(classify(pair).why, "DSI TOTAL") == 0, "a DSI TOTAL off by one is named");
  // Garbage: an unclocked read (all ones) gives no geometry.
  LinkLive z;
  std::memset(&z, 0xff, sizeof z);
  const LinkLive zz[2] = {z, z};
  check(classify(zz).hz == 0 && !geometry(z).ok, "all-ones words give no geometry");
  LinkLive zero;
  check(!geometry(zero).ok, "zero words give no geometry");
}

// Programmable fetch as the boot stages leave it (INTF_CONFIG bit 31): the start keeps its lines before the frame end in every mode.
void testProgFetch()
{
  check(liveProgFetch(0x80800000u, 1234) == 1234 && liveProgFetch(0x00800000u, 1234) == 0, "bit 31 of INTF_CONFIG gates the start");
  LinkLive l = liveY12k();
  l.progFetch = (2310 - 24) * 576 + 1;
  const Geometry g = geometry(l);
  check(g.ok && g.fetchLines == 24 && g.fetchOffset == 0, "24 fetch lines read back from the start");
  const LinkWords w120 = plan(g, *modeByHz(120)), w60 = plan(g, *modeByHz(60)), w144 = plan(g, *modeByHz(144)), w90 = plan(g, *modeByHz(90));
  check(firstDifference(w120, l) == nullptr, "the 120 Hz plan keeps the live start");
  checkEq(w60.progFetch, (4620 - 24) * 576 + 1, "60 Hz moves the start with the long frame");
  checkEq(w144.progFetch, (2310 - 24) * 480 + 1, "144 Hz recounts it in 480-clock lines");
  checkEq(w90.progFetch, (2310 - 24) * 768 + 1, "90 Hz in 768-clock lines");
  checkEq(changedWords(w120, w60), kWordPeriod | kWordDsiTotal | kWordProgFetch, "with fetch on, 120 -> 60 must also move the start");
  check(changedWords(w120, w144) & kWordProgFetch, "and 120 -> 144");
  // The old start at 144 Hz would point past the frame (2286 lines of 576 clocks = 2743 lines of 480): the hazard the plan removes.
  check((l.progFetch - 1) / w144.htotal >= w144.vtotal, "the unmoved 120 Hz start lies past a 144 Hz frame");
  const LinkLive pair[2] = {l, l};
  checkEq(classify(pair).hz, 120, "fetch on still classifies as 120");
  LinkLive bad = liveY12k();
  bad.progFetch = 1000 * 576 + 1;   // inside the active lines
  check(!geometry(bad).ok, "a fetch start inside the active lines is refused");
  l.progFetch = (4620 - 69) * 576 + 1;
  l.period = 4620 * 576;
  const Geometry longFetch = geometry(l);
  check(longFetch.ok && longFetch.fetchLines == 69, "the long 60 Hz porch admits 69 fetch lines");
  checkEq(plan(longFetch, *modeByHz(144)).hz, 0, "the short 144 Hz porch cannot fit 69 fetch lines");
  l.progFetch = (4620 - 68) * 576 + 480 + 1;
  checkEq(plan(geometry(l), *modeByHz(144)).hz, 0, "a fetch offset beyond the short line cannot be preserved");
}

void testPlanLimits()
{
  const Geometry live = geometry(liveY12k());
  Mode mode = *modeByHz(120);
  mode.hfp = 2 * (UINT16_MAX - live.dsiHend);
  checkEq(plan(live, mode).htotal, UINT16_MAX, "the largest HSYNC_CTL line length fits");
  mode.hfp += 2;
  checkEq(plan(live, mode).hz, 0, "an oversized HSYNC_CTL line length is refused");
  mode = *modeByHz(120);
  mode.vfp = UINT16_MAX + 1 - (live.vsync + live.vbp + live.vactive);
  checkEq(plan(live, mode).dsiTotal >> 16, UINT16_MAX, "DSI TOTAL encodes the maximum frame length minus one");
  ++mode.vfp;
  checkEq(plan(live, mode).hz, 0, "an oversized DSI frame is refused");
  Geometry skewed = live;
  skewed.skew = UINT32_MAX;
  checkEq(plan(skewed, *modeByHz(120)).hz, 0, "the display end clock cannot wrap");
}

void testRateGroup()
{
  const Mode &m144 = *modeByHz(144), &m120 = *modeByHz(120), &m90 = *modeByHz(90), &m60 = *modeByHz(60);
  check(sameGroup(m120, m60), "60 and 120 share the panel's rate group (no DCS for a VFP switch)");
  check(!sameGroup(m120, m144) && !sameGroup(m120, m90) && !sameGroup(m144, m90), "144, 120 and 90 are three groups");
  const RateGroup boe = rateGroup(m144, true);   // this unit: p81 35-02-0b, ID pin high
  checkEq(boe.count, 4, "four DCS writes");
  check(boe.w[0].command == 0xff && boe.w[0].param == 0x10 && boe.w[1].command == 0xfb && boe.w[1].param == 0x01,
        "command page 0x10 selected first (FF 10, FB 01)");
  check(boe.w[2].command == 0xb2 && boe.w[2].param == 0x00 && boe.w[3].command == 0xb3 && boe.w[3].param == 0x00, "BOE 144: B2 00, B3 00");
  const RateGroup csot = rateGroup(m90, false);
  check(csot.w[2].command == 0xb3 && csot.w[2].param == 0x80 && csot.w[3].command == 0xb2 && csot.w[3].param == 0x00, "CSOT 90: B3 80, B2 00");
  const RateGroup back = rateGroup(m120, true);
  check(back.w[2].param == 0x91 && back.w[3].param == 0x40, "BOE 120: B2 91, B3 40");
  // The survey's ID-pin decode and this module order agree.
  check(std::strcmp(rateOrder(1), "B2 then B3") == 0 && std::strcmp(panelModule(1), "p81 35-02-0b") == 0, "ID pin high = BOE, B2 first");
}
}  // namespace

int main()
{
  testGeometry();
  testLive120();
  testAllModes();
  testChangedWords();
  testClassifyOthers();
  testProgFetch();
  testPlanLimits();
  testRateGroup();
  if (gFailures) {
    std::printf("PianoDpu modes host tests: %u of %u checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  std::printf("PianoDpu modes host tests: all passed (%u checks)\n", gChecks);
  return 0;
}
