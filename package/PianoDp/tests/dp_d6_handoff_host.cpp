// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. dp_d6_handoff_host.cpp -o /tmp/dp_d6_handoff_host && /tmp/dp_d6_handoff_host
// D6 handoff model. AUX, the work loop, scanout, and IOMFB callbacks are simulated.
// DpHotplug supplies the EDID, link training, IRQ recovery, and timing admission.
#include "DpHandoffModel.h"

namespace {
using namespace pianodp;
using namespace pianodp::test;

void testDebounceAndIrqStorm()
{
  Handoff h;
  const auto panel = h.panel.framebuffer;
  CHECK(h.panel.submit(1) && h.panel.submit(2));
  const Flip panelFlip = h.panel.begin();

  h.signal(true);
  h.advance(Handoff::kDebounceMs - 1);
  CHECK(!h.probes);
  unplug(h); // bounce just before the first deadline
  h.signal(true);
  for (unsigned i = 0; i < 1024; ++i) h.signal(true, true);
  h.advance(Handoff::kDebounceMs - 1);
  CHECK(!h.probes && !h.irqReads && h.notices.empty());
  h.advance(1);
  active(h, kDpHbr2Khz, 4, 1920, 1080, 148500000);
  CHECK(h.probes == 1 && h.phyAttempts == 1 && h.edidReads == 8);

  CHECK(h.external.submit(1) && h.external.submit(2) && h.external.submit(3));
  CHECK(h.vblank(h.external.begin())); // normal completion
  const Flip old = h.external.begin();
  const std::weak_ptr<Framebuffer> retired = h.external.framebuffer;
  for (unsigned turn = 1; turn <= 3; ++turn) {
    for (unsigned i = 0; i < 1024; ++i) h.signal(true, true);
    h.run();
    CHECK(h.irqReads == turn && h.probes == 1 && h.streamStarts == 1 && !h.teardowns);
    CHECK(h.external.pending == old && h.notices.size() == 1 && h.port.lastIrq().linkHealthy);
  }

  for (unsigned i = 0; i < 1024; ++i) h.signal(true, true);
  h.signal(false, true); // HPD-low wins over the queued IRQ and the in-flight external flip
  CHECK(!h.vblank(old));
  h.run();
  parked(h);
  CHECK(h.irqReads == 3 && h.teardowns == 1 && h.notices.size() == 2 && retired.expired());
  CHECK((h.notices.back() == Notice{old.generation, false, kPanelPath, false, false, false, 0}));
  CHECK((h.external.completed == std::vector<Completion>{
      {{old.generation, 1}, true}, {old, false}, {{old.generation, 3}, false}}));
  CHECK(!h.vblank(old) && !h.external.submit(4));
  CHECK(h.panel.framebuffer == panel && h.panel.pending == panelFlip && h.panel.completed.empty());
  CHECK(h.panel.present(panelFlip) && h.panel.present(h.panel.begin()) && panel->lastSwap == 2);

  for (unsigned i = 0; i < 1024; ++i) h.signal(false, true);
  h.run();
  CHECK(h.teardowns == 1 && h.notices.size() == 2 && h.external.completed.size() == 3);
  plug(h);
  CHECK(h.external.submit(old.swap));
  const Flip fresh = h.external.begin();
  CHECK(fresh.generation != old.generation && !h.vblank(old) && h.vblank(fresh));

  // A storm with lost alignment still services one IRQ and makes one fresh link.
  h.healthy = false;
  CHECK(h.external.submit(4));
  const Flip interrupted = h.external.begin();
  for (unsigned i = 0; i < 1024; ++i) h.signal(true, true);
  h.run();
  active(h, kDpHbr2Khz, 4, 1920, 1080, 148500000);
  CHECK(h.irqReads == 4 && h.probes == 3 && h.streamStarts == 3 && h.teardowns == 2);
  CHECK(h.notices.size() == 5 && h.notices[3].generation == interrupted.generation && !h.notices[3].connected);
  CHECK((h.external.completed.back() == Completion{interrupted, false}) && !h.vblank(interrupted));
  CHECK(h.panel.framebuffer == panel && panel->lastSwap == 2 && h.panel.completed.size() == 2);
  unplug(h);
  parked(h);
}

void testTrainingUnplugAndStreamRollback()
{
  for (Pattern phase : {Pattern::Tps1, Pattern::Tps2}) {
    Handoff h;
    CHECK(h.panel.submit(1));
    const Flip panelFlip = h.panel.begin();
    h.unplugDuring = phase;
    plug(h);
    parked(h);
    CHECK(h.probes == 1 && h.edidReads == 8 && h.phyAttempts == 1 && !h.streamStarts);
    CHECK(h.trainingReads == (phase == Pattern::Tps1 ? 0u : 1u) && h.blockedAux == 2);
    CHECK(h.teardowns == 1 && h.notices.empty() && h.external.completed.empty());
    CHECK(h.panel.present(panelFlip));
    plug(h);
    active(h, kDpHbr2Khz, 4, 1920, 1080, 148500000);
    unplug(h);
    parked(h);
  }

  Handoff h;
  constexpr DpuClaims busyIntf{kPanelPath.ctl, kBothPaths.intf, kPanelPath.dsc};
  h.claims = busyIntf; // another client owns INTF0
  plug(h);
  parked(h, Hotplug::StreamFailed, busyIntf);
  unplug(h);
  h.claims = kPanelPath;
  h.failStart = true; // failure after reserving CTL1 and INTF0
  plug(h);
  parked(h, Hotplug::StreamFailed);
  CHECK(h.streamStarts == 2 && h.teardowns == 2 && h.notices.empty() && !h.registrations);
  unplug(h);
  h.failStart = false;
  plug(h);
  active(h, kDpHbr2Khz, 4, 1920, 1080, 148500000);
  unplug(h);
  parked(h);
}

void testModeAdmission()
{
  Handoff h;
  h.sinkEdid = makeEdid(kUhd60, {kCta, displayId()});
  plug(h);
  active(h, kDpHbr2Khz, 4, 3840, 2160, 594000000);
  CHECK(h.port.modes().count == 5); // duplicate UHD60 DTD and VIC share a timing
  unplug(h);

  plug(h, 4); // pin D leaves two lanes for DP
  active(h, kDpHbr2Khz, 2, 3840, 2160, 297000000);
  CHECK(h.port.modes().count == 4 && h.port.modes().modes[0].pixelHz == 241500000);
  CHECK(h.port.modes().modes[1].pixelHz == 148500000 && h.port.modes().modes[2].refresh16_16 == (24u << 16));
  CHECK(h.selected == h.port.modes().modes[3] && h.selected.refresh16_16 == (30u << 16));
  unplug(h);

  h.maxTrainedLanes = 2;
  h.maxTrainedRate = kDpHbrKhz;
  const unsigned attempts = h.phyAttempts;
  plug(h); // four lanes routed, but the sink trains at HBR on two lanes
  active(h, kDpHbrKhz, 2, 1920, 1080, 148500000);
  CHECK(h.phyAttempts == attempts + 5 && h.port.modes().count == 1);
  unplug(h);

  h.maxTrainedRate = kDpHbr2Khz;
  h.sinkEdid = makeEdid(kUhd60, {displayId(1)});
  plug(h, 4);
  active(h, kDpHbr2Khz, 2, 2560, 1440, 241500000);
  CHECK(h.port.modes().count == 1);
  unplug(h);

  const size_t notices = h.notices.size();
  const unsigned starts = h.streamStarts;
  h.sinkEdid = makeEdid(kUhd60);
  plug(h, 4);
  parked(h, Hotplug::NoMode); // preferred timing exceeds the actual budget, with no fallback
  unplug(h);
  h.maxTrainedLanes = 4;
  h.sinkEdid = makeEdid(kHd60, {kCta, displayId(2)});
  plug(h);
  parked(h, Hotplug::NoMode); // multi-tile: reject the entire SST publication, even the 1080p DTD
  CHECK(h.notices.size() == notices && h.streamStarts == starts);
}

void testSleepAndWake()
{
  using Sleep = Handoff::Sleep;
  Handoff h;
  const auto panel = h.panel.framebuffer;
  for (Sleep domain : {Sleep::Display, Sleep::System}) {
    h.sinkEdid = makeEdid(kHd60, {kCta});
    plug(h);
    active(h, kDpHbr2Khz, 4, 1920, 1080, 148500000); // admitted base DTD stays preferred over UHD
    CHECK(h.panel.submit(1) && h.external.submit(1) && h.external.submit(2));
    const Flip panelFlip = h.panel.begin();
    const Flip stale = h.external.begin();
    const std::weak_ptr<Framebuffer> retired = h.external.framebuffer;
    const size_t completions = h.external.completed.size();
    h.signal(true, true); // sleep must cancel the pending IRQ
    h.power(domain, true);
    parked(h);
    CHECK(retired.expired() && !h.vblank(stale));
    CHECK((h.notices.back() == Notice{stale.generation, false, kPanelPath, false, false, false, 0}));
    CHECK(h.external.completed.size() == completions + 2);
    CHECK((h.external.completed[completions] == Completion{stale, false}));
    CHECK((h.external.completed.back() == Completion{{stale.generation, 2}, false}));

    const unsigned probes = h.probes, reads = h.edidReads, teardowns = h.teardowns;
    const size_t notices = h.notices.size();
    h.power(Sleep::System, true);
    h.advance(5 * Handoff::kDebounceMs);
    CHECK(h.teardowns == teardowns && h.notices.size() == notices && !h.irqReads);
    if (domain == Sleep::Display) {
      h.power(Sleep::Display, false); // system sleep is still holding the link down
      h.advance(2 * Handoff::kDebounceMs);
      CHECK(h.probes == probes);
    }
    h.sinkEdid = makeEdid(kUhd60, {displayId()});
    h.signal(true, true, 4); // current pin assignment can change while asleep
    h.power(Sleep::System, false);
    h.advance(Handoff::kDebounceMs - 1);
    CHECK(h.probes == probes && h.edidReads == reads && h.notices.size() == notices);
    h.advance(1);
    active(h, kDpHbr2Khz, 2, 2560, 1440, 241500000);
    CHECK(h.probes == probes + 1 && h.edidReads == reads + 16 && !h.irqReads);
    CHECK(h.external.submit(stale.swap));
    const Flip fresh = h.external.begin();
    CHECK(fresh.generation != stale.generation && !h.vblank(stale) && h.vblank(fresh));
    CHECK(h.panel.framebuffer == panel && h.panel.present(panelFlip));
    unplug(h);
    parked(h);
  }

  h.signal(true);
  h.advance(Handoff::kDebounceMs - 1);
  h.power(Sleep::System, true); // sleep cancels a pending plug, too
  h.advance(1);
  h.signal(false, true);
  const unsigned probes = h.probes;
  h.power(Sleep::System, false);
  h.advance(2 * Handoff::kDebounceMs);
  parked(h);
  CHECK(h.probes == probes);

  plug(h);
  h.power(Sleep::Display, true);
  const size_t notices = h.notices.size();
  h.sinkEdid[127] ^= 1; // fresh EDID is corrupt, so the old mode must not reappear
  h.power(Sleep::Display, false);
  h.advance(Handoff::kDebounceMs);
  parked(h, Hotplug::BadEdid);
  CHECK(h.probes == probes + 2 && h.notices.size() == notices);
}
} // namespace

int main()
{
  testDebounceAndIrqStorm();
  testTrainingUnplugAndStreamRollback();
  testModeAdmission();
  testSleepAndWake();
  std::puts("PianoDp D6 handoff host tests: passed");
}
