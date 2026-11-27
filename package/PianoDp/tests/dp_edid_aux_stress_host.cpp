// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. -I../../PianoDpu -I../../PianoUsbHost -I../../PianoDwc3 -I"${dp_stress_tables:=$(mktemp -d)}$(python3 ../../../tooling/dp/dp-phy-tables.py header ../../../data/dp "${dp_stress_tables:?}/DpQmpTables.h")" dp_edid_aux_stress_host.cpp -o /tmp/dp_edid_aux_stress_host && /tmp/dp_edid_aux_stress_host ; dp_stress_rc=$?; rm -f "${dp_stress_tables:?}/DpQmpTables.h"; rmdir "$dp_stress_tables"; exit "$dp_stress_rc"
#include "DpHandoffModel.h"
#include "DpQmpPhy.h"

#include <span>

namespace {
using namespace pianodp;
using namespace pianodp::test;

constexpr size_t kChecksum = kEdidBlockBytes - 1;
constexpr Descriptor kVga60{0xd5, 0x09, 0x80, 160, 0x20, 0xe0, 45, 0x10, 16, 96, 0xa2, 0, 0, 0, 0, 0, 0, 0x18};

// CTA native 1080p, UHD60 and an HDMI VSDB advertising all four HDMI VICs.
constexpr Block kCtaWithHdmi{
    2, 3, 23, 0, 0x43, 0x90, 97, 95, 0x6e, 3, 0x0c, 0, 0x10, 0, 0, 0, 0x20, 0, 0x80, 1, 2, 3, 4,
};

void setDisplayIdClock(Block &block, uint32_t pixelKhz)
{
  const uint32_t wire = (block[1] == 0x13 ? pixelKhz / 10 : pixelKhz) - 1;
  for (unsigned i = 0; i < 3; ++i) block[8 + i] = uint8_t(wire >> (8 * i));
  checksum(block, 1, 5 + block[2]);
}

Block displayId13(unsigned columns = 0)
{
  Block block = displayId(columns);
  block[1] = 0x13;
  block[5] = 0x03;
  block[6] = 0;
  if (columns) block[28] = 0x12;
  setDisplayIdClock(block, 241500);
  return block;
}

std::vector<uint8_t> fourBlockEdid(bool type7, const Descriptor &preferred = kHd60)
{
  Block first = type7 ? displayId() : displayId13();
  first[4] = 1;
  checksum(first, 1, 5 + first[2]);
  Block second = first;
  second[3] = second[4] = 0;
  setDisplayIdClock(second, 233000);
  return makeEdid(preferred, {kCtaWithHdmi, first, second});
}

void expectInvalid(std::span<const uint8_t> bytes, EdidStatus expected)
{
  static const auto valid = fourBlockEdid(true);
  EdidInfo info;
  CHECK(parseEdid(valid.data(), valid.size(), info) == EdidStatus::Ok);
  CHECK(parseEdid(bytes.data(), bytes.size(), info) == expected);
  CHECK(!info.modeCount && !info.extensionCount && !info.manufacturer[0] && !info.hdmiVicBits);
  CHECK(!info.hasVic(16) && !info.nativeVic(16) && !info.tile.columns && !info.modes[0].timing.pixelKhz);
  CHECK(!buildDpExternalModes(info, {kDpHbr2Khz, 4}, DpPixelFormat::Rgb8).count);
}

void testEdidFraming()
{
  for (bool type7 : {false, true}) {
    const Block did = type7 ? displayId() : displayId13();
    const std::array fixtures{makeEdid(kHd60), makeEdid(kHd60, {kCtaWithHdmi}),
                              makeEdid(kHd60, {kCtaWithHdmi, did}), fourBlockEdid(type7)};
    constexpr std::array<size_t, 4> modeCounts{1, 7, 8, 9};
    for (size_t i = 0; i < fixtures.size(); ++i) {
      const auto &bytes = fixtures[i];
      EdidInfo info;
      CHECK(parseEdid(bytes.data(), bytes.size(), info) == EdidStatus::Ok);
      CHECK(info.extensionCount == i && info.modeCount == modeCounts[i]);
      CHECK(info.modes[0].timing.pixelKhz == 148500);
      if (i >= 1) {
        CHECK(info.nativeVic(16) && info.hasVic(97) && info.hdmiVicBits == 0x0f);
        CHECK(info.hasVic(93) && info.hasVic(94) && info.hasVic(95) && info.hasVic(98));
      }
      if (i >= 2) CHECK(info.modes[7].timing.pixelKhz == 241500);
      if (i == 3) CHECK(info.modes[8].timing.pixelKhz == 233000);

      for (size_t length = 0; length < bytes.size(); ++length) {
        // Allocate only the supplied bytes so ASan can catch reads beyond the input.
        const std::vector<uint8_t> shortEdid(bytes.begin(), bytes.begin() + length);
        expectInvalid(shortEdid, EdidStatus::InvalidSize);
      }

      for (size_t block = 0; block <= i; ++block) {
        auto corrupt = bytes;
        corrupt[block * kEdidBlockBytes + kChecksum] ^= 1;
        expectInvalid(corrupt, EdidStatus::BadChecksum);
      }
    }

    auto badHeader = fixtures.back();
    badHeader[1] ^= 1;
    ++badHeader[kChecksum]; // retain a valid checksum
    expectInvalid(badHeader, EdidStatus::BadHeader);

    Block missingSection = did;
    missingSection[4] = 1;
    checksum(missingSection, 1, 5 + missingSection[2]);
    expectInvalid(makeEdid(kHd60, {kCtaWithHdmi, missingSection}), EdidStatus::BadExtension);

    Block badInnerChecksum = did;
    badInnerChecksum[5 + did[2]] ^= 1;
    expectInvalid(makeEdid(kHd60, {kCtaWithHdmi, badInnerChecksum}), EdidStatus::BadChecksum);

    // Reseal each truncated data-block collection, including sub-header sizes.
    for (size_t length = 1; length < did[2]; ++length) {
      Block truncated = did;
      truncated[2] = uint8_t(length);
      std::fill(truncated.begin() + 5 + length, truncated.end(), 0);
      checksum(truncated, 1, 5 + length);
      expectInvalid(makeEdid(kHd60, {kCtaWithHdmi, truncated}), EdidStatus::BadExtension);
    }

    for (size_t active : {12, 20}) {
      Block badTiming = did;
      badTiming[active] = badTiming[active + 1] = 0xff; // decode would exceed a uint16_t
      checksum(badTiming, 1, 5 + did[2]);
      expectInvalid(makeEdid(kHd60, {badTiming}), EdidStatus::BadTiming);
    }
  }
}

void testCtaFraming()
{
  for (size_t length = 1; length < kHd60.size(); ++length) {
    const size_t start = kChecksum - length;
    Block cta{2, 3, uint8_t(start)};
    std::copy_n(kHd60.begin(), length, cta.begin() + start);
    expectInvalid(makeEdid(kHd60, {cta}), EdidStatus::BadExtension);
  }

  for (uint8_t length = 0; length < 31; ++length) {
    Block cta{2, 3, uint8_t(5 + length), 0, uint8_t(0x40 | (length + 1))};
    std::fill_n(cta.begin() + 5, length, 16);
    expectInvalid(makeEdid(kHd60, {cta}), EdidStatus::BadExtension);
  }

  const Block full = kCtaWithHdmi;
  for (size_t length : {1, 2, 3, 4, 8, 9, 10, 11, 12, 13}) {
    Block cta = full;
    cta[2] = uint8_t(9 + length);
    cta[8] = uint8_t(0x60 | length);
    std::fill(cta.begin() + cta[2], cta.end(), 0);
    expectInvalid(makeEdid(kHd60, {cta}), EdidStatus::BadExtension);
  }

  Descriptor bad = kHd60;
  bad[0] = bad[1] = 0;
  expectInvalid(makeEdid(bad), EdidStatus::BadTiming);
  bad = kHd60;
  bad[2] = 0;
  bad[4] &= 0x0f;
  expectInvalid(makeEdid(bad), EdidStatus::BadTiming);
  bad = kHd60;
  bad[5] = 0;
  bad[7] &= 0x0f;
  expectInvalid(makeEdid(bad), EdidStatus::BadTiming);
}

void testAdmission()
{
  constexpr DpLink fullLink{kDpHbr2Khz, 4};
  for (bool type7 : {false, true}) {
    for (unsigned columns : {1u, 2u, 16u}) {
      const auto bytes = makeEdid(kHd60, {kCtaWithHdmi, type7 ? displayId(columns) : displayId13(columns)});
      EdidInfo info;
      CHECK(parseEdid(bytes.data(), bytes.size(), info) == EdidStatus::Ok);
      CHECK(info.tile.columns == columns && info.tile.rows == 1 && info.modeCount == 8);
      CHECK((preferredFittingMode(info, fullLink) != nullptr) == (columns == 1));
      CHECK((buildDpExternalModes(info, fullLink, DpPixelFormat::Rgb8).count != 0) == (columns == 1));
    }
  }

  // HBR2 payload and the two framing symbols per TU give different ceilings.
  for (unsigned lanes : {1u, 2u, 4u}) {
    const DpLink link{kDpHbr2Khz, lanes};
    const uint32_t payloadCeiling = 180000 * lanes, tuCeiling = 174375 * lanes;
    for (uint32_t clock : {tuCeiling - 1, tuCeiling, tuCeiling + 1, payloadCeiling - 1,
                           payloadCeiling, payloadCeiling + 1}) {
      Block did = displayId();
      setDisplayIdClock(did, clock);
      const auto bytes = makeEdid(kUhd60, {did});
      EdidInfo info;
      CHECK(parseEdid(bytes.data(), bytes.size(), info) == EdidStatus::Ok && info.modeCount == 2);
      const DpTiming &timing = info.modes[1].timing;
      CHECK(timing.pixelKhz == clock && edidModeFits(timing, link) == (clock <= payloadCeiling));
      DpMsaTu msa;
      CHECK(computeDpMsaTu(timing, link, msa) == (clock <= tuCeiling));
      if (clock == tuCeiling)
        CHECK(msa.validSymbols == 62 && msa.validSymbolsFloor == 62 && msa.validSymbolsNumerator == 0);
      if (lanes < 4) {
        const auto modes = buildDpExternalModes(info, link, DpPixelFormat::Rgb8);
        CHECK(modes.count == (clock <= tuCeiling ? 1u : 0u));
        if (modes.count) CHECK(modes.modes[0].pixelHz == clock * 1000u);
      }
    }
  }

  const auto uhd = makeEdid(kUhd60);
  EdidInfo info;
  CHECK(parseEdid(uhd.data(), uhd.size(), info) == EdidStatus::Ok);
  CHECK(buildDpExternalModes(info, fullLink, DpPixelFormat::Rgb8).count == 1);
  ++info.modes[0].timing.pixelKhz;
  CHECK(dpStreamFits(info.modes[0].timing, fullLink));
  CHECK(!buildDpExternalModes(info, fullLink, DpPixelFormat::Rgb8).count);
  CHECK(!buildDpExternalModes(info, {kDpHbr3Khz, 4}, DpPixelFormat::Rgb8).count);
}

enum class Training { Connect, Fallback, Oscillate };
enum class FaultAt { None, Caps, PatternWrite, StatusRead, PatternStop };

constexpr std::array<DpLink, 9> kFallback{{
    {kDpHbr2Khz, 4}, {kDpHbrKhz, 4}, {kDpRbrKhz, 4},
    {kDpHbr2Khz, 2}, {kDpHbrKhz, 2}, {kDpRbrKhz, 2},
    {kDpHbr2Khz, 1}, {kDpHbrKhz, 1}, {kDpRbrKhz, 1},
}};

struct TrainingAttempt {
  DpLink link;
  DpDrive drive{};
  unsigned crPolls = 0, eqPolls = 0, driveChanges = 0, stops = 0;
};

struct StressSink {
  explicit StressSink(Training training = Training::Connect) : training(training) {}

  const Training training;
  DpDrive request{};
  FaultAt faultAt = FaultAt::None;
  Aux fault = Aux::Ack;
  std::span<const uint8_t> edid;
  std::span<const Aux> burst;
  std::optional<size_t> burstOffset; // absent means repeat the burst at each chunk
  size_t edidOffset = 0, burstReplies = 0;
  uint32_t expectedPixelHz = 148500000;
  unsigned edidReads = 0, retryDelays = 0, probeReads = 0, nativeCalls = 0, statusDelays = 0;
  unsigned starts = 0, teardowns = 0;
  bool streaming = false;
  Pattern pattern = Pattern::Disabled;
  std::vector<TrainingAttempt> attempts;

  Aux readEdid(size_t offset, uint8_t *bytes, size_t size)
  {
    CHECK(++edidReads <= 128);
    CHECK(probeReads == 1 && attempts.empty() && size == 16 && offset == edidOffset);
    CHECK(offset + size <= edid.size());
    const Aux reply = !burstOffset || offset == *burstOffset ? burstAtOffset() : Aux::Ack;
    if (reply == Aux::Ack || reply == Aux::ShortTransfer)
      std::copy_n(edid.data() + offset, reply == Aux::Ack ? size : size - 1, bytes);
    if (reply == Aux::Ack) {
      edidOffset += size;
      chunkReplies = 0;
    }
    return reply;
  }

  Aux auxRead(uint32_t address, uint8_t *bytes, size_t size)
  {
    CHECK(++nativeCalls <= 512);
    if (address == 0) {
      CHECK(size == 16 && ++probeReads == 1 && attempts.empty());
      const std::array<uint8_t, 16> caps{0x14, 0x1e, 0x84, 0, 0, 0, 1}; // HBR3 sink, TPS2, 4 lanes
      return readReply(FaultAt::Caps, caps, bytes);
    }

    CHECK(address == 0x202 && size == 6 && !attempts.empty());
    CHECK(pattern == Pattern::Tps1 || pattern == Pattern::Tps2);
    auto &attempt = attempts.back();
    std::array<uint8_t, 6> status{};
    if (pattern == Pattern::Tps1) {
      CHECK(++attempt.crPolls <= 16);
      if (training == Training::Oscillate) status[4] = uint8_t(attempt.crPolls % 2);
      else status[0] = status[1] = 0x11;
    } else {
      CHECK(++attempt.eqPolls <= 6);
      status[0] = status[1] = 0x11;
      status[4] = uint8_t(request.swing | (request.preemphasis << 2));
      if (training == Training::Connect ||
          (training == Training::Fallback && attempts.size() == kFallback.size() && attempt.eqPolls == 6)) {
        status[0] = status[1] = 0x77;
        status[2] = 1;
      }
    }
    return readReply(FaultAt::StatusRead, status, bytes);
  }

  Aux auxWrite(uint32_t address, const uint8_t *bytes, size_t size)
  {
    CHECK(++nativeCalls <= 512 && !attempts.empty());
    auto &attempt = attempts.back();
    const DpDrive drive = attempt.drive;
    const uint8_t laneSet = uint8_t(drive.swing | (drive.preemphasis << 3) |
                                    (drive.swing == 3 ? 4 : 0) | (drive.swing + drive.preemphasis == 3 ? 32 : 0));
    switch (address) {
      case 0x100:
        CHECK(pattern == Pattern::Disabled && size == 2);
        CHECK(bytes[0] * 27000u == attempt.link.rateKhz && bytes[1] == (attempt.link.lanes | 0x80));
        return Aux::Ack;
      case 0x107:
        CHECK(pattern == Pattern::Disabled && size == 2 && bytes[0] == 0 && bytes[1] == 1);
        return Aux::Ack;
      case 0x102:
        CHECK(pattern == Pattern::Tps1 || pattern == Pattern::Tps2);
        if (size == 1) {
          CHECK(!bytes[0] && ++attempt.stops == 1);
          // A different cleanup fault must not overwrite the original status-read error.
          if (faultAt == FaultAt::StatusRead) return Aux::IoError;
          return faultAt == FaultAt::PatternStop ? fault : Aux::Ack;
        }
        CHECK(size == 1 + attempt.link.lanes && bytes[0] == (pattern == Pattern::Tps1 ? 0x21 : 0x22));
        CHECK(std::all_of(bytes + 1, bytes + size, [laneSet](uint8_t byte) { return byte == laneSet; }));
        return faultAt == FaultAt::PatternWrite ? fault : Aux::Ack;
      default:
        CHECK(address == 0x103 && pattern != Pattern::Disabled && size == attempt.link.lanes);
        CHECK(std::all_of(bytes, bytes + size, [laneSet](uint8_t byte) { return byte == laneSet; }));
        return Aux::Ack;
    }
  }

  bool setPhy(uint32_t rate, unsigned lanes, unsigned swing, unsigned preemphasis)
  {
    CHECK(probeReads == 1 && edidOffset == edid.size());
    if (pattern == Pattern::Disabled) {
      CHECK(attempts.size() < kFallback.size() && !swing && !preemphasis);
      CHECK(attempts.empty() || attempts.back().stops == 1);
      attempts.push_back({{rate, lanes}});
    } else {
      CHECK(!attempts.empty() && ++attempts.back().driveChanges <= 15);
      CHECK(attempts.back().link.rateKhz == rate && attempts.back().link.lanes == lanes);
    }
    auto &attempt = attempts.back();
    attempt.drive = {swing, preemphasis};
    DpQmpPhySettings settings;
    CHECK(lookupDpQmpPhy(rate, attempt.drive, settings));
    const bool high = rate == kDpHbr2Khz;
    const auto &swingTable = high ? piano_dp_qmp_swing_hbr3_hbr2 : piano_dp_qmp_swing_hbr_rbr;
    const auto &preTable = high ? piano_dp_qmp_pre_emphasis_hbr3_hbr2 : piano_dp_qmp_pre_emphasis_hbr_rbr;
    CHECK(settings.rate->link_clk_hz == rate * 1000u);
    CHECK(settings.swing == swingTable[swing][preemphasis] && settings.preemphasis == preTable[swing][preemphasis]);
    return true;
  }

  void setPattern(Pattern next) { pattern = next; }

  void delayUs(unsigned us)
  {
    switch (pattern) {
      case Pattern::Disabled:
        CHECK(us == 400 && ++retryDelays <= 96);
        break;
      case Pattern::Tps1:
      case Pattern::Tps2:
        CHECK(us == (pattern == Pattern::Tps1 ? 100u : 400u) && ++statusDelays <= 198);
        break;
      default:
        CHECK(false);
    }
  }

  bool startStream(DpLink link, const DpExternalTiming &mode)
  {
    CHECK(!streaming && ++starts == 1 && pattern == Pattern::Disabled);
    CHECK(!attempts.empty() && link.rateKhz == attempts.back().link.rateKhz && link.lanes == attempts.back().link.lanes);
    CHECK(mode.pixelHz == expectedPixelHz);
    streaming = true;
    return true;
  }

  void teardown()
  {
    CHECK(++teardowns == 1 && pattern == Pattern::Disabled);
    streaming = false;
  }

 private:
  size_t chunkReplies = 0;

  Aux burstAtOffset()
  {
    if (burst.empty()) return Aux::Ack;
    CHECK(chunkReplies < burst.size());
    ++burstReplies;
    return burst[chunkReplies++];
  }

  template <size_t N>
  Aux readReply(FaultAt at, const std::array<uint8_t, N> &reply, uint8_t *bytes) const
  {
    const Aux status = at == faultAt ? fault : Aux::Ack;
    if (status == Aux::Ack || status == Aux::ShortTransfer)
      std::copy_n(reply.begin(), status == Aux::Ack ? N : N - 1, bytes);
    return status;
  }
};

void testEdidRetries()
{
  const auto edid = fourBlockEdid(true);
  const size_t chunks = edid.size() / 16;
  unsigned cases = 0;
  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    for (Aux terminal : {Aux::Ack, Aux::Nack, Aux::ShortTransfer, Aux::Defer, Aux::Timeout}) {
      for (unsigned prefix : {0u, 1u, 3u}) {
        if ((terminal == Aux::Defer || terminal == Aux::Timeout) && prefix != 3) continue;
        std::array<Aux, 4> script{Aux::Defer, Aux::Timeout, Aux::Defer, Aux::Ack};
        if (chunk % 2) script = {Aux::Timeout, Aux::Defer, Aux::Timeout, Aux::Ack};
        script[prefix] = terminal;
        StressSink sink;
        sink.edid = edid;
        sink.burstOffset = chunk * 16;
        sink.burst = script;
        DpHotplug port;
        const bool success = terminal == Aux::Ack;
        const auto expected = success ? Hotplug::Active : Hotplug::EdidAuxError;
        CHECK(port.hpd(sink, true, false, 4) == expected);
        CHECK(sink.burstReplies == prefix + 1 && sink.retryDelays == prefix);
        CHECK(sink.edidReads == (success ? chunks + prefix : chunk + prefix + 1));
        CHECK(sink.edidOffset == (success ? edid.size() : *sink.burstOffset));
        CHECK(sink.streaming == success && sink.starts == unsigned(success));
        CHECK(sink.attempts.size() == unsigned(success) && sink.teardowns == unsigned(!success));
        CHECK((port.modes().count > 0) == success);
        CHECK(port.hpd(sink, false, false, 0) == Hotplug::Disconnected);
        CHECK(!sink.streaming && sink.teardowns == 1 && !port.link().lanes && !port.modes().count);
        ++cases;
      }
    }
  }
  CHECK(cases == 352);
}

void testNativeAuxErrors()
{
  for (Aux reply : {Aux::Defer, Aux::Timeout, Aux::Nack, Aux::ShortTransfer, Aux::IoError}) {
    for (FaultAt stage : {FaultAt::Caps, FaultAt::PatternWrite, FaultAt::StatusRead, FaultAt::PatternStop}) {
      StressSink sink;
      sink.faultAt = stage;
      sink.fault = reply;
      const auto result = trainDpLink(sink, 4);
      CHECK(result.status == DpTrainStatus::AuxError && result.auxStatus == reply);
      CHECK(sink.pattern == Pattern::Disabled && sink.probeReads == 1 && !sink.retryDelays);
      const bool probed = stage != FaultAt::Caps;
      CHECK(sink.attempts.size() == unsigned(probed) && result.attempts == unsigned(probed));
      if (probed) {
        const auto &attempt = sink.attempts.front();
        CHECK(attempt.stops == 1 && attempt.link.rateKhz == kDpHbr2Khz && attempt.link.lanes == 4);
        CHECK(attempt.crPolls == unsigned(stage != FaultAt::PatternWrite));
        CHECK(attempt.eqPolls == unsigned(stage == FaultAt::PatternStop));
      }
    }
  }
}

void checkFallback(const StressSink &sink)
{
  CHECK(sink.attempts.size() == kFallback.size());
  CHECK(sink.probeReads == 1 && sink.pattern == Pattern::Disabled);
  for (size_t i = 0; i < kFallback.size(); ++i) {
    const auto &attempt = sink.attempts[i];
    CHECK(attempt.link.rateKhz == kFallback[i].rateKhz && attempt.link.lanes == kFallback[i].lanes);
    CHECK(attempt.stops == 1);
  }
}

void testFallbackAndQmp()
{
  // Every EDID chunk reaches the last permitted attempt before the EQ fallback matrix runs.
  const auto edid = fourBlockEdid(true, kVga60);
  constexpr std::array burst{Aux::Defer, Aux::Timeout, Aux::Defer, Aux::Ack};
  for (unsigned swing = 0; swing <= 3; ++swing) {
    for (unsigned preemphasis = 0; preemphasis <= 3 - swing; ++preemphasis) {
      StressSink sink(Training::Fallback);
      sink.request = {swing, preemphasis};
      sink.edid = edid;
      sink.burst = burst;
      sink.expectedPixelHz = 25170000;
      DpHotplug port;
      CHECK(port.hpd(sink, true, false, 4) == Hotplug::Active);
      CHECK(port.link().rateKhz == kDpRbrKhz && port.link().lanes == 1 && port.modes().count == 1);
      CHECK(sink.edidReads == 128 && sink.burstReplies == 128 && sink.retryDelays == 96 && sink.edidOffset == edid.size());
      checkFallback(sink);
      for (const auto &attempt : sink.attempts) {
        CHECK(attempt.crPolls == 1 && attempt.eqPolls == 6 && attempt.drive == sink.request);
        CHECK(attempt.driveChanges == unsigned(swing || preemphasis));
      }
      CHECK(sink.statusDelays == 63);
      CHECK(port.hpd(sink, false, false, 0) == Hotplug::Disconnected && sink.teardowns == 1 && !sink.streaming);
    }
  }

  StressSink oscillating(Training::Oscillate);
  const auto result = trainDpLink(oscillating, 4);
  CHECK(result.status == DpTrainStatus::ClockRecoveryFailed && result.auxStatus == Aux::Ack);
  CHECK(result.attempts == kFallback.size() && result.link.rateKhz == kDpRbrKhz && result.link.lanes == 1);
  checkFallback(oscillating);
  for (const auto &attempt : oscillating.attempts)
    CHECK(attempt.crPolls == 16 && !attempt.eqPolls && attempt.driveChanges == 15);
  CHECK(oscillating.statusDelays == 144);
}

void testHpdStorm()
{
  constexpr unsigned cycles = 256;
  Handoff h;
  CHECK(h.panel.submit(100) && h.panel.present(h.panel.begin()));
  CHECK(h.panel.submit(101) && h.panel.submit(102) && h.panel.submit(103));
  const auto panel = h.panel.framebuffer;
  const Framebuffer framebuffer = *panel;
  const Flip panelFlip = h.panel.begin();
  const auto panelQueue = h.panel.queued;
  const auto panelCompletions = h.panel.completed;
  const auto checkPanel = [&] {
    CHECK(h.panel.framebuffer == panel && panel->generation == framebuffer.generation);
    CHECK(panel->width == framebuffer.width && panel->height == framebuffer.height && panel->lastSwap == framebuffer.lastSwap);
    CHECK(h.panel.pending == panelFlip && h.panel.queued == panelQueue && h.panel.completed == panelCompletions);
    CHECK((h.claims.ctl & kPanelPath.ctl) == kPanelPath.ctl && (h.claims.intf & kPanelPath.intf) == kPanelPath.intf);
    CHECK(h.claims.dsc == kPanelPath.dsc);
  };
  std::optional<Flip> stale;
  for (unsigned i = 0; i < cycles; ++i) {
    const uint8_t pin = i % 2 ? 4 : 3;
    h.sinkEdid = i % 2 ? fourBlockEdid(i % 4 == 1) : makeEdid(kHd60);
    const unsigned probes = h.probes, attempts = h.phyAttempts, starts = h.streamStarts, irqs = h.irqReads;
    const unsigned trainingReads = h.trainingReads;

    for (unsigned pulse = 0; pulse < 3; ++pulse) {
      h.signal(true, true, pin);
      h.advance(Handoff::kDebounceMs - 1);
      h.signal(false, true, pin);
      h.run();
      parked(h);
    }
    CHECK(h.probes == probes && h.teardowns == 2 * i);
    checkPanel();

    h.maxTrainedLanes = 4;
    h.maxTrainedRate = kDpHbr2Khz;
    h.unplugDuring = i % 2 ? Pattern::Tps2 : Pattern::Tps1;
    plug(h, pin);
    parked(h);
    CHECK(h.probes == probes + 1 && h.phyAttempts == attempts + 1 && h.streamStarts == starts);
    CHECK(h.trainingReads == trainingReads + i % 2 && h.blockedAux == 2 * (i + 1));
    CHECK(h.teardowns == 2 * i + 1 && h.notices.size() == 2 * i);
    checkPanel();

    // The following high level trains afresh, falling back to two HBR lanes.
    h.maxTrainedLanes = 2;
    h.maxTrainedRate = kDpHbrKhz;
    plug(h, pin);
    active(h, kDpHbrKhz, 2, 1920, 1080, 148500000);
    CHECK(h.probes == probes + 2 && h.phyAttempts == attempts + (pin == 3 ? 6u : 3u));
    CHECK(h.streamStarts == starts + 1 && h.notices.size() == 2 * i + 1);
    CHECK(h.external.submit(1) && h.vblank(h.external.begin()));
    CHECK(h.external.submit(2) && h.external.submit(3));
    const Flip pending = h.external.begin();
    CHECK(!stale || (pending.generation != stale->generation && !h.vblank(*stale)));
    CHECK(h.external.framebuffer->lastSwap == 1 && h.external.pending == pending);
    checkPanel();

    for (unsigned irq = 0; irq < 16; ++irq) h.signal(true, true, pin);
    h.run();
    CHECK(h.irqReads == irqs + 1 && h.external.pending == pending);
    for (unsigned irq = 0; irq < 16; ++irq) h.signal(true, true, pin);
    const std::weak_ptr<Framebuffer> retired = h.external.framebuffer;
    h.signal(false, true, pin);
    CHECK(!h.vblank(pending)); // low HPD gates a late completion before the work loop runs
    h.run();
    parked(h);
    CHECK(h.irqReads == irqs + 1 && h.teardowns == 2 * (i + 1));
    CHECK(retired.expired() && !h.vblank(pending) && !h.external.submit(4));
    CHECK(h.notices.size() == 2 * (i + 1) && h.registrations == i + 1);
    CHECK((h.notices.back() == Notice{pending.generation, false, kPanelPath, false, false, false, 0}));
    const auto &completed = h.external.completed;
    CHECK(completed.size() == 3 * (i + 1));
    CHECK((completed[3 * i] == Completion{{pending.generation, 1}, true}));
    CHECK((completed[3 * i + 1] == Completion{pending, false}));
    CHECK((completed.back() == Completion{{pending.generation, 3}, false}));
    checkPanel();
    stale = pending;
  }
}

} // namespace

int main()
{
  testEdidFraming();
  testCtaFraming();
  testAdmission();
  testEdidRetries();
  testNativeAuxErrors();
  testFallbackAndQmp();
  testHpdStorm();
  std::puts("PianoDp EDID/AUX stress: passed (352 EDID retry scripts, 20 native AUX faults, 30 QMP settings, 256 HPD cycles)");
}
