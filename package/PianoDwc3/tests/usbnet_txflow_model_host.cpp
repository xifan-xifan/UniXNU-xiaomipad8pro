// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline composition of the NCM frame rings and TX gate with the real Gadget and a fake DWC3.
// Build: c++ -std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.. usbnet_txflow_model_host.cpp -o /tmp/usbnet_txflow_model_host && /tmp/usbnet_txflow_model_host
#include "Dwc3FakeHal.h"
#include "UsbHandoff.h"
#include "UsbSleep.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

using namespace pianousb;
using namespace pianousb::dwc3;
using namespace pianousb::ncm;
using pianousb::test::FakeHal;
using pianousb::test::setupBytes;

namespace {

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// Same sizes and zero padding as PianoUsbNet.cpp. Submitters pace admission to the
// family queue capacity, and USB polling and async output service are separate steps.
constexpr unsigned kRingBytes = 64 * 1024, kPendingPackets = 1024, kWireMin = 60;

struct FlowHal : FakeHal {
  using Result = TxGate::Result;
  std::array<uint8_t, kRingBytes> txStorage{}, rxStorage{};
  FrameRing toHost{txStorage.data(), kRingBytes}, fromHost{rxStorage.data(), kRingBytes};
  std::deque<std::string> pending;
  TxGate gate;
  bool stalled = false;
  unsigned stalls = 0, restarts = 0, flushed = 0;

  bool submit(std::string frame)
  {
    if (pending.size() >= kPendingPackets) return false;
    pending.push_back(std::move(frame));
    return true;
  }

  void offerPending()
  {
    while (!stalled && !pending.empty()) {
      const std::string &frame = pending.front();
      const auto result = gate.offer(toHost, frame.size(), linkUp, true, [&](uint8_t *dst, uint32_t offset, uint32_t n) {
        std::memcpy(dst, frame.data() + offset, n);
        return true;
      });
      CHECK(toHost.used() <= kRingBytes);
      if (result == Result::stall) {
        CHECK(!toHost.fits(unsigned(frame.size())));
        stalled = true;
        ++stalls;
        return;
      }
      CHECK(result == Result::queued);
      pending.pop_front();
    }
  }

  void service()
  {
    if (gate.needsReset()) {
      CHECK(!gate.takeRestart());
      flushed += unsigned(pending.size());
      pending.clear();
      stalled = false;
      gate.resetDone();
    } else if (gate.takeRestart()) {
      CHECK(stalled && toHost.space() >= kRingBytes / 2);
      stalled = false;
      ++restarts;
    }
  }

  unsigned netTxNext(uint8_t *dst, unsigned room)
  {
    const unsigned n = toHost.peek(), wire = std::max(n, kWireMin);
    if (!n || wire > room) return 0;
    CHECK(toHost.get(dst, n) == n);
    gate.afterDrain(toHost);
    std::fill(dst + n, dst + wire, 0);
    return wire;
  }

  unsigned netRxSpace() { return fromHost.space(); }
  bool netRxFrame(const uint8_t *src, unsigned n) { return linkUp && fromHost.put(src, n); }
  void netLink(bool up, uint32_t bps)
  {
    FakeHal::netLink(up, bps);
    if (!up) {
      toHost.clear();
      fromHost.clear();
      gate.requestReset();
    }
  }
};

static const uint8_t *bytes(std::string_view s) { return reinterpret_cast<const uint8_t *>(s.data()); }
static uint8_t *bytes(std::string &s) { return reinterpret_cast<uint8_t *>(s.data()); }

static uint32_t randomWord(uint32_t &state)
{
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static std::string packet(unsigned length, uint32_t id)
{
  std::string s(length, '\0');
  uint32_t state = id ^ 0xa5a50f0fu;
  for (size_t at = 0; at < length; at += sizeof(uint32_t)) {
    const uint32_t word = randomWord(state);
    for (size_t i = 0; i < sizeof(word) && at + i < length; ++i) s[at + i] = char(word >> (8 * i));
  }
  return s;
}

static std::string pack(const std::vector<std::string> &frames, uint16_t sequence)
{
  std::string ntb(kNtbMax, char(0xa5));
  NtbWriter writer;
  writer.begin(bytes(ntb), kNtbMax);
  for (const auto &frame : frames) {
    CHECK(frame.size() <= writer.room());
    std::memcpy(bytes(ntb) + writer.offset(), frame.data(), frame.size());
    writer.add(unsigned(frame.size()));
  }
  ntb.resize(writer.finish(sequence));
  return ntb;
}

// Offline receiver policy: use the real parser for format and bounds, then require a full null
// terminator and disjoint datagram/table ranges before exposing any payload to the receiver.
static bool decodeNtb(std::string_view ntb, auto &&sink)
{
  const uint8_t *b = bytes(ntb);
  using Range = std::pair<uint32_t, uint32_t>;
  std::vector<Range> ranges;
  const auto result = parseNtb16(b, unsigned(ntb.size()), [&](uint32_t at, uint32_t n) { ranges.emplace_back(at, at + n); });
  if (result.error || result.bad || !result.datagrams || result.blockBytes != ntb.size()) return false;
  const auto datagrams = ranges.size();
  for (unsigned ndp = rd16(b + 10); ndp; ndp = rd16(b + ndp + 6)) {
    const unsigned end = ndp + rd16(b + ndp + 4);
    ranges.emplace_back(ndp, end);
    unsigned entry = ndp + 8;
    while (entry < end && rd16(b + entry) && rd16(b + entry + 2)) entry += 4;
    if (entry == end || rd32(b + entry) != 0) return false;
  }
  auto sorted = ranges;
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); ++i)
    if (sorted[i].first < sorted[i - 1].second) return false;
  for (size_t i = 0; i < datagrams; ++i)
    sink(ntb.substr(ranges[i].first, ranges[i].second - ranges[i].first));
  return true;
}

struct Session {
  static constexpr unsigned ports = 2, comm = 2 * ports;
  FlowHal h;
  Gadget<FlowHal> g;

  Session() { start(); }

  void start()
  {
    g.bind(&h, ports, kProduct, 0, true);
    uint32_t waited = 0;
    CHECK(g.softReset(true, &waited) && g.init() && g.run(50));
  }

  void step()
  {
    g.poll();
    CHECK(h.rd(GEVNTCOUNT) == 0 && !g.wedged());
    g.pump();
  }

  std::string control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t size = 0, std::string data = {})
  {
    CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
    h.complete(0, setupBytes(type, request, value, index, size));
    step();
    const bool in = type & 0x80;
    if (size) {
      const unsigned phys = in ? 1 : 0;
      CHECK(h.eps[phys].active && h.trbType(phys) == TRBCTL_CONTROL_DATA);
      data = h.complete(phys, data);
      step();
    }
    const unsigned status = in ? 0 : 1;
    h.epEvent(status, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
    step();
    CHECK(h.eps[status].active && h.trbType(status) == (size ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2));
    h.complete(status);
    step();
    return data;
  }

  void enumerate()
  {
    h.devEvent(DEVT_CONNECT_DONE);
    step();
    control(0x00, 5, 7, 0);            // SET_ADDRESS
    control(0x00, 9, 1, 0);            // SET_CONFIGURATION
    control(0x01, 11, 1, comm + 1);    // SET_INTERFACE, NCM data alternate 1
    CHECK(g.stats.address == 7 && g.netLinkUp() && h.linkUp);
    CHECK(h.eps[kNetOutPhys].active && h.eps[kNetNotifyPhys].active);
  }

  void ack(uint16_t sequence, auto &&sink)
  {
    CHECK(h.eps[kNetInPhys].active);
    bool zlp = false;
    const std::string ntb = h.complete(kNetInPhys, {}, &zlp);
    CHECK(ntb.size() <= g.netStats.ntbInSize);
    CHECK(zlp == (ntb.size() < g.netStats.ntbInSize && ntb.size() % 512 == 0));
    CHECK(decodeNtb(ntb, sink) && rd16(bytes(ntb) + 6) == sequence);
    step();
  }

  void receive(const std::string &ntbs)
  {
    CHECK(h.eps[kNetOutPhys].active);
    h.complete(kNetOutPhys, ntbs);
    step();
  }

  std::string readFrame()
  {
    std::string frame(kMaxFrame, '\0');
    frame.resize(h.fromHost.get(bytes(frame), kMaxFrame));
    return frame;
  }
};

static void sustainedTx()
{
  Session s;
  s.enumerate();
  auto &h = s.h;
  constexpr unsigned total = 69 * 1024 * 1024;
  unsigned offered = 0, received = 0, next = 0, expected = 0;
  uint16_t sequence = 0;
  const auto lengthAt = [](unsigned sent) { return std::min(total - sent, unsigned(kMaxFrame)); };
  const auto verify = [&](std::string_view frame) {
    const unsigned length = lengthAt(received);
    CHECK(length >= kWireMin && frame == packet(length, expected++));
    received += length;
  };
  const auto fill = [&] {
    while (offered < total && h.pending.size() < kPendingPackets) {
      const unsigned n = lengthAt(offered);
      CHECK(h.submit(packet(n, next++)));
      offered += n;
    }
    h.offerPending();
  };

  fill();
  CHECK(h.stalled && h.toHost.used() == kRingBytes / (kMaxFrame + 2) * (kMaxFrame + 2));
  const std::string retained = h.pending.front();
  fill();
  CHECK(h.pending.size() == kPendingPackets && !h.submit(packet(kMaxFrame, next)));
  s.step();
  const auto starts = h.cmds.size();
  const auto used = h.toHost.used();
  for (unsigned pause = 0; pause < 16; ++pause) s.step();
  CHECK(h.cmds.size() == starts && h.toHost.used() == used && h.pending.front() == retained && !h.gate.needsService());

  s.ack(sequence++, verify);
  CHECK(h.toHost.space() < kRingBytes / 2 && !h.gate.needsService());
  s.ack(sequence++, verify);
  CHECK(h.toHost.space() >= kRingBytes / 2 && h.gate.needsService());
  h.service();
  h.service();
  CHECK(h.restarts == 1 && !h.stalled && h.pending.front() == retained);

  for (unsigned i = 0; i < total / kMinFrame && received < total; ++i) {
    fill();
    s.step();
    s.ack(sequence++, verify);
    h.service();
  }
  CHECK(received == total && offered == total && expected == next && h.pending.empty() && !h.toHost.used());
  CHECK(!h.eps[kNetInPhys].active && h.stalls == h.restarts && h.stalls > 100 && !h.flushed && !h.gate.needsService());
  CHECK(s.g.netStats.txNtbs == sequence && s.g.netStats.txFrames == expected && s.g.netStats.txBytes == total);
  std::printf("TX flow: %u exact bytes in %u NTBs, %u stalls and restarts\n", received, unsigned(sequence), h.stalls);
}

static void framing()
{
  const auto good = pack({packet(61, 1), packet(79, 2)}, 0xffff);
  unsigned n = 0;
  CHECK(decodeNtb(good, [&](std::string_view frame) { CHECK(frame == packet(n ? 79 : 61, n + 1)); ++n; }) && n == 2);
  const auto reject = [](const std::string &s) { CHECK(!decodeNtb(s, [](auto) { CHECK(false); })); };
  for (size_t size = 0; size < good.size(); ++size) reject(good.substr(0, size));
  const unsigned ndp = rd16(bytes(good) + 10), first = ndp + 8;
  struct Change { unsigned field, value; };
  for (const auto [field, value] : {Change{first + 4, 0}, {first + 6, 0}, {first + 4, kNth16Bytes + 4},
                                    {ndp + 16, kNth16Bytes}, {ndp + 4, 16}}) {
    auto malformed = good;
    wr16(bytes(malformed) + field, value);
    reject(malformed);
  }
  auto tableOverlap = pack({packet(61, 1)}, 0);
  const unsigned table = rd16(bytes(tableOverlap) + 10);
  wr16(bytes(tableOverlap) + table + 8, table - 56);   // the datagram ends five bytes into the NDP
  reject(tableOverlap);
  auto shortTable = good.substr(0, good.size() - 1);
  wr16(bytes(shortTable) + 8, unsigned(shortTable.size()));
  reject(shortTable);

  // Offset + length must be evaluated wider than either field. The later valid frame survives.
  auto outside = good;
  wr16(bytes(outside) + first, 0xfffc);
  const auto result = parseNtb16(bytes(outside), unsigned(outside.size()), [&](uint32_t offset, uint32_t length) {
    CHECK(outside.substr(offset, length) == packet(79, 2));
  });
  CHECK(!result.error && result.bad == 1 && result.datagrams == 1);
  reject(outside);

  Session s;
  s.enumerate();
  for (uint32_t requested : {std::numeric_limits<uint32_t>::max(), kNtbMinIn + 1, 0u}) {
    std::string arg(4, '\0');
    wr32(bytes(arg), requested);
    s.control(0x21, SET_NTB_INPUT_SIZE, 0, Session::comm, 4, arg);
    const auto reply = s.control(0xa1, GET_NTB_INPUT_SIZE, 0, Session::comm, 4);
    CHECK(reply.size() == 4 && rd32(bytes(reply)) == std::clamp(requested, kNtbMinIn, kNtbMax));
  }
  for (uint16_t requested : {uint16_t(1), std::numeric_limits<uint16_t>::max()}) {
    std::string arg(2, '\0');
    wr16(bytes(arg), requested);
    s.control(0x21, SET_MAX_DATAGRAM_SIZE, 0, Session::comm, 2, arg);
    const auto reply = s.control(0xa1, GET_MAX_DATAGRAM_SIZE, 0, Session::comm, 2);
    CHECK(reply.size() == 2 && rd16(bytes(reply)) == kMaxFrame);
  }
  const auto noCopy = [](auto...) { CHECK(false); return false; };
  for (size_t invalid : {size_t(0), size_t(kMinFrame - 1), size_t(kMaxFrame + 1), std::numeric_limits<size_t>::max()})
    CHECK(s.h.gate.offer(s.h.toHost, invalid, true, true, noCopy) == TxGate::Result::badLength);

  for (unsigned i = 0; i < 3; ++i) CHECK(s.h.submit(packet(kMaxFrame, i)));
  s.h.offerPending();
  s.step();
  for (uint16_t sequence = 0; sequence < 3; ++sequence) {
    unsigned frames = 0;
    s.ack(sequence, [&](std::string_view frame) { CHECK(frame == packet(kMaxFrame, sequence)); ++frames; });
    CHECK(frames == 1);  // two full-size datagrams cannot fit at the clamped minimum
  }
  CHECK(!s.h.eps[kNetInPhys].active && !s.h.toHost.used());

  uint32_t seed = 0xb7060871u, id = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    std::string transfer;
    std::vector<std::string> expected;
    for (unsigned block = 0; block < 2; ++block) {
      std::vector<std::string> frames{packet(kMinFrame, id++), packet(kMaxFrame, id++)};
      const unsigned count = randomWord(seed) % 4 + 1;
      for (unsigned i = 0; i < count; ++i)
        frames.push_back(packet(kMinFrame + randomWord(seed) % (kMaxFrame - kMinFrame + 1), id++));
      transfer += pack(frames, uint16_t(2 * trial + block));
      expected.insert(expected.end(), frames.begin(), frames.end());
    }
    CHECK(transfer.size() <= kNtbMax);
    s.receive(transfer);
    for (const auto &frame : expected) CHECK(s.readFrame() == frame);
    CHECK(s.readFrame().empty());
  }
  CHECK(s.g.netStats.rxNtbs == 256 && s.g.netStats.rxFrames == id && s.h.netDone == 128);
  CHECK(!s.g.netStats.rxDrops && !s.g.netStats.ntbErrors && !s.g.netStats.badDatagrams);
  s.receive(pack({packet(kMaxFrame + 1, id++)}, 256));
  CHECK(s.g.netStats.badDatagrams == 1 && s.readFrame().empty());
  s.receive(good.substr(0, good.size() - 1));
  CHECK(s.g.netStats.ntbErrors == 1 && s.readFrame().empty());
}

constexpr unsigned kChainedFrame = 484, kQueuedFrames = 64;
constexpr uint32_t kStaleId = 1000, kFreshId = 2000;
constexpr std::array kNetEndpoints{kNetNotifyPhys, kNetOutPhys, kNetInPhys};

static void prime(Session &s)
{
  s.enumerate();
  CHECK(s.h.submit(packet(kChainedFrame, kStaleId)));
  s.h.offerPending();
  s.step();
  s.receive(pack({packet(80, kStaleId)}, 0));
  for (auto phys : kNetEndpoints) CHECK(s.h.eps[phys].active && (s.h.trb(phys)[3] & TRB_HWO));
  CHECK((s.h.trb(kNetInPhys)[3] & TRB_CHN) && (s.h.trb(kNetInPhys, 1)[3] & TRB_HWO));

  for (unsigned i = 0; i < kQueuedFrames; ++i) CHECK(s.h.submit(packet(kMaxFrame, kStaleId + 1 + i)));
  s.h.offerPending();
  CHECK(s.h.stalled && !s.h.pending.empty() && !s.h.toHost.fits(kMaxFrame) && s.h.fromHost.frames == 1);
}

static void postStaleCompletions(FlowHal &h)
{
  for (auto phys : kNetEndpoints) h.epEvent(phys, EPT_XFER_COMPLETE);
}

static void expectTransfersRetired(Session &s)
{
  CHECK(!s.g.configured() && !s.g.netLinkUp() && !s.g.stats.address && s.h.rd(DALEPENA) == 3);
  for (auto phys : kNetEndpoints) CHECK(!s.h.eps[phys].active);
}

static void reconnectWithPendingReset(Session &s)
{
  auto &h = s.h;
  expectTransfersRetired(s);
  CHECK(!h.linkUp && !h.toHost.used() && !h.fromHost.used() && h.gate.needsReset());
  const auto pending = h.pending.size();
  CHECK(pending > 0);

  // Re-enumeration can beat the network thread. Admission stays closed until the old mbufs are flushed.
  s.enumerate();
  CHECK(!h.eps[kNetInPhys].active);
  CHECK(h.gate.offer(h.toHost, kMaxFrame, h.linkUp, true, [](auto...) { CHECK(false); return false; }) == TxGate::Result::drop);
  h.service();
  CHECK(h.flushed == pending && !h.restarts && h.pending.empty() && !h.stalled && !h.gate.needsService());

  const auto fresh = packet(kMinFrame, kFreshId);
  CHECK(h.submit(fresh));
  h.offerPending();
  s.step();
  const auto stale = s.g.stats.stale;
  postStaleCompletions(h);
  s.step();
  CHECK(s.g.stats.stale == stale + kNetEndpoints.size());
  for (auto phys : kNetEndpoints) CHECK(h.eps[phys].active && (h.trb(phys)[3] & TRB_HWO));

  unsigned received = 0;
  s.ack(0, [&](std::string_view wire) {
    CHECK(wire == fresh + std::string(kWireMin - fresh.size(), '\0'));
    ++received;
  });
  CHECK(received == 1 && !h.eps[kNetInPhys].active && s.readFrame().empty());
  s.receive(pack({fresh}, 0));
  CHECK(s.readFrame() == fresh && s.readFrame().empty());
}

static void resetOrUnplug(bool unplug)
{
  Session s;
  prime(s);
  if (unplug) s.h.devEvent(DEVT_DISCONNECT);
  else {
    // Reset can land after the drain crosses the low watermark and before async service runs.
    unsigned expected = 0;
    for (uint16_t sequence = 0; sequence < 3; ++sequence)
      s.ack(sequence, [&](std::string_view wire) {
        CHECK(wire == packet(expected ? kMaxFrame : kChainedFrame, kStaleId + expected));
        ++expected;
      });
    CHECK(s.h.gate.needsService() && !s.h.gate.needsReset() && s.h.eps[kNetInPhys].active);
    s.h.hostReset();
  }
  postStaleCompletions(s.h);
  s.step();
  CHECK(s.g.stats.stale == kNetEndpoints.size() && s.h.eps[0].active == !unplug);
  CHECK((unplug ? s.g.stats.disconnects : s.g.stats.resets) == 1);
  for (auto phys : kNetEndpoints) CHECK(s.h.count(CMD_ENDTRANSFER, phys) == 1);
  if (unplug) {
    s.h.hostReset();
    s.step();
    CHECK(s.h.eps[0].active);
  }
  reconnectWithPendingReset(s);
}

static void sleepWake(bool keep)
{
  Session s;
  prime(s);
  UsbSleep sleep(keep);
  const auto off = sleep.off(true, s.g.configured(), true);
  CHECK(off.maskIrq && off.keepPolling == keep && off.halt == !keep);
  s.h.wr(GEVNTSIZ, GEVNTSIZ_INTMASK | Layout::kEventBytes);
  if (off.halt) {
    CHECK(s.g.sleepDisconnect());
    expectTransfersRetired(s);
    CHECK(!sleep.coreUsable() && !s.h.eps[0].active && (s.h.rd(DSTS) & DSTS_DEVCTRLHLT));
    for (auto phys : kNetEndpoints) CHECK(s.h.count(CMD_ENDTRANSFER, phys) == 1);
    postStaleCompletions(s.h);   // wake discards events from the old endpoints
  } else {
    CHECK(sleep.coreUsable() && s.h.stalled && !s.h.gate.needsService());
    s.h.devEvent(DEVT_LINK_CHANGE, 3);   // U3 while the system is frozen
  }

  const auto on = sleep.on(true);
  CHECK(on.touch && on.reconnect == !keep && sleep.state() == SleepState::awake);
  if (on.reconnect) {
    CHECK(s.g.sleepConnect());
    CHECK(!s.h.rd(GEVNTCOUNT) && !s.g.stats.stale);
    reconnectWithPendingReset(s);
    return;
  }

  s.step();
  CHECK(s.g.stats.linkState == 3 && s.g.netLinkUp() && s.h.linkEvents == 1 && s.h.eps[kNetInPhys].active);
  s.h.devEvent(DEVT_LINK_CHANGE, 0);   // U0
  s.step();
  CHECK(s.g.stats.linkState == 0 && s.readFrame() == packet(80, kStaleId));
  unsigned expected = 0;
  uint16_t sequence = 0;
  while (s.h.eps[kNetInPhys].active && sequence <= kQueuedFrames) {
    s.ack(sequence++, [&](std::string_view wire) {
      CHECK(wire == packet(expected ? kMaxFrame : kChainedFrame, kStaleId + expected));
      ++expected;
    });
    s.h.service();
    s.h.offerPending();
    s.step();
  }
  CHECK(expected == kQueuedFrames + 1 && !s.h.eps[kNetInPhys].active && !s.h.toHost.used() && s.h.pending.empty());
  CHECK(s.h.stalls == s.h.restarts && !s.h.flushed && s.h.linkEvents == 1);
  for (auto phys : kNetEndpoints) CHECK(s.h.count(CMD_ENDTRANSFER, phys) == 0);
}

static void controllerLoan()
{
  Session s;
  prime(s);
  auto &h = s.h;
  CHECK(handoffDecide(kHoDevice, kAskRelease) == HandoffDo::release && !handoffCoreFree(true, kHoDevice));
  CHECK(s.g.quiesceForHandoff() && (h.rd(DSTS) & DSTS_DEVCTRLHLT));
  CHECK(handoffCoreFree(true, kHoReleased) && (h.rd(GEVNTSIZ) & GEVNTSIZ_INTMASK));
  expectTransfersRetired(s);
  postStaleCompletions(h);
  h.regs[GCTL] = (h.rd(GCTL) & ~GCTL_PRTCAPDIR_MASK) | (ROLE_HOST << 12);
  const auto commands = h.cmds.size();
  uint32_t waited = 0;
  CHECK(!s.g.softReset(true, &waited) && h.cmds.size() == commands && h.softResets == 1);
  CHECK(handoffDecide(kHoReleased, kAskReclaim) == HandoffDo::reclaim);
  UsbSleep sleep;
  sleep.off(false, false, false);
  CHECK(!sleep.takeoverMayRun());
  CHECK(sleep.on(true).touch && sleep.takeoverMayRun());
  h.regs[GCTL] = (h.rd(GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE;
  s.start();   // the reclaim is a fresh takeover. The soft reset clears the old event ring.
  CHECK(h.softResets == 2 && !h.rd(GEVNTCOUNT) && !s.g.stats.stale);
  CHECK(handoffSettled(kAskReclaim, kHoDevice));
  reconnectWithPendingReset(s);
}

}  // namespace

int main()
{
  sustainedTx();
  framing();
  resetOrUnplug(false);
  resetOrUnplug(true);
  sleepWake(true);
  sleepWake(false);
  controllerLoan();
  std::puts("NCM framing, unplug and reset, kept and disconnected sleep, and controller loan model: passed");
}
