// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -pthread -I.. hci_link_model_host.cpp -o /tmp/hci_link_model_host && /tmp/hci_link_model_host
#include "BtHciLinkModel.h"
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <limits>
#include <thread>
#include <vector>

using namespace pianobt;
using namespace pianobt::host;
using Power = HciLinkModel::Power;
using Security = HciLinkModel::Security;
using Packet = std::vector<uint8_t>;
using Peer = std::array<uint8_t, 7>;   // type and six wire-order bytes

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// Invented handles, peers and keys. Only opcodes, event layouts and bit positions are wire constants.
constexpr uint16_t kHandle = 0x02a5, kOtherHandle = 0x0106;
constexpr Peer kPeer = {1, 1, 2, 3, 4, 5, 0xc6}, kOtherPeer = {1, 7, 8, 9, 10, 11, 0xcc};
constexpr HciLinkModel::Key kKey = {1, 2, 3, 4};
constexpr std::array<uint8_t, 8> kFeatures = {0xb4, 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7};

static Packet command(uint16_t opcode, const Packet &p = {})
{
  Packet b(4 + p.size());
  CHECK(p.size() <= 255);
  CHECK(h4Command(b.data(), b.size(), opcode, p.data(), uint8_t(p.size())) == b.size());
  return b;
}

static Packet status(uint16_t opcode, uint8_t result = 0, uint8_t credit = 1)
{
  return {0x04, 0x0f, 4, result, credit, uint8_t(opcode), uint8_t(opcode >> 8)};
}

static Packet complete(uint16_t opcode, uint8_t result = 0, uint8_t credit = 1)
{
  return {0x04, 0x0e, 4, credit, uint8_t(opcode), uint8_t(opcode >> 8), result};
}

static Packet create(uint16_t opcode, const Peer &peer = kPeer)
{
  // Scan interval/window, connection interval min/max, latency, timeout, event length min/max.
  const Packet params = {0x10, 0, 0x10, 0, 6, 0, 12, 0, 0, 0, 20, 0, 0, 0, 0, 0};
  Packet p;
  if (opcode == kCreate) {
    p = {0x10, 0, 0x10, 0, 0};
    p.insert(p.end(), peer.begin(), peer.end());
    p.push_back(0);   // own address type
    p.insert(p.end(), params.begin() + 4, params.end());
  } else {
    p = {0, 0};   // explicit peer, public own address
    p.insert(p.end(), peer.begin(), peer.end());
    p.push_back(0x05);   // LE 1M and LE Coded PHYs
    for (int i = 0; i < 2; ++i) p.insert(p.end(), params.begin(), params.end());
  }
  return command(opcode, p);
}

static Packet connection(uint8_t subevent, uint8_t result = 0, uint16_t handle = kHandle, const Peer &peer = kPeer)
{
  const uint8_t length = subevent == 1 ? 19 : subevent == 0x0a ? 31 : 34;
  Packet p(size_t(3) + length);
  const Packet prefix = {4, 0x3e, length, subevent, result, uint8_t(handle), uint8_t(handle >> 8), 0};
  std::copy(prefix.begin(), prefix.end(), p.begin());
  std::copy(peer.begin(), peer.end(), p.begin() + 8);
  const size_t interval = subevent == 1 ? 15 : 27;
  p[interval] = 12;   // 15 ms
  p[interval + 4] = 20;   // 200 ms supervision timeout
  if (subevent == 0x29) std::fill(p.end() - 3, p.end(), 0xff);   // no advertising or sync handle
  return p;
}

static Packet linkCommand(uint16_t opcode, uint16_t handle = kHandle)
{
  Packet p = {uint8_t(handle), uint8_t(handle >> 8)};
  if (opcode == kDisconnect) p.push_back(0x13);
  return command(opcode, p);
}

static Packet version(uint16_t handle = kHandle, uint8_t result = 0)
{
  return {4, 0x0c, 8, result, uint8_t(handle), uint8_t(handle >> 8), 0x0c, 0, 0, 0, 0};
}

static Packet features(uint16_t handle = kHandle, uint8_t result = 0)
{
  Packet p = {4, 0x3e, 12, 4, result, uint8_t(handle), uint8_t(handle >> 8)};
  p.insert(p.end(), kFeatures.begin(), kFeatures.end());
  return p;
}

static Packet disconnected(uint16_t handle = kHandle, uint8_t reason = 0x13, uint8_t result = 0)
{
  return {4, 5, 4, result, uint8_t(handle), uint8_t(handle >> 8), reason};
}

static Packet completed(uint16_t handle, uint16_t count)
{
  return {4, 0x13, 5, 1, uint8_t(handle), uint8_t(handle >> 8), uint8_t(count), uint8_t(count >> 8)};
}

static Packet acl(uint16_t handle = kHandle)
{
  // First fragment with an SMP Pairing Request on L2CAP CID 0x0006.
  return {2, uint8_t(handle), uint8_t(handle >> 8), 11, 0, 7, 0, 6, 0, 1, 3, 0, 0x09, 16, 0, 0};
}

static bool send(HciLinkModel &m, uint64_t epoch, Packet frame)
{
  return m.transmit(epoch, frame);
}

static uint64_t boot(HciLinkModel &m)
{
  const auto off = m.snapshot();
  CHECK(off.power == Power::off);
  CHECK(m.beginWake(off.epoch));
  const auto epoch = m.snapshot().epoch;
  CHECK(epoch != off.epoch);
  CHECK(m.receive(epoch, complete(kReset)));
  CHECK(send(m, epoch, command(kSetLeEventMask, {0x1f, 0x10, 0, 0, 0, 0, 0, 0})));
  CHECK(m.receive(epoch, complete(kSetLeEventMask)));
  CHECK(m.snapshot().power == Power::ready && m.snapshot().eventMask == 0x185f);
  return epoch;
}

static uint64_t connect(HciLinkModel &m, uint64_t epoch, uint16_t opcode = kCreate, uint8_t sub = 1, const Peer &peer = kPeer)
{
  CHECK(send(m, epoch, create(opcode, peer)));
  CHECK(m.receive(epoch, status(opcode)));
  CHECK(m.receive(epoch, connection(sub, 0, kHandle, peer)));
  CHECK(send(m, epoch, linkCommand(kReadRemoteVersion)));
  CHECK(m.receive(epoch, status(kReadRemoteVersion)));
  CHECK(m.receive(epoch, version()));
  CHECK(send(m, epoch, linkCommand(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, status(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, features()));
  const auto ready = m.snapshot();
  CHECK(ready.handle == kHandle && ready.versionRead && ready.featuresRead && !ready.pending && !ready.procedure);
  CHECK(ready.features.bytes == kFeatures && ready.features.dataLengthExtension() && ready.features.phy2M());
  return ready.linkId;
}

static void checkReleased(const HciLinkModel &m)
{
  const auto s = m.snapshot();
  CHECK(s.handle == kNoHandle && !s.pending && !s.procedure && !s.waitingForLinkLoss);
  CHECK(s.aclInFlight == 0 && s.aclCredits() == HciLinkModel::kAclCapacity);
  CHECK(!s.versionRead && !s.featuresRead && s.features.bytes == LeFeatures{}.bytes && s.security == Security::idle);
}

static void testFramesAndMasks()
{
  const std::array<Packet, 8> frames = {
    Packet{1, 3, 0x0c, 0}, Packet{2, 0xa5, 0x22, 0, 0}, Packet{3, 0xa5, 2, 0}, Packet{4, 0xff, 0},
    command(0x0c01, Packet(255, 0xa1)), acl(), Packet{3, 0xa5, 2, 255}, Packet{4, 0xff, 255},
  };
  for (auto frame : frames) {
    if (frame[0] == kH4Sco && frame[3] == 255) frame.resize(4 + 255);
    if (frame[0] == kH4Event && frame[2] == 255) frame.resize(3 + 255);
    CHECK(h4FrameSize(frame) == frame.size());
    for (size_t n = 0; n < frame.size(); ++n) CHECK(!h4FrameSize(std::span(frame).first(n)));
    for (uint8_t tail : {0x00, 0x01, 0x04, 0xfd}) {
      Packet appended = frame;
      appended.push_back(tail);
      CHECK(!h4FrameSize(appended));
    }
    frame.insert(frame.end(), {4, 0xff, 0});
    CHECK(!h4FrameSize(frame));
  }
  for (uint8_t type : {0x00, 0x05, 0x06, 0xfc, 0xfd, 0xfe, 0xff}) {
    const Packet unknown = {type, 4, 0xff, 0};
    CHECK(!h4FrameSize(unknown));
  }
  for (uint16_t size : {1024, 1025, 65535}) {
    Packet b = {2, 0xa5, 2, uint8_t(size), uint8_t(size >> 8)};
    CHECK(!h4FrameSize(b));
    b.resize(size_t(5) + size);
    CHECK(h4FrameSize(b) == (size == 1024 ? 1029u : 0u));
  }

  const Packet raw = {1, 1, 0x20, 8, 0x1f, 0x10, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
  const Packet patched = {1, 1, 0x20, 8, 0x5f, 0x18, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
  auto mask = raw;
  CHECK(patchLeEventMask(mask, false) && mask == raw);
  CHECK(patchLeEventMask(mask, true) && mask == patched);
  CHECK(patchLeEventMask(mask, true) && mask == patched);
  auto noConnection = raw;
  noConnection[4] &= 0xfe;
  mask = noConnection;
  CHECK(patchLeEventMask(mask, true) && mask == noConnection);
  for (size_t n = 0; n < raw.size(); ++n) {
    Packet prefix(raw.begin(), raw.begin() + n);
    const auto before = prefix;
    CHECK(!patchLeEventMask(prefix, true) && prefix == before);
  }

  auto leFeatures = complete(0x2003);
  leFeatures[2] += 8;
  leFeatures.insert(leFeatures.end(), kFeatures.begin(), kFeatures.end());
  auto classicFeatures = leFeatures;
  classicFeatures[4] = 0x03;
  classicFeatures[5] = 0x10;
  const auto featureCommand = command(0x2074, {0x20, 1});   // LE Set Host Feature
  auto aclLookalike = Packet{2, 0xa5, 0x22, uint8_t(raw.size()), 0};
  aclLookalike.insert(aclLookalike.end(), raw.begin(), raw.end());
  for (const auto &unchanged : {command(0x0c01, Packet(8, 0x1f)), command(0x0c63, Packet(8, 0x1f)),
                                command(kSetLeEventMask, Packet(7, 0x1f)), command(kSetLeEventMask, Packet(9, 0x1f)),
                                command(0x2003), command(0x1003), featureCommand, leFeatures, classicFeatures,
                                features(), aclLookalike}) {
    auto input = unchanged;
    CHECK(patchLeEventMask(input, true) && input == unchanged);
  }
  const LeFeatures onlyLength{{0x20}};
  const LeFeatures only2M{{0, 0x01}};
  CHECK(onlyLength.dataLengthExtension() && !onlyLength.phy2M());
  CHECK(!only2M.dataLengthExtension() && only2M.phy2M());
}

static void testRepliesAndConnection()
{
  HciLinkModel m;
  const auto epoch = boot(m);
  auto masked = command(kSetLeEventMask, {0x1f, 0, 0, 0, 0, 0, 0, 0x80});
  CHECK(m.transmit(epoch, masked));
  CHECK(masked == Packet({1, 1, 0x20, 8, 0x5f, 0x08, 0, 0, 0, 0, 0, 0x80}));
  const Packet goodComplete = {4, 0x0e, 4, 1, 1, 0x20, 0};
  for (auto bad : {complete(0x0120), complete(0x2101), status(kSetLeEventMask), Packet{4, 0x0e, 3, 1, 1, 0x20},
                   Packet{4, 0x0e, 5, 1, 1, 0x20, 0, 0}}) {
    CHECK(!m.receive(epoch, bad));
    CHECK(m.snapshot().pending == kSetLeEventMask && m.snapshot().eventMask == 0x185f);
  }
  CHECK(m.receive(epoch, goodComplete));
  CHECK(m.snapshot().eventMask == 0x800000000000085fULL && !m.snapshot().pending);
  CHECK(!m.receive(epoch, goodComplete));
  CHECK(send(m, epoch, command(kSetLeEventMask, Packet(8, 0))));
  CHECK(m.receive(epoch, complete(kSetLeEventMask, 0x12)));
  CHECK(m.snapshot().eventMask == 0x800000000000085fULL && !m.snapshot().pending);

  auto broken = create(kExtendedCreate);
  broken[13] = 1;   // PHY count disagrees with length
  CHECK(!m.transmit(epoch, broken));
  broken[13] = 0x85;   // reserved PHY bit
  CHECK(!m.transmit(epoch, broken));
  CHECK(!m.snapshot().attempts);
  CHECK(send(m, epoch, create(kCreate)));
  CHECK(!send(m, epoch, create(kExtendedCreate)));
  CHECK(!m.receive(epoch, connection(1)));   // Command Status must arrive first
  const Packet goodStatus = {4, 0x0f, 4, 0, 0, 0x0d, 0x20};
  for (const auto &bad : {status(0x200e), status(0x210d), status(0x0d20), complete(kCreate),
                           Packet{4, 0x0f, 3, 0, 1, 0x0d}, Packet{4, 0x0f, 5, 0, 1, 0x0d, 0x20, 0}}) {
    CHECK(!m.receive(epoch, bad));
    CHECK(m.snapshot().pending == kCreate && !m.snapshot().commandCredit);
  }
  CHECK(m.receive(epoch, goodStatus));
  CHECK(!m.receive(epoch, status(kCreate, kConnectionTimeout)));
  CHECK(!m.snapshot().pending && m.snapshot().procedure == kCreate);
  CHECK(!m.receive(epoch, connection(1, 0, 0x12a5)));
  CHECK(!m.receive(epoch, connection(1, 0, 0x0f00)));
  CHECK(!m.receive(epoch, connection(1, 0, kHandle, kOtherPeer)));
  auto shortConn = connection(1);
  --shortConn[2];
  shortConn.pop_back();
  CHECK(!m.receive(epoch, shortConn));
  CHECK(m.receive(epoch, connection(1)));
  CHECK(!m.receive(epoch, connection(1)));
  CHECK(!send(m, epoch, linkCommand(kReadRemoteVersion)));   // the matched status advertised zero credit
  CHECK(m.receive(epoch, Packet{4, 0x0e, 3, 1, 0, 0}));
  CHECK(!send(m, epoch, linkCommand(kReadRemoteVersion, kOtherHandle)));
  CHECK(send(m, epoch, linkCommand(kReadRemoteVersion)));
  CHECK(!m.receive(epoch, version()));
  // An unrelated credit announcement cannot complete the in-flight command.
  CHECK(m.receive(epoch, status(0)));
  CHECK(m.snapshot().pending == kReadRemoteVersion && !m.snapshot().versionRead);
  CHECK(m.receive(epoch, status(kReadRemoteVersion)));
  CHECK(!m.receive(epoch, version(kOtherHandle)));
  CHECK(!m.receive(epoch, features()));
  CHECK(m.receive(epoch, version()));
  CHECK(!send(m, epoch, linkCommand(kReadRemoteVersion)));

  CHECK(send(m, epoch, linkCommand(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, status(kReadRemoteFeatures)));
  auto badFeatures = features();
  ++badFeatures[2];
  badFeatures.push_back(0);
  CHECK(!m.receive(epoch, badFeatures));
  CHECK(m.receive(epoch, features()));
  CHECK(m.snapshot().features.bytes == kFeatures);
  CHECK(send(m, epoch, linkCommand(kDisconnect)));
  CHECK(m.receive(epoch, status(kDisconnect)));
  CHECK(!send(m, epoch, acl()));
  CHECK(m.receive(epoch, disconnected(kHandle, 0x13, 0x0c)));   // failed disconnect does not release the handle
  CHECK(m.snapshot().handle == kHandle && !m.snapshot().procedure);
  CHECK(send(m, epoch, linkCommand(kDisconnect)));
  CHECK(m.receive(epoch, status(kDisconnect)));
  CHECK(m.receive(epoch, disconnected()));
  checkReleased(m);
}

static void testTimeoutRecovery()
{
  HciLinkModel m;
  const auto epoch = boot(m);
  CHECK(send(m, epoch, create(kCreate)));
  CHECK(m.receive(epoch, status(kCreate, kConnectionTimeout)));
  checkReleased(m);
  CHECK(m.snapshot().retryMs == 20);
  m.advance(epoch, 19);
  CHECK(!send(m, epoch, create(kExtendedCreate)));
  m.advance(epoch, 1);
  CHECK(!send(m, epoch, create(kExtendedCreate, kOtherPeer)));
  CHECK(send(m, epoch, create(kExtendedCreate)));
  CHECK(m.receive(epoch, status(kExtendedCreate)));
  CHECK(!m.receive(epoch, connection(1)));
  CHECK(m.receive(epoch, connection(0x0a, kFailedToEstablish, 0xffff, kOtherPeer)));
  checkReleased(m);
  CHECK(m.snapshot().retryMs == 40);
  m.advance(epoch, 39);
  CHECK(!send(m, epoch, create(kExtendedCreate)));
  m.advance(epoch, 1);
  CHECK(send(m, epoch, create(kExtendedCreate)));
  CHECK(m.receive(epoch, status(kExtendedCreate)));
  CHECK(m.receive(epoch, connection(0x29)));
  CHECK(send(m, epoch, linkCommand(kReadRemoteVersion)));
  CHECK(m.receive(epoch, status(kReadRemoteVersion)));
  CHECK(m.receive(epoch, version()));

  auto fragment = acl();
  fragment[2] |= 0x20;
  CHECK(!m.transmit(epoch, fragment));   // auto-flushable start is invalid for LE host TX
  CHECK(send(m, epoch, acl()));
  fragment[2] = uint8_t((kHandle >> 8) | 0x10);
  CHECK(m.transmit(epoch, fragment));   // continuation with the same 12-bit handle
  for (unsigned n = 2; n < HciLinkModel::kAclCapacity; ++n) CHECK(send(m, epoch, acl()));
  CHECK(!send(m, epoch, acl()));
  CHECK(!m.receive(epoch, completed(kOtherHandle, 1)));
  CHECK(!m.receive(epoch, completed(kHandle, HciLinkModel::kAclCapacity + 1)));
  CHECK(m.receive(epoch, completed(kHandle, 1)));
  CHECK(m.snapshot().aclCredits() == 1 && !send(m, epoch, acl(kOtherHandle)));
  CHECK(send(m, epoch, acl()));
  CHECK(send(m, epoch, linkCommand(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, status(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, features(kHandle, kConnectionTimeout)));
  CHECK(m.snapshot().handle == kHandle && m.snapshot().waitingForLinkLoss && m.snapshot().aclCredits() == 0);
  CHECK(!m.receive(epoch, disconnected(kOtherHandle, kConnectionTimeout)));
  CHECK(m.receive(epoch, disconnected(kHandle, kConnectionTimeout)));
  CHECK(!m.receive(epoch, completed(kHandle, 1)) && !m.receive(epoch, features()));
  checkReleased(m);

  for (int i = 0; i < 10; ++i) {
    m.advance(epoch, 1000);
    CHECK(!send(m, epoch, create(kCreate)) && !send(m, epoch, create(kExtendedCreate)));
  }
  CHECK(m.snapshot().attempts == HciLinkModel::kMaxAttempts && m.snapshot().power == Power::ready);
  CHECK(m.renewRetryBudget(epoch));
  CHECK(!m.renewRetryBudget(epoch));
  const auto link = connect(m, epoch, kExtendedCreate, 0x0a);
  CHECK(link != 0 && m.snapshot().attempts == 1);

  // A spontaneous disconnect can precede Status for a link-bound command.
  CHECK(send(m, epoch, acl()));
  CHECK(send(m, epoch, linkCommand(kDisconnect)));
  CHECK(m.receive(epoch, disconnected(kHandle, kConnectionTimeout)));
  CHECK(m.snapshot().handle == kNoHandle && m.snapshot().aclInFlight == 0);
  m.advance(epoch, 20);
  CHECK(!send(m, epoch, create(kCreate)));   // still owes the wire acknowledgement
  CHECK(!m.receive(epoch, status(kReadRemoteFeatures)));
  CHECK(m.receive(epoch, status(kDisconnect)));
  checkReleased(m);
  CHECK(send(m, epoch, create(kCreate)));
}

static void testBondAndOrderedSuspend()
{
  HciLinkModel m;
  const auto epoch = boot(m);
  CHECK(!m.beginPairing(0));
  const auto firstLink = connect(m, epoch);
  CHECK(!m.acceptsHid(firstLink) && !m.enableHogp(firstLink));
  CHECK(!m.restoreEncryption(firstLink, kKey) && !m.finishPairing(firstLink, kKey));
  CHECK(m.beginPairing(firstLink) && !m.beginPairing(firstLink));
  CHECK(m.finishPairing(firstLink, std::nullopt));
  CHECK(!m.snapshot().bonded && !m.acceptsHid(firstLink));
  CHECK(m.beginPairing(firstLink) && m.finishPairing(firstLink, kKey));
  CHECK(m.snapshot().bonded && !m.acceptsHid(firstLink));
  CHECK(m.enableHogp(firstLink) && m.acceptsHid(firstLink));

  CHECK(send(m, epoch, acl()));
  CHECK(send(m, epoch, command(kSetLeEventMask, {1, 0, 0, 0, 0, 0, 0, 0})));
  CHECK(m.beginSuspend(epoch));
  CHECK(m.snapshot().power == Power::draining && !m.acceptsHid(firstLink));
  CHECK(!m.beginSuspend(epoch) && !m.beginWake(epoch) && !m.powerOff(epoch));
  CHECK(!send(m, epoch, acl()) && !send(m, epoch, command(kReset)));
  CHECK(!send(m, epoch, create(kCreate)) && !send(m, epoch, linkCommand(kDisconnect)));
  CHECK(!m.resetAfterDrain(epoch));
  CHECK(m.receive(epoch, complete(kSetLeEventMask)));
  CHECK(!m.resetAfterDrain(epoch));   // ACL still in flight
  CHECK(m.receive(epoch, completed(kHandle, 1)));
  CHECK(!m.powerOff(epoch) && !m.receive(epoch, complete(kReset)));
  CHECK(m.resetAfterDrain(epoch));
  CHECK(!m.resetAfterDrain(epoch) && !m.beginWake(epoch) && !m.powerOff(epoch));
  CHECK(!m.receive(epoch, status(kReset)) && !m.receive(epoch, complete(kSetLeEventMask)));
  CHECK(m.receive(epoch, complete(kReset)));
  CHECK(m.snapshot().power == Power::powerDownReady && !m.snapshot().forcedPowerCycle);
  checkReleased(m);
  CHECK(m.snapshot().bonded && !m.beginWake(epoch));
  CHECK(m.powerOff(epoch) && m.beginWake(epoch));

  const auto wake = m.snapshot().epoch;
  CHECK(wake != epoch && m.snapshot().power == Power::wakeReset && !m.snapshot().eventMask);
  CHECK(!m.receive(epoch, complete(kReset)));
  CHECK(!send(m, wake, create(kExtendedCreate)) && !send(m, wake, acl()));
  CHECK(m.receive(wake, complete(kReset)));
  CHECK(!send(m, wake, command(kSetLeEventMask, Packet(8, 0))));
  CHECK(send(m, wake, command(kSetLeEventMask, {0x1f, 0, 0, 0, 0, 0, 0, 0})));
  CHECK(!send(m, wake, create(kExtendedCreate)) && !m.beginSuspend(wake));
  CHECK(m.receive(wake, complete(kSetLeEventMask)));
  CHECK(m.snapshot().eventMask == 0x85f && m.snapshot().power == Power::ready);
  const auto secondLink = connect(m, wake, kExtendedCreate, 0x0a);
  CHECK(secondLink != firstLink && m.snapshot().security == Security::idle && !m.acceptsHid(secondLink));
  CHECK(!send(m, epoch, acl()) && !m.receive(epoch, disconnected()));
  CHECK(!m.finishPairing(firstLink, kKey) && !m.restoreEncryption(firstLink, kKey));
  CHECK(!m.restoreEncryption(secondLink, HciLinkModel::Key{9}));
  CHECK(!m.enableHogp(secondLink) && m.restoreEncryption(secondLink, kKey));
  CHECK(!m.acceptsHid(secondLink) && m.enableHogp(secondLink) && m.acceptsHid(secondLink));

  CHECK(m.receive(wake, disconnected()));
  checkReleased(m);
  const auto otherLink = connect(m, wake, kCreate, 1, kOtherPeer);
  CHECK(!m.restoreEncryption(otherLink, kKey));
  CHECK(m.beginPairing(otherLink));
  CHECK(m.receive(wake, disconnected()));
  CHECK(!m.finishPairing(otherLink, HciLinkModel::Key{9}));
  const auto finalLink = connect(m, wake);
  CHECK(m.restoreEncryption(finalLink, kKey));
}

static void testDrainAsyncDisconnect()
{
  HciLinkModel m;
  const auto epoch = boot(m);
  connect(m, epoch);
  CHECK(send(m, epoch, acl()) && send(m, epoch, linkCommand(kDisconnect)));
  CHECK(m.beginSuspend(epoch));
  CHECK(!m.resetAfterDrain(epoch));
  CHECK(m.receive(epoch, status(kDisconnect)));
  CHECK(!m.resetAfterDrain(epoch));   // Command Status leaves the procedure in flight
  CHECK(m.receive(epoch, disconnected()));   // completes the procedure and flushes the ACL credit
  checkReleased(m);
  CHECK(!m.powerOff(epoch) && m.resetAfterDrain(epoch));
  CHECK(m.receive(epoch, complete(kReset)) && m.powerOff(epoch));
}

static void testRecoveryDeadlines()
{
  for (int mode = 0; mode < 6; ++mode) {
    HciLinkModel m;
    const auto epoch = boot(m);
    uint64_t timeout = HciLinkModel::kResponseMs;
    if (mode == 0 || mode == 1) {
      CHECK(send(m, epoch, create(kCreate)));
      if (mode == 1) CHECK(m.receive(epoch, status(kCreate)));
      m.advance(epoch, timeout - 1);
      CHECK(m.receive(epoch, status(0)));
      CHECK(!m.receive(epoch, status(kExtendedCreate)));
    } else if (mode == 2) {
      CHECK(m.receive(epoch, status(0, 0, 0)));   // stuck command credit
      m.advance(epoch, timeout - 1);
      CHECK(!send(m, epoch, create(kCreate)));
      CHECK(m.receive(epoch, status(0, 0, 0)));
    } else if (mode == 3) {
      const auto link = connect(m, epoch);
      CHECK(m.beginPairing(link) && m.finishPairing(link, kKey));
      CHECK(send(m, epoch, acl()));
      CHECK(send(m, epoch, linkCommand(kDisconnect)));
      CHECK(m.receive(epoch, status(kDisconnect)));
      CHECK(m.beginSuspend(epoch));
      timeout = HciLinkModel::kDrainMs;
      m.advance(epoch, timeout - 1);
      CHECK(!m.resetAfterDrain(epoch));
    } else {
      CHECK(m.beginSuspend(epoch) && m.resetAfterDrain(epoch));
      m.advance(epoch, timeout - 1);
      CHECK(!m.powerOff(epoch));
    }

    CHECK(m.snapshot().power != Power::powerDownReady && !m.powerOff(epoch));
    if (mode == 5) CHECK(m.receive(epoch, complete(kReset, 0x0c)));
    else m.advance(epoch, 1);
    CHECK(m.snapshot().power == Power::powerDownReady && m.snapshot().forcedPowerCycle);
    CHECK(m.snapshot().bonded == (mode == 3));
    checkReleased(m);
    CHECK(!m.receive(epoch, connection(1)) && !m.receive(epoch, complete(kReset)));
    CHECK(m.powerOff(epoch));
    const auto wake = boot(m);
    CHECK(!m.snapshot().forcedPowerCycle && !m.snapshot().attempts);
    m.advance(epoch, std::numeric_limits<uint64_t>::max());
    const auto link = connect(m, wake);
    if (mode == 3) CHECK(m.restoreEncryption(link, kKey));
  }

  for (bool resetAnswers : {false, true}) {
    HciLinkModel m;
    CHECK(m.beginWake(0));
    const auto epoch = m.snapshot().epoch;
    if (resetAnswers) CHECK(m.receive(epoch, complete(kReset)));
    m.advance(epoch, std::numeric_limits<uint64_t>::max());
    CHECK(m.snapshot().power == Power::powerDownReady && m.snapshot().forcedPowerCycle);
    checkReleased(m);
    CHECK(m.powerOff(epoch));
    boot(m);
  }
}

// Schedule both calls at the same boundary. Accept either legal linearization and
// verify the subsequent transition is possible exactly once.
template <class A, class B> static void race(A a, B b)
{
  std::latch go(2);
  std::thread first([&] { go.arrive_and_wait(); a(); });
  go.arrive_and_wait();
  b();
  first.join();
}

static void testRaces()
{
  for (int i = 0; i < 16; ++i) {
    HciLinkModel m;
    const auto epoch = boot(m);
    connect(m, epoch);
    bool accepted = false;
    race([&] { CHECK(m.beginSuspend(epoch)); }, [&] { accepted = send(m, epoch, acl()); });
    CHECK(m.snapshot().power == Power::draining && m.snapshot().aclInFlight == unsigned(accepted));
    CHECK(!send(m, epoch, acl()));
    if (accepted) CHECK(m.receive(epoch, completed(kHandle, 1)));
    race([&] { CHECK(m.resetAfterDrain(epoch)); }, [&] { CHECK(!m.beginWake(epoch)); });
    race([&] { CHECK(m.receive(epoch, complete(kReset))); }, [&] { CHECK(!m.beginWake(epoch)); });
    CHECK(m.snapshot().power == Power::powerDownReady);
    bool left = false, right = false;
    race([&] { left = m.powerOff(epoch); }, [&] { right = m.powerOff(epoch); });
    CHECK(left != right);
    race([&] { left = m.beginWake(epoch); }, [&] { right = m.beginWake(epoch); });
    CHECK(left != right && m.snapshot().epoch == epoch + 1 && m.snapshot().power == Power::wakeReset);
    CHECK(!m.powerOff(epoch) && !m.beginSuspend(epoch) && !m.receive(epoch, complete(kReset)));
    CHECK(m.receive(epoch + 1, complete(kReset)));
  }
}

int main()
{
  testFramesAndMasks();
  testRepliesAndConnection();
  testTimeoutRecovery();
  testBondAndOrderedSuspend();
  testDrainAsyncDisconnect();
  testRecoveryDeadlines();
  testRaces();
  std::puts("PASS offline H4 framing, LE mask and features, timeout recovery, SMP/HOGP and ordered sleep/wake");
}
