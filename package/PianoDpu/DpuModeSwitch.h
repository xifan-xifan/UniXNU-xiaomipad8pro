// SPDX-License-Identifier: BSD-2-Clause-Patent
// M3: the verified mode transaction shared by the kernel slice and the timed host model.
#pragma once
#include "DpuModes.h"
#include "DpuPanel.h"
#include "DpuRefresh.h"

namespace pianodpu::modes {

using namespace slices;

template <class Io>
LinkLive readTiming(Io &io, const Link &link, unsigned l)
{
  const unsigned i = l + 1;
  return {
    link.hsyncCtl, link.period, io.rd(intfReg(i, kIntfVsyncWidth)),
    io.rd(intfReg(i, kIntfDispVStart)), io.rd(intfReg(i, kIntfDispVEnd)), io.rd(intfReg(i, kIntfDispHctl)),
    link.dsiTotal, io.rd(dsiReg(l, kDsiActiveH)), io.rd(dsiReg(l, kDsiActiveV)),
    liveProgFetch(link.config, io.rd(intfReg(i, kIntfProgFetch)))
  };
}

constexpr uint32_t shortWrite(uint8_t command, uint8_t value)
{
  return 0x00150000u | (uint32_t(value) << 8) | command;  // DCS short write with one parameter, virtual channel 0
}

template <class Io>
bool readCommandLink(Io &io, unsigned l, panel::LinkSaved &s)
{
  using namespace panel;
  s.ctrl = io.rd(dsiReg(l, kDsiCtrl));
  s.dma = io.rd(dsiReg(l, kDsiCmdDmaCtrl));
  s.dma1 = io.rd(dsiReg(l, kDsiDmaCtrl1));
  s.length = io.rd(dsiReg(l, kDsiDmaLength));
  s.intr = io.rd(dsiReg(l, kDsiIntrCtrl));
  s.trig = io.rd(dsiReg(l, kDsiTrigCtrl));
  s.ack = io.rd(dsiReg(l, kDsiAckErr));
  return (s.ctrl & (kCtrlEnable | kCtrlVideo)) == (kCtrlEnable | kCtrlVideo) &&
         (s.trig & kTrigDmaMask) == kTrigDmaSw && io.rd(dsiReg(l, kDsiTpgCtrl)) == 0 &&
         !(io.rd(dsiReg(l, kDsiStatus0)) & kStatusDmaBusy);
}

constexpr uint32_t lineNs(unsigned htotal) { return uint32_t(1000000000ull * htotal / kPixelClockHz); }

inline bool sameCommandConfig(const panel::LinkSaved &a, const panel::LinkSaved &b)
{
  return a.ctrl == b.ctrl && a.dma == b.dma && a.dma1 == b.dma1 && a.length == b.length && a.trig == b.trig &&
         (a.intr & panel::kIntEnables) == (b.intr & panel::kIntEnables);
}

struct Options {
  refresh::Db db = refresh::Db::sunuefi;
  bool boe = true;
  bool hwGatedAhb = false;
  bool panelUnavailable = false;
  bool allowUnqualified = false;  // host model exercise of all hardware timings
  unsigned fixedHz = kBootModeHz;
};

struct Result {
  unsigned from = 0;
  const char *why = nullptr;
  const char *restoreWhy = nullptr;
  bool reverted = false;
  bool timingTouched = false;
  int ctl = -1;
  uint32_t pendingFlush = 0;
  uint32_t underruns = 0, restoreTransitionUnderruns = 0;
  CounterInit counters;
  Rate rate;
};

template <class Log>
void logFailure(const Result &r, unsigned hz, Log log)
{
  if (r.pendingFlush) {
    log("mode: %u -> %u Hz: %s (CTL%d, CTL_FLUSH 0x%08x); refused", r.from, hz, r.why, r.ctl, r.pendingFlush);
  } else if (r.counters.sampled && !r.counters.moving) {
    const auto &c = r.counters;
    log("mode: %u -> %u Hz: %s (count-en %x/%x, frame deltas %d/%d, lines %u/%u -> %u/%u, line moved %u/%u); refused",
        r.from, hz, r.why, c.countEn[0], c.countEn[1], c.frameDelta[0], c.frameDelta[1], c.firstLine[0], c.firstLine[1],
        c.lastLine[0], c.lastLine[1], unsigned(c.lineMoved[0]), unsigned(c.lineMoved[1]));
  } else {
    log("mode: %u -> %u Hz: %s; %s%s", r.from, hz, r.why,
        !r.reverted ? "refused" : r.restoreWhy ? "restore failed: " : "old mode restored; switching disabled",
        r.restoreWhy ? r.restoreWhy : "");
  }
}

// Use one bonded FIFO transfer of four short packets. The final header owns the last-packet flag.
// Check both line counters again with local interrupts off, including when firmware has enabled the DMA block.
template <class Io>
const char *sendRateGroup(Io &io, const RateGroup &group, const LinkWords &timing, bool &transmitted, bool restoring = false)
{
  using namespace panel;
  constexpr unsigned lo = kVActiveEnd + 4, kLeadLines = 8, kTailLines = 24;
  transmitted = false;
  if (!group.count || group.count > sizeof group.w / sizeof group.w[0]) return "invalid DSI rate group";
  if (!timing.htotal || timing.htotal > UINT16_MAX || timing.vtotal < lo + kLeadLines + kTailLines)
    return "no common DSI front porch for the command";
  LinkSaved saved[2];
  const bool ready[] = {readCommandLink(io, 0, saved[0]), readCommandLink(io, 1, saved[1])};
  if ((!ready[0] || !ready[1]) && (!restoring || (!ready[0] && !ready[1])))
    return "DSI command path is not idle in software-triggered video mode";
  uint32_t packets[4];
  for (unsigned i = 0; i < group.count; ++i)
    packets[i] = shortWrite(group.w[i].command, group.w[i].param);
  packets[group.count - 1] |= 0x80000000u;
  const uint32_t bytes = group.count * sizeof(uint32_t);
  bool prepared = true;
  bool triggered[2] = {};
  bool done[2] = {};
  uint64_t blankStart = 0, blankUs = 0;
  for (unsigned l = 0; l < 2; ++l) {
    if (!ready[l]) continue;
    const auto &s = saved[l];
    const uint32_t dma = (s.dma & ~(kDmaBroadcast | kDmaMaster | kDmaLowPower)) | kDmaFromBuffer;
    prepared &= io.wr(dsiReg(l, kDsiIntrCtrl), (s.intr & kIntEnables) | kIntDmaDone);
    prepared &= io.wr(dsiReg(l, kDsiCtrl), s.ctrl | kCtrlCommand);
    prepared &= io.wr(dsiReg(l, kDsiTpgCtrl), kTpgFifo);
    prepared &= io.wr(dsiReg(l, kDsiCmdDmaCtrl), dma);
    prepared &= io.wr(dsiReg(l, kDsiDmaCtrl1), s.dma1 & ~kDmaCtrl1Bit);
    prepared &= io.wr(dsiReg(l, kDsiDmaLength), bytes);
    prepared &= io.rd(dsiReg(l, kDsiTpgCtrl)) == kTpgFifo &&
                io.rd(dsiReg(l, kDsiCmdDmaCtrl)) == dma &&
                io.rd(dsiReg(l, kDsiDmaCtrl1)) == (s.dma1 & ~kDmaCtrl1Bit) &&
                io.rd(dsiReg(l, kDsiDmaLength)) == bytes &&
                io.rd(dsiReg(l, kDsiCtrl)) == (s.ctrl | kCtrlCommand) &&
                !(io.rd(dsiReg(l, kDsiIntrCtrl)) & kIntDmaDone);
    if (prepared)
      for (unsigned i = 0; i < group.count; ++i) prepared &= io.wr(dsiReg(l, kDsiTpgFifo), packets[i]);
  }
  const char *why = prepared ? "no common DSI front porch for the command" : "DSI command setup failed read-back";
  if (prepared) {
    const unsigned vtotal = timing.vtotal, hi = vtotal - kTailLines;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      if (!waitLines(io, lo + kLeadLines, hi, vtotal, 55000, lineNs(timing.htotal))) break;
      const uintptr_t irq = io.irqOff();
      const unsigned a = io.rd(intfReg(1, kIntfLineCount)), b = io.rd(intfReg(2, kIntfLineCount));
      const bool blank = a >= lo && a <= hi && b >= lo && b <= hi;
      if (blank) {
        blankUs = uint64_t(vtotal - (a > b ? a : b) - 1) * lineNs(timing.htotal) / 1000;
        blankStart = io.nowUs();
        for (unsigned l = 0; l < 2; ++l)
          if (ready[l]) triggered[l] = io.wr(dsiReg(l, kDsiTrigDma), 1);
      }
      io.irqRestore(irq);
      if (blank) {
        transmitted = triggered[0] || triggered[1];
        why = triggered[0] && triggered[1] ? nullptr : "a DSI DMA trigger was refused";
        if (!ready[0] || !ready[1]) why = "panel restore incomplete: a DSI command engine stayed busy";
        break;
      }
      io.delayUs(25);
    }
  }
  const uint64_t start = io.nowUs();
  do {
    for (unsigned l = 0; l < 2; ++l)
      done[l] = !triggered[l] ||
                ((io.rd(dsiReg(l, kDsiIntrCtrl)) & kIntDmaDone) && !(io.rd(dsiReg(l, kDsiStatus0)) & kStatusDmaBusy));
    const uint64_t now = io.nowUs();
    if (done[0] && done[1]) {
      if (!why && now - blankStart >= blankUs) why = "DSI DMA completion was not observed inside the common front porch";
      break;
    }
    if (now - blankStart < blankUs && now - start < 250) io.delayUs(2);
    else io.sleepMs(1);
  } while (io.nowUs() - start < kDmaDoneTimeoutUs);
  for (unsigned l = 0; l < 2; ++l) {
    if (!ready[l]) continue;
    if (!done[l]) {
      why = "DSI rate-group DMA timed out";
      continue;  // the controller still owns the FIFO
    }
    const auto &s = saved[l];
    constexpr uint32_t kDsiErrorStatus = 1u << 24;
    if (triggered[l] && ((io.rd(dsiReg(l, kDsiAckErr)) & ~s.ack) ||
                         (io.rd(dsiReg(l, kDsiIntrCtrl)) & kDsiErrorStatus & ~s.intr)))
      why = "DSI rate-group command reported an error";
    bool restored = io.wr(dsiReg(l, kDsiIntrCtrl), (s.intr & kIntEnables) | kIntDmaDone);
    restored &= io.wr(dsiReg(l, kDsiTpgCtrl), 0);
    // Same FIFO cleanup as the SM8750 command path. The timing transaction has already released DB mode.
    restored &= io.wr(dsiReg(l, kDsiShadow[2]), 1);
    io.delayUs(1);
    restored &= io.wr(dsiReg(l, kDsiShadow[2]), 0);
    restored &= io.wr(dsiReg(l, kDsiCmdDmaCtrl), s.dma);
    restored &= io.wr(dsiReg(l, kDsiDmaCtrl1), s.dma1);
    restored &= io.wr(dsiReg(l, kDsiDmaLength), s.length);
    restored &= io.wr(dsiReg(l, kDsiCtrl), s.ctrl);
    LinkSaved after;
    if (!restored || io.rd(dsiReg(l, kDsiShadow[2])) != 0 ||
        !readCommandLink(io, l, after) || !sameCommandConfig(s, after))
      why = "DSI command restore failed read-back";
  }
  return why;
}

template <class Io>
const char *stageTiming(Io &io, const LivePath &p, const LinkWords from[2], const LinkWords to[2],
                        const refresh::DbWords &db, const LinkWords &window, bool &touched)
{
  const unsigned ctl = unsigned(p.ctl);
  if (!waitLines(io, 200, 1000, window.vtotal, 55000, lineNs(window.htotal))) return "INTF line counter did not reach the timing window";
  const uintptr_t irq = io.irqOff();
  const unsigned a = io.rd(intfReg(1, kIntfLineCount)), b = io.rd(intfReg(2, kIntfLineCount));
  if (a < 190 || a > 1010 || b < 190 || b > 1010) {
    io.irqRestore(irq);
    return "both INTFs did not stay inside the timing window";
  }

  touched = true;
  bool ok = true;
  for (unsigned l = 0; l < 2; ++l) {
    ok &= io.wr(dsiReg(l, db.mode), 1);
    ok &= io.rd(dsiReg(l, db.mode)) == 1;
    if (!ok) break;    // keep TOTAL out of the active bank if DB enable failed
    ok &= io.wr(dsiReg(l, kDsiTotal), to[l].dsiTotal);
    if (!ok) break;
    ok &= io.wr(dsiReg(l, db.flush), 1);
  }
  for (unsigned l = 0; l < 2 && ok; ++l) {
    const unsigned i = l + 1;
    const auto &old = from[l], &w = to[l];
    if (old.hsyncCtl != w.hsyncCtl) ok &= io.wr(intfReg(i, kIntfHsyncCtl), w.hsyncCtl);
    if (old.vsyncWidth != w.vsyncWidth) ok &= io.wr(intfReg(i, kIntfVsyncWidth), w.vsyncWidth);
    if (old.dispVStart != w.dispVStart) ok &= io.wr(intfReg(i, kIntfDispVStart), w.dispVStart);
    if (old.dispVEnd != w.dispVEnd) ok &= io.wr(intfReg(i, kIntfDispVEnd), w.dispVEnd);
    if (old.progFetch != w.progFetch) ok &= io.wr(intfReg(i, kIntfProgFetch), w.progFetch);
    if (old.period != w.period) ok &= io.wr(intfReg(i, kIntfVsyncPeriod), w.period);
  }
  if (ok) ok = io.wr(ctlReg(ctl, kCtlIntfFlush), kIntfFlushDsi) && io.wr(ctlReg(ctl, kCtlFlush), kFlushIntf);
  io.irqRestore(irq);
  if (!ok) return "a timing store or the DSI DB mode read-back failed";
  if (waitFlush(io, ctl, kFlushIntf, refresh::kLatchTimeoutUs) < 0) return "CTL did not latch the new timing";
  for (unsigned l = 0; l < 2; ++l) {
    ok &= io.wr(dsiReg(l, db.mode), 0);
    ok &= io.rd(dsiReg(l, db.mode)) == 0;
  }
  return ok ? nullptr : "DSI timing DB mode did not clear";
}

template <class Io>
const char *verifyTiming(Io &io, const LivePath &before, const LinkWords words[2], bool hwGatedAhb)
{
  const LivePath p = readPath(io, hwGatedAhb);
  if (!p.ok) return p.why;
  if (p.ctl != before.ctl || p.intfActive != before.intfActive || p.intfMaster != before.intfMaster)
    return "the bonded CTL path changed";
  for (unsigned l = 0; l < 2; ++l) {
    if (p.link[l].config != before.link[l].config || p.link[l].dsiCtrl != before.link[l].dsiCtrl ||
        p.link[l].dsiTrig != before.link[l].dsiTrig || io.rd(intfReg(l + 1, kIntfHsyncSkew)) != 0)
      return "an INTF or DSI control word changed";
    if (const char *diff = firstDifference(words[l], readTiming(io, p.link[l], l))) return diff;
  }
  return nullptr;
}

template <class Io>
const char *verifyRate(Io &io, unsigned hz, unsigned vtotal, Rate &rate, uint32_t *underruns = nullptr)
{
  rate = measureRate(io, 38);
  const uint64_t expected = hz * 1000ull, tolerance = expected / 50;
  if (!rate.lockstep || rate.milliHz + tolerance < expected || rate.milliHz > expected + tolerance)
    return "frame counters disagree with the target cadence";
  const uintptr_t irq = io.irqOff();
  const unsigned a = io.rd(intfReg(1, kIntfLineCount)), b = io.rd(intfReg(2, kIntfLineCount));
  io.irqRestore(irq);
  const unsigned phase = a >= b ? a - b : b - a;
  if (a >= vtotal || b >= vtotal || (phase > 12 && phase < vtotal - 12))
    return "INTF line counters are out of phase";
  const uint32_t pending = io.modeUnderruns();
  if (underruns) *underruns = pending;
  if (pending) return "an INTF underran";
  return nullptr;
}

template <class Io>
Result switchTiming(Io &io, unsigned hz, const Options &o)
{
  Result r;
  if (!o.allowUnqualified && hz != o.fixedHz) {
    r.why = "the requested mode is not qualified";
    return r;
  }
  if (o.panelUnavailable) {
    r.why = "the panel is off or its scanout is unavailable";
    return r;
  }
  const Mode *target = modeByHz(hz);
  const auto db = refresh::dbWords(o.db);
  if (!target || !db.mode) {
    r.why = "unsupported mode or DSI shadow layout";
    return r;
  }
  LivePath p = readPath(io, o.hwGatedAhb);
  if (!p.ok || p.intfActive != kIntfFlushDsi || p.intfMaster != 2) {
    r.why = p.ok ? "unexpected bonded CTL interface routing" : p.why;
    return r;
  }
  LinkWords before[2], after[2];
  for (unsigned l = 0; l < 2; ++l) {
    const LinkLive live = readTiming(io, p.link[l], l);
    const Geometry g = geometry(live, io.rd(intfReg(l + 1, kIntfHsyncSkew)));
    if (!g.ok || g.skew) {
      r.why = !g.ok ? g.why : "unexpected INTF timing skew";
      return r;
    }
    for (const Mode &m : kModes) {
      const auto w = plan(g, m);
      if (w.pixelClockHz == kPixelClockHz && !firstDifference(w, live)) before[l] = w;
    }
    after[l] = plan(g, *target);
    if (!before[l].hz || (l && before[l].hz != before[0].hz) || after[l].pixelClockHz != kPixelClockHz ||
        ((p.link[l].config & kIntfConfigProgFetchEn) && !g.fetchLines)) {
      r.why = "live timing or programmable fetch is outside the panel's mode family";
      return r;
    }
    if (io.rd(dsiReg(l, db.mode)) != 0) {
      r.why = "DSI timing DB mode is already in use";
      return r;
    }
  }
  r.from = before[0].hz;
  if (!o.allowUnqualified && r.from != kInheritedModeHz && r.from != kBootModeHz) {
    r.why = "the inherited mode is not qualified for the boot switch";
    return r;
  }
  const Mode &previous = *modeByHz(r.from);
  const bool groupChanges = !sameGroup(previous, *target);
  panel::LinkSaved saved[2];
  if (groupChanges && (!readCommandLink(io, 0, saved[0]) || !readCommandLink(io, 1, saved[1]))) {
    r.why = "DSI command path is not idle in software-triggered video mode";
    return r;
  }
  r.ctl = p.ctl;
  r.pendingFlush = io.rd(ctlReg(unsigned(p.ctl), kCtlFlush));
  if (r.pendingFlush) {
    r.why = "CTL flush is pending";
    return r;
  }
  r.counters = initCounters(io, p);
  if (!r.counters.moving) {
    r.why = "INTF counters not advancing in lockstep";
    return r;
  }
  if (hz == kHighModeHz && !o.allowUnqualified && !io.voteModeBandwidth(*target)) {
    r.why = "the 144 Hz MM0 bandwidth vote did not complete";
    return r;
  }
  if (r.from == hz) {
    r.why = verifyRate(io, hz, before[0].vtotal, r.rate, &r.underruns);
    return r;
  }
  if (!io.clearModeUnderruns()) {
    r.why = "could not clear stale INTF underrun status";
    return r;
  }
  bool commandTriggered = false;
  r.why = stageTiming(io, p, before, after, db, before[0], r.timingTouched);
  if (!r.why) r.why = verifyTiming(io, p, after, o.hwGatedAhb);
  if (!r.why && groupChanges)
    r.why = sendRateGroup(io, rateGroup(*target, o.boe), after[0], commandTriggered);
  if (!r.why) r.why = verifyRate(io, hz, after[0].vtotal, r.rate, &r.underruns);
  if (!r.why) r.why = verifyTiming(io, p, after, o.hwGatedAhb);
  if (!r.why || !r.timingTouched) return r;

  r.reverted = true;
  const auto recovery = readPath(io, o.hwGatedAhb);
  const auto &clocks = recovery.clocks;
  if (!clocks.readable || !branchOn(clocks.byte[0]) || !branchOn(clocks.byte[1]) ||
      !branchOn(clocks.pclk[0]) || !branchOn(clocks.pclk[1])) {
    r.restoreWhy = "the display clock gate closed during the switch";
    return r;
  }
  if (recovery.ctl != p.ctl || recovery.intfActive != p.intfActive || recovery.intfMaster != p.intfMaster ||
      !recovery.link[0].engine() || !recovery.link[1].engine()) {
    r.restoreWhy = "the bonded CTL path changed during the switch";
    return r;
  }
  const auto &window = recovery.link[0].period == before[0].period ? before[0] : after[0];
  r.restoreWhy = stageTiming(io, p, after, before, db, window, r.timingTouched);
  // If the command was triggered, a partial transfer may have changed either panel half.
  // Restore the panel group after the old timing has latched, even when some read-back words are still wrong.
  if (!r.restoreWhy && commandTriggered) {
    bool restoredCommand = false;
    r.restoreWhy = sendRateGroup(io, rateGroup(previous, o.boe), before[0], restoredCommand, true);
  }
  if (!r.restoreWhy) r.restoreWhy = verifyTiming(io, p, before, o.hwGatedAhb);
  if (!r.restoreWhy && groupChanges) {
    for (unsigned l = 0; l < 2; ++l) {
      panel::LinkSaved restored;
      if (!readCommandLink(io, l, restored) || !sameCommandConfig(saved[l], restored))
        r.restoreWhy = "DSI command path was not restored";
    }
  }
  if (r.restoreWhy) return r;

  // The failed mode may still underrun up to the old timing latch and panel command.
  // Start the recovery observation only after old timing and command state read back.
  r.restoreTransitionUnderruns = io.modeUnderruns();
  if (!io.clearModeUnderruns()) {
    r.restoreWhy = "could not clear INTF underrun status on the restored timing";
    return r;
  }
  Rate restoredRate;
  r.restoreWhy = verifyRate(io, r.from, before[0].vtotal, restoredRate);
  if (!r.restoreWhy) r.restoreWhy = verifyTiming(io, p, before, o.hwGatedAhb);
  return r;
}

// Called under the same transaction lock as flips. Invalidate WC verification after any timing stores,
// including an attempted switch that rolled back. Replay colour when either mode has been verified.
template <class Io, class Invalidate, class Replay>
Result switchTo(Io &io, unsigned hz, const Options &o, Invalidate invalidate, Replay replay)
{
  const Result r = switchTiming(io, hz, o);
  if (r.timingTouched) {
    invalidate();
    if (!r.restoreWhy) replay();
  }
  return r;
}

}  // namespace pianodpu::modes
