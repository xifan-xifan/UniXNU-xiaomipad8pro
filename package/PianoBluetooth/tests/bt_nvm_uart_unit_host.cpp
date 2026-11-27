// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synthetic firmware and H4 streams through the host GENI, SPMI and HCI models.
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -pthread -I.. -I../../include bt_nvm_uart_unit_host.cpp -o /tmp/bt_nvm_uart_unit_host && /tmp/bt_nvm_uart_unit_host
#include "BtBoardSim.h"
#include "BtHciLinkModel.h"
#include "BtUartRing.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pianobt;
using namespace pianobt::sim;
using namespace pianobt::host;
using Bytes = std::vector<uint8_t>;
using Power = HciLinkModel::Power;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

void setTlvLength(Bytes &image, size_t header, size_t length)
{
  for (unsigned i = 0; i < 3; ++i) image.at(header + 1 + i) = uint8_t(length >> (8 * i));
}

Bytes tag(uint16_t id, size_t length)
{
  Bytes bytes = {uint8_t(id), uint8_t(id >> 8), uint8_t(length), uint8_t(length >> 8), 0, 0, 0, 0, 0, 0, 0, 0};
  bytes.resize(bytes.size() + length, 0x5a);
  return bytes;
}

Bytes tagList(const Bytes &tags, bool envelope)
{
  Bytes image = {2, 0, 0, 0};
  setTlvLength(image, 0, tags.size());
  image.insert(image.end(), tags.begin(), tags.end());
  if (envelope) {
    image.insert(image.begin(), {4, 0, 0, 0});
    setTlvLength(image, 0, image.size() - 4);
  }
  return image;
}

void expectNvmError(Bytes image, std::string_view why)
{
  const NvmInfo info = patchNvm(image.data(), image.size(), 0x11, 0, 0);
  if (info.ok || !info.why || info.why != why) {
    std::fprintf(stderr, "NVM of %zu bytes: expected %.*s, got %s\n",
                 image.size(), int(why.size()), why.data(), info.why ? info.why : "(no error)");
    std::exit(1);
  }
}

void testNvmBounds()
{
  for (bool envelope : {false, true}) {
    expectNvmError(tagList({}, envelope), "empty tag list");
    const Bytes complete = tag(1, 3);
    for (size_t tail = 1; tail < 12; ++tail) {
      expectNvmError(tagList(Bytes(tail), envelope), "truncated tag header");
      Bytes tags = complete;
      tags.resize(tags.size() + tail);
      expectNvmError(tagList(tags, envelope), "truncated tag header");
    }

    Bytes overrun = complete;
    overrun.pop_back();
    expectNvmError(tagList(overrun, envelope), "a tag runs past the list");
    for (size_t length : {0u, 5u, 7u})
      expectNvmError(tagList(tag(kTagBdAddr, length), envelope), "the address tag is not six bytes");
    for (size_t length : {0u, 1u, 2u})
      expectNvmError(tagList(tag(kTagHci, length), envelope), "the HCI tag is shorter than three bytes");
    expectNvmError(tagList(tag(kTagDeepSleep, 0), envelope), "the sleep tag is empty");

    Bytes longList = tagList(complete, envelope);
    setTlvLength(longList, envelope ? 4 : 0, complete.size() + 1);
    expectNvmError(longList, "the tag list is longer than the file");
  }

  expectNvmError({4, 3, 0, 0, 2, 0, 0}, "an enclosing header without a second one");
  Bytes envelope = makeNvm(true, 0x83, 0x01);
  for (size_t length : {size_t(0), envelope.size() - 5, envelope.size() - 3}) {
    setTlvLength(envelope, 0, length);
    expectNvmError(envelope, "the enclosing length does not match the file");
  }
}

void testNvmPatching()
{
  for (bool envelope : {false, true}) {
    Bytes image = makeNvm(envelope, 0x35, 0xa4);
    if (envelope) {
      // The envelope can hold a second TLV after the Bluetooth tag list.
      image.insert(image.end(), {3, 2, 0, 0, 0x51, 0x52});
      setTlvLength(image, 0, image.size() - 4);
    }
    Bytes expected = image;
    // Offsets in makeNvm's unknown, address, HCI and deep-sleep tag sequence.
    const size_t header = envelope ? 8 : 4;
    const size_t hci = header + 14 + 18 + 12;
    const size_t sleep = hci + 16;
    expected.at(hci) = 0xb5;
    expected.at(hci + 1) = 0x11;
    expected.at(sleep) = 0xa5;
    NvmInfo info = patchNvm(image.data(), image.size(), 0x11, 1, 1);
    CHECK(info.ok && !info.why && info.tags == 4 && info.hci && info.sleep && info.address);
    CHECK(info.addressAt == header + 14 + 12);
    CHECK(info.hciBefore == 0x35 && info.baudBefore == 0 && info.sleepBefore == 0xa4);
    CHECK(info.hciAfter == 0xb5 && info.baudAfter == 0x11 && info.sleepAfter == 0xa5);
    CHECK(image == expected);

    expected.at(hci + 1) = 0x0e;
    info = patchNvm(image.data(), image.size(), 0x0e, -1, -1);
    CHECK(info.ok && image == expected);

    expected.at(hci) = 0x35;
    expected.at(hci + 1) = 0x00;
    expected.at(sleep) = 0xa4;
    info = patchNvm(image.data(), image.size(), 0x00, 0, 0);
    CHECK(info.ok && image == expected);
  }
}

void testDownloadPlans()
{
  constexpr std::array<uint8_t, 7> elf = {0x7f, 'E', 'L', 'F', 1, 1, 1};
  CHECK(planDownload(elf.data(), 6, 6, false).kind == FwKind::unknown);
  const DownloadPlan p = planDownload(elf.data(), 7, 7, false);
  CHECK(p.kind == FwKind::elf && p.mode == 3 && p.segments == 1 && segmentWaits(p, 0, 7));

  std::array<uint8_t, 28> tlv = {1};
  CHECK(planDownload(tlv.data(), 27, 27, false).kind == FwKind::unknown);
  CHECK(planDownload(tlv.data(), 14, 28, false).kind == FwKind::unknown);
  for (uint8_t mode = 0; mode <= 3; ++mode) {
    tlv[14] = mode;
    const DownloadPlan plan = planDownload(tlv.data(), 15, 2 * kSegmentMax, false);
    CHECK(plan.kind == FwKind::tlvPatch && plan.mode == mode && plan.segments == 2);
    CHECK(segmentWaits(plan, 0, kSegmentMax) == (mode == 0 || mode == 2));
    CHECK(segmentWaits(plan, 1, kSegmentMax));
  }
  for (uint32_t length = 0; length < 4; ++length)
    CHECK(planDownload(tlv.data(), length, length, true).kind == FwKind::unknown);
  const DownloadPlan nvm = planDownload(tlv.data(), 4, 4, true);
  CHECK(nvm.kind == FwKind::tlvNvm && nvm.mode == 0 && nvm.segments == 1);
}

void testUartRing()
{
  constexpr size_t kPackets = 2400;
  const std::array<Bytes, 4> headers = {{{kH4Command, 0x01, 0xfc, 0},
                                        {kH4Acl, 0x2a, 0x20, 0, 0},
                                        {kH4Event, kEvVendor, 0},
                                        {kH4Sco, 0x2a, 0x00, 0}}};
  std::vector<Bytes> frames;
  Bytes stream;
  for (size_t i = 0; i < kPackets; ++i) {
    if (i == kPackets / 2) stream.push_back(0x06);   // Unrecognized H4 type between packets.
    Bytes frame = headers[i % headers.size()];
    const bool acl = frame.front() == kH4Acl;
    const size_t payload = acl && i % 31 == 0 ? 257 : i % 47;
    frame[frame.size() - (acl ? 2 : 1)] = uint8_t(payload);
    if (acl) frame.back() = uint8_t(payload >> 8);
    for (size_t j = 0; j < payload; ++j) frame.push_back(uint8_t(i + j));
    stream.insert(stream.end(), frame.begin(), frame.end());
    frames.push_back(std::move(frame));
  }

  Board board;
  const Hw hw = hwFor(board);
  Bringup bt;
  Options options;
  options.level = 3;
  CHECK(bringUp(bt, hw, options, fwFor(board), nullptr, 0) == kBtOk);
  Port &port = bt.port;
  std::array<uint8_t, Board::kRxBytes> raw{};
  port.raw = raw.data();
  port.rawCap = raw.size();
  port.rawHave = 0;

  std::array<uint8_t, BT_TTY_FIFO> ring{};
  uint32_t head = BT_TTY_FIFO - 2, queued = 0;   // The first packet's header crosses the ring boundary.
  unsigned writeWraps = 0, readWraps = 0;
  size_t received = 0;
  H4Decoder decoder;
  const auto sink = [&](uint8_t type, const uint8_t *packet, size_t length) {
    CHECK(received < frames.size());
    const Bytes &expected = frames[received++];
    CHECK(type == expected.front() && length == expected.size() - 1 && packet);
    CHECK(std::equal(expected.begin() + 1, expected.end(), packet));
  };

  const uint64_t initialRx = port.rxBytes;
  for (size_t at = 0, poll = 0; at < stream.size(); ++poll) {
    const size_t count = std::min(stream.size() - at, 3 + poll % (raw.size() - 2));
    board.rxFifo.insert(board.rxFifo.end(), stream.begin() + at, stream.begin() + at + count);
    CHECK(portPoll(port) == count && port.rawHave == count && board.rxFifo.empty());
    CHECK(queued + port.rawHave <= ring.size());
    const uint32_t tail = (head + queued) % BT_TTY_FIFO;
    if (port.rawHave > BT_TTY_FIFO - tail) ++writeWraps;
    bt_tty_fifo_put(ring.data(), head, &queued, raw.data(), uint32_t(port.rawHave));
    port.rawHave = 0;
    at += count;

    // Read in differently sized chunks, keeping some bytes queued between polls.
    std::array<uint8_t, 17> chunk{};
    while (queued >= chunk.size() || (at == stream.size() && queued)) {
      const uint32_t n = std::min(queued, uint32_t(chunk.size()));
      if (n > BT_TTY_FIFO - head) ++readWraps;
      bt_tty_fifo_get(ring.data(), &head, &queued, chunk.data(), n);
      decoder.feed(chunk.data(), n, sink);
    }
  }
  CHECK(writeWraps > 1 && readWraps > 1 && !queued);
  CHECK(received == kPackets && decoder.idle());
  const auto &stats = decoder.stats();
  CHECK(stats.packets == kPackets && stats.resyncs == 1 && !stats.oversized && !stats.ibs);
  CHECK(stats.bytes == stream.size() && port.rxBytes - initialRx == stream.size() && !port.backpressurePolls);
  port.raw = nullptr;
  port.rawCap = 0;
  safeOff(bt, hw, options);
}

Bytes complete(uint16_t opcode)
{
  return {kH4Event, kEvCommandComplete, 4, 1, uint8_t(opcode), uint8_t(opcode >> 8), 0};
}

uint64_t drainLink(HciLinkModel &link, bool timeout)
{
  CHECK(link.beginWake(link.snapshot().epoch));
  const uint64_t epoch = link.snapshot().epoch;
  CHECK(link.receive(epoch, complete(kReset)));
  Bytes mask = {kH4Command, 0x01, 0x20, 8, 1, 0, 0, 0, 0, 0, 0, 0};
  CHECK(link.transmit(epoch, mask) && link.receive(epoch, complete(kSetLeEventMask)));
  CHECK(link.snapshot().power == Power::ready && link.snapshot().commandCredit);
  CHECK(link.snapshot().aclCredits() == HciLinkModel::kAclCapacity);

  // Connect to an explicit synthetic peer, then fill the ACL window on that handle.
  constexpr std::array<uint8_t, 7> peer = {1, 1, 2, 3, 4, 5, 0xc6};
  Bytes create = {kH4Command, 0x0d, 0x20, 25, 0x10, 0, 0x10, 0, 0};
  create.insert(create.end(), peer.begin(), peer.end());
  create.insert(create.end(), {0, 6, 0, 12, 0, 0, 0, 20, 0, 0, 0, 0, 0});
  const Bytes status = {kH4Event, kEvCommandStatus, 4, 0, 1, 0x0d, 0x20};
  Bytes connected = {kH4Event, 0x3e, 19, 1, 0, 0x2a, 0, 0};
  connected.insert(connected.end(), peer.begin(), peer.end());
  connected.insert(connected.end(), {12, 0, 0, 0, 20, 0, 0});
  CHECK(link.transmit(epoch, create) && !link.snapshot().commandCredit);
  CHECK(link.receive(epoch, status) && link.receive(epoch, connected));
  Bytes acl = {kH4Acl, 0x2a, 0, 4, 0, 0, 0, 4, 0};
  for (unsigned i = 0; i < HciLinkModel::kAclCapacity; ++i) CHECK(link.transmit(epoch, acl));
  CHECK(!link.snapshot().aclCredits() && !link.transmit(epoch, acl));

  CHECK(link.beginSuspend(epoch) && !link.resetAfterDrain(epoch));
  if (timeout) {
    link.advance(epoch, HciLinkModel::kDrainMs);
  } else {
    const Bytes completed = {kH4Event, 0x13, 5, 1, 0x2a, 0, HciLinkModel::kAclCapacity, 0};
    CHECK(link.receive(epoch, completed));
    CHECK(link.snapshot().commandCredit && link.snapshot().aclCredits() == HciLinkModel::kAclCapacity);
    CHECK(link.resetAfterDrain(epoch) && link.receive(epoch, complete(kReset)));
  }
  const auto down = link.snapshot();
  CHECK(down.power == Power::powerDownReady && down.forcedPowerCycle == timeout);
  CHECK(!down.pending && !down.commandCredit && down.aclCredits() == HciLinkModel::kAclCapacity);
  return epoch;
}

void testDownloadsAndPowerCycles()
{
  static_assert(kSegmentMax == 243);
  const std::vector<unsigned> cases[] = {{242}, {243}, {243, 1}, {243, 243}, {243, 243, 1}};
  Board board;
  const Hw hw = hwFor(board);
  const FwIo fw = fwFor(board);
  const auto initialRails = board.rails.enabled;
  Options options;
  options.level = 5;
  Bringup bt;
  HciLinkModel link;
  std::array<uint8_t, kNvmMaxBytes> scratch{};
  const auto checkOff = [&] {
    CHECK(board.rails.enabled == initialRails && !board.railState && !board.pmuOwner);
    CHECK(!board.powered && !(board.bten[kPgOutSrc] & kPgOutLevel));
    CHECK(!bt.up && !bt.power.railsHeld && !bt.uart.engineUp && !board.sActive && !board.mActive);
    CHECK(!board.g(kGccQup2Vote) && !board.unclocked);
    const auto off = link.snapshot();
    CHECK(off.power == Power::off && !off.commandCredit && off.aclCredits() == HciLinkModel::kAclCapacity);
  };

  for (const auto &lengths : cases) {
    size_t bytes = 0;
    for (unsigned length : lengths) bytes += length;
    Bytes patch(bytes);
    for (size_t i = 0; i < bytes; ++i) patch[i] = uint8_t(i * 7 + 3);
    constexpr std::array<uint8_t, 7> elf = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::copy(elf.begin(), elf.end(), patch.begin());
    Bytes tags = tag(kTagHci, 3);
    tags.at(12) = 0xda;   // IBS set in the image.
    const Bytes padding = tag(1, bytes - 8 - tags.size() - 12);
    tags.insert(tags.end(), padding.begin(), padding.end());
    const Bytes nvm = tagList(tags, true);
    Bytes patchedNvm = nvm;
    patchedNvm.at(8 + 12) = 0x5a;
    patchedNvm.at(8 + 13) = 0x11;
    board.chip.files["gngbtfw20.mbn"] = patch;
    board.chip.files["gngbtnv20.b2e"] = nvm;
    board.chip.expect[Sim::kPatch] = uint32_t(bytes);
    board.chip.expect[Sim::kNvm] = uint32_t(bytes);

    CHECK(bringUp(bt, hw, options, fw, scratch.data(), bytes) == kBtOk);
    CHECK(bt.up && bt.power.railsHeld && board.powered && board.rails.allOn());
    CHECK(bt.setup.rampatchPlan.kind == FwKind::elf && bt.setup.nvmPlan.kind == FwKind::tlvNvm);
    CHECK(board.chip.segmentLengths[Sim::kPatch] == lengths && board.chip.segmentLengths[Sim::kNvm] == lengths);
    CHECK(board.chip.received[Sim::kPatch] == patch && board.chip.received[Sim::kNvm] == patchedNvm);
    CHECK(bt.setup.acks == 1 + lengths.size());   // Final ELF segment, then every NVM segment.

    const uint64_t epoch = drainLink(link, bytes % 2 != 0);
    safeOff(bt, hw, options);
    CHECK(link.powerOff(epoch));
    checkOff();

    // qcaSetup rejects the file after the ELF transfer. bringUp must unwind before returning.
    board.chip.files["gngbtnv20.b2e"] = tagList(Bytes{0}, true);
    const unsigned transactions = board.rails.txs;
    CHECK(bringUp(bt, hw, options, fw, scratch.data(), scratch.size()) == kBtSetup);
    CHECK(bt.setup.step == -kStepNvm && bt.setup.error == kSetupBadFile);
    CHECK(bt.setup.nvmInfo.why && std::string_view(bt.setup.nvmInfo.why) == "truncated tag header");
    CHECK(board.rails.txs > transactions);
    checkOff();
  }
}
}  // namespace

int main()
{
  testNvmBounds();
  testNvmPatching();
  testDownloadPlans();
  testUartRing();
  testDownloadsAndPowerCycles();
  std::puts("PASS NVM bounds, firmware segment boundaries, 2400 H4 packets across GENI/tty wraps, and repeat power/credit cleanup");
}
