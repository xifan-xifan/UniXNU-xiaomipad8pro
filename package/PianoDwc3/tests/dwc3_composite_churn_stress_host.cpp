// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline composite USB traffic against the portable gadget, byte rings, HID parser and handoff/sleep policies.
// Build: c++ -std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.. dwc3_composite_churn_stress_host.cpp -o /tmp/dwc3_composite_churn_stress_host && /tmp/dwc3_composite_churn_stress_host
#include "ByteRing.h"
#include "Dwc3FakeHal.h"
#include "HidInput.h"
#include "UsbHandoff.h"
#include "UsbSleep.h"

#include <array>
#include <cstdlib>
#include <string_view>

using namespace pianousb;
using namespace pianousb::dwc3;
using namespace pianousb::ncm;
using pianousb::test::FakeHal;
using pianousb::test::setupBytes;

namespace {

unsigned cycle;
const char *phase = "start";
#define CHECK(c) do { if (!(c)) { \
  std::fprintf(stderr, "cycle %u, %s, line %d: %s\n", cycle, phase, __LINE__, #c); std::abort(); \
} } while (0)

constexpr unsigned kComm = 2 * kPorts, kData = kComm + 1;
constexpr unsigned bulk(unsigned port, bool in) { return physOf(bulkEp(port), in); }
constexpr unsigned kConsoleIn = bulk(kPortConsole, true), kPipeIn = bulk(kPortPipe, true), kPipeOut = bulk(kPortPipe, false),
                   kHidIn = bulk(kPortInput, true), kHidOut = bulk(kPortInput, false);
constexpr std::array kTraffic{kNetInPhys, kNetOutPhys, kConsoleIn, kPipeIn, kPipeOut, kHidOut};

const uint8_t *bytes(std::string_view s) { return reinterpret_cast<const uint8_t *>(s.data()); }
uint8_t *bytes(std::string &s) { return reinterpret_cast<uint8_t *>(s.data()); }

template <unsigned Size>
struct PortRing {
  std::vector<uint8_t> storage = std::vector<uint8_t>(Size);
  ByteRing ring{storage.data(), Size, UINT32_MAX - 31, UINT32_MAX - 31, 0};

  unsigned put(std::string_view data) { return ring.put(bytes(data), uint32_t(data.size())); }
};

struct CompositeHal : FakeHal {
  PortRing<256 * 1024> console;
  PortRing<64 * 1024> pipeIn, pipeOut;
  PortRing<4096> hidIn;
  PortRing<16 * 1024> hidOut;
  std::vector<uint8_t> txFrames = std::vector<uint8_t>(64 * 1024);
  FrameRing frames{txFrames.data(), uint32_t(txFrames.size())};
  TxGate gate;

  // Controller-side accounting. A retired descriptor may still have its HWO bit
  // set after ENDTRANSFER, but the resource must no longer reference the slot.
  struct Transfers { unsigned started = 0, completed = 0, ended = 0; };
  std::array<Transfers, kPhys> transfers{};

  uint32_t rd(uint32_t offset)
  {
    CHECK(roleOf(regs[GCTL]) == ROLE_DEVICE || offset == GCTL);
    return FakeHal::rd(offset);
  }

  void wr(uint32_t offset, uint32_t value)
  {
    CHECK(roleOf(regs[GCTL]) == ROLE_DEVICE);
    const bool command = offset >= depcmdBase(0) && offset < depcmdBase(kPhys) && (offset & 15) == 12;
    const unsigned phys = command ? (offset - depcmdBase(0)) / 16 : 0;
    const bool active = eps[phys].active;
    if (offset == DCTL && (value & DCTL_CSFTRST)) checkActive(0);
    FakeHal::wr(offset, value);
    if (command && (value & 15) == CMD_STARTTRANSFER && eps[phys].active && !active) {
      ++transfers[phys].started;
      checkDma(phys);
    }
    if (command && (value & 15) == CMD_ENDTRANSFER && active && !eps[phys].active) ++transfers[phys].ended;
  }

  void checkActive(uint32_t mask)
  {
    for (unsigned phys = 0; phys < kPhys; ++phys) {
      const auto &t = transfers[phys];
      const bool expected = (mask >> phys) & 1;
      CHECK(eps[phys].active == expected);
      CHECK(t.started == t.completed + t.ended + unsigned(expected));
      if (expected) checkDma(phys);
    }
  }

  void checkDma(unsigned phys)
  {
    CHECK(eps[phys].trb == base + Layout::kTrbs + 32 * phys);
    const uint32_t *t = trb(phys);
    CHECK(t[3] & TRB_HWO);
    uint32_t offset = Layout::kZero, capacity = 0;
    if (phys < 2) {
      if (trbType(phys) == TRBCTL_CONTROL_SETUP) { offset = Layout::kSetup; capacity = 8; }
      if (trbType(phys) == TRBCTL_CONTROL_DATA) { offset = Layout::kEp0; capacity = Layout::kEp0Bytes; }
    } else if (phys >= kNetNotifyPhys) {
      if (phys == kNetNotifyPhys) { offset = Layout::kNetNotify; capacity = 16; }
      if (phys == kNetOutPhys) { offset = Layout::kNetOut; capacity = kNtbMax; }
      if (phys == kNetInPhys) { offset = Layout::kNetIn; capacity = kNtbMax; }
    } else {
      const unsigned port = (phys - 2) / 4;
      CHECK(phys == bulk(port, phys & 1));
      offset = Layout::portOff(port) + ((phys & 1) ? Layout::kInOff : Layout::kOutOff);
      capacity = (phys & 1) ? Layout::kInBytes : Layout::kOutBytes;
    }
    CHECK((uint64_t(t[1]) << 32 | t[0]) == base + offset && trbResidual(t[2]) <= capacity);
    if (t[3] & TRB_CHN) {
      const uint32_t *zlp = trb(phys, 1);
      CHECK((uint64_t(zlp[1]) << 32 | zlp[0]) == base + Layout::kZero && trbResidual(zlp[2]) == 0);
      CHECK((zlp[3] & (TRB_HWO | TRB_LST)) == (TRB_HWO | TRB_LST));
    }
  }

  std::string complete(unsigned phys, const std::string &data = {})
  {
    CHECK(eps[phys].active);
    checkDma(phys);
    std::string result = FakeHal::complete(phys, data);
    ++transfers[phys].completed;
    return result;
  }

  unsigned txFill(unsigned port, uint8_t *dst, unsigned cap)
  {
    if (port == kPortConsole) return console.ring.get(dst, cap);
    if (port == kPortPipe) return pipeIn.ring.get(dst, cap);
    CHECK(port == kPortInput);
    return hidIn.ring.get(dst, cap);
  }
  unsigned rxSpace(unsigned port)
  {
    if (port == kPortConsole) return FakeHal::rxSpace(port);
    return port == kPortPipe ? pipeOut.ring.space() : hidOut.ring.space();
  }
  void rxData(unsigned port, const uint8_t *src, unsigned n)
  {
    if (port == kPortConsole) { FakeHal::rxData(port, src, n); return; }
    auto &ring = port == kPortPipe ? pipeOut.ring : hidOut.ring;
    CHECK(ring.put(src, n) == n);
  }
  void lineState(unsigned port, uint16_t state)
  {
    if ((state & 1) && !(lines[port] & 1)) {
      auto &ring = port == kPortConsole ? console.ring : port == kPortPipe ? pipeIn.ring : hidIn.ring;
      ring.head = ring.tail;
    }
    FakeHal::lineState(port, state);
  }

  void enqueueFrame(std::string_view frame)
  {
    CHECK(gate.offer(frames, frame.size(), linkUp, true, [&](uint8_t *dst, uint32_t at, uint32_t n) {
      std::memcpy(dst, frame.data() + at, n);
      return true;
    }) == TxGate::Result::queued);
  }
  unsigned netTxNext(uint8_t *dst, unsigned cap)
  {
    const unsigned n = frames.get(dst, cap);
    gate.afterDrain(frames);
    return n;
  }
  void netLink(bool up, uint32_t bps)
  {
    FakeHal::netLink(up, bps);
    if (!up) {
      frames.clear();
      netRx.clear();
      gate.requestReset();
    }
  }
};

std::string payload(unsigned id, char stream, unsigned length)
{
  const auto tag = std::string(1, stream) + std::to_string(id) + "/";
  std::string data(length, stream);
  for (size_t i = 0; i < data.size(); ++i) data[i] = tag[i % tag.size()];
  return data;
}

using Burst = std::array<std::string, 3>;

// Host NTB uses the common NDP-first layout, independent of the gadget's NDP-last writer.
std::string hostNtb(const Burst &frames, unsigned sequence)
{
  constexpr unsigned ndp = 12, ndpLength = 8 + 4 * (std::tuple_size_v<Burst> + 1);
  std::string block(ndp + ndpLength, '\0');
  block.replace(0, 4, "NCMH");
  wr16(bytes(block) + 4, ndp);
  wr16(bytes(block) + 6, sequence);
  wr16(bytes(block) + 10, ndp);
  block.replace(ndp, 4, "NCM0");
  wr16(bytes(block) + ndp + 4, ndpLength);
  for (unsigned i = 0; i < frames.size(); ++i) {
    block.resize((block.size() + 3) & ~size_t(3));
    wr16(bytes(block) + ndp + 8 + 4 * i, block.size());
    wr16(bytes(block) + ndp + 10 + 4 * i, frames[i].size());
    block += frames[i];
  }
  wr16(bytes(block) + 8, block.size());
  return block;
}

struct Batch {
  unsigned id;
  Burst toHost{payload(id, 'N', 65 + id % 127), payload(id, 'M', 444), payload(id, 'L', 1311)};
  Burst fromHost{payload(id, 'n', 1514), payload(id, 'm', 77 + id % 251), payload(id, 'l', 92)};
  std::string console = payload(id, 'C', id % 2 ? 512 : 513);
  std::string pipeIn = payload(id, 'S', 19 + id % 651), pipeOut = payload(id, 's', 14 + id % 397);
  int dx = int(id % 3001), dy = -dx - 256;
  std::string hidTail = "1 " + std::to_string(dx) + " " + std::to_string(dy) + " -2 3\r\nC 2e9\n";
  std::array<std::string, 3> reports{std::string{5, 0, 0, 4, 0, 0, 0, 0, 0},
    std::string{1, char(dx), char(uint16_t(dx) >> 8), char(dy), char(uint16_t(dy) >> 8), char(254), 3},
    std::string{6, char(0xe9), 2}};
};

struct Session {
  CompositeHal h;
  Gadget<CompositeHal> g{};
  LineAssembler lines;
  unsigned serial = 0, completed = 0, reports = 0, loans = 0, sequence = 0, hidCommands = 0;
  unsigned eventWrite = 0, ringWraps = 0;

  void start()
  {
    uint32_t waited = 0;
    g.bind(&h, kPorts, kProduct, 0, true);
    CHECK(g.halt(100) && g.softReset(true, &waited) && g.init() && g.run(50));
    CHECK(h.rd(GEVNTCOUNT) == 0 && g.stats.stale == 0);
    eventWrite = 0;
    h.checkActive(1);
  }

  void step()
  {
    while (g.poll()) {}
    g.pump();
    g.tick();
    ringWraps += h.evWrite < eventWrite;
    eventWrite = h.evWrite;
    CHECK(h.rd(GEVNTCOUNT) == 0 && !g.wedged());
    CHECK(!g.stats.cmdErrors && !g.stats.cmdTimeouts && !g.stats.startErrors && !g.stats.ep0Timeouts && !g.stats.unknown);
    CHECK(!g.netStats.ntbErrors && !g.netStats.rxDrops && !g.netStats.badDatagrams);
  }

  void setup(const std::string &request)
  {
    CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP && !h.owes);
    h.complete(0, request);
    step();
  }

  void status(unsigned phys, unsigned type)
  {
    h.epEvent(phys, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
    step();
    CHECK(h.eps[phys].active && h.trbType(phys) == type);
    h.complete(phys);
    step();
    CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP && !h.owes);
  }

  void noData(uint8_t type, uint8_t request, uint16_t value, uint16_t index = 0)
  {
    setup(setupBytes(type, request, value, index, 0));
    status(1, TRBCTL_CONTROL_STATUS2);
  }

  std::string readControl(const std::string &request)
  {
    setup(request);
    CHECK(h.eps[1].active && h.trbType(1) == TRBCTL_CONTROL_DATA);
    const auto data = h.complete(1);
    step();
    status(0, TRBCTL_CONTROL_STATUS3);
    return data;
  }

  void configure(bool readNotifications = true)
  {
    noData(0, 9, 1);
    CHECK(g.configured() && !g.netLinkUp());
    for (unsigned p = 0; p < kPorts; ++p) noData(0x21, 0x22, 3, uint16_t(2 * p));
    noData(1, 0x0b, 1, kData);
    CHECK(g.netLinkUp() && h.linkUp);
    sequence = 0;
    if (h.gate.needsReset()) h.gate.resetDone();
    if (!readNotifications) return;
    const auto speed = h.complete(kNetNotifyPhys);
    CHECK(speed.size() == 16 && bytes(speed)[1] == kSpeedChange && rd16(bytes(speed) + 4) == kComm);
    step();
    const auto link = h.complete(kNetNotifyPhys);
    CHECK(link.size() == 8 && bytes(link)[1] == kNetworkConnection && rd16(bytes(link) + 2) == 1);
    step();
  }

  void enumerate(bool connect, bool readNotifications = true)
  {
    if (connect) h.devEvent(DEVT_CONNECT_DONE);
    step();
    CHECK(!g.configured() && !g.stats.address);
    CHECK(readControl(setupBytes(0x80, 6, 0x100, 0, 18)).size() == 18);
    const auto config = readControl(setupBytes(0x80, 6, 0x200, 0, 0xffff));
    CHECK(config.size() == 9 + 66 * kPorts + kNetFunctionBytes);
    CHECK(rd16(bytes(config) + 2) == config.size() && bytes(config)[4] == kData + 1);
    noData(0, 5, 1 + cycle % 127);
    CHECK(g.stats.address == 1 + cycle % 127);
    configure(readNotifications);
  }

  void drainHid(auto &&check)
  {
    uint8_t data[256];
    while (const unsigned n = h.hidOut.ring.get(data, sizeof data)) {
      for (unsigned i = 0; i < n; ++i) {
        const int result = lines.push(char(data[i]));
        if (!result) continue;
        CHECK(result == 1);
        const auto cmd = parseHidCommand(lines.line());
        CHECK(cmd.op == HidOp::keyboard || cmd.op == HidOp::pointer || cmd.op == HidOp::consumer);
        check(std::string_view(reinterpret_cast<const char *>(cmd.report), cmd.bytes));
        ++reports;
        h.hidIn.put("ok " + std::to_string(++hidCommands) + "\n");
      }
    }
  }

  void stage(const Batch &b)
  {
    for (const auto &frame : b.toHost) h.enqueueFrame(frame);
    CHECK(h.console.put(b.console) == b.console.size() && h.pipeIn.put(b.pipeIn) == b.pipeIn.size());
    step();
    for (unsigned phys : kTraffic) CHECK(h.eps[phys].active);
  }

  void exchange(const Batch &b)
  {
    const auto ready = h.netDone;
    for (unsigned i = 0; i < kTraffic.size(); ++i) {
      const unsigned phys = kTraffic[(i + b.id) % kTraffic.size()];
      switch (phys) {
      case kNetInPhys: {
        const std::string ntb = h.complete(phys);
        CHECK(ntb.size() >= kNth16Bytes && rd16(bytes(ntb) + 6) == uint16_t(sequence++));
        unsigned seen = 0;
        const auto r = parseNtb16(bytes(ntb), ntb.size(), [&](uint32_t at, uint32_t n) {
          CHECK(seen < b.toHost.size() && std::string_view(ntb).substr(at, n) == b.toHost[seen++]);
        });
        CHECK(!r.error && !r.bad && r.blockBytes == ntb.size() && seen == b.toHost.size());
        break;
      }
      case kNetOutPhys: h.complete(phys, hostNtb(b.fromHost, b.id)); break;
      case kConsoleIn: CHECK(h.complete(phys) == b.console); break;
      case kPipeIn: CHECK(h.complete(phys) == b.pipeIn); break;
      case kPipeOut: h.complete(phys, b.pipeOut); break;
      case kHidOut: h.complete(phys, "K 0 04\nP "); break;
      }
    }
    step();
    CHECK(h.netDone == ready + 1 && h.netRx.size() == b.fromHost.size());
    for (unsigned i = 0; i < b.fromHost.size(); ++i) CHECK(h.netRx[i] == b.fromHost[i]);
    h.netRx.clear();
    std::string pipe(b.pipeOut.size(), '\0');
    CHECK(h.pipeOut.ring.get(bytes(pipe), pipe.size()) == pipe.size() && pipe == b.pipeOut && !h.pipeOut.ring.used());
    CHECK(h.rx[kPortConsole].empty());

    unsigned seen = 0;
    std::string replies;
    auto report = [&](std::string_view received) {
      CHECK(seen < b.reports.size() && received == b.reports[seen++]);
      replies += "ok " + std::to_string(hidCommands + 1) + "\n";
    };
    drainHid(report);
    CHECK(seen == 1);
    const auto firstReply = replies.size();
    h.complete(kHidOut, b.hidTail);
    step();
    drainHid(report);
    CHECK(seen == b.reports.size());
    CHECK(h.complete(kHidIn) == replies.substr(0, firstReply));
    step();
    CHECK(h.complete(kHidIn) == replies.substr(firstReply));
    step();
    CHECK(!h.eps[kHidIn].active && !h.eps[kNetInPhys].active && !h.eps[kConsoleIn].active && !h.eps[kPipeIn].active);
    ++completed;
  }

  void traffic(unsigned count)
  {
    while (count--) {
      const Batch b{++serial};
      stage(b);
      exchange(b);
    }
  }

  void retired(bool setupArmed)
  {
    h.checkActive(setupArmed ? 1 : 0);
    CHECK(h.rd(DALEPENA) == 3 && !g.configured() && !g.netLinkUp() && !h.linkUp && !h.frames.used());
    CHECK(!h.rd(GEVNTCOUNT) && !g.ep0Active());
  }

  void injectOldEvents()
  {
    for (unsigned phys : kTraffic) h.epEvent(phys, EPT_XFER_COMPLETE);
  }

  void reset(bool unplug)
  {
    stage(Batch{++serial});
    if (unplug) h.devEvent(DEVT_DISCONNECT);
    else h.hostReset();
    const auto stale = g.stats.stale;
    injectOldEvents();
    step();
    CHECK(g.stats.stale == stale + kTraffic.size() && h.netRx.empty() && !h.hidOut.ring.used());
    retired(!unplug);
    if (unplug) h.hostReset();
    enumerate(unplug);
  }

  void reconfigure()
  {
    stage(Batch{++serial});
    const auto address = g.stats.address;
    noData(0, 9, 0);
    retired(true);
    CHECK(g.stats.address == address);
    configure();
  }

  void sleepWake(bool keep)
  {
    const Batch b{++serial};
    stage(b);
    UsbSleep sleep(keep);
    const auto off = sleep.off(true, g.configured(), true);
    CHECK(off.maskIrq && off.halt == !keep && off.keepPolling == keep);
    h.wr(GEVNTSIZ, GEVNTSIZ_INTMASK | Layout::kEventBytes);
    const auto commands = h.cmds.size();
    if (keep) h.devEvent(DEVT_LINK_CHANGE, 3);
    else {
      CHECK(g.sleepDisconnect() && !sleep.coreUsable() && (h.rd(DSTS) & DSTS_DEVCTRLHLT));
      retired(false);
      injectOldEvents();
    }
    h.now += 2000000;   // no USB worker passes during the simulated freeze
    const auto stale = g.stats.stale;
    const auto on = sleep.on(true);
    CHECK(on.touch && on.reconnect == !keep && sleep.takeoverMayRun());
    if (!keep) {
      CHECK(g.sleepConnect() && !h.rd(GEVNTCOUNT) && g.stats.stale == stale);
      enumerate(true);
      return;
    }
    step();
    CHECK(g.stats.linkState == 3 && h.cmds.size() == commands && g.netLinkUp());
    h.devEvent(DEVT_LINK_CHANGE, 0);
    step();
    CHECK(!g.stats.linkState && g.stats.stale == stale && h.cmds.size() == commands);
    exchange(b);
  }

  void loan()
  {
    // Start a fresh notification alongside all bulk endpoints and leave EP0
    // with either a SETUP or a descriptor data stage outstanding.
    noData(1, 0x0b, 1, kData);
    stage(Batch{++serial});
    if (cycle % 2) setup(setupBytes(0x80, 6, 0x200, 0, 255));
    h.hidIn.put("ok pending\n");
    g.pump();
    CHECK(h.eps[kHidIn].active && h.eps[kNetNotifyPhys].active);
    h.enqueueFrame(payload(serial, 'Q', 191));   // queued behind the NTB in flight
    h.complete(kHidOut, "K 0 05\n");           // event waiting to be consumed by the USB worker
    CHECK(!h.hidOut.ring.used() && h.rd(GEVNTCOUNT) == 4);

    CHECK(handoffDecide(kHoDevice, kAskRelease) == HandoffDo::release && !handoffCoreFree(true, kHoDevice));
    CHECK(g.quiesceForHandoff());
    retired(false);
    CHECK(h.rd(DSTS) & DSTS_DEVCTRLHLT);
    CHECK(h.rd(GEVNTSIZ) & GEVNTSIZ_INTMASK);
    for (unsigned port = 0; port < kPorts; ++port) CHECK(!h.lines[port]);
    CHECK(!h.hidOut.ring.used() && h.gate.needsReset());
    CHECK(handoffSettled(kAskRelease, kHoReleased) && handoffCoreFree(true, kHoReleased));

    // Model the old event ring surviving the role switch until the next
    // takeover. Reclaim must clear it before arming a new SETUP.
    injectOldEvents();
    h.regs[GCTL] = (h.regs[GCTL] & ~GCTL_PRTCAPDIR_MASK) | (ROLE_HOST << 12);
    uint32_t waited = 0;
    const auto commands = h.cmds.size();
    CHECK(!g.quiesceForHandoff() && !g.softReset(true, &waited) && h.cmds.size() == commands);
    CHECK(handoffDecide(kHoReleased, kAskReclaim) == HandoffDo::reclaim);
    h.regs[GCTL] = (h.regs[GCTL] & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE;
    const auto resets = h.softResets;
    start();
    CHECK(h.softResets == resets + 1 && handoffSettled(kAskReclaim, kHoDevice));
    CHECK(!h.hidOut.ring.used() && h.netRx.empty());
    enumerate(true);
    CHECK(!g.stats.stale);
    ++loans;
  }

  void malformed()
  {
    const auto stalled = g.stats.stalls;
    for (const auto &request : {setupBytes(0x21, SET_NET_ADDRESS, 0, kComm, 0xffff),
                               setupBytes(0x80, 6, 0x03ff, 0, 255), setupBytes(0x9f, 0, 0, 0, 2)}) {
      const auto before = g.stats.stalls;
      setup(request);
      CHECK(g.stats.stalls == before + 1 && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
    }
    CHECK(g.stats.stalls == stalled + 3);
    const auto coding = readControl(setupBytes(0xa1, 0x21, 0, 2, 7));
    CHECK(coding == std::string("\0\xc2\x01\0\0\0\x08", 7));
    setup(setupBytes(0x21, 0x20, 0, 2, 7));
    CHECK(h.trbType(0) == TRBCTL_CONTROL_DATA && trbResidual(h.trb(0)[2]) == 64);
    h.complete(0, "short");
    step();
    status(1, TRBCTL_CONTROL_STATUS3);
    CHECK(readControl(setupBytes(0xa1, 0x21, 0, 2, 7)) == coding);
    setup(setupBytes(0x21, SET_NTB_INPUT_SIZE, 0, kComm, 4));
    CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_DATA);
    h.complete(0, "\1\2\3");
    step();
    status(1, TRBCTL_CONTROL_STATUS3);
    const auto size = readControl(setupBytes(0xa1, GET_NTB_INPUT_SIZE, 0, kComm, 4));
    CHECK(size.size() == 4 && rd32(bytes(size)) == kNtbMax && g.netLinkUp());
  }

  void saturate()
  {
    const auto console = payload(++serial, 'B', h.console.ring.size + 47);
    CHECK(h.console.put(console) == h.console.ring.size && h.console.ring.dropped == 47);
    std::string hidBlock;
    while (hidBlock.size() < Layout::kOutBytes) hidBlock += "K 0 04\r\n";
    step();
    for (unsigned i = 0; i < h.hidOut.ring.size / Layout::kOutBytes; ++i) {
      h.complete(kHidOut, hidBlock);
      step();
    }
    CHECK(!h.eps[kHidOut].active && !h.hidOut.ring.space() && !h.hidOut.ring.dropped);

    std::string replies;
    const auto before = reports;
    drainHid([&](std::string_view report) {
      CHECK(report == (std::string{5, 0, 0, 4, 0, 0, 0, 0, 0}));
      replies += "ok " + std::to_string(hidCommands + 1) + "\n";
    });
    CHECK(reports == before + h.hidOut.ring.size / 8);
    CHECK(h.hidIn.ring.used() == h.hidIn.ring.size && h.hidIn.ring.dropped == replies.size() - h.hidIn.ring.size);
    step();
    CHECK(h.eps[kHidOut].active && h.complete(kHidIn) == replies.substr(0, h.hidIn.ring.size));
    size_t drained = 0;
    for (unsigned i = 0; i < h.console.ring.size / Layout::kInBytes; ++i) {
      CHECK(h.trb(kConsoleIn)[3] & TRB_CHN);
      const auto data = h.complete(kConsoleIn);
      CHECK(data.size() == Layout::kInBytes && data == std::string_view(console).substr(drained, data.size()));
      drained += data.size();
      step();
    }
    CHECK(drained == h.console.ring.size && !h.console.ring.used() && !h.eps[kConsoleIn].active);
  }
};

void refuseUnconfirmedLoan()
{
  Session s;
  s.start();
  s.h.hostReset();
  s.enumerate(true);
  s.stage(Batch{1});
  // The fake can leave an endpoint command active, as when the core wedges.
  s.h.fsm = s.h.stuck = true;
  CHECK(!s.g.quiesceForHandoff() && s.g.wedged() && s.g.stats.cmdTimeouts == 1);
  CHECK(!handoffCoreFree(true, kHoUnsafe) && handoffDecide(kHoUnsafe, kAskReclaim) == HandoffDo::reclaim);
}

}  // namespace

int main()
{
  constexpr unsigned kBatchesPerTransition = 24;
  Session s;
  s.start();
  s.h.hostReset();
  s.enumerate(true);
  phase = "ring saturation";
  s.saturate();
  for (cycle = 0; cycle < 64; ++cycle) {
    phase = "malformed EP0";
    s.malformed();
    phase = "before bus reset";
    s.traffic(kBatchesPerTransition);
    s.reset(false);
    phase = "before SET_CONFIGURATION 0/1";
    s.traffic(kBatchesPerTransition);
    s.reconfigure();
    phase = "before suspend and resume";
    s.traffic(kBatchesPerTransition);
    s.sleepWake(true);
    s.sleepWake(false);
    phase = "before cable unplug and replug";
    s.traffic(kBatchesPerTransition);
    s.reset(true);
    phase = "before controller loan and reclaim";
    s.traffic(kBatchesPerTransition);
    s.loan();
  }
  phase = "last reclaim traffic and detach";
  s.traffic(1);
  s.h.devEvent(DEVT_DISCONNECT);
  s.step();
  s.retired(false);
  CHECK(s.completed >= 2000 && s.loans >= 50);
  CHECK(s.ringWraps >= s.loans);
  CHECK(!s.g.stats.stale);
  phase = "refused controller loan";
  refuseUnconfirmedLoan();
  std::printf("composite churn: %u verified traffic batches, %u HID reports, %u controller loans, %u event ring wraps\n",
              s.completed, s.reports, s.loans, s.ringWraps);
}
