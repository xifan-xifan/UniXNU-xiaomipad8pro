// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's run-time snapshot (B1 milestone 1, `piano-dpu-snapshot=1`, read through `sysctl machdep.piano_dpu_snapshot`): the
// registers the later milestones depend on, read fresh at the call on the running system (idle, while something moves, during
// display sleep, after a wake), next to what the planners would write. Portable and store-free (host tests in
// tests/dpu_snapshot_host.cpp): every access is a Load, a function of a physical address returning the 32-bit word there.
//
// Behind the survey's own gate (DpuSurvey.h surveyClocks): nothing of MDSS, the DPU or the DSI controllers is loaded unless GCC's
// display AHB, the MDSS GDSC and its AHB/MDP branches are on, and nothing of a DSI controller unless its byte and pixel clocks run.
// What it answers (docs: the B1 analysis, its section 2 "unknown" rows):
//   - the DSI video timing shadow words (base + 0x1e4 / 0x1e8 / 0x1ec) on both links: where the DB mode and the timing flush are
//     (B1c's open question 5, needed by every porch switch, 60 and 90/144 alike);
//   - the DSI command path as the boot stages left it: CMD_DMA_CTRL, TRIG_CTRL (DMA trigger, BLOCK_DMA_WITHIN_FRAME), INTR_CTRL,
//     TEST_PATTERN_GEN_CTRL, +0x390, STATUS0 (B1e's open question 8; the panel's rate-group DCS of 90/144 goes the same way);
//   - the DSI clock control and lane status (the clock lane kept in high speed, Ubuntu 6d57c462);
//   - INTF1/INTF2's engines and counters (the DPU fetch during display sleep: do the counters move with the backlight off?);
//   - the DSPPs' PCC and GC words (Night Shift's target; read back after a colour change);
//   - the live mode classified against DpuModes.h's plans, and each mode's words; the VFP slice's verdict (DpuRefresh.h refuse).
#pragma once
#include <stdint.h>
#include "DpuModes.h"
#include "DpuRefresh.h"

namespace pianodpu {
namespace snapshot {

using slices::Link;
using slices::LivePath;

struct DsiWords {
  bool read = false;
  uint32_t ctrl = 0, status0 = 0, total = 0, activeH = 0, activeV = 0, shadow[3] = {0, 0, 0}, cmdDmaCtrl = 0, trigCtrl = 0, intrCtrl = 0,
           tpgCtrl = 0, dmaCtrl1 = 0, clkCtrl = 0, laneStatus = 0, laneCtrl = 0;
};
struct IntfWords {
  uint32_t vsyncWidth = 0, dispVStart = 0, dispVEnd = 0, dispHctl = 0, skew = 0, progFetch = 0, intrStatus = 0;
};
struct DsppWords {
  uint32_t pcc[13] = {};
  uint32_t gcEnable = 0;
};
struct Snapshot {
  LivePath path;               // the gate, the CTL, INTF1/2 engine/timing/counters, DSI CTRL/TOTAL/TRIG (DpuSlices.h readPath)
  bool readable = false;       // the gate passed: the DPU was read
  IntfWords intf[2];
  DsiWords dsi[2];
  DsppWords dspp[2];
  uint32_t ctlFlush = 0, ctlIntfFlush = 0, ctlDsppFlush[2] = {0, 0};
  uint32_t topIntrEn = 0, topIntrStatus = 0, mdssIntrStatus = 0;
  uint32_t panelIdIo = 0;
  modes::LinkLive live[2];
  modes::Classified mode;
  const char *vfpVerdict = "not read";   // nullptr: the 120 Hz family takes DpuRefresh.h's switch
  unsigned loads = 0;
};

// The snapshot itself. hwGatedAhb: the survey's gate rule as piano-dpu-ahb-vote set it (1 on piano: enabled + hardware-gated = on).
template <class Load>
void take(Snapshot &s, Load load, bool hwGatedAhb)
{
  struct Io {
    Load &ld;
    unsigned &n;
    uint32_t rd(uint64_t pa)
    {
      ++n;
      return ld(pa);
    }
  } io{load, s.loads};
  s.panelIdIo = io.rd(kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride + 4);
  s.path = slices::readPath(io, hwGatedAhb);
  if (!s.path.clocks.readable) {
    s.vfpVerdict = s.path.why;
    return;
  }
  s.readable = true;
  s.mdssIntrStatus = io.rd(kMdss + kMdssIntrStatus);
  s.topIntrEn = io.rd(kMdp + kTopIntrEn);
  s.topIntrStatus = io.rd(kMdp + kTopIntrStatus);
  for (unsigned i = 0; i < 2; ++i) {
    IntfWords &w = s.intf[i];
    w.vsyncWidth = io.rd(slices::intfReg(i + 1, kIntfVsyncWidth));
    w.dispVStart = io.rd(slices::intfReg(i + 1, kIntfDispVStart));
    w.dispVEnd = io.rd(slices::intfReg(i + 1, kIntfDispVEnd));
    w.dispHctl = io.rd(slices::intfReg(i + 1, kIntfDispHctl));
    w.skew = io.rd(slices::intfReg(i + 1, kIntfHsyncSkew));
    w.progFetch = io.rd(slices::intfReg(i + 1, kIntfProgFetch));
    w.intrStatus = io.rd(slices::intfReg(i + 1, kIntfIntrStatus));
  }
  if (s.path.ctl >= 0) {
    const unsigned c = unsigned(s.path.ctl);
    s.ctlFlush = io.rd(slices::ctlReg(c, kCtlFlush));
    s.ctlIntfFlush = io.rd(slices::ctlReg(c, kCtlIntfFlush));
    for (unsigned d = 0; d < 2; ++d) s.ctlDsppFlush[d] = io.rd(slices::ctlReg(c, kCtlDspp0Flush + 4 * d));
  }
  for (unsigned d = 0; d < 2; ++d) {
    for (unsigned k = 0; k < 13; ++k) s.dspp[d].pcc[k] = io.rd(slices::pccReg(d, 4 * k));
    s.dspp[d].gcEnable = io.rd(kMdp + kDsppBase + uint64_t(kDsppStride) * d + kGc);
  }
  // A DSI controller only with its own link clocks on (DpuSurvey.h surveyDpu's rule).
  for (unsigned l = 0; l < 2; ++l) {
    if (!branchOn(s.path.clocks.byte[l]) || !branchOn(s.path.clocks.pclk[l])) continue;
    DsiWords &d = s.dsi[l];
    d.read = true;
    d.ctrl = io.rd(slices::dsiReg(l, kDsiCtrl));
    d.status0 = io.rd(slices::dsiReg(l, kDsiStatus0));
    d.total = io.rd(slices::dsiReg(l, kDsiTotal));
    d.activeH = io.rd(slices::dsiReg(l, kDsiActiveH));
    d.activeV = io.rd(slices::dsiReg(l, kDsiActiveV));
    for (unsigned k = 0; k < 3; ++k) d.shadow[k] = io.rd(slices::dsiReg(l, slices::kDsiShadow[k]));
    d.cmdDmaCtrl = io.rd(slices::dsiReg(l, slices::kDsiCmdDmaCtrl));
    d.trigCtrl = io.rd(slices::dsiReg(l, slices::kDsiTrigCtrl));
    d.intrCtrl = io.rd(slices::dsiReg(l, kDsiIntrCtrl));
    d.tpgCtrl = io.rd(slices::dsiReg(l, slices::kDsiTpgCtrl));
    d.dmaCtrl1 = io.rd(slices::dsiReg(l, slices::kDsiDmaCtrl1));
    d.clkCtrl = io.rd(slices::dsiReg(l, kDsiClkCtrl));
    d.laneStatus = io.rd(slices::dsiReg(l, kDsiLaneStatus));
    d.laneCtrl = io.rd(slices::dsiReg(l, kDsiLaneCtrl));
  }
  for (unsigned i = 0; i < 2; ++i) {
    modes::LinkLive &v = s.live[i];
    const Link &k = s.path.link[i];
    v.hsyncCtl = k.hsyncCtl;
    v.period = k.period;
    v.vsyncWidth = s.intf[i].vsyncWidth;
    v.dispVStart = s.intf[i].dispVStart;
    v.dispVEnd = s.intf[i].dispVEnd;
    v.dispHctl = s.intf[i].dispHctl;
    v.dsiTotal = s.dsi[i].read ? s.dsi[i].total : 0;
    v.dsiActiveH = s.dsi[i].activeH;
    v.dsiActiveV = s.dsi[i].activeV;
    v.progFetch = modes::liveProgFetch(k.config, s.intf[i].progFetch);
  }
  if (s.dsi[0].read && s.dsi[1].read && s.intf[0].skew == s.intf[1].skew) s.mode = modes::classify(s.live, s.intf[0].skew);
  else s.mode.why = !s.dsi[0].read || !s.dsi[1].read ? "a DSI controller was not read (its link clocks are off)" : "the INTF skews differ";
  s.vfpVerdict = refresh::refuse(s.path);
}

// ------------------------------------------------------------------------------------------------------------- the text
// Out: `void line(const char *fmt, ...)` printf-like, one line per call (the kernel's appends to the sysctl's buffer, the host's
// to a string). At most kMaxLines lines of at most ~300 characters.
constexpr unsigned kMaxLines = 24;
constexpr uint32_t kTrigDmaMask = 7u;   // TRIG_CTRL bits 2:0, the DMA trigger (4 = software; DpuPanel.h)

inline const char *trigName(uint32_t trig)
{
  const uint32_t t = trig & kTrigDmaMask;
  return t == 4 ? "software" : t == 0 ? "none" : "hardware";
}

// The live link's nine words in a plan's shape (for modes::changedWords).
inline modes::LinkWords liveWords(const modes::LinkLive &l)
{
  modes::LinkWords v;
  v.hsyncCtl = l.hsyncCtl;
  v.period = l.period;
  v.vsyncWidth = l.vsyncWidth;
  v.dispVStart = l.dispVStart;
  v.dispVEnd = l.dispVEnd;
  v.dispHctl = l.dispHctl;
  v.dsiTotal = l.dsiTotal;
  v.dsiActiveH = l.dsiActiveH;
  v.dsiActiveV = l.dsiActiveV;
  v.progFetch = l.progFetch;
  return v;
}

template <class Out>
void format(const Snapshot &s, uint64_t serial, uint64_t atUs, Out &out)
{
  const ClockGate &g = s.path.clocks;
  out.line("snapshot %llu at %llu us: %u loads, 0 stores; gate %s (gcc disp ahb %08x, gdsc %08x, ahb %08x, mdp %08x, byte %08x/%08x, "
           "pclk %08x/%08x); panel id io %08x (%s, rate group %s)",
           (unsigned long long)serial, (unsigned long long)atUs, s.loads, g.readable ? "open" : g.why, g.gccDispAhb, g.gdsc, g.ahb, g.mdp,
           g.byte[0], g.byte[1], g.pclk[0], g.pclk[1], s.panelIdIo, panelModule(s.panelIdIo), rateOrder(s.panelIdIo));
  if (!s.readable) {
    out.line("snapshot: nothing of MDSS, the DPU or DSI was read (the gate is closed)");
    return;
  }
  out.line("ctl%d: top %08x intf-active %08x master %08x lms %02x flush %08x intf-flush %08x dspp-flush %08x/%08x; top intr en %08x "
           "status %08x; mdss intr %08x; path %s",
           s.path.ctl, s.path.ctlTop, s.path.intfActive, s.path.intfMaster, s.path.lms, s.ctlFlush, s.ctlIntfFlush, s.ctlDsppFlush[0],
           s.ctlDsppFlush[1], s.topIntrEn, s.topIntrStatus, s.mdssIntrStatus, s.path.why);
  for (unsigned i = 0; i < 2; ++i) {
    const Link &k = s.path.link[i];
    const IntfWords &w = s.intf[i];
    out.line("intf%u: en %08x status %08x config %08x hsync %08x period %08x vwidth %08x dv %08x-%08x dhctl %08x skew %08x "
             "prog-fetch %08x intr %08x; counters en %08x frame %u line %u (htotal %u vtotal %u)",
             i + 1, k.en, k.status, k.config, k.hsyncCtl, k.period, w.vsyncWidth, w.dispVStart, w.dispVEnd, w.dispHctl, w.skew,
             w.progFetch, w.intrStatus, k.countEn, k.frame, k.line, k.htotal, k.vtotal);
  }
  for (unsigned l = 0; l < 2; ++l) {
    const DsiWords &d = s.dsi[l];
    if (!d.read) {
      out.line("dsi%u: not read (its byte or pixel clock is off)", l);
      continue;
    }
    out.line("dsi%u: ctrl %08x status0 %08x total %08x active-h %08x active-v %08x; timing shadow +1e4 %08x +1e8 %08x +1ec %08x; "
             "cmd-dma %08x trig %08x (dma trigger %s, block-in-frame %u) intr %08x tpg %08x +390 %08x; clk-ctrl %08x lanes %08x/%08x",
             l, d.ctrl, d.status0, d.total, d.activeH, d.activeV, d.shadow[0], d.shadow[1], d.shadow[2], d.cmdDmaCtrl, d.trigCtrl,
             trigName(d.trigCtrl), (d.trigCtrl >> 12) & 1u, d.intrCtrl, d.tpgCtrl, d.dmaCtrl1, d.clkCtrl, d.laneStatus, d.laneCtrl);
  }
  for (unsigned d = 0; d < 2; ++d) {
    const DsppWords &p = s.dspp[d];
    out.line("dspp%u: pcc en %08x c %08x %08x %08x | %08x %08x %08x | %08x %08x %08x (+%08x %08x %08x); gc en %08x", d, p.pcc[0],
             p.pcc[4], p.pcc[5], p.pcc[6], p.pcc[7], p.pcc[8], p.pcc[9], p.pcc[10], p.pcc[11], p.pcc[12], p.pcc[1], p.pcc[2], p.pcc[3],
             p.gcEnable);
  }
  if (s.mode.hz) out.line("mode: live %u Hz (%s)", s.mode.hz, s.mode.why);
  else out.line("mode: live is none of 144/120/90/60 (link %u: %s)", s.mode.link, s.mode.why);
  const modes::Geometry geo = modes::geometry(s.live[0], s.intf[0].skew);
  if (geo.ok) {
    const modes::LinkWords now = liveWords(s.live[0]);
    for (const Mode &m : kModes) {
      const modes::LinkWords w = modes::plan(geo, m);
      out.line("plan %u Hz: hsync %08x period %08x vwidth %08x dv %08x-%08x dhctl %08x prog-fetch %08x; dsi total %08x active-h %08x "
               "active-v %08x; intf clock %llu Hz; words changed from live %03x",
               m.hz, w.hsyncCtl, w.period, w.vsyncWidth, w.dispVStart, w.dispVEnd, w.dispHctl, w.progFetch, w.dsiTotal, w.dsiActiveH,
               w.dsiActiveV, (unsigned long long)w.pixelClockHz, modes::changedWords(w, now));
    }
    out.line("plan: programmable fetch %s", geo.fetchLines ? "on (as the boot stages left it): its start moves with every porch switch" : "off");
  } else {
    out.line("plan: no geometry from the live words (%s)", geo.why);
  }
  out.line("vfp switch (DpuRefresh.h): %s", s.vfpVerdict ? s.vfpVerdict : "the path takes it (120 Hz family, totals agree, no programmable fetch)");
}

}  // namespace snapshot
}  // namespace pianodpu
