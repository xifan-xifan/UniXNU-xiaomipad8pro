// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Scripted Iris encoder firmware shared by the E1 and resident handoff tests.
// The including test supplies CHECK so peer failures use its test harness.
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../VideoEncodeSession.h"

namespace encfixture {
using namespace pianovideo;
using namespace pianovideo::enc;

struct TestHw {
  uint64_t clock = 0;
  int phase = 0;
  std::string log;
  void barrier() {}
  void stage(int s, const char *) { phase = s; }
  void sleepMs(unsigned n) { clock += uint64_t(n) * 1000; }
  uint64_t nowUs() { return clock += 10; }
  void note(const char *fmt, ...)
  {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log += "PianoVideo: ";
    log += b;
    log += '\n';
    if (std::getenv("VIDEO_ENC_ECHO")) std::printf("PianoVideo: %s\n", b);
  }
};
struct TestOps {
  bool fault = false, watchdog = false;
  uint32_t rd(uint64_t pa) { return pa == kIris + kWrapperIntrStatus ? (watchdog ? 8 : 0) : (fault && (pa & 4095) == vsmmu::kFsr ? 2 : 0); }
};
struct Sent {
  uint32_t type, flags, port, payload, words, v0, v1;
  int stage;
};
enum class OutputReply { Normal, AtCapacity, Empty, WrongIova, BadCapacity, BadOffset, BadLength, Wraparound, BadTimestamp };
// The encoder firmware as iris_hfi_gen2_response.c expects it to answer: RAW back after a frame, BITSTREAM
// with the picture type before it, DRAIN ack + an empty LAST bitstream, STOP and CLOSE acks.
struct Peer {
  TestHw hw;
  TestOps ops;
  vsmmu::Plan plan;
  hfi::Mem mem;
  hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords] = {}, rx[hfi::kBufferWords] = {};
  struct { volatile uint8_t *arena = nullptr; } dma;
  struct { uint32_t fsr = 0; } rep;
  unsigned watchdogSamples = 0, watchdogSeen = 0, rxEvents = 0;
  uint32_t activeSid = 0;
  std::vector<uint32_t> uc;
  std::vector<uint8_t> arena;
  std::vector<Sent> sent;
  std::vector<uint32_t> openedSessions, closedSessions;
  std::vector<uint32_t> arp;
  std::vector<std::vector<uint32_t>> arpPackets;
  std::vector<std::vector<uint32_t>> internals;
  std::vector<std::vector<uint32_t>> bitstreamQueue;   // queued BITSTREAM buffer packets (v[0..15])
  std::vector<std::vector<uint32_t>> rawPending;       // raw frames waiting for an output buffer
  Geometry g;
  session::Layout *layout = nullptr;
  uint32_t fps = 30;
  unsigned frame = 0, patternOk = 0, patternBad = 0, rawBatch = 1, peakRaw = 0, peakBitstream = 0;
  unsigned rawSlots = 0, bitstreamSlots = 0;
  bool sessionErrorOnProp = false, dropLast = false, dropCloseAck = false, systemError = false, badBitstream = false;
  bool faultOnRaw = false, watchdogOnStop = false;
  bool rejectArp = false, reportArpOnStop = false, errorCloseAck = false, errorBitstreamStart = false;
  bool reorderFirstBatch = false, zeroRawTimestamps = false, wrongRawIndex = false;
  OutputReply firstOutput = OutputReply::Normal;
  uint32_t sessionErrorProp = 0;
  Peer() : uc(kUcBytes / 4)
  {
    mem.uc = uc.data();
    hfi::initQueues(mem);
    plan.bank = 3;
    plan.pages = 128;
  }
  void bind(const Geometry &geometry, session::Layout &l)
  {
    arena.resize(l.bytes);
    bind(geometry, l, arena.data(), mem);
  }
  void bind(const Geometry &geometry, session::Layout &l, volatile uint8_t *backing, hfi::Mem queues)
  {
    g = geometry;
    layout = &l;
    dma.arena = backing;
    mem = queues;
  }
  static std::vector<uint32_t> packet(uint32_t type, uint32_t flags, uint32_t port, uint32_t payload, const std::vector<uint32_t> &v = {})
  {
    std::vector<uint32_t> p = {uint32_t(32 + 4 * v.size()), type, flags, payload, port, 0xeeee, 0, 0};
    p.insert(p.end(), v.begin(), v.end());
    return p;
  }
  void reply(std::vector<std::vector<uint32_t>> packets, uint32_t sid = 0)
  {
    std::vector<uint32_t> b = {32, sid ? sid : activeSid, 0xdddd, 0, 0, 0, 0, uint32_t(packets.size())};
    for (const auto &p : packets) b.insert(b.end(), p.begin(), p.end());
    b[0] = uint32_t(4 * b.size());
    CHECK(hfi::write(mem, hfi::kMsgq, b.data(), b[0], [] {}) == 0);
  }
  void packetTrace(const char *, unsigned, const uint32_t *, unsigned) {}
  void drainDebug() {}
  // One coded frame into the first queued bitstream buffer: SPS+PPS+IDR for frame 0 (header joined), a P slice after.
  bool emit(const std::vector<uint32_t> &raw)
  {
    if (bitstreamQueue.empty()) return false;
    std::vector<uint32_t> out = bitstreamQueue.front();
    bitstreamQueue.erase(bitstreamQueue.begin());
    bitstreamSlots |= 1u << out[1];
    std::vector<uint8_t> nal;
    auto put = [&](uint8_t type, unsigned n) {
      const uint8_t sc[4] = {0, 0, 0, 1};
      nal.insert(nal.end(), sc, sc + 4);
      nal.push_back(uint8_t(0x60 | type));
      for (unsigned i = 0; i < n; ++i) nal.push_back(uint8_t(0x80 | (i & 0x7f)));
    };
    const bool idr = raw[8] == 0;
    const auto mode = idr ? firstOutput : OutputReply::Normal;
    const uint32_t dataOffset = mode == OutputReply::AtCapacity ? 4 : 0;
    if (badBitstream) nal = {0x12, 0x34, 0x56};
    else if (idr) {
      put(7, 12); put(8, 4); put(5, 3000);
      if (mode == OutputReply::AtCapacity) nal.resize(out[5] - dataOffset, 0x80);
    } else put(1, 800);
    CHECK(nal.size() <= out[5] - dataOffset);
    const uint32_t off = out[2] - session::kArenaBase;
    std::copy(nal.begin(), nal.end(), dma.arena + off + dataOffset);
    out[6] = dataOffset;
    out[7] = uint32_t(nal.size());
    switch (mode) {
    case OutputReply::Empty: out[7] = 0; break;
    case OutputReply::WrongIova: out[2] += 4096; break;
    case OutputReply::BadCapacity: out[5] = out[7] - 1; break;
    case OutputReply::BadOffset: out[6] = out[5] + 1; out[7] = 0; break;
    case OutputReply::BadLength: out[6] = 4; out[7] = out[5] - 3; break;
    case OutputReply::Wraparound: out[6] = out[5]; out[7] = UINT32_MAX; break;
    case OutputReply::BadTimestamp:
    case OutputReply::Normal:
    case OutputReply::AtCapacity: break;
    }
    out[8] = mode == OutputReply::BadTimestamp ? UINT32_MAX : raw[8];
    out[9] = mode == OutputReply::BadTimestamp ? UINT32_MAX : raw[9];
    auto returnedRaw = raw;
    if (zeroRawTimestamps) returnedRaw[8] = returnedRaw[9] = 0;
    if (wrongRawIndex && idr) returnedRaw[1] = (raw[1] + 1) % kRaws;
    reply({packet(PropPictureType, 2, PortBitstream, 1, {idr ? PictureIdr : PictureP}),
           packet(Buffers, 1, PortBitstream, 5, out), packet(Buffers, 1, PortRaw, 5, returnedRaw)});
    return true;
  }
  void flushRaw()
  {
    while (!rawPending.empty() && emit(rawPending.front())) rawPending.erase(rawPending.begin());
  }
  int send(unsigned bytes, const char *)
  {
    if (rejectArp && tx[9] == 0x01000009 && tx[16] == 9) return kErrHfi;
    CHECK(hfi::write(mem, hfi::kCmdq, tx, bytes, [] {}) == 0);
    uint32_t data[hfi::kBufferWords], size = 0;
    CHECK(hfi::read(mem, hfi::kCmdq, data, sizeof data, &size, [] {}) == 0);
    CHECK(size == bytes && data[7] == 1 && data[0] == bytes);
    const uint32_t *p = data + 8;
    const uint32_t type = p[1], flags = p[2], port = p[4], payload = p[3], words = (p[0] - 32) / 4;
    sent.push_back({type, flags, port, payload, words, words ? p[8] : 0, words > 1 ? p[9] : 0, hw.phase - 80});
    if (systemError && sent.size() == 6) { reply({packet(0x05000001, 8, 0, 0)}, 0); return 0; }
    if (type == Open) {
      CHECK(!data[1] && port == 0 && flags == 3 && payload == PayloadU32 && p[8]);
      CHECK(!activeSid && arp.empty() && internals.empty() && bitstreamQueue.empty() && rawPending.empty());
      activeSid = p[8];
      openedSessions.push_back(activeSid);
      frame = 0;
      return 0;
    }
    CHECK(activeSid && data[1] == activeSid);
    if (type > 0x03000000 && type < 0x03ffffff) {
      if (sessionErrorOnProp && type == sessionErrorProp) reply({packet(type, 4 | 1, port, 0)});
      return 0;
    }
    std::vector<uint32_t> v(p + 8, p + 8 + words);
    if (type == Buffers) {
      CHECK(words == 16 && flags == 1 && payload == 5 && v[10] == kHostNonSecure);
      const auto *b = layout->find(v[0], v[1]);
      CHECK(b && v[2] == b->iova() && v[5] == b->capacity && port == b->port);
      CHECK(!v[3] && !v[4] && !v[6]);
      if (v[0] == BufBitstream) {
        CHECK(port == PortBitstream && v[1] < 4 && !v[7] && !v[8] && !v[9]);
        CHECK(std::none_of(bitstreamQueue.begin(), bitstreamQueue.end(), [&](const auto &out) { return out[1] == v[1]; }));
        bitstreamQueue.push_back(v);
        peakBitstream = std::max(peakBitstream, unsigned(bitstreamQueue.size()));
      } else if (v[0] == BufRaw) {
        CHECK(port == PortRaw && v[1] < 4 && v[7] == g.rawBytes);
        // The frame really is the pattern of its timestamp's frame number (active pixels and the padding).
        const uint64_t timestamp = uint64_t(v[9]) << 32 | v[8];
        CHECK(timestamp == uint64_t(frame) * 1000000000ULL / fps);
        const volatile uint8_t *base = dma.arena + (v[2] - session::kArenaBase);
        bool same = true;
        pattern(g, frame, [&](uint32_t at, uint8_t want) { same &= base[at] == want; });
        (same ? patternOk : patternBad)++;
        ++frame;
        CHECK(std::none_of(rawPending.begin(), rawPending.end(), [&](const auto &in) { return in[1] == v[1]; }));
        rawSlots |= 1u << v[1];
        rawPending.push_back(v);
        peakRaw = std::max(peakRaw, unsigned(rawPending.size()));
        if (rawPending.size() == 4 && frame == 4 && reorderFirstBatch) std::swap(rawPending[1], rawPending[2]);
        if (rawPending.size() >= rawBatch) flushRaw();
        if (faultOnRaw) ops.fault = true;
      } else if (v[0] == 9) {
        CHECK(arp.empty() && p[4] == 0 && b->queued);
        arp.assign(v.begin(), v.end());
        arpPackets.emplace_back(data, data + size / 4);
      } else {
        CHECK(!v[7] && !v[8] && !v[9]);
        internals.push_back(v);
      }
      return 0;
    }
    if (type == Drain) {
      CHECK(port == PortRaw && flags == 7);
      flushRaw();
      std::vector<std::vector<uint32_t>> r = {packet(Drain, 1, PortRaw, 0)};
      if (!dropLast && !bitstreamQueue.empty()) {
        std::vector<uint32_t> out = bitstreamQueue.front();
        bitstreamQueue.erase(bitstreamQueue.begin());
        out[7] = 0;
        out[10] = kFlagLast;
        r.push_back(packet(Buffers, 1, PortBitstream, 5, out));
      }
      reply(r);
      return 0;
    }
    if (type == Stop) {
      if (watchdogOnStop) ops.watchdog = true;
      // STOP hands back what the port still holds (the output buffers), then acknowledges.
      std::vector<std::vector<uint32_t>> r;
      if (port == PortBitstream) {
        for (auto &out : bitstreamQueue) {
          out[7] = 0;
          r.push_back(packet(Buffers, 1, PortBitstream, 5, out));
        }
        bitstreamQueue.clear();
      } else {
        for (const auto &in : rawPending) r.push_back(packet(Buffers, 1, PortRaw, 5, in));
        rawPending.clear();
      }
      for (auto it = internals.begin(); it != internals.end();) {
        const auto *b = layout->find((*it)[0], (*it)[1]);
        if (b->port == port) {
          r.push_back(packet(Buffers, 1, port, 5, *it));
          it = internals.erase(it);
        } else {
          ++it;
        }
      }
      if (!arp.empty()) {
        CHECK(layout->find(9, 0)->queued);
        if (reportArpOnStop) r.push_back(packet(Buffers, 1, 0, 5, arp));
      }
      r.push_back(packet(Stop, 1, port, 0));
      reply(r);
      return 0;
    }
    if (type == Close) {
      CHECK(bitstreamQueue.empty() && rawPending.empty());
      if (!arp.empty()) CHECK(layout->find(9, 0)->queued);
      if (!dropCloseAck) {
        reply({packet(Close, errorCloseAck ? 5 : 1, 0, 0)});
        if (!errorCloseAck) {
          arp.clear();
          internals.clear();
          closedSessions.push_back(activeSid);
          activeSid = 0;
        }
      }
      return 0;
    }
    if (type == Start) {
      CHECK(!arp.empty() && layout->find(9, 0)->queued);
      reply({packet(Start, port == PortBitstream && errorBitstreamStart ? 4 : 1, port, 0)});
    }
    return 0;
  }
};
} // namespace encfixture
