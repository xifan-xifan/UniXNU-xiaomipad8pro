// SPDX-License-Identifier: BSD-2-Clause-Patent
// A timed register model of piano's panel path for the B1c/B1d/B1e host tests (dpu_refresh_host.cpp, dpu_pcc_host.cpp,
// dpu_panel_host.cpp): the Io of DpuSlices.h over simulated time. What it models, from the facts in DpuHw.h / DpuSlices.h:
//   - INTF1/INTF2 timing engines: frame length and line time at a fixed pixel clock, frame and line counters, INTF_STATUS;
//     INTF1 gates INTF2 while bonded. A handed-off slave latches EN = 0 at its own frame end.
//     VSYNC_PERIOD_F0 is a shadow taken at a CTL INTF flush;
//   - the CTL: FLUSH bits latch at INTF1's frame start. A stopped CTL consumes a route change after SW_RESET and START.
//   - the DSPPs' PCC words: shadows taken at a CTL DSPP flush (CTL_DSPP_n_FLUSH bit 4);
//   - each DSI controller's video TOTAL with its timing shadow (DB mode, flush) at one of the two candidate places (DbHw), latched at
//     its INTF's frame start; a frame whose DSI and INTF totals differ, or a TOTAL that changes inside a frame, is a glitch and sets
//     the INTF's underrun bit in DPU TOP0 (cleared by INTR_CLEAR);
//   - each DSI controller's command DMA from its FIFO: TRIG_DMA sends the FIFO's packet in the next vertical blanking (or at once when
//     the engine is stopped), sets INTR_CTRL bit 0; the panel halves follow the DCS commands they receive;
//   - the store guard: a store DpuSlices.h does not allow is refused and recorded.
// Fault injection: a DMA that never finishes, an engine that does not stop or start, a flush that never latches, counters off.
#pragma once
#include "DpuSlices.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace model {
using namespace pianodpu;
using namespace pianodpu::slices;

enum class DbHw : uint8_t { sunuefi, downstream };   // where the hardware's DSI timing DB mode / flush really are
constexpr uint32_t kHtotal = 576;

struct Store {
  uint64_t ns, pa;
  uint32_t v;
  uint32_t frame, line;   // INTF1's at the store
};
struct Dcs {
  uint64_t ns;
  unsigned link;
  uint8_t command;
  bool lowPower;
  uint8_t parameter;
};

struct Dpu {
  // ---- configuration (set before use)
  DbHw dbHw = DbHw::sunuefi;
  bool blockDma = true;                 // TRIG_CTRL bit 12 as the boot stages left it
  bool dmaHang[2] = {false, false};     // a link's command DMA never finishes
  bool retainBusyVideo[2] = {};         // losing the INTF input can strand the host video FSM
  int dmaBudget[2] = {-1, -1};          // >= 0: this many more transfers finish, then the link hangs
  bool engineStuck[2] = {false, false}; // EN = 0 does not stop INTF1 / INTF2
  bool engineDead[2] = {false, false};  // EN = 1 does not start it
  bool flushStuck = false;              // CTL_FLUSH never latches
  bool modeWrites = false;              // M3's extra INTF timing registers are writable
  uint64_t dropStore = 0;               // an accepted store whose hardware read-back does not change
  uint32_t dropValue = 0;
  unsigned dropCount = 0;
  unsigned underrunAfterDma = 0;       // inject one underrun after this many bonded commands
  uint32_t minFetchLineNs = 0;          // inject recurring SSPP starvation below this line-time budget
  bool bandwidthVoteFails = false;
  uint32_t votedMinFetchLineNs = 0;    // model's service limit after a successful vote
  bool handleUnderrunIrq = false;
  uint32_t irqUnderruns = 0;
  bool resetPccOnTimingChange = false;
  unsigned lateDmas = 0;               // delay this many DMA completions past a short front porch
  bool countEnableIgnored[2] = {false, false};   // COUNT_EN writes do not change the readback or live counters
  bool ctlResetStuck = false;
  // ---- state
  uint64_t ns = 0;
  std::map<uint64_t, uint32_t> regs;
  std::vector<Store> stores;
  std::vector<uint64_t> refused;
  std::vector<Dcs> dcs;
  std::vector<size_t> bandwidthVotes; // store log position when the request completed
  unsigned glitches = 0, midFrameTotal = 0, commandsInFrame = 0, desync = 0, irqDepth = 0;
  struct Intf {
    bool on = true, stopPending = false, counting = true;
    uint64_t start = 0;
    uint32_t frames = 1000, period = 2310 * kHtotal, shadow = 2310 * kHtotal, cur = 2310 * kHtotal;
    uint32_t htotal = kHtotal;
    uint32_t fetch = 0;
    uint64_t underrunAt = UINT64_MAX;
  } intf[3];
  struct Dsi {
    uint32_t total = 2309u << 16 | 575, shadow = 2309u << 16 | 575;
    bool db = false, armed = false;
    std::vector<uint32_t> fifo;
    bool dmaPending = false;
    bool videoStuck = false;
    uint64_t dmaAt = 0;
    uint32_t intr = 0x00000002;   // the DMA done enable bit on, status clear
  } dsi[2];
  uint32_t ctlFlush = 0, ctlIntfFlush = 0, dsppFlush[4] = {0, 0, 0, 0};
  uint32_t latchedMaster = kDsiIntfMaster;
  uint64_t ctlResetDoneNs = 0;
  uint32_t pccShadow[4][13] = {}, pccActive[4][13] = {};
  uint32_t topStatus = 0;
  struct Half {
    bool displayOn = true, sleeping = false;
    uint8_t page = 0x10, b2 = 0x91, b3 = 0x40;
    bool unlocked = true;
  } half[2];
  uint64_t startNs[3] = {0, 0, 0};

  Dpu()
  {
    regs[kGcc + kGccDispAhb] = 1;
    regs[kDispCc + kCcMdssGdsc] = 0x80000000u;
    regs[kDispCc + kCcAhb] = 1;
    regs[kDispCc + kCcMdp] = 1;
    regs[ctlReg(0, kCtlTop)] = 0xf0000000u;
    regs[ctlReg(0, kCtlIntfActive)] = 0x6;
    regs[ctlReg(0, kCtlIntfMaster)] = 0x2;
    regs[ctlReg(0, kCtlLayerActive)] = 0x3;
    for (unsigned i = 1; i <= 2; ++i) {
      regs[intfReg(i, kIntfEn)] = 1;
      regs[intfReg(i, kIntfHsyncCtl)] = kHtotal << 16 | 16;
      regs[intfReg(i, kIntfCountEn)] = 3;
      regs[intfReg(i, kIntfConfig2)] = 0x1011;
    }
    for (unsigned l = 0; l < 2; ++l) {
      regs[dsiReg(l, kDsiCtrl)] = 0x1f3;
      regs[dsiReg(l, kDsiCmdDmaCtrl)] = kDmaLowPowerFact | 0x10000000u;
      regs[dsiReg(l, kDsiDmaCtrl1)] = 0x2;
      regs[dsiReg(l, kDsiDmaLength)] = 0;
      regs[dsiReg(l, kDsiClkCtrl)] = 0x33fu;
    }
  }
  static constexpr uint32_t kDmaLowPowerFact = 1u << 26;

  // ---- time
  uint64_t frameNs(unsigned i) const { return uint64_t(intf[i].cur) * kLineNs / kHtotal; }
  uint32_t vtotal(unsigned i) const { return intf[i].cur / intf[i].htotal; }
  uint32_t line(unsigned i) const
  {
    return intf[i].on ? uint32_t((ns - intf[i].start) * kHtotal / (intf[i].htotal * kLineNs)) : 0;
  }
  void latchCtl()
  {
    if (!ctlFlush || flushStuck) return;
    bool timingChanged = false;
    if (ctlFlush & kFlushIntf)
      for (unsigned i = 1; i <= 2; ++i)
        if ((ctlIntfFlush >> i) & 1u) {
          timingChanged |= intf[i].period != intf[i].shadow || intf[i].htotal != (regs[intfReg(i, kIntfHsyncCtl)] >> 16);
          intf[i].period = intf[i].shadow;
          intf[i].htotal = regs[intfReg(i, kIntfHsyncCtl)] >> 16;
          intf[i].fetch = regs[intfReg(i, kIntfProgFetch)];
        }
    if (timingChanged && resetPccOnTimingChange) {
      for (unsigned d = 0; d < 2; ++d)
        for (unsigned k = 0; k < 13; ++k) pccActive[d][k] = pccShadow[d][k] = 0;
    }
    if (ctlFlush & kFlushDspp)
      for (unsigned d = 0; d < 4; ++d)
        if (dsppFlush[d] & kDsppFlushPcc)
          for (unsigned k = 0; k < 13; ++k) pccActive[d][k] = pccShadow[d][k];
    ctlFlush = ctlIntfFlush = 0;
    for (unsigned &f : dsppFlush) f = 0;
  }
  void checkFrame(unsigned i)
  {
    const Dsi &d = dsi[i - 1];
    const auto &f = intf[i];
    const bool fetchOn = (regs[intfReg(i, kIntfConfig)] & kIntfConfigProgFetch) != 0;
    const bool fetchInBlank = f.fetch > kVActiveEnd * f.htotal && f.fetch <= f.cur;
    if ((d.total >> 16) + 1 != vtotal(i) || (d.total & 0xffffu) + 1 != f.htotal || (fetchOn && !fetchInBlank)) {
      ++glitches;
      topStatus |= 1u << kIntrIntfUnderrun[i];
    }
  }
  void advanceTo(uint64_t t)
  {
    for (;;) {
      uint64_t next = UINT64_MAX;
      for (unsigned i = 1; i <= 2; ++i)
        if (intf[i].on && intf[i].start + frameNs(i) < next) next = intf[i].start + frameNs(i);
      for (unsigned i = 1; i <= 2; ++i)
        if (intf[i].underrunAt < next) next = intf[i].underrunAt;
      for (unsigned l = 0; l < 2; ++l)
        if (dsi[l].dmaPending && dsi[l].dmaAt < next) next = dsi[l].dmaAt;
      if (next > t) break;
      ns = next;
      for (unsigned i = 1; i <= 2; ++i) {
        if (intf[i].underrunAt != next) continue;
        intf[i].underrunAt = UINT64_MAX;
        const uint32_t bit = 1u << kIntrIntfUnderrun[i];
        if (handleUnderrunIrq) irqUnderruns |= bit;
        else topStatus |= bit;
      }
      for (unsigned l = 0; l < 2; ++l)
        if (dsi[l].dmaPending && dsi[l].dmaAt == next) finishDma(l);
      bool started[3] = {false, false, false};
      for (unsigned i = 1; i <= 2; ++i) {
        if (!intf[i].on || intf[i].start + frameNs(i) != next) continue;
        if (intf[i].stopPending) {
          intf[i].on = false;
          dsi[i - 1].videoStuck |= retainBusyVideo[i - 1];
          intf[i].stopPending = false;
          if (i == 1 && latchedMaster == kDsiIntfMaster) {
            intf[2].on = false;
            dsi[1].videoStuck |= retainBusyVideo[1];
            intf[2].stopPending = false;
          }
          continue;
        }
        if (intf[i].counting) ++intf[i].frames;
        intf[i].start = next;
        started[i] = true;
      }
      if (started[1]) latchCtl();
      for (unsigned i = 1; i <= 2; ++i) {
        if (!started[i]) continue;
        intf[i].cur = intf[i].period;
        Dsi &d = dsi[i - 1];
        if (d.armed) {
          d.total = d.shadow;
          d.armed = false;
        }
        checkFrame(i);
        const uint64_t lineNs = uint64_t(intf[i].htotal) * kLineNs / kHtotal;
        if (lineNs < minFetchLineNs)
          intf[i].underrunAt = next + (kVActiveEnd - 1) * lineNs;
      }
    }
    ns = t;
  }
  void tick(uint64_t dns) { advanceTo(ns + dns); }

  bool voteModeBandwidth(const Mode &)
  {
    bandwidthVotes.push_back(stores.size());
    if (bandwidthVoteFails) return false;
    if (votedMinFetchLineNs) minFetchLineNs = votedMinFetchLineNs;
    return true;
  }

  // ---- the DSI command DMA
  void finishDma(unsigned l)
  {
    Dsi &d = dsi[l];
    d.dmaPending = false;
    const unsigned words = regs[dsiReg(l, kDsiDmaLength)] / 4;
    for (unsigned index = 0; index < words && index < d.fifo.size(); ++index) {
      const uint32_t w = d.fifo[index];
      const uint8_t type = uint8_t((w >> 16) & 0x3fu), command = uint8_t(w & 0xffu);
      const uint8_t parameter = uint8_t(w >> 8);
      Half &h = half[l];
      const bool lp = (regs[dsiReg(l, kDsiCmdDmaCtrl)] & kDmaLowPowerFact) != 0;
      if (type == 0x05) {
        dcs.push_back({ns, l, command, lp, 0});
        if (command == 0x28) h.displayOn = false;
        if (command == 0x29) h.displayOn = true;
        if (command == 0x10) h.sleeping = true;
        if (command == 0x11) h.sleeping = false;
      }
      if (type == 0x15) {
        dcs.push_back({ns, l, command, lp, parameter});
        if (command == 0xff) { h.page = parameter; h.unlocked = false; }
        if (command == 0xfb && parameter == 1) h.unlocked = true;
        if (h.page == 0x10 && h.unlocked && command == 0xb2) h.b2 = parameter;
        if (h.page == 0x10 && h.unlocked && command == 0xb3) h.b3 = parameter;
      }
      if (w & (1u << 31)) break;
    }
    if (l == 1 && underrunAfterDma && --underrunAfterDma == 0) {
      if (handleUnderrunIrq) irqUnderruns |= kUnderrunBits;
      else topStatus |= kUnderrunBits;
    }
    d.fifo.clear();
    d.intr |= 1u;
  }
  void trigger(unsigned l)
  {
    Dsi &d = dsi[l];
    const uint32_t ctrl = regs[dsiReg(l, kDsiCtrl)], tpg = regs[dsiReg(l, kDsiTpgCtrl)], dma = regs[dsiReg(l, kDsiCmdDmaCtrl)];
    if (!(ctrl & 1u) || !(ctrl & 4u) || tpg != 0x30006u || !(dma & (1u << 28)) || !regs[dsiReg(l, kDsiDmaLength)] || d.fifo.empty())
      return;   // not a FIFO transfer the engine takes: nothing happens
    if (dmaBudget[l] == 0) dmaHang[l] = true;
    if (dmaBudget[l] > 0) --dmaBudget[l];
    if (dmaHang[l] || d.videoStuck) {   // the engine took it and cannot finish without a reset
      d.dmaPending = true;
      d.dmaAt = UINT64_MAX;
      return;
    }
    const unsigned i = l + 1;
    const uint64_t duration = lateDmas ? 300000 : 20000;
    if (lateDmas) --lateDmas;
    d.dmaPending = true;
    if (!intf[i].on) {
      d.dmaAt = ns + duration;
      return;
    }
    const uint32_t ln = line(i);
    if (blockDma) {
      d.dmaAt = ln >= kVActiveEnd ? ns + duration
                                 : intf[i].start + uint64_t(kVActiveEnd + 1) * intf[i].htotal * kLineNs / kHtotal;
    } else {
      if (ln < kVActiveEnd) ++commandsInFrame;   // a command inside the active frame: the picture tears
      d.dmaAt = ns + duration;
    }
  }

  // ---- the Io
  uint32_t rd(uint64_t pa)
  {
    tick(150);
    for (unsigned i = 1; i <= 2; ++i) {
      if (pa == intfReg(i, kIntfStatus)) return intf[i].on ? 1u : 0u;
      if (pa == intfReg(i, kIntfFrameCount)) return intf[i].frames;
      if (pa == intfReg(i, kIntfLineCount)) return intf[i].counting ? line(i) : 0;
      if (pa == intfReg(i, kIntfVsyncPeriod)) return intf[i].shadow;
      if (pa == intfReg(i, kIntfEn)) return regs[pa];
    }
    if (pa == ctlReg(0, kCtlFlush)) return ctlFlush;
    if (pa == ctlReg(0, kCtlIntfFlush)) return ctlIntfFlush;
    if (pa == ctlReg(0, kCtlSwReset)) return ns < ctlResetDoneNs ? 1 : 0;
    for (unsigned d = 0; d < 4; ++d) {
      if (pa == ctlReg(0, kCtlDspp0Flush + 4 * d)) return dsppFlush[d];
      if (pa >= pccReg(d, 0) && pa <= pccReg(d, 0x30)) return pccShadow[d][(pa - pccReg(d, 0)) / 4];
    }
    if (pa == kMdp + kTopIntrStatus) return topStatus;
    for (unsigned l = 0; l < 2; ++l) {
      if (pa == dsiReg(l, kDsiTotal)) return dsi[l].db ? dsi[l].shadow : dsi[l].total;
      if (pa == dsiReg(l, kDsiIntrCtrl)) return dsi[l].intr;
      if (pa == dsiReg(l, kDsiStatus0)) {
        const bool video = (regs[dsiReg(l, kDsiCtrl)] & 2u) && intf[l + 1].on;
        return (dsi[l].dmaPending ? 2u : 0u) | (video || dsi[l].videoStuck ? 8u : 0u);
      }
      if (pa == dsiReg(l, kDsiTrigCtrl)) return 4u | (blockDma ? 1u << 12 : 0u);
      if (pa == dsiReg(l, dbModeWord())) return dsi[l].db ? 1u : 0u;
    }
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  uint32_t dbModeWord() const { return dbHw == DbHw::sunuefi ? kDsiShadow[2] : kDsiShadow[1]; }
  uint32_t dbFlushWord() const { return dbHw == DbHw::sunuefi ? kDsiShadow[1] : kDsiShadow[0]; }
  bool wr(uint64_t pa, uint32_t v, int panelCtl = -1)
  {
    tick(150);
    if (!storeAllowed(pa, v, modeWrites, panelCtl)) {
      refused.push_back(pa);
      return false;
    }
    stores.push_back({ns, pa, v, intf[1].frames, line(1)});
    if (dropCount && pa == dropStore && v == dropValue) {
      --dropCount;
      return true;
    }
    for (unsigned i = 1; i <= 2; ++i) {
      if (pa == intfReg(i, kIntfVsyncPeriod)) {
        intf[i].shadow = v;
        return true;
      }
      if (pa == intfReg(i, kIntfCountEn)) {
        if (!countEnableIgnored[i - 1]) {
          regs[pa] = v;
          intf[i].counting = (v & 3u) == 3u;
        }
        return true;
      }
      if (pa == intfReg(i, kIntfEn)) {
        regs[pa] = v;
        const bool gatedSlave = i == 2 && latchedMaster == kDsiIntfMaster;
        if (!v && intf[i].on && !engineStuck[i - 1] && !gatedSlave) intf[i].stopPending = true;
        if (v && !intf[i].on && !engineDead[i - 1]) {
          intf[i].on = true;
          intf[i].stopPending = false;
          intf[i].start = ns;
          startNs[i] = ns;
          const unsigned other = 3 - i;
          if (intf[other].on) {   // started beside a running engine: in phase with its frame start, or the halves tear
            const uint64_t phase = (ns - intf[other].start) % frameNs(other);
            if (phase > 4 * kLineNs && phase + 4 * kLineNs < frameNs(other)) ++desync;
            if (i == 2 && ns - intf[1].start < 4 * kLineNs) {   // the pair started together: one frame start, one latch
              latchCtl();
              for (unsigned k = 1; k <= 2; ++k) intf[k].cur = intf[k].period;
              intf[2].start = intf[1].start;
            }
          }
        }
        if (v) intf[i].stopPending = false;
        return true;
      }
    }
    if (pa == ctlReg(0, kCtlSwReset)) {
      if (intf[1].on || intf[2].on || ctlFlush) ++glitches;
      ctlResetDoneNs = ctlResetStuck ? UINT64_MAX : ns + 30000;
      return true;
    }
    if (pa == ctlReg(0, kCtlStart)) {
      if (!ctlResetDoneNs || ns < ctlResetDoneNs || !ctlFlush || flushStuck) return true;
      ctlResetDoneNs = 0;
      const uint32_t active = regs[ctlReg(0, kCtlIntfActive)];
      latchedMaster = regs[ctlReg(0, kCtlIntfMaster)];
      latchCtl();
      if (active == kDsiIntfSlave && !latchedMaster && regs[intfReg(2, kIntfEn)]) {
        intf[2].on = true;
        intf[2].start = ns;
      }
      return true;
    }
    if (pa == ctlReg(0, kCtlFlush)) {
      ctlFlush |= v;
      return true;
    }
    if (pa == ctlReg(0, kCtlIntfFlush)) {
      ctlIntfFlush = v;
      return true;
    }
    for (unsigned d = 0; d < 4; ++d) {
      if (pa == ctlReg(0, kCtlDspp0Flush + 4 * d)) {
        dsppFlush[d] = v;
        return true;
      }
      if (pa >= pccReg(d, 0) && pa <= pccReg(d, 0x30)) {
        pccShadow[d][(pa - pccReg(d, 0)) / 4] = v;
        return true;
      }
    }
    if (pa == kMdp + kTopIntrClear) {
      topStatus &= ~v;
      return true;
    }
    for (unsigned l = 0; l < 2; ++l) {
      Dsi &d = dsi[l];
      const unsigned i = l + 1;
      if (pa == dsiReg(l, kDsiSwReset) && v) {
        if (intf[1].on || intf[2].on || (regs[dsiReg(l, kDsiCtrl)] & 7u) ||
            (regs[dsiReg(l, kDsiClkCtrl)] & kDsiResetClocks) != kDsiResetClocks) {
          ++glitches;
          return true;
        }
        d.dmaPending = d.videoStuck = false;
        d.fifo.clear();
        d.intr &= ~1u;
      }
      if (pa == dsiReg(l, kDsiTotal)) {
        if (d.db) {
          d.shadow = v;
        } else {
          if (v != d.total && intf[i].on) {   // the video timing changes inside a frame
            ++midFrameTotal;
            topStatus |= 1u << kIntrIntfUnderrun[i];
          }
          d.total = d.shadow = v;
        }
        return true;
      }
      if (pa == dsiReg(l, dbModeWord())) {
        if (!v && d.armed) ++glitches;   // DB mode dropped before the latch
        d.db = v & 1u;
        return true;
      }
      if (pa == dsiReg(l, dbFlushWord())) {
        if (v & 1u && d.db) d.armed = true;
        return true;
      }
      if (pa == dsiReg(l, kDsiIntrCtrl)) {
        d.intr = (d.intr & ~(v & 1u)) | (v & ~1u);
        return true;
      }
      if (pa == dsiReg(l, kDsiTpgFifo)) {
        if (regs[dsiReg(l, kDsiTpgCtrl)] == 0x30006u) d.fifo.push_back(v);
        return true;
      }
      if (pa == dsiReg(l, kDsiTrigDma)) {
        if (v & 1u) trigger(l);
        return true;
      }
      if (dbHw == DbHw::downstream && pa == dsiReg(l, kDsiShadow[2])) {   // the TPG FIFO reset there
        if (v & 1u) d.fifo.clear();
        return true;
      }
    }
    regs[pa] = v;
    return true;
  }
  void delayUs(uint32_t us) { tick(uint64_t(us) * 1000); }
  void sleepMs(uint32_t ms) { tick(uint64_t(ms) * 1000000); }
  uint64_t pollDeadline(uint32_t us) const { return ns + uint64_t(us) * 1000; }
  void pollWait(uint64_t deadline, bool) { if (deadline > ns) tick(deadline - ns); }
  uint32_t modeUnderruns() { return (rd(kMdp + kTopIntrStatus) & kUnderrunBits) | irqUnderruns; }
  bool clearModeUnderruns()
  {
    irqUnderruns = 0;
    return wr(kMdp + kTopIntrClear, kUnderrunBits);
  }
  uint64_t nowUs() { return ns / 1000; }
  uintptr_t irqOff() { return irqDepth++; }
  void irqRestore(uintptr_t) { --irqDepth; }

  // ---- helpers for the tests
  unsigned storesTo(uint64_t pa) const
  {
    unsigned n = 0;
    for (const Store &s : stores) n += s.pa == pa;
    return n;
  }
  int firstStore(uint64_t pa, uint32_t v) const
  {
    for (size_t k = 0; k < stores.size(); ++k)
      if (stores[k].pa == pa && stores[k].v == v) return int(k);
    return -1;
  }
  const Dcs *firstDcs(uint8_t command, unsigned link) const
  {
    for (const Dcs &d : dcs)
      if (d.command == command && d.link == link) return &d;
    return nullptr;
  }
  bool anyStoreIn(uint64_t base, uint64_t bytes) const
  {
    for (const Store &s : stores)
      if (s.pa >= base && s.pa < base + bytes) return true;
    return false;
  }
};

// The inherited 120 Hz scanout with programmable fetch and the idle DSI command path.
inline Dpu modeSwitchFixture()
{
  Dpu io;
  io.modeWrites = true;
  io.blockDma = false;
  for (unsigned l = 0; l < 2; ++l) {
    const unsigned i = l + 1;
    io.regs[intfReg(i, kIntfConfig)] = 0x80800000;
    io.regs[intfReg(i, kIntfVsyncWidth)] = 0x480;
    io.regs[intfReg(i, kIntfDispVStart)] = 0xee80;
    io.regs[intfReg(i, kIntfDispVEnd)] = 0x13b47f;
    io.regs[intfReg(i, kIntfDispHctl)] = 0x01ca0020;
    io.intf[i].fetch = io.regs[intfReg(i, kIntfProgFetch)] = 0x13b6c1;
    io.intf[i].counting = false;
    io.regs[intfReg(i, kIntfCountEn)] = 0;
    io.regs[dsiReg(l, kDsiActiveH)] = 0x01cb0020;
    io.regs[dsiReg(l, kDsiActiveV)] = 0x08c2006a;
    io.regs[dsiReg(l, kDsiCtrl)] = 0x000001f7;
    io.regs[dsiReg(l, kDsiCmdDmaCtrl)] = 0x14000000;
    io.regs[dsiReg(l, kDsiDmaCtrl1)] = 0;
    io.dsi[l].intr = 0x00010000;
  }
  return io;
}

}  // namespace model
