// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// PianoVideo E1 (piano-video-enc=1): one bounded H.264 encode session over the verified V3 owner, on synthetic NV12 frames.
// The order is Linux iris_venc's (VideoEncode.h names the pin): OPEN, CODEC; streamon raw (input config, capabilities, ARP, VPSS,
// START raw); streamon bitstream (output config, capabilities, COMV count, BIN/COMV/NON_COMV/LINE/DPB, START bitstream); queue the
// bitstream buffers; ETB the frames across four RAW slots; DRAIN raw -> LAST; STOP bitstream, STOP raw, CLOSE.
// The Owner is the decoder's (session::SelfTest's duck type: hw, ops, plan, mem, ids, tx, rx, dma.arena, rep, watchdog words,
// send, packetTrace, drainDebug). Nothing here frees memory: the arena belongs to the V3 owner, as for the decoder.
#pragma once
#include "VideoEncode.h"
#include "VideoOps.h"
#include "VideoHfi.h"
#include "VideoSmmu.h"

namespace pianovideo
{
namespace enc
{
constexpr uint32_t Open = 0x01000003, Close = 0x01000004, Start = 0x01000005, Stop = 0x01000006, Drain = 0x01000007,
                   Buffers = 0x01000009;
struct FrameRecord {
  uint32_t bytes = 0, picture = 0, offset = 0;
  uint64_t etbUs = 0, ebdUs = 0, fbdUs = 0;
  FrameNals nals;
};
struct Result {
  int error = 0, close = 0;
  unsigned stage = 0, failedStage = 0;
  unsigned etb = 0, ebd = 0, ftb = 0, fbd = 0, empty = 0, idr = 0, sps = 0, pps = 0, slices = 0, badNal = 0;
  uint32_t streamBytes = 0, errorType = 0, errorFlags = 0, errorPort = 0;
  bool drainAck = false, last = false, stopBitstream = false, stopRaw = false, closed = false, truncated = false;
  bool pass() const { return !error && !close && closed && fbd >= etb && etb && idr >= 1 && sps >= 1 && pps >= 1 && !badNal && !truncated; }
};
enum Stage : unsigned { Prepared = 0, Opening, InputConfig, InputStart, OutputConfig, OutputStart, Frames, Draining, Stopping, Closing, Finished };

template <class Owner> class EncodeSelfTest
{
public:
  Owner &o;
  session::Layout &layout;
  Geometry g;
  Settings set;
  Result r;
  int deviceError = 0;
  FrameRecord frames[kMaxFrames] = {};
  unsigned rawFrame[kRaws] = {};
  uint32_t sessionId = 0x56340e01;
  uint8_t *stream = nullptr;   // the export buffer (owner's), the whole Annex B stream in output order
  uint32_t streamCap = 0;
  bool openSent = false, inputStarted = false, outputStarted = false, drainSent = false, closeSent = false, cleanup = false;
  uint32_t stopPending = 0, picture = 0, lastFirmwareProp = 0;
  unsigned unknownProps = 0;
  uint64_t deadline = 0;
  EncodeSelfTest(Owner &owner, session::Layout &l) : o(owner), layout(l) {}

  void stage(Stage s)
  {
    r.stage = s;
    o.hw.stage(80 + int(s), "E1 encode session");
    o.hw.note("E1 phase %u", unsigned(s));
  }
  int fail(int code, uint32_t type = 0, uint32_t flags = 0, uint32_t port = 0)
  {
    if (!r.error) {
      r.error = code; r.failedStage = r.stage; r.errorType = type; r.errorFlags = flags; r.errorPort = port;
      o.hw.note("E1 first-error %d stage %u session %08x type %08x flags %08x port %u", code, r.stage, sessionId, type, flags, port);
    }
    return code;
  }
  int tx(uint32_t type, uint32_t flags, uint32_t port, uint32_t payload, const uint32_t *data = nullptr, unsigned words = 0,
         bool systemOpen = false)
  {
    if (words > hfi::kBufferWords - 16) return fail(kErrHfi);
    hfi::begin(o.tx, systemOpen ? 0 : sessionId, o.ids);
    hfi::add(o.tx, type, flags, payload, data, words, o.ids, port);
    o.hw.note("E1 TX stage %u header %u packet %u type %08x port %u flags %08x payload %u words %u%s%08x", r.stage, o.tx[2], o.tx[13],
              type, port, flags, payload, words, words ? " v0 " : " - ", words ? data[0] : 0);
    const int rc = o.send(o.tx[0], "E1-session");
    return rc ? fail(rc, type, flags, port) : 0;
  }
  int props(const Script &s)
  {
    for (unsigned i = 0; i < s.n; ++i) {
      const Prop &p = s.p[i];
      if (tx(p.id, 0, p.port, p.payload, p.v, p.words)) return r.error;
    }
    return 0;
  }
  int buffer(session::Buffer &b, uint32_t dataSize = 0, uint64_t timestamp = 0)
  {
    if (!b.size || b.size > b.capacity || b.queued || b.pendingRelease || dataSize > b.size) return fail(kErrHfi, Buffers);
    const uint32_t v[16] = {b.type, b.index, b.iova(), 0, 0, b.size, 0, dataSize, uint32_t(timestamp), uint32_t(timestamp >> 32),
                            kHostNonSecure, 0, 0, 0, 0, 0};
    b.queued = true;
    b.released = false;
    const int rc = tx(Buffers, kHfiHostIntr, b.port, PayloadStructure, v, 16);
    if (rc) b.queued = false;
    return rc;
  }
  bool validate(const uint32_t *b, uint32_t bytes) const
  {
    if (bytes < 64 || bytes > kHfiCorePacketBytes || (bytes & 3) || b[0] != bytes || !b[7] || b[7] > (bytes - 32) / 32) return false;
    uint32_t at = 32;
    for (unsigned i = 0; i < b[7]; ++i) {
      if (at > bytes || bytes - at < 32) return false;
      const uint32_t *p = b + at / 4;
      if (p[0] < 32 || (p[0] & 3) || p[0] > bytes - at || p[4] > 2) return false;
      at += p[0];
    }
    return at == bytes;
  }
  void completion(const uint32_t *p)
  {
    const uint32_t *v = p + 8;
    if (p[0] < 32 + 4 * 11) { fail(kErrHfi, Buffers, p[2], p[4]); return; }
    session::Buffer *b = layout.find(v[0], v[1]);
    o.hw.note("E1 completion type %u index %u port %u iova %08x size %u data-offset %u data-size %u timestamp %08x%08x flags %08x "
              "picture %08x", v[0], v[1], p[4], v[2], v[5], v[6], v[7], v[9], v[8], v[10], picture);
    if (!b || b->port != p[4] || v[2] != b->iova() || v[3] || v[4] || v[5] > b->size || v[6] > v[5] || v[7] > v[5] - v[6]) {
      fail(kErrHfi, Buffers, p[2], p[4]);
      return;
    }
    if (!b->queued) {
      // An internal buffer handed back at CLOSE is not ours to reuse; anything else unexpected is a protocol error.
      if (!cleanup) fail(kErrHfi, Buffers, p[2], p[4]);
      return;
    }
    // ARP lives outside both port buffer tables. A buffer reply does not
    // release its session backing. Keep ownership until a successful CLOSE.
    if (b->type == BufArp) return;

    const uint64_t now = o.hw.nowUs();
    if (b->type == BufRaw) {
      // Iris matches RAW returns by buffer index and uses the timestamp on BITSTREAM returns.
      if (b->index >= kRaws || rawFrame[b->index] >= r.etb) {
        fail(kErrHfi, Buffers, p[2], p[4]);
        return;
      }
      b->queued = false;
      frames[rawFrame[b->index]].ebdUs = now;
      ++r.ebd;
      return;
    }
    if (b->type != BufBitstream) {
      b->queued = false;
      if (!cleanup) fail(kErrHfi, Buffers, p[2], p[4]);
      return;
    }
    if (v[10] & kFlagLast) {
      if (!drainSent) { fail(kErrHfi, Buffers, v[10], p[4]); return; }
      r.last = true;
    }
    if (!v[7]) {
      b->queued = false;
      ++r.empty;
      if (!(v[10] & kFlagLast) && !cleanup) fail(kErrHfi, Buffers, v[10], p[4]);
      return;
    }
    const unsigned f = frameIndex(uint64_t(v[8]) | uint64_t(v[9]) << 32, set.fps);
    if (f >= r.etb || frames[f].bytes) {
      fail(kErrHfi, Buffers, p[2], p[4]);
      return;
    }
    b->queued = false;
    ++r.fbd;
    FrameRecord &rec = frames[f];
    o.hw.barrier();
    const volatile uint8_t *data = o.dma.arena + b->off + v[6];
    // Copy out first (the buffer goes back to the firmware below), then classify the copy.
    const uint32_t at = r.streamBytes;
    uint32_t kept = 0;
    if (stream && at < streamCap) {
      kept = v[7] <= streamCap - at ? v[7] : streamCap - at;
      for (uint32_t i = 0; i < kept; ++i) stream[at + i] = data[i];
      r.streamBytes += kept;
    }
    if (kept != v[7]) r.truncated = true;
    rec.bytes = v[7];
    rec.picture = picture;
    rec.offset = at;
    rec.fbdUs = now;
    if (kept == v[7] && stream) rec.nals = classify(stream + at, kept);
    r.sps += rec.nals.sps;
    r.pps += rec.nals.pps;
    r.idr += rec.nals.idr;
    r.slices += rec.nals.slice;
    r.badNal += rec.nals.bad;
    o.hw.note("E1 FBD frame %u bytes %u picture %08x NAL sps %u pps %u idr %u slice %u other %u bad %u etb-to-fbd-us %llu", f,
              v[7], picture, rec.nals.sps, rec.nals.pps, rec.nals.idr, rec.nals.slice, rec.nals.other, unsigned(rec.nals.bad),
              (unsigned long long)(now - rec.etbUs));
    picture = 0;
    if (!r.last && !cleanup && !r.error && !buffer(*b)) ++r.ftb;
  }
  int receive(const uint32_t *b, uint32_t bytes)
  {
    if (!validate(b, bytes)) return fail(kErrHfi);
    for (unsigned at = 32; at < bytes; at += b[at / 4]) {
      const uint32_t *p = b + at / 4;
      if ((p[2] & 8) || (p[1] > 0x05000000 && p[1] < 0x05ffffff)) {
        deviceError = kErrSysError;
        return fail(deviceError, p[1], p[2], p[4]);
      }
    }
    if (!b[1]) return 0;   // a system message with no error: nothing outstanding
    if (b[1] != sessionId || r.closed) return fail(kErrHfi, 0, b[1]);
    // Errors, then info, then properties (the picture type precedes its buffer), then commands: the decoder's four passes.
    for (unsigned pass = 0; pass < 4; ++pass)
      for (unsigned at = 32; at < bytes; at += b[at / 4]) {
        const uint32_t *p = b + at / 4;
        const uint32_t type = p[1], flags = p[2], port = p[4];
        if (pass == 0) {
          if ((flags & 4) || (type > 0x04000000 && type < 0x04ffffff)) fail(kErrHfi, type, flags, port);
          continue;
        }
        const bool kind = pass == 1 ? (type > 0x06000000 && type < 0x06ffffff)
                          : pass == 2 ? (type > 0x03000000 && type < 0x03ffffff)
                                      : (type > 0x01000000 && type < 0x01ffffff);
        if (!kind) continue;
        o.hw.note("E1 RX pass %u stage %u packet %u type %08x port %u flags %08x bytes %u%s%08x", pass, r.stage, p[5], type, port, flags,
                  p[0], p[0] >= 36 ? " v0 " : " - ", p[0] >= 36 ? p[8] : 0);
        if (pass == 1) {
          if (type == 0x06000001 || type == 0x06000002 || type == 0x06000004) fail(kErrHfi, type, flags, port);
          else if (type == 0x06000006) {
            if (!drainSent) fail(kErrHfi, type);
            else r.last = true;
          }
        } else if (pass == 2) {
          if (p[0] < 36) continue;
          if (type == PropPictureType) picture = p[8];
          else { ++unknownProps; lastFirmwareProp = type; }
        } else if (type == Buffers) {
          completion(p);
        } else if (type == Drain) {
          if (!drainSent || port != PortRaw || !(flags & 1)) fail(kErrHfi, type, flags, port);
          else r.drainAck = true;
        } else if (type == Stop) {
          if (stopPending != port || !(flags & 1)) fail(kErrHfi, type, flags, port);
          else {
            (port == PortBitstream ? r.stopBitstream : r.stopRaw) = true;
            stopPending = 0;
          }
        } else if (type == Close) {
          if (!closeSent || port || !(flags & kHfiFwSuccess) || (flags & kHfiFwSessionError)) fail(kErrHfi, type, flags, port);
          else r.closed = true;
        }
      }
    return r.error;
  }
  int pump()
  {
    if (deviceError) return deviceError;
    const uint32_t irq = o.ops.rd(kIris + kWrapperIntrStatus);
    ++o.watchdogSamples;
    o.watchdogSeen |= irq & (1u << 3);
    if (o.watchdogSeen) return deviceError = fail(kErrWatchdog);
    const vsmmu::Fault fault = vsmmu::fault(o.plan, o.ops);
    o.rep.fsr |= fault.fsr & vsmmu::kFsrFaults;
    if (o.rep.fsr) return deviceError = fail(kErrFault);
    int rc = 0;
    for (unsigned n = 0; n < 64; ++n) {
      uint32_t bytes = 0;
      const int read = hfi::read(o.mem, hfi::kMsgq, o.rx, sizeof o.rx, &bytes, [this] { o.hw.barrier(); });
      if (read == 1) break;
      if (read) { rc = fail(kErrHfi); break; }
      o.packetTrace("RX", ++o.rxEvents, o.rx, bytes);
      if (const int got = receive(o.rx, bytes)) rc = got;
      if (deviceError) return deviceError;
    }
    o.drainDebug();
    if (o.hw.nowUs() >= deadline && !cleanup) rc = fail(kErrHfiTimeout);
    return rc;
  }
  template <class Done> int wait(Done done, unsigned ms = 1000)
  {
    const uint64_t until = o.hw.nowUs() + uint64_t(ms) * 1000;
    for (;;) {
      const int rc = pump();
      if (deviceError || (rc && !cleanup)) return rc;
      if (done()) return 0;
      if (o.hw.nowUs() >= until) return fail(kErrHfiTimeout);
      o.hw.sleepMs(1);
    }
  }
  int consumed()
  {
    const uint32_t target = o.mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQWrite);
    return wait([this, target] { return o.mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQRead) == target; });
  }
  session::Buffer *freeRaw(unsigned first)
  {
    for (unsigned i = 0; i < kRaws; ++i) {
      session::Buffer *b = layout.find(BufRaw, (first + i) % kRaws);
      if (b && !b->queued) return b;
    }
    return nullptr;
  }
  int internals(bool input)
  {
    for (uint32_t type : kInternalOrder) {
      if ((type == BufVpss) != input) continue;
      session::Buffer *b = layout.find(type, 0);
      if (!b) {
        if (type == BufVpss) continue;   // no scaling: VPSS is 0 bytes and not queued (Linux skips a 0-size buffer)
        return fail(kErrNoMemory);
      }
      if (type == BufComv && tx(PropComvCount, 0, PortBitstream, PayloadU32, &ComvCountValue, 1)) return r.error;
      if (buffer(*b)) return r.error;
    }
    return 0;
  }
  static constexpr uint32_t ComvCountValue = ComvCount;
  int transaction()
  {
    if (!g.width || !admitted(set)) return fail(kErrArgument);
    o.hw.note("E1 encode %ux%u stride %u scanlines %u uv %u raw %u bitstream %u coded %ux%u fps %u bitrate %u gop %u frames %u profile %u "
              "level %u arena %u entries %u", g.width, g.height, g.stride, g.scanlines, g.uvOffset, g.rawBytes, g.bitstreamBytes,
              g.codedWidth, g.codedHeight, set.fps, set.bitrate, set.gop, set.frames, set.profile, set.level, layout.bytes, layout.used);
    o.hw.note("E1 internal BIN %u COMV %u NON_COMV %u LINE %u DPB %u VPSS %u (iris_vpu33_buf_size at the pin)", g.internal.bin,
              g.internal.comv, g.internal.nonComv, g.internal.line, g.internal.dpb, g.internal.vpss);
    deadline = o.hw.nowUs() + 10000000;
    stage(Opening);
    if (tx(Open, 3, PortNone, PayloadU32, &sessionId, 1, true)) return r.error;
    openSent = true;
    const uint32_t codec = CodecEncodeAvc;
    if (consumed() || tx(PropCodec, 0, PortNone, PayloadEnum, &codec, 1)) return r.error;
    stage(InputConfig);
    if (props(inputConfig(g)) || props(capabilities(set))) return r.error;
    session::Buffer *arp = layout.find(BufArp, 0);
    if (!arp) return fail(kErrNoMemory);
    if (buffer(*arp) || internals(true)) return r.error;
    stage(InputStart);
    if (tx(Start, 3, PortRaw, 0)) return r.error;
    inputStarted = true;
    if (consumed()) return r.error;
    stage(OutputConfig);
    if (props(outputConfig(g, set)) || props(capabilities(set)) || internals(false)) return r.error;
    stage(OutputStart);
    if (tx(Start, 3, PortBitstream, 0)) return r.error;
    outputStarted = true;
    if (consumed()) return r.error;
    for (unsigned i = 0; i < kBitstreams; ++i) {
      session::Buffer *b = layout.find(BufBitstream, i);
      if (!b) return fail(kErrNoMemory);
      if (buffer(*b)) return r.error;
      ++r.ftb;
    }
    stage(Frames);
    for (unsigned f = 0; f < set.frames; ++f) {
      session::Buffer *raw = nullptr;
      if (wait([this, f, &raw] { return (raw = freeRaw(f % kRaws)) != nullptr; }, 2000)) return r.error;
      volatile uint8_t *base = o.dma.arena + raw->off;
      pattern(g, f, [base](uint32_t at, uint8_t v) { base[at] = v; });
      o.hw.barrier();
      frames[f].etbUs = o.hw.nowUs();
      deadline = o.hw.nowUs() + 5000000;
      if (buffer(*raw, g.rawBytes, frameNs(f, set.fps))) return r.error;
      rawFrame[raw->index] = f;
      ++r.etb;
      if (pump() && r.error) return r.error;
    }
    stage(Draining);
    drainSent = true;
    deadline = o.hw.nowUs() + 5000000;
    if (tx(Drain, 7, PortRaw, 0) || wait([this] { return r.fbd >= r.etb && r.ebd >= r.etb && r.drainAck && r.last; }, 5000))
      return r.error;
    o.hw.note("E1 frame gate PASS ETB %u EBD %u FTB %u FBD %u empty %u IDR %u SPS %u PPS %u slices %u bytes %u", r.etb, r.ebd, r.ftb,
              r.fbd, r.empty, r.idr, r.sps, r.pps, r.slices, r.streamBytes);
    return 0;
  }
  int shutdown()
  {
    // As with native decode, keep DMA and skip further HFI after a device-level failure.
    if (deviceError) return deviceError;
    cleanup = true;
    int rc = 0;
    auto step = [&](uint32_t port, bool &done, bool started) {
      if (!started || deviceError) return;
      stopPending = port;
      int got = tx(Stop, 7, port, 0);
      if (!got) got = wait([&done] { return done; });
      if (got && !rc) rc = got;
    };
    stage(Stopping);
    step(PortBitstream, r.stopBitstream, outputStarted);
    step(PortRaw, r.stopRaw, inputStarted);
    if (openSent && !deviceError) {
      stage(Closing);
      closeSent = true;
      int got = tx(Close, 7, PortNone, 0);
      if (!got) got = wait([this] { return r.closed; });
      if (got && !rc) rc = got;
    }
    if (const int late = pump(); late && !rc) rc = late;
    r.close = rc;
    // CLOSE acknowledges session-wide DMA quiescence, including ARP. The owner
    // keeps its arena mapped and quarantines it on any error.
    if (r.closed && !deviceError)
      for (unsigned i = 0; i < layout.used; ++i) layout.buf[i].queued = false;
    unsigned queued = 0;
    for (unsigned i = 0; i < layout.used; ++i) queued += layout.buf[i].queued;
    o.hw.note("E1 session end closed %u close-error %d stop %u/%u queued-at-close %u arena-retained 1 unknown-props %u last-prop %08x",
              unsigned(r.closed), r.close, unsigned(r.stopBitstream), unsigned(r.stopRaw), queued, unknownProps, lastFirmwareProp);
    stage(Finished);
    return rc;
  }
  int run()
  {
    const int rc = transaction();
    shutdown();
    for (unsigned f = 0; f < r.etb; ++f)
      if (frames[f].bytes)
        o.hw.note("E1 frame %u bytes %u picture %08x encode-us %llu", f, frames[f].bytes, frames[f].picture,
                  (unsigned long long)(frames[f].fbdUs - frames[f].etbUs));
    o.hw.note("E1 result %s error %d stage %u failed-stage %u close %d ETB %u EBD %u FBD %u IDR %u SPS %u PPS %u slices %u bad-nal %u "
              "bytes %u truncated %u", r.pass() ? "PASS" : "FAIL", r.error, r.stage, r.failedStage, r.close, r.etb, r.ebd, r.fbd, r.idr,
              r.sps, r.pps, r.slices, r.badNal, r.streamBytes, unsigned(r.truncated));
    return rc ? rc : r.error ? r.error : r.close;
  }
};
} // namespace enc
} // namespace pianovideo
