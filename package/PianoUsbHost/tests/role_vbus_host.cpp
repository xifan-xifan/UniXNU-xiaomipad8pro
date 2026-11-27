// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoDwc3 role_vbus_host.cpp -o /tmp/role_vbus_host && /tmp/role_vbus_host
#include "UsbRoleVbusModel.h"
#include <array>
#include <cassert>
#include <cstdio>

using namespace pianousbhost;
using namespace pianousbhost::b7;
using namespace pianoucsi;
using namespace pianousb;

namespace {

constexpr Route kPinned{pianohw::kUsbStream, 7, true};
constexpr GadgetRelease kReleased{kHoReleased, 0, dwc3::DSTS_DEVCTRLHLT, true};
constexpr uint32_t kConnect = kPortPp | kPortCcs | kPortCsc;
constexpr uint32_t kEnabledHs = kPortPp | kPortCcs | kPortPed | (3u << 10);
constexpr uint32_t kEnabledSs = kPortPp | kPortCcs | kPortPed | (4u << 10);
constexpr PowerSample kOff{Level::safe0v}, kOn{Level::safe5v};
constexpr uint64_t kEntry = 1 + Model::kDebounceMs;

struct Notification {
  PianoUsbcObservation status{};
  std::array<uint8_t, 32> pan{};

  Notification(uint64_t now, uint32_t seq, bool connected, bool source, uint8_t partner, bool reverse = false,
               uint32_t gen = 1, uint16_t version = kV1_1)
  {
    status.version = PIANO_USBC_OBSERVATION_VERSION;
    status.owned = status.ready = status.valid = 1;
    status.connector = 1;
    status.generation = gen;
    status.seq = seq;
    status.updated_ms = now;
    status.expires_ms = now > UINT64_MAX - 1000 ? UINT64_MAX : now + 1000;
    status.ucsi_version = version;
    status.status_bytes = version >= kV2_0 ? 11 : 9;
    if (connected) {
      status.status[2] = kModeDefault | (1u << 3) | (source ? 1u << 4 : 0) | (kPartnerFlagUsb << 5);
      status.status[3] = uint8_t(partner << 5);
      status.status[10] = reverse ? 1u << 6 : 0;
    }
    put32(pan.data(), kOwnerPort);
    put32(pan.data() + 4, kTypeRequest);
    put32(pan.data() + 8, kOpPortNotify);
    pan[13] = connected ? uint8_t(reverse) : 0xff;
    pan[14] = connected ? kMuxUsb3 : kMuxNone;
  }

  bool send(Model &m, uint64_t now) const { return m.observe(status, pan.data(), pan.size(), now); }
  Connector decode(uint64_t now) const { return decodeConnector(status, pan.data(), pan.size(), now); }
};

void invariants(const Model &m)
{
  switch (m.phase()) {
    case Phase::device:
    case Phase::releasing: assert(m.owner() == Owner::device); break;
    case Phase::ramping:
    case Phase::waitingPort:
    case Phase::resetting:
    case Phase::enumerating:
    case Phase::online:
    case Phase::stopping: assert(m.owner() == Owner::host); break;
    case Phase::reclaiming: assert(m.owner() == Owner::none); break;
    case Phase::held: break;
  }
  if (m.gadgetActive()) {
    assert(m.owner() == Owner::device && !m.runXhci() && !m.source5v());
  }
  if (m.source5v()) {
    assert(m.owner() == Owner::host && m.connector().sourceHost() && !m.latchedFaults() && m.detachRequired());
  } else {
    assert(!m.runXhci() && !m.portPower());
  }
  if (m.phase() == Phase::stopping || m.phase() == Phase::reclaiming || m.phase() == Phase::held) {
    assert(!m.gadgetActive() && !m.source5v() && !m.resetRequest());
  }
  assert(m.resetAttempts() <= Model::kResetAttempts);
}

void expect(const Model &m, Phase phase, End end = End::none)
{
  assert(m.phase() == phase && m.end() == end);
  invariants(m);
}

void sourceAttach(Model &m, uint64_t now = 1, uint32_t seq = 1, bool reverse = false)
{
  assert(Notification(now, seq, true, true, kPartnerUfp, reverse).send(m, now));
}

void detach(Model &m, uint64_t now, uint32_t seq = 2, uint32_t gen = 1)
{
  assert(Notification(now, seq, false, false, kPartnerNone, false, gen).send(m, now));
  assert(m.connector().detached() && m.connector().cc == Cc::none);
}

uint64_t startRelease(Model &m)
{
  m.power(kOff, 0);
  sourceAttach(m);
  assert(!m.requestHost(kEntry - 1));
  const uint64_t token = m.requestHost(kEntry);
  assert(token && m.gadgetActive() && !m.source5v());
  expect(m, Phase::releasing);
  return token;
}

void startRamp(Model &m, uint64_t token, uint64_t now = kEntry + 1)
{
  assert(m.release(token, kReleased, kPinned, now));
  expect(m, Phase::ramping);
  assert(m.source5v() && !m.portPower() && !m.gadgetActive());
}

void powerPort(Model &m, uint64_t now = kEntry + 2)
{
  m.power(kOn, now);
  expect(m, Phase::waitingPort);
  assert(m.portPower() && m.runXhci());
}

void returnDevice(Model &m, uint64_t token, uint64_t now, End reason, bool haltFirst = true)
{
  expect(m, Phase::stopping, reason);
  assert(!m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, now));
  if (haltFirst) m.xhci(token, 0, kUsbStsHalted, now);
  else m.power(kOff, now);
  expect(m, Phase::stopping, reason);
  if (haltFirst) m.power(kOff, now + 1);
  else m.xhci(token, 0, kUsbStsHalted, now + 1);
  expect(m, Phase::reclaiming, reason);
  assert(!m.reclaim(token + 1, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, now + 2));
  assert(m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, now + 2));
  expect(m, Phase::device, reason);
  assert(m.gadgetActive() && m.route().bank == kPinned.bank && m.route().sid == kPinned.sid);
}

void connectorDecoding()
{
  // All encodings of binary power direction, partner type, operation mode and PAN orientation.
  for (uint16_t version : {kV1_0, kV1_1, kV1_2, kV2_0}) {
    for (bool provider : {false, true}) {
      for (uint8_t partner = 0; partner < 8; ++partner) {
        for (uint8_t mode = 0; mode < 8; ++mode) {
          for (unsigned orientation = 0; orientation < 256; ++orientation) {
            const bool reverse = orientation == 1;
            Notification n(1, 1, true, provider, partner, reverse, 1, version);
            n.status.status[2] = uint8_t((n.status.status[2] & ~7u) | mode);
            n.pan[13] = uint8_t(orientation);
            const Connector result = n.decode(1);
            const bool supported = mode >= kModeDefault && mode <= kModeTypec3_0 && orientation <= 1 &&
                                   (partner == kPartnerDfp || partner == kPartnerUfp || partner == kPartnerCableUfp);
            assert(result.known == supported);
            if (supported) {
              assert(result.power == (provider ? PowerRole::source : PowerRole::sink));
              assert(result.data == (partner == kPartnerDfp ? DataRole::device : DataRole::host));
              assert(result.cc == (reverse ? Cc::reverse : Cc::normal));
            }
          }
        }
      }
    }
  }

  const Notification source(10, 1, true, true, kPartnerUfp, true, 1, kV2_0);
  for (unsigned len = 0; len < 256; ++len) {
    auto n = source;
    n.status.status_bytes = uint8_t(len);
    assert(n.decode(10).known == (len >= 11 && len <= 16));
  }
  for (unsigned len = 0; len <= 33; ++len) {
    // Allocate exactly each length so ASan also checks the parser's truncated input path.
    uint8_t *frame = new uint8_t[len];
    for (unsigned i = 0; i < len; ++i) frame[i] = i < source.pan.size() ? source.pan[i] : 0;
    assert(decodeConnector(source.status, frame, len, 10).known == (len == source.pan.size()));
    delete[] frame;
  }
  assert(!decodeConnector(source.status, nullptr, source.pan.size(), 10).known);
  assert(!source.decode(9).known && source.decode(1010).known && !source.decode(1011).known);
  for (unsigned field = 0; field < 13; ++field) {
    auto n = source;
    switch (field) {
      case 0: n.status.version = 0; break;
      case 1: n.status.owned = 0; break;
      case 2: n.status.ready = 0; break;
      case 3: n.status.valid = 0; break;
      case 4: n.status.connector = 0; break;
      case 5: n.status.ucsi_version = 0xf123; break;
      case 6: n.pan[0] ^= 1u; break;
      case 7: n.pan[4] ^= 1u; break;
      case 8: n.pan[8] ^= 1u; break;
      case 9: n.pan[12] = 1; break;
      case 10: n.pan[13] = 0; break;  // valid orientation contradicts UCSI 2.0
      case 11: n.pan[14] = kMuxDp4; break;
      case 12: n.status.status[2] &= uint8_t(~(kPartnerFlagUsb << 5)); break;
    }
    assert(!n.decode(10).known);
  }
  auto absent = Notification(10, 1, false, false, kPartnerNone);
  assert(absent.decode(10).detached());
  absent.status.status[2] = 1u << 4;  // a detached provider cannot rearm the model
  assert(!absent.decode(10).known);
  absent.status.status[2] = 0;
  absent.pan[14] = kMuxUsb3;
  assert(!absent.decode(10).known);
}

void drpAndReplay()
{
  Model m(kPinned);
  m.power(kOn, 0);
  assert(Notification(1, 1, true, false, kPartnerDfp).send(m, 1));
  assert(m.connector().data == DataRole::device && m.connector().power == PowerRole::sink);
  assert(!m.requestHost(11) && m.gadgetActive());
  detach(m, 12);
  m.power(kOff, 12);
  sourceAttach(m, 13, 3, true);
  detach(m, 14, 4);  // bounce before debounce
  sourceAttach(m, 15, 5, true);
  assert(!m.requestHost(24));
  const uint64_t token = m.requestHost(25);
  assert(token && m.connector().cc == Cc::reverse);
  startRamp(m, token, 26);
  powerPort(m, 27);
  assert(!Notification(28, 5, false, false, kPartnerNone).send(m, 28));  // duplicate
  assert(!Notification(28, 4, false, false, kPartnerNone).send(m, 28));  // out of order
  assert(!Notification(14, 99, false, false, kPartnerNone).send(m, 28)); // older sample time
  assert(!Notification(28, 0, false, false, kPartnerNone).send(m, 28));  // missing sequence
  assert(!Notification(28, 99, false, false, kPartnerNone, false, 0).send(m, 28));
  assert(!m.advance(27));  // backwards clock
  expect(m, Phase::waitingPort);
  detach(m, 29, 6);
  returnDevice(m, token, 30, End::detach, false);
  assert(!m.detachRequired());
  assert(Notification(33, 7, true, false, kPartnerDfp).send(m, 33));
  expect(m, Phase::device, End::detach);
  assert(m.connector().data == DataRole::device && !m.requestHost(45));

  // The parser recognizes independent power/data roles. The source-only loan needs both.
  assert(Notification(46, 8, true, false, kPartnerUfp).send(m, 46));
  assert(m.connector().data == DataRole::host && m.connector().power == PowerRole::sink && !m.requestHost(56));
  assert(Notification(57, 9, true, true, kPartnerDfp).send(m, 57));
  assert(m.connector().data == DataRole::device && m.connector().power == PowerRole::source && !m.requestHost(67));

  assert(Notification(68, 1, true, true, kPartnerUfp, false, 2).send(m, 68));
  const auto second = m.requestHost(78);
  assert(second > token && !m.release(token, kReleased, kPinned, 78));
  startRamp(m, second, 79);
  assert(!Notification(79, 99, false, false, kPartnerNone).send(m, 79));  // previous generation
  assert(Notification(80, 1, true, true, kPartnerUfp, false, 3).send(m, 80));
  returnDevice(m, second, 81, End::witness);
  assert(m.detachRequired());

  Model late(kPinned);
  const uint64_t pending = startRelease(late);
  detach(late, kEntry + 1);
  expect(late, Phase::device, End::detach);
  assert(!late.release(pending, kReleased, kPinned, kEntry + 1) && !late.detachRequired());
  sourceAttach(late, kEntry + 2, 3);
  const uint64_t next = late.requestHost(kEntry + 2 + Model::kDebounceMs);
  assert(next > pending && !late.release(pending, kReleased, kPinned, kEntry + 2 + Model::kDebounceMs));
  expect(late, Phase::releasing);
}

void releaseAndReclaim()
{
  for (GadgetRelease sample : {GadgetRelease{kHoDevice, dwc3::DCTL_RUN_STOP, 0, false},
                               GadgetRelease{kHoStarting, 0, 0, false}}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    assert(!m.release(token, sample, kPinned, kEntry + Model::kReleaseMs - 1));
    expect(m, Phase::releasing);
    assert(m.gadgetActive());
    assert(!m.release(token, kReleased, kPinned, kEntry + Model::kReleaseMs));
    expect(m, Phase::device, End::release);
    assert(m.gadgetActive() && m.detachRequired());
  }
  for (GadgetRelease sample : {GadgetRelease{kHoUnsafe, 0, 0, false}, GadgetRelease{kHoFailed, 0, 0, false},
                               GadgetRelease{kHoReleased, dwc3::DCTL_RUN_STOP, dwc3::DSTS_DEVCTRLHLT, true},
                               GadgetRelease{kHoReleased, 0, 0, true}, GadgetRelease{kHoReleased, 0, UINT32_MAX, true},
                               GadgetRelease{kHoReleased, 0, dwc3::DSTS_DEVCTRLHLT, false}}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    assert(!m.release(token, sample, kPinned, kEntry + 1));
    expect(m, Phase::held, End::release);
    assert(m.owner() == Owner::device && !m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, kEntry + 2));
  }
  for (Route bad : {Route{0x41, kPinned.bank, true}, Route{kPinned.sid, kPinned.bank + 1, true},
                    Route{kPinned.sid, kPinned.bank, false}, Route{kPinned.sid, 128, true}}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    assert(!m.release(token, kReleased, bad, kEntry + 1));
    expect(m, Phase::reclaiming, End::release);
    assert(!m.reclaim(token, kHoReclaiming, dwc3::GCTL_PRTCAP_DEVICE, kPinned, kEntry + 2));
    assert(m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, kEntry + 3));
    expect(m, Phase::device, End::release);
  }
  for (unsigned failure = 0; failure < 4; ++failure) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    startRamp(m, token);
    detach(m, kEntry + 2);
    m.xhci(token, 0, kUsbStsHalted, kEntry + 3);
    m.power(kOff, kEntry + 4);
    expect(m, Phase::reclaiming, End::detach);
    const Route bad = failure == 2 ? Route{kPinned.sid, kPinned.bank + 1, true} : kPinned;
    const uint64_t now = kEntry + 4 + (failure == 3 ? Model::kReclaimMs : 1);
    assert(!m.reclaim(token, failure == 0 ? kHoFailed : kHoDevice, failure == 1 ? 0 : dwc3::GCTL_PRTCAP_DEVICE, bad, now));
    expect(m, Phase::held, End::reclaim);
    assert(m.owner() == Owner::none);
  }
  Model badDomain({kPinned.sid, kPinned.bank, false});
  badDomain.power(kOff, 0);
  sourceAttach(badDomain);
  assert(!badDomain.requestHost(kEntry) && badDomain.gadgetActive());
}

void powerInterlocks()
{
  constexpr std::array levels{Level::unknown, Level::safe0v, Level::unsettled, Level::safe5v};
  for (Level level : levels) {
    for (bool charging : {false, true}) {
      for (bool sink : {false, true}) {
        for (uint32_t faults = 0; faults <= (overCurrent | thermal | battery); ++faults) {
          const PowerSample sample{level, charging, sink, faults};
          const bool mayStart = level == Level::safe0v && !charging && !sink && !faults;
          Model idle(kPinned);
          idle.power(sample, 0);
          sourceAttach(idle);
          assert(bool(idle.requestHost(kEntry)) == mayStart);
          assert(!idle.source5v());
          invariants(idle);

          Model pending(kPinned);
          const auto pendingToken = startRelease(pending);
          pending.power(sample, kEntry + 1);
          assert(pending.release(pendingToken, kReleased, kPinned, kEntry + 2) == mayStart);
          assert(pending.source5v() == mayStart);
          invariants(pending);

          Model active(kPinned);
          const auto token = startRelease(active);
          startRamp(active, token);
          powerPort(active);
          active.power(sample, kEntry + 3);
          const bool keepsSource = level == Level::safe5v && !charging && !sink && !faults;
          assert(active.source5v() == keepsSource && active.latchedFaults() == faults);
          if (keepsSource) expect(active, Phase::waitingPort);
          else returnDevice(active, token, kEntry + 4, faults ? End::fault : charging || sink ? End::powerConflict : End::powerLost);
        }
      }
    }
  }

  Model m(kPinned);
  const uint64_t token = startRelease(m);
  startRamp(m, token);
  m.power({Level::safe5v, false, false, overCurrent | thermal | battery}, kEntry + 2);
  expect(m, Phase::stopping, End::fault);
  detach(m, kEntry + 3);  // physical fault has not cleared
  returnDevice(m, token, kEntry + 4, End::fault);
  assert(m.detachRequired() && m.latchedFaults() == (overCurrent | thermal | battery));
  assert(!Notification(kEntry + 7, 2, false, false, kPartnerNone).send(m, kEntry + 7));
  auto brokenDetach = Notification(kEntry + 8, 3, false, false, kPartnerNone);
  brokenDetach.status.status_bytes = 8;
  assert(!brokenDetach.send(m, kEntry + 8) && m.latchedFaults());
  sourceAttach(m, kEntry + 9, 4);
  assert(!m.requestHost(kEntry + 9 + Model::kDebounceMs));
  detach(m, kEntry + 20, 5);
  assert(!m.latchedFaults() && !m.detachRequired());
  sourceAttach(m, kEntry + 21, 6);
  const uint64_t rearmed = m.requestHost(kEntry + 21 + Model::kDebounceMs);
  assert(rearmed > token);

  Model bounce(kPinned);
  const auto b = startRelease(bounce);
  startRamp(bounce, b);
  detach(bounce, kEntry + 2);
  sourceAttach(bounce, kEntry + 3, 3);
  returnDevice(bounce, b, kEntry + 4, End::detach);
  assert(bounce.detachRequired() && !bounce.requestHost(kEntry + 3 + Model::kDebounceMs));
}

void deadlines()
{
  for (uint64_t delay : {Model::kRampMs - 1, Model::kRampMs, Model::kRampMs + 1}) {
    Model m(kPinned);
    const auto token = startRelease(m);
    startRamp(m, token);
    m.power({Level::unsettled}, kEntry + 2);
    expect(m, Phase::ramping);
    m.power(kOn, kEntry + 1 + delay);
    if (delay < Model::kRampMs) expect(m, Phase::waitingPort);
    else returnDevice(m, token, kEntry + 2 + delay, End::ramp);
  }
  for (bool haltFirst : {false, true}) {
    const uint64_t abort = kEntry + 3;
    for (bool late : {false, true}) {
      Model m(kPinned);
      const auto token = startRelease(m);
      startRamp(m, token);
      powerPort(m);
      detach(m, abort);
      const uint64_t limit = haltFirst ? Model::kDischargeMs : Model::kHaltMs;
      if (haltFirst) {
        m.xhci(token, 0, kUsbStsHalted, abort + 1);
        m.power(kOff, abort + limit - !late);
      } else {
        m.power(kOff, abort + 1);
        m.xhci(token, 0, kUsbStsHalted, abort + limit - !late);
      }
      if (!late) {
        expect(m, Phase::reclaiming, End::detach);
      } else {
        expect(m, Phase::held, haltFirst ? End::discharge : End::halt);
        assert(m.owner() == Owner::host);
        assert(!m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, abort + limit + 1));
        m.power(kOff, abort + limit + 1);
        m.xhci(token, 0, kUsbStsHalted, abort + limit + 1);
        assert(m.owner() == Owner::host && !m.gadgetActive());
      }
    }
  }

  Model lease(kPinned);
  const auto token = startRelease(lease);
  startRamp(lease, token);
  powerPort(lease);
  assert(lease.advance(kEntry + Model::kLoanMs - 1));
  expect(lease, Phase::waitingPort);
  assert(lease.advance(kEntry + Model::kLoanMs));
  returnDevice(lease, token, kEntry + Model::kLoanMs + 1, End::loan);

  Model overflow(kPinned);
  constexpr uint64_t nearEnd = UINT64_MAX - Model::kDebounceMs - Model::kReleaseMs;
  overflow.power(kOff, nearEnd);
  sourceAttach(overflow, nearEnd);
  assert(overflow.requestHost(nearEnd + Model::kDebounceMs));
  assert(overflow.advance(UINT64_MAX));
  expect(overflow, Phase::device, End::release);
}

void rootHub()
{
  for (Protocol protocol : {Protocol::usb2, Protocol::usb3}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    assert(!m.port(token, {protocol, kConnect}, kEntry));
    startRamp(m, token);
    assert(!m.port(token, {protocol, kConnect}, kEntry + 1));
    powerPort(m);
    assert(m.port(token, {protocol, kPortCcs | kPortCsc}, kEntry + 3) == kPortCsc);  // PP absent
    assert(!m.port(token, {protocol, kPortCcs | kPortPp}, kEntry + 4));  // CSC absent
    assert(!m.port(token + 1, {protocol, kConnect}, kEntry + 5));
    expect(m, Phase::waitingPort);
    assert(m.port(token, {protocol, kConnect | kPortPec}, kEntry + 6) == (kPortCsc | kPortPec));
    expect(m, Phase::resetting);
    const bool hs = protocol == Protocol::usb2;
    const uint32_t reset = hs ? kPortPr : kPortWpr;
    const uint32_t change = hs ? kPortPrc : kPortWrc;
    const uint32_t enabled = hs ? kEnabledHs : kEnabledSs;
    assert(m.resetRequest() == reset && m.resetAttempts() == 1);
    assert(!m.enumerated(token, true, true, kEntry + 7));
    m.port(token, {protocol, enabled | reset | change, 1}, kEntry + 7);  // reset still active
    m.port(token, {protocol, enabled | (hs ? kPortWrc : kPortPrc), 1}, kEntry + 8);  // wrong change
    expect(m, Phase::resetting);
    m.port(token, {protocol, (enabled & ~kPortPed) | change, 1}, kEntry + 9);  // completed disabled, retry
    assert(m.resetAttempts() == 2);
    m.port(token, {protocol, enabled | change, 1}, kEntry + 10);  // completion from the first attempt
    expect(m, Phase::resetting);
    const Protocol other = hs ? Protocol::usb3 : Protocol::usb2;
    m.port(token, {other, kPortPp | kPortCsc}, kEntry + 11);  // other logical port disconnects
    expect(m, Phase::resetting);
    m.port(token, {protocol, enabled | change, 2}, kEntry + 12);
    expect(m, Phase::enumerating);
    assert(!m.resetRequest() && !m.enumerated(token + 1, true, true, kEntry + 13));
    assert(m.enumerated(token, true, true, kEntry + 14));
    expect(m, Phase::online);
    assert(!m.enumerated(token, true, true, kEntry + 14));
    assert(m.port(token, {protocol, kPortPp | kPortCsc}, kEntry + 15) == kPortCsc);
    assert(!m.port(token, {protocol, enabled | kPortCsc}, kEntry + 15));
    returnDevice(m, token, kEntry + 16, End::detach);
    assert(m.detachRequired());  // root-hub disconnect alone cannot replace a clean Type-C detach
  }

  for (uint32_t reg : {kEnabledSs, kEnabledSs | (1u << 5), kEnabledSs | kPortWpr, kEnabledHs}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    startRamp(m, token);
    powerPort(m);
    m.port(token, {Protocol::usb3, reg | kPortCsc}, kEntry + 3);
    if (reg == kEnabledSs) {
      expect(m, Phase::enumerating);
      assert(m.resetAttempts() == 0);
      assert(m.enumerated(token, true, true, kEntry + 4));
      expect(m, Phase::online);
    } else {
      expect(m, Phase::resetting);
      assert(m.resetRequest() == kPortWpr);
    }
  }

  for (bool useTimeout : {false, true}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    startRamp(m, token);
    powerPort(m);
    constexpr uint64_t resetAt = kEntry + 3;
    m.port(token, {Protocol::usb2, kConnect}, resetAt);
    if (useTimeout) {
      assert(m.advance(resetAt + Model::kResetMs - 1) && m.resetAttempts() == 1);
      m.port(token, {Protocol::usb2, kEnabledHs | kPortPrc, 1}, resetAt + Model::kResetMs);
      expect(m, Phase::resetting);
      assert(m.resetAttempts() == 2);
      assert(m.advance(resetAt + 2 * Model::kResetMs));
      returnDevice(m, token, resetAt + 2 * Model::kResetMs + 1, End::reset);
    } else {
      m.port(token, {Protocol::usb2, kEnabledSs | kPortPrc, 1}, resetAt + 1); // wrong speed
      m.port(token, {Protocol::usb2, kEnabledSs | kPortPrc, 2}, resetAt + 2);
      returnDevice(m, token, resetAt + 3, End::reset);
    }
    assert(m.resetAttempts() == Model::kResetAttempts);
  }

  for (unsigned outcome = 0; outcome < 6; ++outcome) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    startRamp(m, token);
    powerPort(m);
    const uint64_t connectedAt = kEntry + 3;
    m.port(token, {Protocol::usb3, kEnabledSs | kPortCsc}, connectedAt);
    expect(m, Phase::enumerating);
    uint64_t now = connectedAt + 1;
    End reason = End::enumeration;
    switch (outcome) {
      case 0:
        assert(!m.enumerated(token, false, true, now));
        break;
      case 1:
        assert(!m.enumerated(token, true, false, now));
        break;
      case 2:
        now = connectedAt + Model::kEnumerationMs;
        assert(!m.enumerated(token, true, true, now));
        break;
      case 3:
        m.port(token, {Protocol::usb3, kPortPp}, now);  // disconnect even when CSC was already acknowledged
        reason = End::detach;
        break;
      case 4:
        m.port(token, {Protocol::usb3, kEnabledSs & ~kPortPp}, now);
        reason = End::powerLost;
        break;
      case 5:
        m.port(token, {Protocol::usb3, kEnabledSs | kPortCsc}, now);  // bounce between PORTSC reads
        reason = End::detach;
        break;
    }
    assert(!m.enumerated(token, true, true, now));
    returnDevice(m, token, now + 1, reason);
  }

  for (uint32_t oc : {kPortOca, kPortOcc}) {
    Model m(kPinned);
    const uint64_t token = startRelease(m);
    startRamp(m, token);
    powerPort(m);
    m.port(token, {Protocol::usb2, kConnect | oc}, kEntry + 3);
    expect(m, Phase::stopping, End::fault);
    assert(m.latchedFaults() == overCurrent && m.resetAttempts() == 0);
    returnDevice(m, token, kEntry + 4, End::fault);
  }
}

void haltProof()
{
  for (bool rs : {false, true}) {
    for (bool hch : {false, true}) {
      for (bool cnr : {false, true}) {
        for (bool hcrst : {false, true}) {
          Model m(kPinned);
          const uint64_t token = startRelease(m);
          startRamp(m, token);
          powerPort(m);
          constexpr uint64_t stop = kEntry + 3;
          detach(m, stop);
          m.power(kOff, stop + 1);
          const uint32_t cmd = (rs ? kUsbCmdRun : 0) | (hcrst ? kUsbCmdHcrst : 0);
          const uint32_t sts = (hch ? kUsbStsHalted : 0) | (cnr ? kUsbStsCnr : 0);
          m.xhci(token + 1, 0, kUsbStsHalted, stop + 2);  // someone else's halt
          expect(m, Phase::stopping, End::detach);
          m.xhci(token, cmd, sts, stop + 3);
          expect(m, !rs && hch && !cnr && !hcrst ? Phase::reclaiming : Phase::stopping, End::detach);
        }
      }
    }
  }

  Model m(kPinned);
  const auto token = startRelease(m);
  startRamp(m, token);
  powerPort(m);
  m.xhci(token, 0, kUsbStsHalted, kEntry + 3);  // unexpected halt triggers shutdown and cannot also confirm it
  expect(m, Phase::stopping, End::fault);
  assert(m.latchedFaults() == controller);
  m.power(kOff, kEntry + 4);
  expect(m, Phase::stopping, End::fault);
  m.power({Level::unsettled}, kEntry + 5);  // discharge witness no longer holds
  m.xhci(token, 0, kUsbStsHalted, kEntry + 6);
  expect(m, Phase::stopping, End::fault);
  m.xhci(token, kUsbCmdRun, 0, kEntry + 7);  // halt witness no longer holds
  m.power(kOff, kEntry + 8);
  expect(m, Phase::stopping, End::fault);
  m.xhci(token, 0, kUsbStsHalted, kEntry + 9);
  expect(m, Phase::reclaiming, End::fault);
}

void abortEveryLoanPhase()
{
  enum class Abort { detach, role, cc, malformed, relay, expire, sleep, dmaFault };
  for (Phase phase : {Phase::releasing, Phase::ramping, Phase::waitingPort, Phase::resetting, Phase::enumerating, Phase::online}) {
    for (Abort abort : {Abort::detach, Abort::role, Abort::cc, Abort::malformed, Abort::relay, Abort::expire, Abort::sleep, Abort::dmaFault}) {
      Model m(kPinned);
      const auto token = startRelease(m);
      if (phase != Phase::releasing) startRamp(m, token);
      if (phase >= Phase::waitingPort) powerPort(m);

      if (phase == Phase::resetting) m.port(token, {Protocol::usb2, kConnect}, kEntry + 3);
      if (phase >= Phase::enumerating) m.port(token, {Protocol::usb3, kEnabledSs | kPortCsc}, kEntry + 3);
      if (phase == Phase::online) assert(m.enumerated(token, true, true, kEntry + 4));
      expect(m, phase);

      uint64_t now = kEntry + 5;
      End reason = End::witness;
      switch (abort) {
        case Abort::detach:
          detach(m, now);
          reason = End::detach;
          break;
        case Abort::role:
          assert(Notification(now, 2, true, false, kPartnerUfp).send(m, now));
          reason = End::role;
          break;
        case Abort::cc:
          sourceAttach(m, now, 2, true);
          reason = End::role;
          break;
        case Abort::malformed: {
          auto malformed = Notification(now, 2, false, false, kPartnerNone);
          malformed.pan[13] = 7;
          assert(!malformed.send(m, now));
          assert(!Notification(now, 2, false, false, kPartnerNone).send(m, now));
          assert(!m.connector().known && m.detachRequired());
          break;
        }
        case Abort::relay:
          m.relayLost(now);
          assert(!m.connector().known);
          break;
        case Abort::expire: {
          auto brief = Notification(now, 2, true, true, kPartnerUfp);
          brief.status.expires_ms = now;
          assert(brief.send(m, now));
          expect(m, phase);
          assert(m.advance(++now));
          assert(!m.connector().known);
          break;
        }
        case Abort::sleep: {
          m.suspend(now);
          reason = End::sleep;
          assert(!m.gadgetActive());
          const auto preWake = Notification(now + 1, 2, true, true, kPartnerUfp);
          assert(!preWake.send(m, now + 1) && !m.requestHost(now + 1));
          if (phase != Phase::releasing) {
            m.xhci(token, 0, kUsbStsHalted, now + 1);
            m.power(kOff, now + 2);
            expect(m, Phase::reclaiming, reason);
            assert(!m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, now + Model::kReclaimMs + 3));
          }
          now += Model::kReclaimMs + 4;
          m.wake(now);
          assert(!preWake.send(m, now) && !m.requestHost(now) && !m.connector().known);
          if (phase != Phase::releasing) assert(m.reclaim(token, kHoDevice, dwc3::GCTL_PRTCAP_DEVICE, kPinned, now));
          break;
        }
        case Abort::dmaFault:
          m.power({Level::safe5v, false, false, dma}, now);
          assert(m.latchedFaults() == dma);
          reason = End::fault;
          break;
      }

      if (abort == Abort::sleep || phase == Phase::releasing) expect(m, Phase::device, reason);
      else returnDevice(m, token, now + 1, reason);
      if (abort != Abort::detach) assert(m.detachRequired());
    }
  }
}

}  // namespace

int main()
{
  connectorDecoding();
  drpAndReplay();
  releaseAndReclaim();
  powerInterlocks();
  deadlines();
  rootHub();
  haltProof();
  abortEveryLoanPhase();
  std::puts("role_vbus_host: B7 connector, exclusive loan, VBUS and xHCI invariants passed");
}
