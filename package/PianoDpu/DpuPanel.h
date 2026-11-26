// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1e: the panel's light sleep with the display (docs/drivers/dpu.md §4.5), portable (tests/dpu_panel_host.cpp).
//
// Off (after PianoBacklight turned the backlight off on the same display-power change): DCS display-off (0x28), 20 ms, sleep-in
// (0x10), 100 ms, then stop INTF1 and hand off the CTL to INTF2 before stopping INTF2 (Ubuntu 1.4.0, K:1fead1793340).
// Reset the CTL only after both effective engines are quiescent. Keep the pixel resources bound through the slave's final frame
// and clear programmable fetch after both engines stop. The PHYs, panel reset and supplies retain the boot configuration.
// On: with both INTFs stopped, reset both DSI hosts to clear the old video FSM, send DCS sleep-out (0x11), 120 ms, display-on
// (0x29), 10 ms in command mode, then restore video. Restore fetch and the bonded route, latch the shared CTL flush, then start
// both INTFs and verify lockstep. The caller re-applies PCC and lights the backlight.
// While timing runs, DCS commands use vertical blanking (the DMA reads the
// controller's own command FIFO, so no buffer has to be reachable by the DSI's DMA through the SMMU), on both links, one trigger each
// (DSI0 then DSI1, back to back, as msm sends a bonded pair's commands). Per link, from msm's dsi_host.c (the video-mode transfer:
// command mode enabled beside video for the transfer, the DMA's low-power bit as the panel's MIPI_DSI_MODE_LPM asks, the done bit)
// and UniXNU-sm8750-common's PeripheralIO.h (BSD-2-Clause-Patent: the FIFO route on SM8750, its words and its restore):
//   - refuse unless CTRL has enable + video, TEST_PATTERN_GEN_CTRL is 0 (the FIFO free) and TRIG_CTRL's DMA trigger is software
//     (4); wait until STATUS0's command-DMA busy bit (1) is clear (20 ms);
//   - INTR_CTRL = its enables (odd bits) | 1 (the done bit is write-one-to-clear); CTRL |= command mode (bit 2);
//     TEST_PATTERN_GEN_CTRL = 0x30006 (CMD_DMA_TPG_EN, TPG_DMA_FIFO_MODE, pattern 3: the DMA FIFO); the packet's words into the
//     FIFO (+0x17c), an odd count padded with a zero word; CMD_DMA_CTRL: broadcast (31) and master (30) cleared, FROM_FRAME_BUFFER
//     (28) set, LOW_POWER (26) set for LP; the second DMA control word (+0x390) bit 1 cleared; DMA length = the packet's bytes;
//   - TRIG_DMA = 1 on DSI0 and DSI1; in video, when TRIG_CTRL's BLOCK_DMA_WITHIN_FRAME (bit 12) is clear the trigger waits for INTF1's line
//     counter in the front porch first; INTR_CTRL bit 0 on both within 50 ms;
//   - restore: INTR_CTRL enables | 1, TEST_PATTERN_GEN_CTRL = 0, +0x1ec pulsed 1 then 0 (PeripheralIO's FIFO reset; SunUEFI's
//     DSI timing DB mode word, idle here either way), CMD_DMA_CTRL, +0x390, the length and CTRL as found. A link whose DMA did not
//     finish still owns the command. A stopped-host wake can abort it with a controller reset before restoring those words.
// A DCS short write without a parameter: data type 0x05, header word = last-command flag (bit 31) | type << 16 | command.
#pragma once
#include "DpuPanelTrace.h"

namespace pianodpu {
namespace panel {
using namespace slices;

constexpr uint8_t kDcsDisplayOff = 0x28, kDcsSleepIn = 0x10, kDcsSleepOut = 0x11, kDcsDisplayOn = 0x29;
constexpr uint32_t kCtrlEnable = 1u, kCtrlVideo = 1u << 1, kCtrlCommand = 1u << 2;
constexpr uint32_t kStatusDmaBusy = 1u << 1, kStatusVideoBusy = 1u << 3;
constexpr uint32_t kCtrlRun = kCtrlEnable | kCtrlVideo | kCtrlCommand;
constexpr uint32_t kIntDmaDone = 1u, kIntEnables = 0xa222aa02u;   // the odd (enable) bits PeripheralIO keeps; bit 0 = CMD_DMA_DONE
constexpr uint32_t kTpgFifo = 0x30006u;
constexpr uint32_t kDmaBroadcast = 1u << 31, kDmaMaster = 1u << 30, kDmaFromBuffer = 1u << 28, kDmaLowPower = 1u << 26;
constexpr uint32_t kTrigDmaMask = 7u, kTrigDmaSw = 4u, kTrigBlockDmaInFrame = 1u << 12;
constexpr uint32_t kDmaCtrl1Bit = 1u << 1;
constexpr uint32_t kDmaDoneTimeoutUs = 50000, kDmaBusyTimeoutUs = 20000, kEngineStopUs = 40000, kEngineStartUs = 50000;
constexpr uint32_t kPanelReplyTimeoutMs = 1500;

// Owned by the request lock. If a dispatched transition failed, the caller may still have a wake to complete.
struct RequestGate {
  bool ready = false;
  bool wakePending = false;
  bool fallback = false;

  void latchFallback()
  {
    ready = wakePending = false;
    fallback = true;
  }

  void sleepTimedOut(int &queued)
  {
    queued = -1;   // the worker takes and clears it under the same lock
    latchFallback();
  }

  bool undoLateSleep(int dispatched, bool asleep) const { return fallback && dispatched == 0 && asleep; }

  template <class Log>
  bool admit(bool on, Log log) const
  {
    if (fallback) return false;
    if (ready || (on && wakePending)) return true;
    log(on ? "panel: wake refused: panel sleep unavailable" : "panel: sleep refused: panel sleep unavailable");
    return false;
  }
};

inline bool dcsAllowed(uint8_t c) { return c == kDcsDisplayOff || c == kDcsSleepIn || c == kDcsSleepOut || c == kDcsDisplayOn; }
struct Packet {
  uint32_t words[2] = {0, 0};
  unsigned count = 0;
  uint32_t bytes = 0;
};
inline Packet dcsShort(uint8_t command)
{
  Packet p;
  p.words[0] = 0x80000000u | (0x05u << 16) | command;
  p.words[1] = 0;   // the odd count's pad word
  p.count = 2;
  p.bytes = 4;
  return p;
}

struct Options {
  bool lowPower = true;   // the panel's MIPI_DSI_MODE_LPM (piano-dpu-panel-hs=1: high speed)
  bool engines = true;    // stop the timing engines (piano-dpu-panel-engines=0: DCS only)
  uint32_t displayOffMs = 20, sleepInMs = 100, sleepOutMs = 120, displayOnMs = 10;
};

struct LinkSaved {
  uint32_t ctrl = 0, dma = 0, dma1 = 0, length = 0, intr = 0, tpg = 0, trig = 0, status = 0, fifo = 0, ack = 0;
  uint32_t lane = 0, clocks = 0;
};
struct Sent {
  bool ok = false;
  const char *why = "";
  uint8_t command = 0;
  bool done[2] = {false, false};
  uint32_t doneUs = 0;          // trigger -> both done
  bool waitedBlanking = false;  // TRIG_CTRL had no BLOCK_DMA_WITHIN_FRAME: triggered in the front porch
  unsigned stores = 0, refused = 0;
  LinkSaved before[2];
  uint32_t ackAfter[2] = {0, 0}, fifoAfter[2] = {0, 0};
  uint32_t intrPoll[2] = {};
  unsigned ackBeforeMask = 0, ackAfterMask = 0;
  bool triggerAttempted = false;

  unsigned dmaDoneMask() const
  {
    return triggerAttempted ? unsigned(done[0]) | (unsigned(done[1]) << 1) : 0;
  }
};

template <class Io, class Write>
void restoreDcsLink(Io &io, unsigned l, const LinkSaved &k, bool clearFifo, Write wr)
{
  wr(dsiReg(l, kDsiIntrCtrl), (k.intr & kIntEnables) | kIntDmaDone);
  wr(dsiReg(l, kDsiTpgCtrl), 0);
  if (clearFifo) {
    wr(dsiReg(l, kDsiShadow[2]), 1);
    io.delayUs(1);
    wr(dsiReg(l, kDsiShadow[2]), 0);
  }
  wr(dsiReg(l, kDsiCmdDmaCtrl), k.dma);
  wr(dsiReg(l, kDsiDmaCtrl1), k.dma1);
  wr(dsiReg(l, kDsiDmaLength), k.length);
  wr(dsiReg(l, kDsiCtrl), k.ctrl);
}

template <class Io>
Sent sendDcs(Io &io, uint8_t command, const Options &o, bool commandOnly = false)
{
  Sent s;
  s.command = command;
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++s.stores;
    if (!io.wr(pa, v)) ++s.refused;
  };
  if (!dcsAllowed(command)) {
    s.why = "not one of the sleep commands (0x28 0x10 0x11 0x29)";
    return s;
  }
  for (unsigned l = 0; l < 2; ++l) {
    LinkSaved &k = s.before[l];
    k.ctrl = io.rd(dsiReg(l, kDsiCtrl));
    k.dma = io.rd(dsiReg(l, kDsiCmdDmaCtrl));
    k.dma1 = io.rd(dsiReg(l, kDsiDmaCtrl1));
    k.length = io.rd(dsiReg(l, kDsiDmaLength));
    k.intr = io.rd(dsiReg(l, kDsiIntrCtrl));
    k.tpg = io.rd(dsiReg(l, kDsiTpgCtrl));
    k.trig = io.rd(dsiReg(l, kDsiTrigCtrl));
    k.fifo = io.rd(dsiReg(l, kDsiFifoStatus));
    k.ack = io.rd(dsiReg(l, kDsiAckErr));
    k.lane = io.rd(dsiReg(l, kDsiLaneStatus));
    k.clocks = io.rd(dsiReg(l, kDsiClkStatus));
    s.ackBeforeMask |= 1u << l;
    if (commandOnly && (k.ctrl & kCtrlRun) != (kCtrlEnable | kCtrlCommand)) s.why = "a DSI host is not in command-only mode";
    else if (!commandOnly && (k.ctrl & (kCtrlEnable | kCtrlVideo)) != (kCtrlEnable | kCtrlVideo)) s.why = "a DSI controller is not enabled in video mode";
    else if (k.tpg) s.why = "a DSI controller's test-pattern / command FIFO is in use";
    else if ((k.trig & kTrigDmaMask) != kTrigDmaSw) s.why = "a DSI controller's command DMA is not software-triggered (TRIG_CTRL)";
    if (*s.why) return s;
  }
  const uint64_t busyEnd = io.nowUs() + kDmaBusyTimeoutUs;
  const uint32_t busy = kStatusDmaBusy | (commandOnly ? kStatusVideoBusy : 0);
  for (unsigned l = 0; l < 2; ++l)
    while ((s.before[l].status = io.rd(dsiReg(l, kDsiStatus0))) & busy) {
      if (io.nowUs() >= busyEnd) {
        s.why = "a DSI host stayed busy for 20 ms (nothing written)";
        return s;
      }
      io.delayUs(100);
    }
  const Packet pk = dcsShort(command);
  for (unsigned l = 0; l < 2; ++l) {
    const LinkSaved &k = s.before[l];
    wr(dsiReg(l, kDsiIntrCtrl), (k.intr & kIntEnables) | kIntDmaDone);
    wr(dsiReg(l, kDsiCtrl), k.ctrl | kCtrlCommand);
    wr(dsiReg(l, kDsiTpgCtrl), kTpgFifo);
    for (unsigned w = 0; w < pk.count; ++w) wr(dsiReg(l, kDsiTpgFifo), pk.words[w]);
    wr(dsiReg(l, kDsiCmdDmaCtrl), (k.dma & ~(kDmaBroadcast | kDmaMaster | kDmaLowPower)) | kDmaFromBuffer | (o.lowPower ? kDmaLowPower : 0));
    wr(dsiReg(l, kDsiDmaCtrl1), k.dma1 & ~kDmaCtrl1Bit);
    wr(dsiReg(l, kDsiDmaLength), pk.bytes);
  }
  if (s.refused) {
    s.why = "the store guard refused a store";
    return s;
  }
  // Without the hardware's block, trigger in the front porch (INTF1's lines 2242 .. vtotal - 8).
  if (!commandOnly && (!(s.before[0].trig & kTrigBlockDmaInFrame) || !(s.before[1].trig & kTrigBlockDmaInFrame))) {
    const uint32_t ht = io.rd(intfReg(1, kIntfHsyncCtl)) >> 16;
    const uint32_t vt = ht ? io.rd(intfReg(1, kIntfVsyncPeriod)) / ht : 0;
    s.waitedBlanking = true;
    if (vt <= kVActiveEnd + 16 || !waitLines(io, kVActiveEnd + 2, vt - 8, vt, kDmaDoneTimeoutUs)) {
      s.why = "INTF1's line counter never reached the front porch";
      // nothing triggered: restore below as after a finished transfer
      s.done[0] = s.done[1] = true;
    }
  }
  const uint64_t t0 = io.nowUs();
  if (!*s.why) {
    s.triggerAttempted = true;
    const uintptr_t irq = commandOnly ? 0 : io.irqOff();
    wr(dsiReg(0, kDsiTrigDma), 1);
    wr(dsiReg(1, kDsiTrigDma), 1);
    if (!commandOnly) io.irqRestore(irq);
    for (;;) {
      for (unsigned l = 0; l < 2; ++l) {
        if (s.done[l]) continue;
        s.intrPoll[l] = io.rd(dsiReg(l, kDsiIntrCtrl));
        s.done[l] = (s.intrPoll[l] & kIntDmaDone) != 0;
      }
      if (s.done[0] && s.done[1]) break;
      if (io.nowUs() - t0 >= kDmaDoneTimeoutUs) break;
      io.sleepMs(1);
    }
    s.doneUs = uint32_t(io.nowUs() - t0);
  }
  for (unsigned l = 0; l < 2; ++l) {
    if (!s.done[l]) continue;   // still the engine's: not touched
    restoreDcsLink(io, l, s.before[l], true, wr);
    s.ackAfter[l] = io.rd(dsiReg(l, kDsiAckErr));
    s.fifoAfter[l] = io.rd(dsiReg(l, kDsiFifoStatus));
    s.ackAfterMask |= 1u << l;
  }
  if (*s.why) return s;
  if (!s.done[0] || !s.done[1]) s.why = s.done[0] ? "DSI1's command DMA did not finish within 50 ms" : "DSI0's command DMA did not finish within 50 ms";
  else if (s.refused) s.why = "the store guard refused a store";
  else {
    s.ok = true;
    s.why = "ok";
  }
  return s;
}

// --------------------------------------------------------------------------------------------------------- the engines
struct Engines {
  bool ok = false;
  const char *why = "";
  int32_t us[2] = {-1, -1};   // stop/start time per INTF (index 0 = INTF1)
  uint32_t status[2] = {0, 0};
  Rate rate;                   // after a start
  int32_t latchUs = -1;
  unsigned stores = 0, refused = 0;
};

struct Fetch {
  uint32_t config[2] = {};
  uint32_t start[2] = {};
};

template <class Io>
bool programFetch(Io &io, const Fetch &fetch, bool enable, Engines &e)
{
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++e.stores;
    if (!io.wr(pa, v)) ++e.refused;
  };
  for (unsigned i = 0; i < 2; ++i) {
    if (!(fetch.config[i] & kIntfConfigProgFetch)) continue;
    const uint32_t config = enable ? fetch.config[i] : fetch.config[i] & ~kIntfConfigProgFetch;
    if (enable) wr(intfReg(i + 1, kIntfProgFetch), fetch.start[i]);
    wr(intfReg(i + 1, kIntfConfig), config);
    if (io.rd(intfReg(i + 1, kIntfConfig)) != config ||
        (enable && io.rd(intfReg(i + 1, kIntfProgFetch)) != fetch.start[i])) return false;
  }
  return !e.refused;
}
// Stop one engine. With INTF1 selected as shared master, its final frame
// must also retire on INTF2 before the CTL can be reset.
template <class Io>
int32_t stopEngine(Io &io, unsigned intf, Engines &e, const Trace &trace)
{
  ++e.stores;
  if (!io.wr(intfReg(intf, kIntfEn), 0)) {
    ++e.refused;
    return -1;
  }
  const uint64_t t0 = io.nowUs();
  for (;;) {
    const uint64_t pa = intfReg(intf, kIntfStatus);
    const uint32_t st = e.status[intf - 1] = io.rd(pa);
    PollRead slave;
    if (intf == 1) slave = {intfReg(2, kIntfStatus), e.status[1] = io.rd(intfReg(2, kIntfStatus))};
    const bool stopped = !((st | slave.value) & kIntfStatusOn) && io.rd(intfReg(intf, kIntfEn)) == 0;
    const uint64_t elapsed = io.nowUs() - t0;
    if (stopped || elapsed >= kEngineStopUs) {
      trace.sample(io, {pa, st}, slave);
      return stopped ? int32_t(elapsed) : -1;
    }
    trace.pause(io, 1000, true, {pa, st}, slave);
  }
}

template <class Io>
bool timingQuiescent(Io &io)
{
  uint32_t frame[2], line[2];
  for (unsigned i = 1; i <= 2; ++i) {
    if (io.rd(intfReg(i, kIntfStatus)) & kIntfStatusOn) return false;
    frame[i - 1] = io.rd(intfReg(i, kIntfFrameCount));
    line[i - 1] = io.rd(intfReg(i, kIntfLineCount));
    if (frame[i - 1] == UINT32_MAX || line[i - 1] == UINT32_MAX) return false;
  }
  io.delayUs(100);
  for (unsigned i = 1; i <= 2; ++i)
    if ((io.rd(intfReg(i, kIntfStatus)) & kIntfStatusOn) ||
        io.rd(intfReg(i, kIntfFrameCount)) != frame[i - 1] ||
        io.rd(intfReg(i, kIntfLineCount)) != line[i - 1]) return false;
  return true;
}

template <class Io>
bool panelStore(Io &io, const LivePath &p, uint32_t offset, uint32_t value, Engines &e)
{
  ++e.stores;
  if (io.wr(ctlReg(unsigned(p.ctl), offset), value, p.ctl)) return true;
  ++e.refused;
  return false;
}

template <class Io>
bool resetPanelCtl(Io &io, const LivePath &p, Engines &e, Trace &trace)
{
  const unsigned ctl = unsigned(p.ctl);
  const uint64_t reset = ctlReg(ctl, kCtlSwReset);
  trace.boundary(io, TraceStep::ctlReset);
  if (io.rd(ctlReg(ctl, kCtlFlush)) || (io.rd(reset) & 1u) || !timingQuiescent(io) ||
      !panelStore(io, p, kCtlSwReset, 1, e)) return false;
  const uint64_t end = io.nowUs() + 2000;  // the reference CTL soft reset bound
  do {
    if (!(io.rd(reset) & 1u)) {
      trace.sample(io);
      return io.rd(ctlReg(ctl, kCtlTop)) == p.ctlTop && (io.rd(ctlReg(ctl, kCtlLayerActive)) & 0xffu) == p.lms;
    }
    trace.pause(io, 50, false);
  } while (io.nowUs() < end);
  trace.sample(io);
  return false;
}

template <class Io>
int32_t submitPanelRoute(Io &io, const LivePath &p, uint32_t active, uint32_t master, Engines &e, Trace &trace)
{
  const unsigned ctl = unsigned(p.ctl);
  const uint64_t flush = ctlReg(ctl, kCtlFlush), intfFlush = ctlReg(ctl, kCtlIntfFlush);
  if (!panelStore(io, p, kCtlIntfActive, active, e) || !panelStore(io, p, kCtlIntfMaster, master, e) ||
      io.rd(ctlReg(ctl, kCtlIntfActive)) != active || io.rd(ctlReg(ctl, kCtlIntfMaster)) != master ||
      !panelStore(io, p, kCtlIntfFlush, kIntfFlushDsi, e) || !panelStore(io, p, kCtlFlush, kFlushIntf, e) ||
      io.rd(intfFlush) != kIntfFlushDsi || io.rd(flush) != kFlushIntf) return -1;
  trace.sample(io);
  if (!panelStore(io, p, kCtlStart, 1, e)) return -1;
  const int32_t latched = waitFlush(io, ctl, UINT32_MAX, kEngineStopUs, trace);
  if (latched < 0 || io.rd(ctlReg(ctl, kCtlIntfActive)) != active || io.rd(ctlReg(ctl, kCtlIntfMaster)) != master) return -1;
  return latched;
}

// Both on again: the CTL's INTF flush first, then INTF1 and INTF2 with nothing in between; running and in lockstep. After a failed
// stop one engine may still run: INTF2 alone is started at INTF1's frame start (its line counter wrapping), so the halves stay in
// phase; INTF1 alone (INTF2 running) cannot be aligned that way and is reported.
template <class Io>
Engines startEngines(Io &io, const LivePath &p, const Fetch &fetch, Trace &trace)
{
  Engines e;
  const unsigned ctl = unsigned(p.ctl);
  const uint32_t active = io.rd(ctlReg(ctl, kCtlIntfActive)), master = io.rd(ctlReg(ctl, kCtlIntfMaster));
  const bool handedOff = active == kDsiIntfSlave && master == 0;
  if ((!handedOff && (active != p.intfActive || master != p.intfMaster)) || io.rd(ctlReg(ctl, kCtlTop)) != p.ctlTop ||
      (io.rd(ctlReg(ctl, kCtlSwReset)) & 1u) || io.rd(ctlReg(ctl, kCtlFlush))) {
    e.why = "the shared CTL is not ready to restart";
    return e;
  }
  if (handedOff && (io.rd(intfReg(1, kIntfEn)) || io.rd(intfReg(2, kIntfEn)) || !resetPanelCtl(io, p, e, trace))) {
    e.why = "could not reset the stopped CTL for the bonded wake";
    return e;
  }
  trace.boundary(io, TraceStep::fetchOn);
  if (!programFetch(io, fetch, true, e)) {
    e.why = "could not restore programmable fetch";
    return e;
  }
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++e.stores;
    if (!io.wr(pa, v)) ++e.refused;
  };
  if (handedOff) {
    trace.boundary(io, TraceStep::routeOn);
    e.latchUs = submitPanelRoute(io, p, p.intfActive, p.intfMaster, e, trace);
    if (e.latchUs < 0) {
      e.why = "the bonded wake route did not flush";
      return e;
    }
  } else {
    wr(ctlReg(ctl, kCtlIntfFlush), kIntfFlushDsi);
    wr(ctlReg(ctl, kCtlFlush), kFlushIntf);
    if (e.refused) {
      e.why = "the store guard refused the INTF wake flush";
      return e;
    }
  }
  trace.boundary(io, TraceStep::enable);
  const bool on1 = io.rd(intfReg(1, kIntfStatus)) & kIntfStatusOn, on2 = io.rd(intfReg(2, kIntfStatus)) & kIntfStatusOn;
  if ((!on1 && on2) || (handedOff && on1)) {
    e.why = "a timing engine is running outside the bonded wake sequence";
    return e;
  }
  const char *phase = nullptr;
  if (on1 && !on2) {
    const unsigned vt = p.link[0].vtotal ? p.link[0].vtotal : kVtotal120;
    trace.step = TraceStep::align;
    if (!waitLines(io, 0, 2, vt, 3 * vt * kLineNs / 1000, kLineNs, trace))
      phase = "INTF1's line counter did not wrap: INTF2 started out of phase";
    const uintptr_t irq = io.irqOff();
    wr(intfReg(2, kIntfEn), 1);
    io.irqRestore(irq);
    trace.sample(io);   // review-y12o 29a9ce72: record the line at the actual enable store
  } else if (!on1 && !on2) {
    const uintptr_t irq = io.irqOff();
    wr(intfReg(1, kIntfEn), 1);
    wr(intfReg(2, kIntfEn), 1);
    io.irqRestore(irq);
  } else {   // both still run (a stop that did not happen): cancel a pending EN = 0
    for (unsigned i = 1; i <= 2; ++i)
      if (!(io.rd(intfReg(i, kIntfEn)) & 1u)) wr(intfReg(i, kIntfEn), 1);
  }
  const uint64_t t0 = io.nowUs();
  trace.step = TraceStep::start;
  bool on = false;
  while (!on && io.nowUs() - t0 < kEngineStartUs) {
    on = true;
    for (unsigned i = 0; i < 2; ++i) {
      e.status[i] = io.rd(intfReg(i + 1, kIntfStatus));
      on &= (e.status[i] & kIntfStatusOn) != 0;
    }
    const PollRead a{intfReg(1, kIntfStatus), e.status[0]}, b{intfReg(2, kIntfStatus), e.status[1]};
    if (on) {
      e.us[0] = e.us[1] = int32_t(io.nowUs() - t0);
      trace.sample(io, a, b);
    } else {
      trace.pause(io, 1000, true, a, b);
    }
  }
  if (!handedOff) {
    trace.step = TraceStep::flush;
    e.latchUs = waitFlush(io, ctl, kFlushIntf, 60000, trace);
  }
  if (on) {
    trace.step = TraceStep::rate;
    e.rate = measureRate(io, 100, trace);
  }
  if (e.refused) e.why = "the store guard refused a store";
  else if (!on) e.why = "a timing engine did not report running within 50 ms";
  else if (e.latchUs < 0) e.why = "the INTF wake flush did not latch";
  else if (io.rd(intfReg(1, kIntfEn)) != 1 || io.rd(intfReg(2, kIntfEn)) != 1) e.why = "both INTF enable requests did not read back";
  else if (!e.rate.milliHz) e.why = "the frame counter did not move after the start";
  else if (!e.rate.lockstep) e.why = "INTF2 does not count INTF1's frames (the halves are out of step)";
  else if (phase) e.why = phase;
  else {
    e.ok = true;
    e.why = "ok";
  }
  return e;
}

// ----------------------------------------------------------------------------------------------------- off and on
struct Result {
  bool ok = false;
  const char *why = "";
  unsigned step = 0;         // the step reached (failed in, when !ok)
  bool restored = false;     // a failure undone: the panel shows again
  bool safeAwake = false;    // refusal before DCS reached either panel link
  bool scanoutRestored = false; // bonded video can run even if the panel did not accept wake
  bool enginesStopped = false;
  Fetch fetch;
  Sent dcs[2];               // the two commands of the sequence
  uint32_t dsiBeforeReset[2] = {};
  Engines engines;
  uint64_t tUs[6] = {};      // when each step started (relative to the call)
  uint32_t stores = 0, refused = 0;
};

struct DsiHost {
  uint32_t ctrl, clocks;
};

template <class Io>
bool resetDsiHosts(Io &io, int ctl, const DsiHost (&saved)[2], Trace &trace)
{
  trace.boundary(io, TraceStep::dsiReset);
  if (io.rd(intfReg(1, kIntfEn)) || io.rd(intfReg(2, kIntfEn)) || !timingQuiescent(io)) return false;
  bool ok = true;
  for (unsigned l = 0; l < 2; ++l)
    ok &= io.wr(dsiReg(l, kDsiCtrl), saved[l].ctrl & ~kCtrlRun);
  for (unsigned l = 0; l < 2; ++l) {
    const uint32_t clocks = saved[l].clocks | kDsiResetClocks;
    ok &= io.wr(dsiReg(l, kDsiClkCtrl), clocks, ctl);
    ok &= io.rd(dsiReg(l, kDsiClkCtrl)) == clocks && io.rd(dsiReg(l, kDsiCtrl)) == (saved[l].ctrl & ~kCtrlRun);
  }
  if (ok) {
    for (unsigned l = 0; l < 2; ++l) {
      ok &= io.wr(dsiReg(l, kDsiSwReset), 1, ctl);
      io.delayUs(1);
      ok &= io.wr(dsiReg(l, kDsiSwReset), 0, ctl);
      ok &= io.rd(dsiReg(l, kDsiSwReset)) == 0;
      ok &= !(io.rd(dsiReg(l, kDsiStatus0)) & (kStatusDmaBusy | kStatusVideoBusy));
    }
  }
  for (unsigned l = 0; l < 2; ++l) {
    ok &= io.wr(dsiReg(l, kDsiClkCtrl), saved[l].clocks, ctl);
    ok &= io.rd(dsiReg(l, kDsiClkCtrl)) == saved[l].clocks;
  }
  trace.sample(io);
  return ok;
}

template <class Io>
bool setDsiMode(Io &io, const DsiHost (&saved)[2], bool commandOnly)
{
  bool ok = true;
  for (unsigned l = 0; l < 2; ++l) {
    const uint32_t ctrl = commandOnly ? (saved[l].ctrl & ~kCtrlVideo) | kCtrlCommand : saved[l].ctrl;
    ok &= io.wr(dsiReg(l, kDsiCtrl), ctrl);
    ok &= io.rd(dsiReg(l, kDsiCtrl)) == ctrl;
  }
  return ok;
}

// Undo after a failed off: engines (if any stopped) on, sleep-out, display-on.
template <class Io>
void undoOff(Io &io, const LivePath &p, const Options &o, Result &r, bool sleeping, Trace &trace)
{
  trace.boundary(io, TraceStep::undo);
  if (r.step >= 3) {
    const Engines e = startEngines(io, p, r.fetch, trace);
    r.stores += e.stores;
    r.refused += e.refused;
    if (!e.ok) return;
    r.enginesStopped = false;
  }
  if (sleeping) {
    trace.boundary(io, TraceStep::sleepOut);
    const Sent s = sendDcs(io, kDcsSleepOut, o);
    if (!s.ok) return;
    io.sleepMs(o.sleepOutMs);
  }
  trace.boundary(io, TraceStep::displayOn);
  r.restored = sendDcs(io, kDcsDisplayOn, o).ok;
  if (r.restored) io.sleepMs(o.displayOnMs);
}

template <class Io>
Result panelOff(Io &io, const LivePath &p, const Options &o, TraceRing *ring = nullptr)
{
  Result r;
  Trace trace{ring, unsigned(p.ctl)};
  const uint64_t t0 = io.nowUs();
  auto finish = [&](const char *why) {
    r.why = why;
    trace.boundary(io, TraceStep::end);
    return r;
  };
  if (o.engines) {
    const unsigned ctl = unsigned(p.ctl);
    if (!p.ok || p.intfActive != kIntfFlushDsi || p.intfMaster != kDsiIntfMaster) {
      r.safeAwake = true;
      return finish("INTF1 is not the master of the validated two-link CTL");
    }
    trace.boundary(io, TraceStep::drain);
    Frames edge;
    if (waitFlush(io, ctl, UINT32_MAX, kEngineStopUs, trace) < 0 || !waitFrameEdge(io, kEngineStopUs, &edge, trace)) {
      r.safeAwake = true;
      return finish("the master did not drain a frame with no pending CTL flush");
    }
  }
  r.step = 1;
  r.tUs[0] = io.nowUs() - t0;
  trace.boundary(io, TraceStep::displayOff);
  r.dcs[0] = sendDcs(io, kDcsDisplayOff, o);
  trace.boundary(io, TraceStep::displayOffDone);
  if (!r.dcs[0].ok) {
    r.safeAwake = !r.dcs[0].stores ||
                  (r.dcs[0].waitedBlanking && r.dcs[0].done[0] && r.dcs[0].done[1] && !r.dcs[0].refused);
    return finish(r.dcs[0].why);
  }
  io.sleepMs(o.displayOffMs);
  r.step = 2;
  r.tUs[1] = io.nowUs() - t0;
  trace.boundary(io, TraceStep::sleepIn);
  r.dcs[1] = sendDcs(io, kDcsSleepIn, o);
  trace.boundary(io, TraceStep::sleepInDone);
  if (!r.dcs[1].ok) {
    if (!r.dcs[1].stores || (r.dcs[1].done[0] && r.dcs[1].done[1] && !r.dcs[1].refused))
      undoOff(io, p, o, r, !r.dcs[1].waitedBlanking && r.dcs[1].stores, trace);
    return finish(r.dcs[1].why);
  }
  io.sleepMs(o.sleepInMs);
  if (!o.engines) {
    r.ok = true;
    return finish("ok (DCS only: the timing engines keep running)");
  }
  r.step = 3;
  r.tUs[2] = io.nowUs() - t0;
  trace.boundary(io, TraceStep::stopMaster);
  for (unsigned i = 0; i < 2; ++i) {
    r.fetch.config[i] = io.rd(intfReg(i + 1, kIntfConfig));
    r.fetch.start[i] = io.rd(intfReg(i + 1, kIntfProgFetch));
  }
  r.engines.us[0] = stopEngine(io, 1, r.engines, trace);
  if (r.engines.us[0] < 0) {
    undoOff(io, p, o, r, true, trace);
    return finish(r.engines.refused ? "the store guard refused a store" : "the master did not stop both timing engines within 40 ms");
  }
  r.step = 4;
  r.tUs[3] = io.nowUs() - t0;
  if (!resetPanelCtl(io, p, r.engines, trace)) {
    undoOff(io, p, o, r, true, trace);
    return finish("could not reset the stopped CTL for the slave handoff");
  }
  trace.boundary(io, TraceStep::handoff);
  r.engines.latchUs = submitPanelRoute(io, p, kDsiIntfSlave, 0, r.engines, trace);
  if (r.engines.latchUs < 0) {
    undoOff(io, p, o, r, true, trace);
    return finish("the slave handoff did not flush");
  }
  trace.boundary(io, TraceStep::stopSlave);
  r.engines.us[1] = stopEngine(io, 2, r.engines, trace);
  if (r.engines.us[1] < 0 || io.rd(intfReg(1, kIntfEn)) != 0 || !timingQuiescent(io)) {
    undoOff(io, p, o, r, true, trace);
    return finish(r.engines.refused ? "the store guard refused a store" : "INTF2 did not quiesce after the handoff");
  }
  // Keep prefetch active for the last scanned frame. The wake flush latches
  // the restored configuration before the first complete frame is sent.
  r.step = 5;
  trace.boundary(io, TraceStep::fetchOff);
  if (!programFetch(io, r.fetch, false, r.engines)) {
    undoOff(io, p, o, r, true, trace);
    return finish("could not disable programmable fetch");
  }
  r.tUs[4] = io.nowUs() - t0;
  r.enginesStopped = true;
  r.ok = true;
  return finish("ok");
}

template <class Io>
Result panelOn(Io &io, const LivePath &p, const Options &o, bool enginesStopped, const Fetch &fetch, TraceRing *ring = nullptr)
{
  Result r;
  Trace trace{ring, unsigned(p.ctl)};
  const uint64_t t0 = io.nowUs();
  auto finish = [&](const char *why) {
    r.why = why;
    trace.boundary(io, TraceStep::end);
    return r;
  };
  r.step = 1;
  DsiHost saved[2] = {};
  if (enginesStopped) {
    if (!p.ok || p.intfActive != kIntfFlushDsi || p.intfMaster != kDsiIntfMaster)
      return finish("the saved bonded video path is unavailable");
    for (unsigned l = 0; l < 2; ++l) {
      saved[l] = {io.rd(dsiReg(l, kDsiCtrl)), io.rd(dsiReg(l, kDsiClkCtrl))};
      r.dsiBeforeReset[l] = io.rd(dsiReg(l, kDsiStatus0));
      if (saved[l].ctrl != p.link[l].dsiCtrl || saved[l].clocks == UINT32_MAX ||
          io.rd(dsiReg(l, kDsiTpgCtrl)) || (r.dsiBeforeReset[l] & kStatusDmaBusy))
        return finish("the retained DSI host configuration changed");
    }
    if (!resetDsiHosts(io, p.ctl, saved, trace))
      return finish("the stopped DSI hosts did not reset");
    if (!setDsiMode(io, saved, true)) {
      if (setDsiMode(io, saved, false)) {
        r.engines = startEngines(io, p, fetch, trace);
        r.scanoutRestored = r.engines.ok;
      }
      return finish("the DSI hosts did not enter the stopped command window");
    }
  }

  r.step = 2;
  r.tUs[1] = io.nowUs() - t0;
  trace.boundary(io, TraceStep::sleepOut);
  r.dcs[0] = sendDcs(io, kDcsSleepOut, o, enginesStopped);
  if (r.dcs[0].ok) {
    io.sleepMs(o.sleepOutMs);
    r.step = 3;
    r.tUs[2] = io.nowUs() - t0;
    trace.boundary(io, TraceStep::displayOn);
    r.dcs[1] = sendDcs(io, kDcsDisplayOn, o, enginesStopped);
    if (r.dcs[1].ok) io.sleepMs(o.displayOnMs);
  }
  const Sent &last = r.dcs[r.step - 2];
  r.tUs[3] = io.nowUs() - t0;
  if (enginesStopped) {
    // An unfinished transfer owns its FIFO. Abort it before restoring the DMA words and re-enabling video.
    bool clean = true;
    if (!last.ok && last.stores) {
      clean = resetDsiHosts(io, p.ctl, saved, trace);
      if (clean) {
        auto wr = [&](uint64_t addr, uint32_t value) { clean &= io.wr(addr, value); };
        for (unsigned l = 0; l < 2; ++l)
          restoreDcsLink(io, l, last.before[l], false, wr);
      }
    }
    if (!clean || !setDsiMode(io, saved, false)) return finish("could not restore the DSI hosts after wake DCS");
    r.engines = startEngines(io, p, fetch, trace);
    r.scanoutRestored = r.engines.ok;
    if (!r.engines.ok && last.ok) {
      r.step = 4;
      return finish(r.engines.why);
    }
  }
  if (!last.ok) return finish(last.why);
  r.scanoutRestored = true;
  r.ok = true;
  return finish("ok");
}

}  // namespace panel
}  // namespace pianodpu
