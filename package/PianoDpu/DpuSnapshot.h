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
           tpgCtrl = 0, dmaCtrl1 = 0, clkCtrl = 0, laneStatus = 0, laneCtrl = 0, videoCompression = 0;
};
// dsi.xml (with the host's 6G +4 shift) and dsi_phy_7nm.xml (unshifted common block),
// as pinned in dpu-m5-analysis.md. Preserve the raw words for bright/dark comparisons.
enum M5HostWord {
  VidCfg0, VidCfg1, Hsync, VsyncHpos, VsyncVpos, Fifo, AckErr, LaneSwap, PhyErr,
  LpTimer, HsTimer, Timeout, Clkout, Eot, ErrMask, ClkStatus, TClkPre, CmdComp, CmdComp2,
  CmdCfg0, CmdCfg1, DmaLength, HostReset, MisrCtrl, HostWordCount
};
constexpr uint32_t kM5HostOffsets[HostWordCount] = {
    kDsiVidCfg0, kDsiVidCfg1, kDsiActiveHsync, kDsiVsyncHpos, kDsiVsyncVpos, kDsiFifoStatus, slices::kDsiAckErr,
    0x0b0, 0x0b4, 0x0b8, 0x0bc, 0x0c0, 0x0c4, 0x0cc, kDsiErrIntMask0, kDsiClkStatus, 0x180, 0x2a8, 0x2ac,
    0x040, 0x044, slices::kDsiDmaLength, slices::kDsiSwReset, 0x0a4};
enum M5PhyWord {
  ClkCfg0, ClkCfg1, RbufCtrl, PhyStatus, LaneStatus0, LaneStatus1, Ctrl1, Ctrl2, Ctrl3, Ctrl5,
  LaneCfg0, LaneCfg1, PllCtrl, LaneCtrl0, LaneCtrl1, Ctrl0, PhyWordCount
};
constexpr uint32_t kM5PhyOffsets[PhyWordCount] = {
    0x010, 0x014, 0x01c, 0x140, 0x148, 0x14c, 0x028, 0x02c, 0x030, 0x1b0, 0x034, 0x038, 0x03c, 0x0a0, 0x0a4, 0x024};
constexpr uint32_t kM5PhyTimingOffsets[] = {
    0x0b4, 0x0b8, 0x0bc, 0x0c0, 0x0c4, 0x0c8, 0x0cc, 0x0d0, 0x0d4, 0x0d8, 0x0dc, 0x0e0, 0x0e4, 0x0e8};
constexpr uint32_t kM5PllStatus = 0x1b0;  // PLL_COMMON_STATUS_ONE, bit 0 is the local PLL lock
constexpr uint32_t kM5PllPowerMask = (1u << 6) | (1u << 5);  // CTRL_0 DIGTOP_PWRDN_B | PLL_SHUTDOWNB
static_assert(kM5PllStatus % sizeof(uint32_t) == 0 && kM5PllStatus <= kDsiPllBytes - sizeof(uint32_t));

template <uint32_t Bytes, const auto &Offsets, class Io, unsigned N>
void readM5Words(Io &io, uint64_t base, uint32_t (&words)[N])
{
  static_assert(Bytes >= sizeof(uint32_t));
  static_assert(N == sizeof Offsets / sizeof Offsets[0]);
  static_assert([] {
    for (uint32_t offset : Offsets)
      if (offset % sizeof(uint32_t) || offset > Bytes - sizeof(uint32_t)) return false;
    return true;
  }());
  for (unsigned i = 0; i < N; ++i) words[i] = io.rd(base + Offsets[i]);
}

struct M5LinkWords {
  bool read = false;
  uint32_t host[HostWordCount] = {}, phy[PhyWordCount] = {};
  uint32_t timing[14] = {};
  uint32_t pllStatus = UINT32_MAX;
};

template <class Io>
bool m5LinkClocked(Io &io, unsigned link, bool hwGatedAhb)
{
  const auto enabled = [](uint32_t cbcr) { return branchOn(cbcr) && (cbcr & kCbcrEnable); };
  const uint32_t gccAhb = io.rd(kGcc + kGccDispAhb);
  if (!enabled(gccAhb) && !(hwGatedAhb && branchHwGated(gccAhb))) return false;
  const uint32_t gdsc = io.rd(kDispCc + kCcMdssGdsc);
  constexpr uint32_t kSwCollapse = 1u;  // GDSCR SW_COLLAPSE (qcom gdsc.c)
  // Both PHYs' DT iface clock is DISP_CC_MDSS_AHB_CLK. Require a valid
  // powered MDSS domain and explicitly running, enabled AHB and link branches.
  return gdscOn(gdsc) && !(gdsc & kSwCollapse) &&
         enabled(io.rd(kDispCc + kCcAhb)) && enabled(io.rd(kDispCc + kCcMdp)) &&
         enabled(io.rd(kDispCc + (link == 0 ? kCcByte0 : kCcByte1))) &&
         enabled(io.rd(kDispCc + (link == 0 ? kCcPclk0 : kCcPclk1)));
}
struct IntfWords {
  uint32_t vsyncWidth = 0, dispVStart = 0, dispVEnd = 0, dispHctl = 0, skew = 0, progFetch = 0, intrStatus = 0,
           config2 = 0, dataHctl = 0;
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
  bool m5 = false;
  M5LinkWords m5Link[2];
  DsppWords dspp[2];
  uint32_t ctlFlush = 0, ctlIntfFlush = 0, ctlDsppFlush[2] = {0, 0};
  uint32_t ctlDscActive = 0, ctlDscFlush = 0, ctlPipeActive = 0, ctlFetchActive = 0;
  uint32_t topIntrEn = 0, topIntrStatus = 0, mdssIntrStatus = 0;
  uint32_t panelIdIo = 0;
  modes::LinkLive live[2];
  modes::Classified mode;
  const char *vfpVerdict = "not read";   // nullptr: the 120 Hz family takes DpuRefresh.h's switch
  unsigned loads = 0;
};

// The snapshot itself. hwGatedAhb: the survey's gate rule as piano-dpu-ahb-vote set it (1 on piano: enabled + hardware-gated = on).
template <class Load>
void take(Snapshot &s, Load load, bool hwGatedAhb, bool m5 = false)
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
  s.m5 = m5;
  s.panelIdIo = io.rd(kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride + 4);
  s.path = slices::readPath(io, hwGatedAhb, true);
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
    w.config2 = io.rd(slices::intfReg(i + 1, kIntfConfig2));
    w.dataHctl = io.rd(slices::intfReg(i + 1, kIntfDataHctl));
  }
  if (s.path.ctl >= 0) {
    const unsigned c = unsigned(s.path.ctl);
    s.ctlFlush = io.rd(slices::ctlReg(c, kCtlFlush));
    s.ctlIntfFlush = io.rd(slices::ctlReg(c, kCtlIntfFlush));
    s.ctlDscActive = io.rd(slices::ctlReg(c, kCtlDscActive));
    s.ctlDscFlush = io.rd(slices::ctlReg(c, kCtlDscFlush));
    s.ctlPipeActive = io.rd(slices::ctlReg(c, kCtlPipeActive));
    s.ctlFetchActive = io.rd(slices::ctlReg(c, kCtlFetchActive));
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
    d.videoCompression = io.rd(slices::dsiReg(l, kDsiVidCompress));
    // Recheck immediately before accessing the PHY common block and host probe words.
    if (!s.m5 || !m5LinkClocked(io, l, hwGatedAhb)) continue;
    M5LinkWords &m = s.m5Link[l];
    readM5Words<kDsiPhyBytes, kM5PhyTimingOffsets>(io, kDsiPhy[l], m.timing);
    readM5Words<kDsiPhyBytes, kM5PhyOffsets>(io, kDsiPhy[l], m.phy);
    // CTRL_0 is sampled last above. Read the separate PLL status only with its digital top and bias up.
    const uint32_t ctrl0 = m.phy[Ctrl0];
    if (ctrl0 != UINT32_MAX && (ctrl0 & kM5PllPowerMask) == kM5PllPowerMask) m.pllStatus = io.rd(kDsiPll[l] + kM5PllStatus);
    readM5Words<kDsiBytes, kM5HostOffsets>(io, kDsi[l], m.host);
    m.read = true;
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
constexpr unsigned kMaxLines = 28;
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
  if (s.path.ctl >= 0)
    out.line("scanout ctl%d: dsc-active %08x dsc-flush %08x pipe-active %08x fetch-active %08x",
             s.path.ctl, s.ctlDscActive, s.ctlDscFlush, s.ctlPipeActive, s.ctlFetchActive);
  for (unsigned i = 0; i < 2; ++i) {
    const Link &k = s.path.link[i];
    const IntfWords &w = s.intf[i];
    out.line("intf%u: en %08x status %08x config %08x hsync %08x period %08x vwidth %08x dv %08x-%08x dhctl %08x skew %08x "
             "prog-fetch %08x intr %08x; counters en %08x frame %u line %u (htotal %u vtotal %u); config2 %08x data-hctl %08x",
             i + 1, k.en, k.status, k.config, k.hsyncCtl, k.period, w.vsyncWidth, w.dispVStart, w.dispVEnd, w.dispHctl, w.skew,
             w.progFetch, w.intrStatus, k.countEn, k.frame, k.line, k.htotal, k.vtotal, w.config2, w.dataHctl);
  }
  for (unsigned l = 0; l < 2; ++l) {
    const DsiWords &d = s.dsi[l];
    if (!d.read) {
      out.line("dsi%u: not read (its byte or pixel clock is off)", l);
      continue;
    }
    out.line("dsi%u: ctrl %08x status0 %08x total %08x active-h %08x active-v %08x; timing shadow +1e4 %08x +1e8 %08x +1ec %08x; "
             "cmd-dma %08x trig %08x (dma trigger %s, block-in-frame %u) intr %08x tpg %08x +390 %08x; clk-ctrl %08x lanes %08x/%08x; "
             "video-compression %08x",
             l, d.ctrl, d.status0, d.total, d.activeH, d.activeV, d.shadow[0], d.shadow[1], d.shadow[2], d.cmdDmaCtrl, d.trigCtrl,
             trigName(d.trigCtrl), (d.trigCtrl >> 12) & 1u, d.intrCtrl, d.tpgCtrl, d.dmaCtrl1, d.clkCtrl, d.laneStatus, d.laneCtrl,
             d.videoCompression);
    if (!s.m5) continue;
    const M5LinkWords &m = s.m5Link[l];
    if (!m.read) {
      out.line("dsi%u m5: host and PHY probe skipped (clock gate closed or unreadable)", l);
      continue;
    }
    const auto &h = m.host;
    const auto &p = m.phy;
    out.line("dsi%u m5: vid-cfg0 %08x vid-cfg1 %08x hsync %08x vsync-hpos %08x vsync-vpos %08x "
             "lp-timer %08x hs-timer %08x clkout %08x eot %08x lane-swap %08x t-clk-pre-extend %08x "
             "cmd-compression %08x/%08x clk-status %08x err-mask %08x ack-err %08x fifo %08x phy-err %08x timeout %08x",
             l, h[VidCfg0], h[VidCfg1], h[Hsync], h[VsyncHpos], h[VsyncVpos], h[LpTimer], h[HsTimer], h[Clkout], h[Eot],
             h[LaneSwap], h[TClkPre], h[CmdComp], h[CmdComp2], h[ClkStatus], h[ErrMask], h[AckErr], h[Fifo], h[PhyErr], h[Timeout]);
    out.line("dsi%u m5: cmd-cfg0 %08x cmd-cfg1 %08x dma-length %08x reset %08x misr-ctrl %08x",
             l, h[CmdCfg0], h[CmdCfg1], h[DmaLength], h[HostReset], h[MisrCtrl]);
    out.line("dsi%u phy: clk-cfg0 %08x clk-cfg1 %08x rbuf-ctrl %08x phy-status %08x lane-status0 %08x lane-status1 %08x",
             l, p[ClkCfg0], p[ClkCfg1], p[RbufCtrl], p[PhyStatus], p[LaneStatus0], p[LaneStatus1]);
    out.line("dsi%u phy: ctrl0 %08x ctrl1 %08x ctrl2 %08x ctrl3 %08x ctrl5 %08x lane-cfg0 %08x lane-cfg1 %08x "
             "lane-ctrl0 %08x lane-ctrl1 %08x pll-ctrl %08x",
             l, p[Ctrl0], p[Ctrl1], p[Ctrl2], p[Ctrl3], p[Ctrl5], p[LaneCfg0], p[LaneCfg1], p[LaneCtrl0], p[LaneCtrl1], p[PllCtrl]);
    const auto &t = m.timing;
    out.line("dsi%u phy timing 0..13: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
             l, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12], t[13]);
    if (m.pllStatus == UINT32_MAX) out.line("dsi%u pll: status unread", l);
    else out.line("dsi%u pll: status %08x local-lock %u", l, m.pllStatus, m.pllStatus & 1u);
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
      if (!w.hz) {
        out.line("plan %u Hz: timing cannot be represented", m.hz);
        continue;
      }
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
