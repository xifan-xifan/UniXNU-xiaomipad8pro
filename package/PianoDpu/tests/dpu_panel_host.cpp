// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1e (DpuPanel.h) on the timed register model (dpu_model.h):
//   - the DCS short packet and its FIFO words; one command on both links: the stores in order, low power, sent in the vertical
//     blanking (by the hardware's block or, without it, triggered in the front porch), every word restored as found; the
//     refusals (not video mode, the FIFO in use, no software trigger, a command that is not a sleep command, a busy DMA);
//   - counter setup from disabled, ten cycles with advancing counters despite zero COUNT_EN, and the logged refusal when stuck;
//   - y12o's observed slave timeout, master timeout recovery, and poll timing with tracing on/off;
//   - display sleep: 0x28, >= 20 ms, 0x10, >= 100 ms, master stop, CTL handoff, slave stop, both halves asleep; wake:
//     both DSI hosts reset, 0x11, >= 120 ms, 0x29, the bonded route and INTF flush, both INTFs started in lockstep; 50 cycles;
//   - the failures: a DMA that never finishes (the link left alone, nothing after it), an engine that does not stop (the panel
//     brought back), an engine that does not start; DCS only; high speed;
//   - nothing written outside the guard's registers: never TLMM (the panel's reset and supply pins).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_panel_host.cpp -o /tmp/dpu_panel_host && /tmp/dpu_panel_host
#include "DpuPanel.h"
#include "dpu_model.h"
#include "../../NativeGopDisplay/GopCompose.h"
#include "../../NativeGopDisplay/GopPanelPower.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace pianodpu;
using namespace pianodpu::panel;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool clean(const model::Dpu &m)
{
  return m.refused.empty() && !m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kDsiPhy[0], 0x1000) && !m.anyStoreIn(kDsiPhy[1], 0x1000) &&
         !m.anyStoreIn(kMdp + 0x4000, 0x8000) && !m.anyStoreIn(kMdp + 0x24000, 0xc000) && !m.anyStoreIn(kMdp + kLmBase, 0x8000);
}

static void testPacket()
{
  const Packet p = dcsShort(kDcsDisplayOff);
  CHECK(p.words[0] == 0x80050028u && p.words[1] == 0 && p.count == 2 && p.bytes == 4);
  CHECK(dcsShort(kDcsSleepOut).words[0] == 0x80050011u);
  CHECK(dcsAllowed(0x28) && dcsAllowed(0x10) && dcsAllowed(0x11) && dcsAllowed(0x29) && !dcsAllowed(0x51) && !dcsAllowed(0x01));
}

static void testSend()
{
  model::Dpu m;
  m.sleepMs(3);
  uint32_t found[2][4];
  for (unsigned l = 0; l < 2; ++l) {
    m.regs[dsiReg(l, kDsiAckErr)] = 0x10u << l;
    found[l][0] = m.rd(dsiReg(l, kDsiCtrl));
    found[l][1] = m.rd(dsiReg(l, kDsiCmdDmaCtrl));
    found[l][2] = m.rd(dsiReg(l, kDsiDmaCtrl1));
    found[l][3] = m.rd(dsiReg(l, kDsiDmaLength));
  }
  const size_t before = m.stores.size();
  Sent s = sendDcs(m, kDcsDisplayOff, Options());
  CHECK(s.ok && s.done[0] && s.done[1] && !s.waitedBlanking && s.refused == 0);
  CHECK(m.dcs.size() == 2 && m.dcs[0].command == 0x28 && m.dcs[1].command == 0x28 && m.dcs[0].lowPower && m.dcs[1].lowPower);
  CHECK(m.commandsInFrame == 0 && !m.half[0].displayOn && !m.half[1].displayOn);
  CHECK(s.ackBeforeMask == 3 && s.ackAfterMask == 3);
  for (unsigned l = 0; l < 2; ++l) {   // every word as found, the FIFO free, the done bit clear
    CHECK(s.before[l].ack == (0x10u << l) && s.ackAfter[l] == s.before[l].ack);
    CHECK(m.rd(dsiReg(l, kDsiCtrl)) == found[l][0] && m.rd(dsiReg(l, kDsiCmdDmaCtrl)) == found[l][1]);
    CHECK(m.rd(dsiReg(l, kDsiDmaCtrl1)) == found[l][2] && m.rd(dsiReg(l, kDsiDmaLength)) == found[l][3]);
    CHECK(m.rd(dsiReg(l, kDsiTpgCtrl)) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 1u) == 0 && (m.rd(dsiReg(l, kDsiIntrCtrl)) & 2u));
  }
  // DSI0's preparation in order, DSI1's, then the two triggers back to back
  const uint64_t prep[] = {kDsiIntrCtrl, kDsiCtrl, kDsiTpgCtrl, kDsiTpgFifo, kDsiTpgFifo, kDsiCmdDmaCtrl, kDsiDmaCtrl1, kDsiDmaLength};
  for (unsigned l = 0; l < 2; ++l)
    for (unsigned k = 0; k < 8; ++k) CHECK(m.stores[before + 8 * l + k].pa == dsiReg(l, uint32_t(prep[k])));
  CHECK(m.stores[before + 1].v == (found[0][0] | 4u) && m.stores[before + 2].v == 0x30006u && m.stores[before + 3].v == 0x80050028u &&
        m.stores[before + 4].v == 0 && m.stores[before + 5].v == ((found[0][1] & ~0xc4000000u) | 0x14000000u) &&
        m.stores[before + 6].v == (found[0][2] & ~2u) && m.stores[before + 7].v == 4);
  CHECK(m.stores[before + 16].pa == dsiReg(0, kDsiTrigDma) && m.stores[before + 17].pa == dsiReg(1, kDsiTrigDma));
  CHECK(m.stores[before + 17].ns - m.stores[before + 16].ns < 1000);
  CHECK(m.storesTo(dsiReg(0, 0x1ec)) == 2 && m.firstStore(dsiReg(0, 0x1ec), 1) >= 0);   // PeripheralIO's FIFO reset pulse
  CHECK(clean(m));
  // high speed: the low-power bit clear
  Options hs;
  hs.lowPower = false;
  CHECK(sendDcs(m, kDcsDisplayOn, hs).ok && !m.dcs.back().lowPower && m.half[0].displayOn);
  // no hardware block within the frame: triggered in the front porch
  model::Dpu n;
  n.blockDma = false;
  n.sleepMs(3);
  s = sendDcs(n, kDcsDisplayOff, Options());
  CHECK(s.ok && s.waitedBlanking && n.commandsInFrame == 0 && n.dcs.size() == 2);
  CHECK(n.stores[n.firstStore(dsiReg(0, kDsiTrigDma), 1)].line >= kVActiveEnd);
}

static void testRefusals()
{
  CHECK(storeAllowed(ctlReg(0, kCtlSwReset), 1, false, 0) && storeAllowed(ctlReg(0, kCtlStart), 1, false, 0));
  CHECK(!storeAllowed(ctlReg(0, kCtlSwReset), 1) && !storeAllowed(ctlReg(0, kCtlStart), 1, true));
  CHECK(!storeAllowed(ctlReg(1, kCtlSwReset), 1, false, 0) && !storeAllowed(ctlReg(0, kCtlSwReset), 2, false, 0));
  CHECK(!storeAllowed(ctlReg(0, kCtlIntfActive), 0, false, 0) && !storeAllowed(ctlReg(0, kCtlIntfMaster), kDsiIntfSlave, false, 0));
  for (unsigned l = 0; l < 2; ++l) {
    CHECK(!storeAllowed(dsiReg(l, kDsiSwReset), 1) && !storeAllowed(dsiReg(l, kDsiClkCtrl), kDsiResetClocks, true));
    CHECK(storeAllowed(dsiReg(l, kDsiSwReset), 0, false, 0) && storeAllowed(dsiReg(l, kDsiSwReset), 1, false, 0));
    CHECK(!storeAllowed(dsiReg(l, kDsiSwReset), 2, false, 0));
  }
  {
    model::Dpu m;
    m.regs[dsiReg(1, kDsiCtrl)] = 0x1f5;   // DSI1 in command mode
    m.sleepMs(2);
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "video mode") && m.stores.empty());
  }
  {
    model::Dpu m;
    m.regs[dsiReg(0, kDsiTpgCtrl)] = 0x30006;   // the FIFO in use
    CHECK(!sendDcs(m, kDcsDisplayOff, Options()).ok && m.stores.empty());
  }
  {
    model::Dpu m;
    CHECK(!sendDcs(m, 0x51, Options()).ok && m.stores.empty());   // brightness: not this slice's
  }
  {
    model::Dpu m;
    m.dsi[0].dmaPending = true;   // a DMA of someone else's that never ends
    m.dsi[0].dmaAt = UINT64_MAX;
    const Sent s = sendDcs(m, kDcsDisplayOff, Options());
    CHECK(!s.ok && std::strstr(s.why, "busy") && m.stores.empty());
  }
  {
    model::Dpu m;
    m.blockDma = false;
    const LivePath p = readPath(m);
    m.regs[intfReg(1, kIntfHsyncCtl)] = 0;  // no usable porch geometry at command preparation
    const Result r = panelOff(m, p, Options());
    CHECK(!r.ok && r.safeAwake && m.dcs.empty() && m.storesTo(dsiReg(0, kDsiTrigDma)) == 0 &&
          m.rd(dsiReg(0, kDsiTpgCtrl)) == 0);
    CHECK(!r.dcs[0].triggerAttempted && r.dcs[0].dmaDoneMask() == 0 && r.dcs[0].ackAfterMask == 3);
  }
}

static void testSleepWake()
{
  model::Dpu m;
  m.blockDma = false;
  constexpr uint32_t config = 0x80800000u;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.intf[i].counting = false;
    m.regs[intfReg(i, kIntfConfig)] = config;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = (2290 + i) * model::kHtotal;
  }
  m.sleepMs(3);
  LivePath p = readPath(m);
  CHECK(p.ok && p.link[0].countEn == 0 && p.link[1].countEn == 0);
  const CounterInit init = initCounters(m, p);
  CHECK(init.enabled && init.moving && m.stores.size() == 2);
  CHECK(m.firstStore(intfReg(1, kIntfCountEn), 3) == 0 && m.firstStore(intfReg(2, kIntfCountEn), 3) == 1);
  Result off = panelOff(m, p, Options());
  CHECK(off.ok && off.enginesStopped && off.step == 5 && off.dcs[0].waitedBlanking);
  const model::Dcs *d28 = m.firstDcs(0x28, 0), *d10 = m.firstDcs(0x10, 0), *e28 = m.firstDcs(0x28, 1), *e10 = m.firstDcs(0x10, 1);
  CHECK(d28 && d10 && e28 && e10);
  if (d28 && d10) CHECK(d10->ns - d28->ns >= 20000000ull);
  const int stop2 = m.firstStore(intfReg(2, kIntfEn), 0), stop1 = m.firstStore(intfReg(1, kIntfEn), 0);
  const int reset = m.firstStore(ctlReg(0, kCtlSwReset), 1);
  const int handoff = m.firstStore(ctlReg(0, kCtlStart), 1);
  CHECK(stop1 >= 0 && stop1 < reset && reset < handoff && handoff < stop2);
  CHECK(m.rd(ctlReg(0, kCtlIntfActive)) == kDsiIntfSlave && m.rd(ctlReg(0, kCtlIntfMaster)) == 0);
  CHECK(readPath(m).ctl < 0 && readPath(m, false, true).ctl == p.ctl);
  for (unsigned i = 1; i <= 2; ++i) {
    const int disable = m.firstStore(intfReg(i, kIntfConfig), config & ~kIntfConfigProgFetch);
    CHECK(disable > stop2);
    CHECK(m.rd(intfReg(i, kIntfConfig)) == (config & ~kIntfConfigProgFetch));
    m.regs[intfReg(i, kIntfProgFetch)] = 0;   // prove the wake uses the saved registers
  }
  if (d10 && stop1 >= 0) CHECK(m.stores[size_t(stop1)].ns - d10->ns >= 100000000ull);   // 100 ms after sleep-in
  CHECK(!m.intf[1].on && !m.intf[2].on && m.half[0].sleeping && m.half[1].sleeping && !m.half[0].displayOn);
  const uint32_t f1 = m.intf[1].frames, f2 = m.intf[2].frames;
  m.sleepMs(1000);   // asleep: no frames, no fetch
  CHECK(m.intf[1].frames == f1 && m.intf[2].frames == f2 && m.rd(intfReg(1, kIntfStatus)) == 0);
  const size_t before = m.stores.size();
  Result on = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
  CHECK(on.ok && on.engines.ok && on.engines.rate.lockstep && on.engines.latchUs >= 0);
  size_t restart = before;
  while (restart < m.stores.size() && m.stores[restart].pa != ctlReg(0, kCtlSwReset)) ++restart;
  CHECK(m.stores.size() >= restart + 12);
  if (!on.ok || m.stores.size() < restart + 12) return;
  for (unsigned i = 1; i <= 2; ++i) {
    CHECK(m.stores[restart + 1 + 2 * (i - 1)].pa == intfReg(i, kIntfProgFetch));
    CHECK(m.stores[restart + 2 + 2 * (i - 1)].pa == intfReg(i, kIntfConfig));
    CHECK(m.rd(intfReg(i, kIntfConfig)) == config && m.rd(intfReg(i, kIntfProgFetch)) == (2290 + i) * model::kHtotal);
  }
  CHECK(m.stores[restart].pa == ctlReg(0, kCtlSwReset));
  CHECK(m.stores[restart + 5].pa == ctlReg(0, kCtlIntfActive) && m.stores[restart + 5].v == kIntfFlushDsi);
  CHECK(m.stores[restart + 6].pa == ctlReg(0, kCtlIntfMaster) && m.stores[restart + 6].v == kDsiIntfMaster);
  CHECK(m.stores[restart + 7].pa == ctlReg(0, kCtlIntfFlush) && m.stores[restart + 8].pa == ctlReg(0, kCtlFlush));
  CHECK(m.stores[restart + 9].pa == ctlReg(0, kCtlStart));
  CHECK(m.stores[restart + 10].pa == intfReg(1, kIntfEn) && m.stores[restart + 11].pa == intfReg(2, kIntfEn));
  CHECK(m.stores[restart + 11].ns - m.stores[restart + 10].ns < 1000 && m.desync == 0);
  CHECK(m.intf[1].on && m.intf[2].on && m.intf[1].frames > f1 && m.intf[1].frames - f1 == m.intf[2].frames - f2);
  const model::Dcs *d11 = m.firstDcs(0x11, 0), *d29 = nullptr;
  for (const model::Dcs &x : m.dcs)
    if (x.command == 0x29 && x.link == 1 && d11 && x.ns > d11->ns) d29 = &x;
  CHECK(d11 && d29);
  if (d11 && d29) {
    CHECK(d29->ns - d11->ns >= 120000000ull);
    CHECK(d29->ns < m.stores[restart].ns);
  }
  CHECK(!m.half[0].sleeping && !m.half[1].sleeping && m.half[0].displayOn && m.half[1].displayOn);
  CHECK(m.glitches == 0 && clean(m));
  // 50 cycles
  using namespace gopcompose;
  PanelPower power;
  Slot slot;
  Busy busy;
  Job shown;
  unsigned ok = 0;
  for (unsigned k = 0; k < 50; ++k) {
    m.sleepMs(3 * k % 17);
    Job flip;
    flip.serial = 2 * k + 1;
    flip.flip = true;
    flip.kind = Job::kFull;
    CHECK(offer(slot, flip, nullptr) == Offer::Taken && takeJob(slot, busy, &shown));
    CHECK(power.request(false) && !power.canPresent());
    uint64_t done = 0;
    CHECK(finishJob(busy, &done, flip.serial) && done == flip.serial);  // drain the serving flip before DCS
    bool want;
    CHECK(power.begin(want) && !want);
    const Result sleep = panelOff(m, p, Options());
    ok += sleep.ok;
    power.finish(false);
    flip.serial++;
    const Admission held = admitSwap(slot, busy, flip, LayerChange(), kDeferAlways);
    CHECK(held.hadJob && held.offer == Offer::Taken && held.waitSerial == flip.serial);
    const uint32_t asleepFrame = m.intf[1].frames;
    m.sleepMs(500);
    CHECK(!power.canPresent() && slot.busy && m.intf[1].frames == asleepFrame);
    CHECK(power.request(true) && power.begin(want) && want && !power.canPresent());
    const Result wake = panelOn(m, p, Options(), sleep.enginesStopped, sleep.fetch);
    ok += wake.ok;
    power.finish(true);
    CHECK(power.wakeComplete && power.canPresent() && takeJob(slot, busy, &shown) && shown.serial == flip.serial && shown.flip);
    CHECK(finishJob(busy, &done, flip.serial) && done == flip.serial);
  }
  CHECK(ok == 100 && m.desync == 0 && m.glitches == 0 && m.commandsInFrame == 0 && m.half[0].displayOn && clean(m));
}

static void testZeroCounterReadback()
{
  model::Dpu m;
  m.blockDma = false;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.countEnableIgnored[i - 1] = true;
    m.regs[intfReg(i, kIntfConfig)] = kIntfConfigProgFetch;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = 2290 * model::kHtotal;   // the merged model (M3) keeps the fetch per INTF
  }
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && !init.enabled && init.moving && m.stores.size() == 2);
  CHECK(p.link[0].countEn == 0 && p.link[1].countEn == 0);
  for (unsigned k = 0; k < 10; ++k) {
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok && off.enginesStopped && off.dcs[0].waitedBlanking);
    const Frames asleep = readFrames(m);
    m.sleepMs(100);
    const Frames after = readFrames(m);
    for (unsigned i = 1; i <= 2; ++i) {
      CHECK(m.rd(intfReg(i, kIntfEn)) == 0 && m.rd(intfReg(i, kIntfStatus)) == 0 &&
            !(m.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch));
      CHECK(after.f[i - 1] == asleep.f[i - 1]);
    }
    const Result on = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(on.ok && on.engines.rate.lockstep);
    for (unsigned i = 1; i <= 2; ++i) {
      CHECK(m.rd(intfReg(i, kIntfEn)) == 1 && m.rd(intfReg(i, kIntfStatus)) == 1 &&
            (m.rd(intfReg(i, kIntfConfig)) & kIntfConfigProgFetch));
      CHECK(m.rd(intfReg(i, kIntfFrameCount)) > asleep.f[i - 1] && m.rd(intfReg(i, kIntfCountEn)) == 0);
    }
  }
  CHECK(m.dcs.size() == 80 && m.desync == 0 && m.glitches == 0 && m.commandsInFrame == 0 && clean(m));
}

static void testUnavailablePanel()
{
  model::Dpu m;
  // The second timing engine runs, but its counters do not advance even with the enable bits set.
  m.intf[2].counting = false;
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && init.enabled && !init.moving && m.stores.empty());
  RequestGate gate;
  gate.ready = init.moving;
  unsigned logs = 0;
  const char *message = nullptr;
  auto log = [&](const char *line) { ++logs; message = line; };
  if (gate.admit(false, log)) panelOff(m, p, Options());
  CHECK(logs == 1 && message && std::strstr(message, "sleep refused"));
  CHECK(m.dcs.empty() && m.stores.empty() && m.intf[1].on && m.intf[2].on);
  CHECK(!gate.admit(true, log) && logs == 2 && std::strstr(message, "wake refused"));
  gate.wakePending = true;   // a dispatched transition may still need a recovery wake after sleep was disabled
  CHECK(gate.admit(true, log) && logs == 2);
  CHECK(!gate.admit(false, log) && logs == 3);
}

struct TraceIo : model::Dpu {
  uint64_t sampleStartNs = 0, maxSampleNs = 0, maxPollOverrunNs = 0;
  std::vector<uint64_t> masterPollNs;
  bool masterStop = false;

  bool wr(uint64_t pa, uint32_t value, int panelCtl = -1)
  {
    if (pa == intfReg(1, kIntfEn) && value == 0) masterStop = true;
    return Dpu::wr(pa, value, panelCtl);
  }

  uint32_t rd(uint64_t pa)
  {
    const uint32_t value = Dpu::rd(pa);
    if (masterStop && pa == intfReg(1, kIntfStatus) && masterPollNs.size() < 41) masterPollNs.push_back(ns);
    return value;
  }

  uint64_t pollDeadline(uint32_t us)
  {
    sampleStartNs = ns;
    return Dpu::pollDeadline(us);
  }

  void pollWait(uint64_t deadline, bool sleep)
  {
    if (ns - sampleStartNs > maxSampleNs) maxSampleNs = ns - sampleStartNs;
    if (ns > deadline && ns - deadline > maxPollOverrunNs) maxPollOverrunNs = ns - deadline;
    Dpu::pollWait(deadline, sleep);
  }
};

static Result masterStopTimeout(TraceIo &m, bool dropped, TraceRing *ring)
{
  m.blockDma = false;
  m.engineStuck[0] = !dropped;
  m.dropStore = intfReg(1, kIntfEn);
  m.dropCount = dropped ? 1 : 0;
  for (unsigned i = 1; i <= 2; ++i) {
    m.regs[intfReg(i, kIntfCountEn)] = 0;
    m.countEnableIgnored[i - 1] = true;
    m.regs[intfReg(i, kIntfConfig)] = kIntfConfigProgFetch;
    m.intf[i].fetch = m.regs[intfReg(i, kIntfProgFetch)] = 2290 * model::kHtotal;
  }
  LivePath p = readPath(m);
  const CounterInit init = initCounters(m, p);
  CHECK(p.ok && !init.enabled && init.moving);
  RequestGate gate;
  gate.ready = init.moving;
  unsigned sleepRefusals = 0;
  auto log = [&](const char *msg) { if (std::strstr(msg, "sleep refused")) ++sleepRefusals; };
  const auto offAdmitted = [&](bool blank, bool touchParked) { return blank && touchParked && gate.admit(false, log); };
  CHECK(!offAdmitted(false, false) && m.dcs.empty() && (!ring || !ring->size()));  // pre-first-on guard
  CHECK(offAdmitted(true, true));   // first runtime off, with dual blank and touch park confirmed
  const Result r = panelOff(m, p, Options(), ring);
  CHECK(!r.ok && r.step == 3 && r.restored && !r.enginesStopped && !r.safeAwake);
  CHECK(std::strstr(r.why, "the master did not stop both timing engines"));
  CHECK(r.dcs[0].command == 0x28 && r.dcs[1].command == 0x10);
  for (const Sent &s : r.dcs) {
    CHECK(s.ok && s.waitedBlanking && s.dmaDoneMask() == 3 && s.doneUs > 0 && s.doneUs < 2000);
    CHECK(s.ackBeforeMask == 3 && s.ackAfterMask == 3 && !s.before[0].ack && !s.before[1].ack && !s.ackAfter[0] && !s.ackAfter[1]);
    CHECK((s.intrPoll[0] & kIntDmaDone) && (s.intrPoll[1] & kIntDmaDone));
  }
  CHECK(m.firstStore(intfReg(2, kIntfEn), 0) < 0 && m.firstStore(ctlReg(0, kCtlSwReset), 1) < 0);
  CHECK((m.firstStore(intfReg(1, kIntfEn), 1) >= 0) == !dropped);
  CHECK(m.dcs.size() == 8 && m.firstDcs(0x11, 1) && m.firstDcs(0x29, 1));
  CHECK(m.intf[1].on && m.intf[2].on && m.half[0].displayOn && m.half[1].displayOn && clean(m));
  gate.ready = r.ok;
  gate.wakePending = true;
  CHECK(gate.admit(true, log) && r.restored);   // worker's recovery wake has no new hardware sequence
  gate.wakePending = false;
  const unsigned rows = ring ? ring->total : 0;
  const size_t stores = m.stores.size();
  for (unsigned i = 0; i < 10; ++i) CHECK(!offAdmitted(true, true));
  CHECK(sleepRefusals == 10 && m.stores.size() == stores && m.dcs.size() == 8 && (!ring || ring->total == rows));
  return r;
}

static void testStopTimeoutTrace()
{
  uint64_t maxSampleNs = 0, maxPollShiftNs = 0;
  for (bool dropped : {false, true}) {
    TraceRing ring;
    TraceIo logged, baseline;
    const Result plain = masterStopTimeout(baseline, dropped, nullptr);
    const Result traced = masterStopTimeout(logged, dropped, &ring);
    CHECK(traced.step == plain.step && traced.engines.status[0] == plain.engines.status[0]);
    CHECK(logged.stores.size() == baseline.stores.size());
    for (size_t i = 0; i < logged.stores.size() && i < baseline.stores.size(); ++i) {
      CHECK(logged.stores[i].pa == baseline.stores[i].pa && logged.stores[i].v == baseline.stores[i].v);
    }
    CHECK(logged.masterPollNs.size() == 41 && logged.masterPollNs.size() == baseline.masterPollNs.size());
    for (size_t i = 1; i < logged.masterPollNs.size() && i < baseline.masterPollNs.size(); ++i) {
      const uint64_t withTrace = logged.masterPollNs[i] - logged.masterPollNs[0];
      const uint64_t withoutTrace = baseline.masterPollNs[i] - baseline.masterPollNs[0];
      CHECK(withTrace == withoutTrace);
      const uint64_t shift = withTrace > withoutTrace ? withTrace - withoutTrace : withoutTrace - withTrace;
      if (shift > maxPollShiftNs) maxPollShiftNs = shift;
    }
    if (logged.maxSampleNs > maxSampleNs) maxSampleNs = logged.maxSampleNs;
    CHECK(logged.maxPollOverrunNs == 0);

    CHECK(ring.size() == ring.total && ring.size() > 41);
    CHECK(ring.at(0).boundary && ring.at(0).step == TraceStep::drain);
    const TraceSample &last = ring.at(ring.size() - 1);
    CHECK(last.boundary && last.step == TraceStep::end && last.link[1].en == 1 && last.link[1].status == 1);
    const TraceSample *firstStopPoll = nullptr, *lastStopPoll = nullptr;
    unsigned stopPolls = 0;
    bool undo = false, flushed = false, latched = false, verified = false;
    for (unsigned i = 0; i < ring.size(); ++i) {
      const TraceSample &s = ring.at(i);
      CHECK(s.top == 0xf0000000u && s.active == 0x6);
      CHECK(!i || s.us >= ring.at(i - 1).us);
      CHECK(s.step != TraceStep::stopSlave && s.step != TraceStep::handoff);
      if (s.step == TraceStep::stopMaster) {
        CHECK(s.link[1].en == 1 && s.link[0].status == 1 && s.link[1].status == 1 && !s.flush && !s.intfFlush);
        CHECK(s.link[0].en == (s.boundary || dropped ? 1u : 0u));
        if (s.boundary) continue;
        ++stopPolls;
        if (!firstStopPoll) firstStopPoll = &s;
        lastStopPoll = &s;
      }
      if (s.step == TraceStep::undo) undo = s.boundary && s.link[0].en == unsigned(dropped) && s.link[0].status == 1;
      if (s.step == TraceStep::flush && (s.flush & kFlushIntf)) flushed |= s.intfFlush == kIntfFlushDsi;
      if (s.step == TraceStep::flush && s.flush == 0) latched = true;
      if (s.step == TraceStep::rate && !s.boundary) verified = true;
    }
    CHECK(undo && flushed && latched && verified && stopPolls == 41);
    if (firstStopPoll && lastStopPoll) {
      CHECK(lastStopPoll->us - firstStopPoll->us >= 40000);
      for (unsigned i = 0; i < 2; ++i) {
        CHECK(lastStopPoll->link[i].frame > firstStopPoll->link[i].frame);
        CHECK(lastStopPoll->link[i].line != firstStopPoll->link[i].line);
      }
    }
  }
  TraceIo io;
  TraceRing ring;
  const LivePath p = readPath(io);
  const Result off = panelOff(io, p, Options(), &ring);
  CHECK(off.ok && ring.at(ring.size() - 1).link[1].status == 0);
  ring.reset();
  const Result on = panelOn(io, p, Options(), off.enginesStopped, off.fetch, &ring);
  CHECK(on.ok && on.engines.rate.lockstep && ring.at(0).step == TraceStep::dsiReset);
  CHECK(ring.at(ring.size() - 1).link[1].status == 1 && io.desync == 0);
  CHECK(io.maxPollOverrunNs == 0);
  ring.reset();
  for (unsigned i = 0; i < TraceRing::kCapacity + 5; ++i) ring.next().us = i;
  CHECK(ring.size() == TraceRing::kCapacity && ring.at(0).us == 5 && ring.at(ring.size() - 1).us == TraceRing::kCapacity + 4);
  ring.reset();
  CHECK(!ring.size());
  CHECK(maxSampleNs > 0 && maxSampleNs < kLineNs && maxPollShiftNs < kLineNs / 10);
  std::printf("M5 trace model: max in-wait sample %llu ns, added stop poll drift %llu ns (line %u ns)\n",
              (unsigned long long)maxSampleNs, (unsigned long long)maxPollShiftNs, kLineNs);
}

// Trace fixtures preserve the sampled words and intervals, with a rebased timestamp origin.
static std::ifstream openTrace(const char *name)
{
  const std::string src = __FILE__;
  const std::size_t slash = src.find_last_of('/');
  return std::ifstream((slash == std::string::npos ? std::string() : src.substr(0, slash + 1)) + name);
}

static void readTraceLine(std::istream &file, const char *prefix)
{
  std::string line;
  std::getline(file, line);
  CHECK(line.starts_with(prefix));
}

static std::vector<TraceSample> readTraceRows(std::istream &file, unsigned serial, unsigned count)
{
  std::string line;
  std::vector<TraceSample> rows;
  while (rows.size() < count && std::getline(file, line)) {
    TraceSample s{};
    unsigned found = 0, index = 0;
    unsigned long long us = 0;
    char step[16] = {}, kind = 0;
    auto &a = s.link[0], &b = s.link[1];
    const int n = std::sscanf(line.c_str(), "m5 %u %u %llu %15s %c %x/%x/%x/%x %x/%x/%x/%x %x/%x/%x/%x",
                              &found, &index, &us, step, &kind, &a.en, &a.status, &a.frame, &a.line,
                              &b.en, &b.status, &b.frame, &b.line, &s.top, &s.active, &s.flush, &s.intfFlush);
    CHECK(n == 17 && found == serial && index == rows.size() && (kind == 'B' || kind == 'P'));
    if (n != 17) return {};
    s.us = us;
    s.boundary = kind == 'B';
    unsigned tag = 0;
    for (; tag <= unsigned(TraceStep::end); ++tag)
      if (!std::strcmp(step, traceStepName(TraceStep(tag)))) break;
    CHECK(tag <= unsigned(TraceStep::end));
    s.step = TraceStep(tag);
    rows.push_back(s);
  }
  CHECK(rows.size() == count);
  return rows;
}

static int64_t scanPosition(uint32_t frame, uint32_t line)
{
  return int64_t(frame) * kVtotal120 + line;
}

static void testY12oTrace()
{
  auto file = openTrace("m5-y12o-off.trace");
  readTraceLine(file, "m5 1 off ctl0 ok=0 step=3 restored=1 rows=216 dropped=0");
  readTraceLine(file, "m5 1 dcs 28 trig=1 done=3 us=1245 porch=1");
  readTraceLine(file, "m5 1 dcs 10 trig=1 done=3 us=1270 porch=1");
  const auto rows = readTraceRows(file, 1, 216);
  if (rows.size() != 216) return;
  model::Dpu replay;
  const auto &stop = rows[4];
  const auto &firstPoll = rows[5];
  const auto &lastPoll = rows[38];
  CHECK(stop.step == TraceStep::stopSlave && stop.boundary && firstPoll.step == stop.step && !firstPoll.boundary);
  CHECK(lastPoll.step == stop.step && !lastPoll.boundary && lastPoll.us - firstPoll.us == 40229);
  // Seed the clock at the recorded stop boundary and run every row at its
  // captured time. Reproduce the legacy timing and flush stores.
  const uint64_t originNs = replay.frameNs(1);
  replay.advanceTo(originNs);
  for (unsigned i = 1; i <= 2; ++i) {
    const auto &atStop = stop.link[i - 1];
    const auto &atStart = rows[0].link[i - 1];
    const uint64_t endPositionNs = uint64_t(scanPosition(atStop.frame, atStop.line)) * kLineNs;
    const uint64_t linePosition = (endPositionNs - stop.us * 1000) / kLineNs;
    replay.intf[i].frames = atStart.frame;
    CHECK(linePosition / kVtotal120 == atStart.frame);
    replay.intf[i].start = replay.ns - linePosition % kVtotal120 * kLineNs;
  }
  model::Dpu run = replay;
  for (size_t index = 0; index < rows.size(); ++index) {
    const auto &s = rows[index];
    CHECK(!index || s.us > rows[index - 1].us);
    replay.advanceTo(originNs + s.us * 1000);
    if (index == 5) CHECK(replay.wr(intfReg(2, kIntfEn), 0));
    if (index == 42) {
      CHECK(replay.wr(ctlReg(0, kCtlIntfFlush), kIntfFlushDsi));
      CHECK(replay.wr(ctlReg(0, kCtlFlush), kFlushIntf));
      CHECK(replay.wr(intfReg(2, kIntfEn), 1));
    }
    CHECK(replay.rd(ctlReg(0, kCtlTop)) == s.top && replay.rd(ctlReg(0, kCtlIntfActive)) == s.active);
    CHECK(replay.rd(ctlReg(0, kCtlFlush)) == s.flush && replay.rd(ctlReg(0, kCtlIntfFlush)) == s.intfFlush);
    for (unsigned i = 1; i <= 2; ++i) {
      const auto &l = s.link[i - 1];
      CHECK(replay.rd(intfReg(i, kIntfEn)) == l.en && replay.rd(intfReg(i, kIntfStatus)) == l.status);
      const auto observed = scanPosition(l.frame, l.line);
      const auto simulated = scanPosition(replay.intf[i].frames, replay.line(i));
      // The nominal model clock and per-register read times differ slightly
      // from the capture. Even near a frame boundary compare linear position.
      CHECK(simulated >= observed - 12 && simulated <= observed + 12);
      if (index) {
        const auto &before = rows[index - 1].link[i - 1];
        CHECK(observed > scanPosition(before.frame, before.line));
      }
    }
  }

  run.blockDma = false;
  for (unsigned i = 1; i <= 2; ++i) {
    run.countEnableIgnored[i - 1] = true;
    run.regs[intfReg(i, kIntfCountEn)] = 0;
    run.regs[intfReg(i, kIntfConfig)] = kIntfConfigProgFetch;
    run.intf[i].fetch = run.regs[intfReg(i, kIntfProgFetch)] = 2290 * model::kHtotal;
  }
  LivePath p = readPath(run);
  const CounterInit init = initCounters(run, p);
  CHECK(p.ok && !init.enabled && init.moving);
  run.sleepMs(3);
  TraceRing ring;
  const Result off = panelOff(run, p, Options(), &ring);
  CHECK(off.ok && off.enginesStopped && off.dcs[0].dmaDoneMask() == 3 && off.dcs[1].dmaDoneMask() == 3);
  if (!off.ok) return;
  CHECK(off.dcs[0].waitedBlanking && off.dcs[1].waitedBlanking);
  CHECK(ring.total == ring.size() && ring.at(ring.size() - 1).active == kDsiIntfSlave);
  const Frames asleep = readFrames(run);
  const uint32_t asleepLine[2] = {run.rd(intfReg(1, kIntfLineCount)), run.rd(intfReg(2, kIntfLineCount))};
  run.sleepMs(100);
  for (unsigned i = 1; i <= 2; ++i) {
    CHECK(run.rd(intfReg(i, kIntfEn)) == 0 && run.rd(intfReg(i, kIntfStatus)) == 0);
    CHECK(run.rd(intfReg(i, kIntfFrameCount)) == asleep.f[i - 1] && run.rd(intfReg(i, kIntfLineCount)) == asleepLine[i - 1]);
  }
  ring.reset();
  const Result on = panelOn(run, p, Options(), off.enginesStopped, off.fetch, &ring);
  CHECK(on.ok && on.engines.rate.lockstep && run.half[0].displayOn && run.half[1].displayOn && clean(run));
  CHECK(ring.total == ring.size() && ring.at(ring.size() - 1).active == kIntfFlushDsi && run.latchedMaster == kDsiIntfMaster);
}

static void replayMovingCounters(const std::vector<TraceSample> &rows, unsigned link)
{
  model::Dpu clock;
  const unsigned intf = link + 1;
  const uint64_t base = clock.frameNs(intf);
  const uint64_t origin = rows.front().us;
  bool seeded = false;
  for (const auto &s : rows) {
    const auto &sample = s.link[link];
    const uint64_t when = base + (s.us - origin) * 1000;
    if (!(sample.status & kIntfStatusOn)) {
      seeded = false;
      continue;
    }
    // Counters sit at zero briefly when a link first starts driving video.
    if (!seeded && !sample.frame) continue;
    if (!seeded) {
      clock.ns = when;
      clock.intf[intf].frames = sample.frame;
      clock.intf[intf].start = when - sample.line * kLineNs;
      clock.intf[3 - intf].on = false;
      seeded = true;
    }
    clock.advanceTo(when);
    const auto measured = scanPosition(sample.frame, sample.line);
    const auto simulated = scanPosition(clock.intf[intf].frames, clock.line(intf));
    CHECK(simulated >= measured - 14 && simulated <= measured + 14);
  }
}

static void compareTraceBoundaries(const std::vector<TraceSample> &captured, const TraceRing &model)
{
  unsigned next = 0;
  for (const auto &s : captured) {
    if (!s.boundary) continue;
    while (next < model.size() && (!model.at(next).boundary || model.at(next).step != s.step)) ++next;
    CHECK(next < model.size());
    if (next == model.size()) return;
    const auto &m = model.at(next++);
    CHECK(m.active == s.active && m.top == s.top && m.flush == s.flush && m.intfFlush == s.intfFlush);
    for (unsigned l = 0; l < 2; ++l)
      CHECK(m.link[l].en == s.link[l].en && m.link[l].status == s.link[l].status);
  }
}

static void testY12pWakeTrace()
{
  auto file = openTrace("m5-y12p-wake.trace");
  readTraceLine(file, "m5 1 off ctl0 ok=1 step=5 restored=0 rows=144 dropped=0");
  readTraceLine(file, "m5 1 dcs 28 trig=1 done=3 us=1185 porch=1");
  readTraceLine(file, "m5 1 dcs 10 trig=1 done=3 us=1265 porch=1");
  const auto off = readTraceRows(file, 1, 144);
  readTraceLine(file, "m5 2 wake ctl0 ok=0 step=2 restored=0 rows=68 dropped=0");
  const auto wake = readTraceRows(file, 2, 68);
  if (off.size() != 144 || wake.size() != 68) return;
  CHECK(file.peek() == std::char_traits<char>::eof());
  for (unsigned l = 0; l < 2; ++l) {
    replayMovingCounters(off, l);
    replayMovingCounters(wake, l);
    CHECK(off.back().link[l].frame == wake.front().link[l].frame && off.back().link[l].line == wake.front().link[l].line);
  }

  model::Dpu m;
  m.blockDma = false;
  m.retainBusyVideo[0] = true;  // the DSI0 timeout observed in serial 2
  m.advanceTo(m.frameNs(1));
  const auto origin = m.ns;
  for (unsigned l = 0; l < 2; ++l) {
    m.intf[l + 1].frames = off.front().link[l].frame;
    m.intf[l + 1].start = m.ns - off.front().link[l].line * kLineNs;
    m.regs[dsiReg(l, kDsiCtrl)] = 0x1f7;
    m.regs[dsiReg(l, kDsiDmaLength)] = l ? 0x84 : 4;
    m.regs[intfReg(l + 1, kIntfConfig)] = kIntfConfigProgFetch;
    m.regs[intfReg(l + 1, kIntfProgFetch)] = m.intf[l + 1].fetch = 2290 * model::kHtotal;
  }
  const LivePath path = readPath(m);
  TraceRing ring;
  const Result slept = panelOff(m, path, Options(), &ring);
  CHECK(slept.ok && slept.enginesStopped && m.dsi[0].videoStuck);
  CHECK(slept.dcs[0].dmaDoneMask() == 3 && slept.dcs[1].dmaDoneMask() == 3);
  compareTraceBoundaries(off, ring);
  const Frames frozen = readFrames(m);
  m.advanceTo(origin + wake.front().us * 1000);
  const Frames after = readFrames(m);
  CHECK(frozen.f[0] == after.f[0] && frozen.f[1] == after.f[1]);
  const model::Dpu asleep = m;

  // Replay y12p's wake. Restarted timing does not clear the stuck video FSM.
  ring.reset();
  Trace legacy{&ring, 0};
  const Engines engines = startEngines(m, path, slept.fetch, legacy);
  CHECK(engines.ok && engines.rate.lockstep);
  legacy.boundary(m, TraceStep::sleepOut);
  const Sent failed = sendDcs(m, kDcsSleepOut, Options());
  legacy.boundary(m, TraceStep::end);
  CHECK(!failed.ok && failed.waitedBlanking && failed.triggerAttempted && !failed.done[0] && failed.doneUs >= kDmaDoneTimeoutUs);
  CHECK(m.dsi[0].dmaPending && !m.firstDcs(kDcsDisplayOn, 0));
  compareTraceBoundaries(wake, ring);
  const auto &dcs11 = wake[wake.size() - 2];
  CHECK(dcs11.step == TraceStep::sleepOut && wake.back().us - dcs11.us == 58801);

  m = asleep;
  ring.reset();
  const Result fixed = panelOn(m, path, Options(), slept.enginesStopped, slept.fetch, &ring);
  CHECK(fixed.ok && fixed.scanoutRestored && fixed.engines.rate.lockstep && (fixed.dsiBeforeReset[0] & kStatusVideoBusy));
  CHECK(fixed.dcs[0].dmaDoneMask() == 3 && fixed.dcs[1].dmaDoneMask() == 3 && !fixed.dcs[0].waitedBlanking && !fixed.dcs[1].waitedBlanking);
  const auto *d11 = m.firstDcs(kDcsSleepOut, 0), *d29 = m.firstDcs(kDcsDisplayOn, 1);
  CHECK(d11 && d29 && d11->ns + 120000000 <= d29->ns && d29->ns < m.startNs[1] && d29->ns < m.startNs[2]);
  for (unsigned l = 0; l < 2; ++l) {
    CHECK(m.storesTo(dsiReg(l, kDsiSwReset)) == 2 && !m.dsi[l].videoStuck && m.half[l].displayOn && !m.half[l].sleeping);
    CHECK(m.rd(dsiReg(l, kDsiCtrl)) == path.link[l].dsiCtrl &&
          m.rd(dsiReg(l, kDsiClkCtrl)) == 0x33f && m.rd(dsiReg(l, kDsiDmaLength)) == (l ? 0x84u : 4u));
  }
  CHECK(m.glitches == 0 && m.desync == 0 && m.commandsInFrame == 0 && clean(m));

  // A real DMA fault in either wake command must release presentation and retire the FIFO.
  for (unsigned budget : {0u, 1u}) {
    m = asleep;
    const unsigned link = budget;
    m.dmaBudget[link] = int(budget);
    const auto start = m.nowUs();
    const Result timedOut = panelOn(m, path, Options(), slept.enginesStopped, slept.fetch);
    CHECK(!timedOut.ok && timedOut.scanoutRestored && timedOut.step == 2 + budget);
    CHECK(timedOut.dcs[budget].triggerAttempted && timedOut.dcs[budget].dmaDoneMask() == (3u ^ (1u << link)));
    CHECK(m.nowUs() - start < kPanelReplyTimeoutMs * 1000u && m.intf[1].on && m.intf[2].on);
    if (!budget) CHECK(!m.firstDcs(kDcsDisplayOn, 0) && !m.firstDcs(kDcsDisplayOn, 1));
    for (unsigned l = 0; l < 2; ++l)
      CHECK(m.storesTo(dsiReg(l, kDsiSwReset)) == 4 && !m.dsi[l].dmaPending && !m.rd(dsiReg(l, kDsiTpgCtrl)) &&
            m.rd(dsiReg(l, kDsiCtrl)) == path.link[l].dsiCtrl && m.rd(dsiReg(l, kDsiClkCtrl)) == 0x33f &&
            m.rd(dsiReg(l, kDsiDmaLength)) == (l ? 0x84u : 4u));
    CHECK(m.glitches == 0 && clean(m));
    RequestGate gate;
    gate.ready = true;
    gate.latchFallback();
    auto log = [](const char *) { CHECK(false); };
    CHECK(!gate.admit(false, log) && !gate.admit(true, log));
  }
}

static void testSleepReplyTimeout()
{
  auto unexpectedLog = [](const char *) { CHECK(false); };
  RequestGate queued;
  queued.ready = true;
  int want = 0;
  queued.sleepTimedOut(want);
  CHECK(want == -1 && !queued.admit(false, unexpectedLog) && !queued.admit(true, unexpectedLog));
  CHECK(!queued.undoLateSleep(want, true));

  RequestGate dispatched;
  dispatched.ready = true;
  model::Dpu m;
  m.sleepMs(3);
  const LivePath path = readPath(m);
  CHECK(!dispatched.undoLateSleep(0, true));
  dispatched.sleepTimedOut(want);  // the worker already took the off request
  const Result off = panelOff(m, path, Options());
  CHECK(off.ok && dispatched.undoLateSleep(0, off.ok) && !dispatched.undoLateSleep(0, false));
  const Result on = panelOn(m, path, Options(), off.enginesStopped, off.fetch);
  CHECK(on.ok && on.scanoutRestored && !dispatched.undoLateSleep(1, true));
  CHECK(!dispatched.admit(false, unexpectedLog) && !dispatched.admit(true, unexpectedLog));
}

static void testFailures()
{
  {   // display-off never finishes on DSI1: stop there, DSI1's words left to its engine, nothing more
    model::Dpu m;
    m.dmaHang[1] = true;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    const Result r = panelOff(m, p, Options());
    CHECK(!r.ok && !r.safeAwake && r.step == 1 && std::strstr(r.why, "DSI1"));
    CHECK(r.dcs[0].dmaDoneMask() == 1 && r.dcs[0].ackBeforeMask == 3 && r.dcs[0].ackAfterMask == 1);
    CHECK(m.rd(dsiReg(1, kDsiTpgCtrl)) == 0x30006u && m.rd(dsiReg(0, kDsiTpgCtrl)) == 0);
    CHECK(m.storesTo(intfReg(1, kIntfEn)) == 0 && m.storesTo(intfReg(2, kIntfEn)) == 0 && m.intf[1].on);
  }
  {   // sleep-in never finishes on DSI0: no engine store, no undo on a link the engine still owns
    model::Dpu m;
    m.dmaBudget[0] = 1;
    m.sleepMs(3);
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 2 && !r.restored && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
  {   // INTF2 never drains once it has its own route. Keep the display dark.
    model::Dpu m;
    m.engineStuck[1] = true;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.step == 4 && !r.restored && std::strstr(r.why, "INTF2"));
    CHECK(!m.intf[1].on && m.intf[2].on && m.storesTo(ctlReg(0, kCtlSwReset)) == 1 && m.dcs.size() == 4);
  }
  {   // The selected video CTL must have INTF1 as its only timing master.
    model::Dpu m;
    m.regs[ctlReg(0, kCtlIntfMaster)] = kDsiIntfSlave;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.safeAwake && std::strstr(r.why, "master") && m.stores.empty());
  }
  {   // Fence any in-flight submission before blanking the panel or stopping timing.
    model::Dpu m;
    m.ctlFlush = kFlushDspp;
    m.flushStuck = true;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.safeAwake && std::strstr(r.why, "drain") && m.stores.empty());
  }
  {   // A stuck reset cannot consume a route submission.
    model::Dpu m;
    m.ctlResetStuck = true;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && !r.restored && std::strstr(r.why, "reset") && m.dcs.size() == 4);
    CHECK(m.storesTo(ctlReg(0, kCtlIntfActive)) == 0 && m.storesTo(ctlReg(0, kCtlStart)) == 0);
  }
  {   // A handoff without a latched START must neither stop the slave nor run wake DCS.
    model::Dpu m;
    m.dropStore = ctlReg(0, kCtlStart);
    m.dropValue = m.dropCount = 1;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && !r.restored && std::strstr(r.why, "handoff did not flush") && m.dcs.size() == 4);
    CHECK(m.latchedMaster == kDsiIntfMaster && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
  {   // Fetch failed after the handoff. Undo restores both the route and saved fetch.
    model::Dpu m;
    m.regs[intfReg(2, kIntfConfig)] = kIntfConfigProgFetch;
    m.intf[2].fetch = m.regs[intfReg(2, kIntfProgFetch)] = 2290 * model::kHtotal;
    m.dropStore = intfReg(2, kIntfConfig);
    m.dropValue = 0;
    m.dropCount = 1;
    const Result r = panelOff(m, readPath(m), Options());
    CHECK(!r.ok && r.restored && std::strstr(r.why, "programmable fetch") && m.dcs.size() == 8);
    CHECK(m.latchedMaster == kDsiIntfMaster && readPath(m).ok && m.half[0].displayOn && clean(m));
  }
  {   // the engines do not start after the command-only wake
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok);
    m.engineDead[0] = m.engineDead[1] = true;
    const size_t dcs = m.dcs.size();
    const Result r = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(!r.ok && r.step == 4 && !r.scanoutRestored && std::strstr(r.why, "running") && m.dcs.size() == dcs + 4);
  }
  {   // the bonded wake route never latches
    model::Dpu m;
    const LivePath p = readPath(m);
    const Result off = panelOff(m, p, Options());
    CHECK(off.ok);
    m.flushStuck = true;
    const size_t dcs = m.dcs.size();
    const Result wake = panelOn(m, p, Options(), off.enginesStopped, off.fetch);
    CHECK(!wake.ok && std::strstr(wake.why, "flush") && m.dcs.size() == dcs + 4 && !m.intf[1].on && !m.intf[2].on);
  }
  {   // DCS only: the engines keep running
    model::Dpu m;
    m.sleepMs(3);
    const LivePath p = readPath(m);
    Options o;
    o.engines = false;
    const Result off = panelOff(m, p, o);
    CHECK(off.ok && !off.enginesStopped && m.intf[1].on && m.half[0].sleeping && m.storesTo(intfReg(1, kIntfEn)) == 0);
    const Result on = panelOn(m, p, o, off.enginesStopped, off.fetch);
    CHECK(on.ok && m.half[0].displayOn && !m.half[1].sleeping && m.storesTo(intfReg(2, kIntfEn)) == 0);
  }
}

static void testBacklightOnlyAndSupersededWake()
{
  using namespace gopcompose;
  PanelPower power;
  Slot slot;
  Job frame;
  frame.kind = Job::kFull;
  frame.flip = true;
  frame.serial = 42;
  CHECK(offer(slot, frame, nullptr) == Offer::Taken);
  bool on;
  CHECK(power.request(false) && power.begin(on) && !on);
  power.finish(false);
  CHECK(power.request(true) && power.begin(on) && on);
  power.finish(true);  // error restored through the backlight path
  CHECK(power.canPresent() && power.wakeComplete && slot.job.serial == 42);
  CHECK(power.request(false) && power.request(true) && power.begin(on) && on);
  CHECK(power.request(false));   // a new sleep arrived while the wake was in progress
  power.finish(true);
  CHECK(!power.canPresent() && !power.wakeComplete && slot.job.serial == 42);
  CHECK(power.begin(on) && !on);
  power.finish(false);    // a clean refusal of the hardware sleep
  CHECK(power.request(true) && power.begin(on) && on);
  power.finish(true);
  Busy busy;
  Job resumed;
  CHECK(power.canPresent() && power.wakeComplete && takeJob(slot, busy, &resumed) && resumed.serial == 42 && resumed.flip);
}

int main()
{
  testPacket();
  testSend();
  testRefusals();
  testSleepWake();
  testZeroCounterReadback();
  testUnavailablePanel();
  testStopTimeoutTrace();
  testY12oTrace();
  testY12pWakeTrace();
  testSleepReplyTimeout();
  testFailures();
  testBacklightOnlyAndSupersededWake();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu panel host tests: all passed\n");
  return 0;
}
