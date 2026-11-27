// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared host fixtures for serialized HPD and D6 scanout.
#pragma once
#include "DpHotplug.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pianodp::test {
using Aux = DpAuxStatus;
using Pattern = DpTrainingPattern;
using Hotplug = DpHotplugStatus;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

using Block = std::array<uint8_t, kEdidBlockBytes>;
using Descriptor = std::array<uint8_t, 18>;
constexpr Descriptor kUhd60{0x08, 0xe8, 0x00, 0x30, 0xf2, 0x70, 0x5a, 0x80, 176, 88, 0x8a, 0, 0, 0, 0, 0, 0, 0x1e};
constexpr Descriptor kHd60{0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 88, 44, 0x45, 0, 0, 0, 0, 0, 0, 0x1e};

inline void checksum(Block &block, size_t first, size_t end)
{
  unsigned sum = 0;
  for (size_t i = first; i < end; ++i) sum += block[i];
  block[end] = uint8_t(0u - sum);
}

inline std::vector<uint8_t> makeEdid(const Descriptor &preferred, std::initializer_list<Block> extensions = {})
{
  Block base{0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0x52, 0x74}; // synthetic TST manufacturer
  base[18] = 1;
  base[19] = 4;
  base[20] = 0xa5; // digital DP, 8 bpc
  base[24] = 2;    // preferred detailed timing
  std::copy(preferred.begin(), preferred.end(), base.begin() + 54);
  for (size_t offset = 72; offset <= 108; offset += 18) base[offset + 3] = 0x10; // dummy descriptors
  base[126] = uint8_t(extensions.size());
  checksum(base, 0, 127);
  std::vector<uint8_t> bytes(base.begin(), base.end());
  for (Block block : extensions) {
    checksum(block, 0, 127);
    bytes.insert(bytes.end(), block.begin(), block.end());
  }
  return bytes;
}

// CTA SVDs deliberately advertise 1080p60 and UHD24 before UHD30 and UHD60.
constexpr Block kCta{2, 3, 9, 0, 0x44, 16, 93, 95, 97};

inline Block displayId(unsigned columns = 0)
{
  // DisplayID 2.0 Type VII, 2560x1440@60 with a 241.5 MHz clock.
  Block block{0x70, 0x20, 23, 3, 0, 0x22, 2, 20, 0x5b, 0xaf, 3, 0,
              0xff, 9, 159, 0, 47, 0x80, 31, 0, 0x9f, 5, 40, 0, 2, 0, 4, 0};
  if (columns) {
    block[2] += 25;
    const uint8_t tile[]{0x28, 0, 22, 0x80, uint8_t((columns - 1) << 4), 0, 0, 0xff, 9, 0x9f, 5};
    std::copy(std::begin(tile), std::end(tile), block.begin() + 28);
  }
  checksum(block, 1, 5 + block[2]);
  return block;
}

struct DpuClaims {
  unsigned ctl, intf, dsc;
  bool operator==(const DpuClaims &) const = default;
};

// NT36532 is bonded on INTF1/2 with two DSC encoders. SST uses INTF0 and no DSC.
constexpr DpuClaims kPanelPath{0x1, 0x6, 0x3};
constexpr DpuClaims kBothPaths{0x3, 0x7, 0x3};
constexpr DpuClaims kDpPath{0x2, 1u << kIntfDp, 0};

struct Framebuffer {
  unsigned generation, width, height, lastSwap = 0;
};

struct Flip {
  unsigned generation, swap;
  bool operator==(const Flip &) const = default;
};

struct Completion {
  Flip flip;
  bool presented;
  bool operator==(const Completion &) const = default;
};

struct Scanout {
  std::shared_ptr<Framebuffer> framebuffer;
  std::deque<unsigned> queued;
  std::optional<Flip> pending;
  std::vector<Completion> completed;

  bool submit(unsigned swap)
  {
    if (!framebuffer) return false;
    queued.push_back(swap);
    return true;
  }

  Flip begin()
  {
    CHECK(framebuffer && !queued.empty() && !pending);
    pending = Flip{framebuffer->generation, queued.front()};
    queued.pop_front();
    return *pending;
  }

  bool present(Flip flip)
  {
    if (!framebuffer || pending != flip) return false;
    framebuffer->lastSwap = flip.swap;
    completed.push_back({flip, true});
    pending.reset();
    return true;
  }

  void detach()
  {
    if (!framebuffer) return;
    if (pending) completed.push_back({*pending, false});
    for (unsigned swap : queued) completed.push_back({{framebuffer->generation, swap}, false});
    pending.reset();
    queued.clear();
    framebuffer.reset();
  }
};

struct Notice {
  unsigned generation;
  bool connected;
  DpuClaims claims;
  bool framebuffer, registered, linkPowered;
  size_t outstanding;
  bool operator==(const Notice &) const = default;
};

// One serialized work loop with a virtual clock. Raw HPD can change during an AUX delay
// without re-entering DpHotplug. A low level gates AUX immediately and takes priority
// over queued IRQ work. Only a new high level needs to debounce.
class Handoff {
public:
  static constexpr unsigned kDebounceMs = 100;
  enum class Sleep : unsigned { Display = 1, System = 2 };

  DpHotplug port;
  Scanout panel{.framebuffer = std::make_shared<Framebuffer>(Framebuffer{1, 3200, 2136}),
                .queued = {}, .pending = {}, .completed = {}};
  Scanout external;
  DpuClaims claims = kPanelPath;
  std::vector<Notice> notices;
  std::vector<uint8_t> sinkEdid = makeEdid(kHd60);
  uint32_t maxTrainedRate = kDpHbr2Khz;
  unsigned maxTrainedLanes = 4;
  Pattern unplugDuring = Pattern::Disabled;
  bool healthy = true, failStart = false;

  unsigned probes = 0, edidReads = 0, phyAttempts = 0, streamStarts = 0, teardowns = 0;
  unsigned irqReads = 0, trainingReads = 0, blockedAux = 0, registrations = 0, removals = 0;
  bool auxOpen = false;
  DpLink phy{};
  Pattern pattern = Pattern::Disabled;
  std::optional<unsigned> registration;
  DpExternalTiming selected{};

  void signal(bool high, bool irq = false, uint8_t pin = 3)
  {
    std::array<uint8_t, kNotifyBytes> wire{0x0c, 0x80, 0, 0, 0, 0, 0, 0, 0x16, 0, 1, 0xff};
    wire[14] = pin == 4 ? kMuxUsb3Dp : kMuxDp4;
    wire[20] = uint8_t(pin | (high ? 0x40 : 0) | (irq ? 0x80 : 0));
    const PortEvent event = decodePortEvent(wire.data(), wire.size());
    CHECK(event.valid && event.dp && event.hpd == high && event.irq == irq);
    const PortStep step = portStep(pins, event);
    if (!pins.hpd || (step.changes & kChPin)) {
      disconnectPending = true;
      plugDue.reset();
      irqPending = false;
    }
    if (sleepMask || !pins.hpd) return;
    if (step.changes & (kChHpdUp | kChPin)) plugDue = nowMs + kDebounceMs;
    if ((step.changes & kChIrqHpd) && external.framebuffer && !disconnectPending) irqPending = true;
  }

  void advance(unsigned ms)
  {
    nowMs += ms;
    run();
  }

  void run()
  {
    if (std::exchange(disconnectPending, false)) port.hpd(*this, false, false, 0);
    if (sleepMask || !pins.hpd) return;

    const bool plug = plugDue && *plugDue <= nowMs;
    const bool irq = std::exchange(irqPending, false);
    if (!plug && !irq) return;
    plugDue.reset();
    const Hotplug status = port.hpd(*this, true, irq, plugPlan(pins).lanes);
    if (!pins.hpd) {
      disconnectPending = false;
      port.hpd(*this, false, false, 0);
    } else if (status == Hotplug::Active && !external.framebuffer) {
      publish();
    }
  }

  void power(Sleep domain, bool sleeping)
  {
    const unsigned bit = unsigned(domain);
    if (sleeping) {
      plugDue.reset();
      irqPending = disconnectPending = false;
      port.hpd(*this, false, false, 0);
      sleepMask |= bit; // suspend may proceed once the external link is parked
    } else if (sleepMask & bit) {
      sleepMask &= ~bit;
      if (!sleepMask && pins.hpd) plugDue = nowMs + kDebounceMs;
    }
  }

  bool vblank(Flip flip)
  {
    return pins.hpd && registration == flip.generation && external.present(flip);
  }

  Aux readEdid(size_t offset, uint8_t *bytes, size_t size)
  {
    if (!pins.hpd) return Aux::Nack;
    CHECK(auxOpen && !phy.lanes && offset == edidBytes && size == kDpEdidAuxChunk);
    ++edidReads;
    if (offset > sinkEdid.size() || size > sinkEdid.size() - offset) return Aux::ShortTransfer;
    std::copy_n(sinkEdid.data() + offset, size, bytes);
    edidBytes += size;
    return Aux::Ack;
  }

  Aux auxRead(uint32_t address, uint8_t *bytes, size_t size)
  {
    if (!pins.hpd) {
      ++blockedAux;
      return Aux::Nack;
    }
    if (address == 0 && size == kDpReceiverCapBytes) {
      CHECK(!auxOpen && !edidBytes && !sleepMask);
      ++probes;
      auxOpen = true;
      const std::array<uint8_t, kDpReceiverCapBytes> caps{0x14, 0x1e, 0x84, 0, 0, 0, 1}; // HBR3, 4 lanes
      std::copy(caps.begin(), caps.end(), bytes);
      return Aux::Ack;
    }
    CHECK(auxOpen);
    if (address == 0x202 && size == 6) {
      CHECK(pattern != Pattern::Disabled && phy.lanes && edidBytes == sinkEdid.size());
      ++trainingReads;
      const bool locked = phy.rateKhz <= maxTrainedRate && phy.lanes <= maxTrainedLanes;
      const uint8_t lanes = !locked ? 0 : pattern == Pattern::Tps1 ? 0x11 : 0x77;
      const uint8_t status[]{lanes, lanes, uint8_t(locked), 0, 0, 0};
      std::copy(std::begin(status), std::end(status), bytes);
      return Aux::Ack;
    }
    CHECK(address == 0x200 && size == 8 && external.framebuffer && pattern == Pattern::Disabled);
    ++irqReads;
    const uint8_t irq[]{1, 4, 0x77, 0x77, uint8_t(healthy ? 0x81 : 0x80), 0, 0, 0};
    std::copy(std::begin(irq), std::end(irq), bytes);
    return Aux::Ack;
  }

  Aux auxWrite(uint32_t address, const uint8_t *bytes, size_t size)
  {
    CHECK(auxOpen && phy.lanes);
    if (!pins.hpd) {
      CHECK(address == 0x102 && size == 1 && bytes[0] == 0); // retire the sink pattern if AUX is reachable
      ++blockedAux;
      return Aux::Nack;
    }
    if (address == 0x100) {
      CHECK(size == 2 && bytes[0] * 27000u == phy.rateKhz && (bytes[1] & 0x1fu) == phy.lanes);
    } else if (address == 0x107) {
      CHECK(size == 2 && bytes[1] == 1); // 8b/10b
    } else {
      CHECK(address == 0x102 && (size == 1 || size == 1 + phy.lanes));
    }
    return Aux::Ack;
  }

  bool setPhy(uint32_t rate, unsigned lanes, unsigned swing, unsigned preemphasis)
  {
    CHECK(auxOpen && pins.hpd && !swing && !preemphasis && pattern == Pattern::Disabled);
    CHECK(validDpLink({rate, lanes}) && rate <= kDpHbr2Khz && lanes <= plugPlan(pins).lanes);
    ++phyAttempts;
    phy = {rate, lanes};
    return true;
  }

  void setPattern(Pattern next) { pattern = next; }

  void delayUs(unsigned)
  {
    if (unplugDuring != Pattern::Disabled && pattern == unplugDuring) {
      unplugDuring = Pattern::Disabled;
      signal(false, true);
    }
  }

  bool startStream(DpLink link, const DpExternalTiming &mode)
  {
    // Reserve the path for the admitted stream. The publisher chooses its
    // initial timing and exposes the framebuffer in this same work-loop turn.
    CHECK(mode.pixelHz && pattern == Pattern::Disabled && pins.hpd);
    CHECK(link.rateKhz == phy.rateKhz && link.lanes == phy.lanes);
    ++streamStarts;
    if ((claims.ctl & kDpPath.ctl) || (claims.intf & kDpPath.intf) || (claims.dsc & kDpPath.dsc))
      return false;
    claims = {claims.ctl | kDpPath.ctl, claims.intf | kDpPath.intf, claims.dsc | kDpPath.dsc};
    ownsPath = true;
    return !failStart;
  }

  void teardown()
  {
    CHECK(auxOpen && !sleepMask && pattern == Pattern::Disabled);
    ++teardowns;
    const auto old = registration;
    if (registration) ++removals;
    registration.reset();
    external.detach();
    if (ownsPath) {
      claims = {claims.ctl & ~kDpPath.ctl, claims.intf & ~kDpPath.intf, claims.dsc & ~kDpPath.dsc};
      ownsPath = false;
    }
    selected = {};
    phy = {};
    auxOpen = false;
    edidBytes = 0;
    if (old) record(*old, false);
  }

private:
  PortState pins;
  unsigned nowMs = 0, sleepMask = 0, nextGeneration = 2;
  std::optional<unsigned> plugDue;
  bool irqPending = false, disconnectPending = false, ownsPath = false;
  size_t edidBytes = 0;

  void record(unsigned generation, bool connected)
  {
    notices.push_back({generation, connected, claims, bool(external.framebuffer), registration.has_value(), auxOpen || phy.lanes,
                       external.queued.size() + external.pending.has_value()});
  }

  void publish()
  {
    CHECK(ownsPath && !registration && edidBytes == sinkEdid.size() && port.modes().count);
    // The publisher has the same EDID bytes. Use the base preferred timing if
    // admitted. For fallback, choose the largest raster, then the highest refresh.
    EdidInfo edid;
    CHECK(parseEdid(sinkEdid.data(), edidBytes, edid) == EdidStatus::Ok);
    edid.modeCount = 1;
    const DpExternalModes preferred = buildDpExternalModes(edid, port.link(), DpPixelFormat::Rgb8);
    const DpExternalModes &modes = port.modes();
    selected = modes.modes[0];
    if (!preferred.count) {
      const auto rank = [](const DpExternalTiming &t) {
        return std::pair{uint32_t(t.horizontal.active) * t.vertical.active, t.refresh16_16};
      };
      for (size_t i = 1; i < modes.count; ++i)
        if (rank(modes.modes[i]) > rank(selected)) selected = modes.modes[i];
    } else {
      CHECK(selected == preferred.modes[0]);
    }
    const unsigned generation = nextGeneration++;
    external.framebuffer = std::make_shared<Framebuffer>(
        Framebuffer{generation, selected.horizontal.active, selected.vertical.active});
    registration = generation;
    ++registrations;
    record(generation, true);
  }
};

inline void parked(const Handoff &h, Hotplug status = Hotplug::Disconnected, DpuClaims claims = kPanelPath)
{
  CHECK(h.port.status() == status && !h.port.link().lanes && !h.port.link().rateKhz);
  CHECK(!h.port.modes().count && !h.port.modes().timingId(0));
  CHECK(!h.auxOpen && !h.phy.lanes && !h.phy.rateKhz && h.pattern == Pattern::Disabled && !h.selected.pixelHz);
  CHECK(h.claims == claims && !h.registration && h.registrations == h.removals);
  CHECK(!h.external.framebuffer && !h.external.pending && h.external.queued.empty());
}

inline void plug(Handoff &h, uint8_t pin = 3)
{
  h.signal(true, false, pin);
  h.advance(Handoff::kDebounceMs);
}

inline void unplug(Handoff &h)
{
  h.signal(false, true);
  h.run();
}

inline void active(const Handoff &h, uint32_t rate, unsigned lanes, unsigned width, unsigned height, unsigned pixelHz)
{
  CHECK(h.port.status() == Hotplug::Active && h.port.link().rateKhz == rate && h.port.link().lanes == lanes);
  CHECK(h.claims == kBothPaths && h.selected.pixelHz == pixelHz);
  CHECK(h.selected.horizontal.active == width && h.selected.vertical.active == height);
  CHECK(h.external.framebuffer && h.external.framebuffer->width == width && h.external.framebuffer->height == height);
  const unsigned generation = h.external.framebuffer->generation;
  CHECK(h.registration == generation && h.registrations == h.removals + 1);
  CHECK(!h.notices.empty() && (h.notices.back() == Notice{generation, true, kBothPaths, true, true, true, 0}));
}

} // namespace pianodp::test
