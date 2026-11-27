// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// V4/V5: one bounded AVC transaction over the verified V3 owner.
// V4 supplies the fixture; V5 supplies caller bytes/session/token and retains RAW until the consumer finishes.
// Protocol facts: Linux iris gen2/VPU35 at the pin in UPSTREAM; UniXNU-sheng used as a read-only sequence reference.
#pragma once
#include "VideoOps.h"
#include "VideoSmmu.h"
#include "VideoHfi.h"
#include "VideoBuffers.h"
#include "VideoFixture.h"
#include "VideoStreamState.h"
#include "VideoDiagnostics.h"
namespace pianovideo
{
namespace session
{
constexpr uint32_t Open = 0x01000003, Close = 0x01000004, Start = 0x01000005, Stop = 0x01000006, Drain = 0x01000007, Resume = 0x01000008,
                   Buffers = 0x01000009, Subscribe = 0x0100000b, Settings = 0x0100000c;
constexpr uint32_t kSessionId = 0x56340001, kTimestamp = 1000000;
constexpr uint32_t kPscIds[10] = {0x0300015b, 0x03000103, 0x03000105, 0x0300010f, 0x03000120,
                                  0x03000124, 0x03000128, 0x03000107, 0x03000108, 0x03000155};
constexpr uint32_t kHevcTier = 0x03000109;
struct Psc {
  uint32_t resolution = 0x014000f0, crop[2] = {0, 0}, depth = 0x00080008, coded = 1, n = 4, poc = 0, profile = 4, level = 18,
           color = 0x14000003, tier = 0;
  uint32_t returned = 0;
  uint32_t value(unsigned i) const
  {
    return i == 1   ? resolution
           : i == 2 ? crop[0]
           : i == 3 ? depth
           : i == 4 ? coded
           : i == 5 ? n
           : i == 6 ? poc
           : i == 7 ? profile
           : i == 8 ? level
           : i == 9 ? color
           : i == 10 ? tier
                    : 0;
  }
  bool supported() const
  {
    // Returned values must agree with the fixture. Seeded optional fields remain explicitly identified in the log.
    return resolution == 0x014000f0 && !crop[0] && !crop[1] && depth == 0x00080008 && coded == 1 && n && n <= kMaxN &&
           (!(returned & (1u << 6)) || poc == 2) && (!(returned & (1u << 7)) || profile <= 1) && (!(returned & (1u << 8)) || level == 4);
  }
};
enum Stage : unsigned {
  Prepared = 0,
  Opening = 1,
  Config = 2,
  Input = 3,
  FirstPsc = 4,
  Reconfigure = 5,
  Output = 6,
  Draining = 7,
  StopRaw = 8,
  StopInput = 9,
  Closing = 10,
  Finished = 11
};
struct Result {
  int error = 0, close = 0;
  unsigned stage = 0, failedStage = 0, psc = 0, etb = 0, ebd = 0, decoded = 0, releases = 0, rawReturns = 0;
  uint32_t rawIndex = ~0u, rawBytes = 0;
  uint32_t errorType = 0, errorFlags = 0, errorPort = 0, errorId = 0, errorHeader = 0;
  bool checksum = false, drainAck = false, last = false, stopRaw = false, stopInput = false, closed = false;
};
struct Observation {
  Result r;
  Psc psc;
  stream::Ledger ledger;
  uint32_t sid=0,flags=0,outputCount=0,queued=0,releasing=0,readonly=0;
  uint64_t token=0,deadline=0;
  int closeRc=0,teardownRc=0;
  bool dmaHeld=false;
  template<class Log> void print(Log &l,const char *which) const {
    l.note("V6f session %s sid %08x phase %u failed %u error %d flags %08x token %llu deadline %llu ETB %u EBD %u PSC %u decoded %u",
      which,sid,r.stage,r.failedStage,r.error,flags,(unsigned long long)token,(unsigned long long)deadline,r.etb,r.ebd,r.psc,r.decoded);
    l.note("V6f state %s close-rc %d teardown %d dma-held %u STOP %u/%u CLOSE %u ACK %u LAST %u queued %u releasing %u readonly %u outputs %u",
      which,closeRc,teardownRc,unsigned(dmaHeld),unsigned(r.stopRaw),unsigned(r.stopInput),unsigned(r.closed),unsigned(r.drainAck),unsigned(r.last),queued,releasing,readonly,outputCount);
    l.note("V6f PSC %s returned %08x res %08x crop %08x/%08x depth %08x coded %u N %u POC %u profile %u level %u color %08x",
      which,psc.returned,psc.resolution,psc.crop[0],psc.crop[1],psc.depth,psc.coded,psc.n,psc.poc,psc.profile,psc.level,psc.color);
    l.note("V6f ledger %s pending %u accepted %llu retired %u cancelled %u high-water %u draining %u paused %u stopped %u",
      which,ledger.pending,(unsigned long long)ledger.accepted,ledger.retired,ledger.cancelled,ledger.highWater,unsigned(ledger.draining),unsigned(ledger.paused),unsigned(ledger.stopped));
    for(unsigned i=0;i<stream::Inputs;++i)l.note("V6f input %s index %u owned %u token %llu uses %u",which,i,unsigned(ledger.inputOwned[i]),(unsigned long long)ledger.inputToken[i],ledger.inputUses[i]);
    for(unsigned i=0;i<stream::Credits;++i) {
      const auto &f=ledger.frames[i];
      l.note("V6f frame %s index %u phase %u token %llu serial %llu flags %08x",which,i,unsigned(f.phase),(unsigned long long)f.token,(unsigned long long)f.serial,f.flags);
    }
    unsigned held=0;for(unsigned i=0;i<stream::Raws;++i)if(ledger.rawHeld[i]) {
      ++held;l.note("V6f RAW %s index %u token %llu uses %u",which,i,(unsigned long long)ledger.rawToken[i],ledger.rawUses[i]);
    }
    l.note("V6f RAW %s held %u",which,held);
  }
};
template<class Log> void printTimeoutObservation(Log &log,const diagnostic::Hardware &hardware,
    const Observation &previous,const Observation &current,const diagnostic::History &history) {
  log.note("V6f snapshot begin version 1 error %d sid %08x before-cleanup 1",current.r.error,current.sid);
  hardware.print(log);previous.print(log,"previous");current.print(log,"current");
  history.commands.print(log,"TX");history.messages.print(log,"RX");
  log.note("V6f snapshot end sid %08x before-cleanup 1",current.sid);
}
// Session has no resource destructor: only the V3 PAS/detach/TLB owner can free the arena.
// Every RX is validated before a four-pass error/info/property/command dispatch. Single worker, no callback re-entry.
template <class Owner> class SelfTest
{
public:
  Owner &o;
  Layout &layout;
  Result r;
  Psc psc;
  bool openSent = false, inputStarted = false, rawStarted = false, drainSent = false, closeSent = false, cleanup = false;
  bool failClosedCleanup = false;
  uint32_t stopPending = 0, picture = 0, errorSerial = 0;
  bool frameError = false;
  uint64_t deadline = 0;
  uint32_t sessionId = kSessionId;
  uint64_t token = kTimestamp;
  const uint8_t *accessUnit = kAccessUnit;
  uint32_t accessUnitBytes = sizeof kAccessUnit;
  bool checkGolden = true;
  bool streaming = false;
  // The original single-picture ladder verifies a sentinel and dumps packets.
  // Production frames use validated FBD ownership and retained binary history.
  bool qualificationTrace() const { return !streaming || !layout.general; }
  uint32_t expectedPoc = 2, expectedProfile = 0, expectedLevel = 4;
  uint32_t expectedDepth = 8, expectedTier = 0;
  uint32_t activeWidth = 320, activeHeight = 240;
  Psc initialPsc;
  unsigned pscCount() const { return layout.hevc ? 11 : 10; }
  bool pscField(unsigned i) const { return !layout.hevc || (i != 4 && i != 6); }
  uint32_t pscId(unsigned i) const { return i == 10 ? kHevcTier : kPscIds[i]; }
  uint32_t rawStride() const { return layout.hevc ? layout.hevcGeometry.stride : layout.general ? layout.geometry.stride : 384; }
  uint32_t rawScanlines() const { return layout.hevc ? layout.hevcGeometry.scanlines : layout.general ? layout.geometry.scanlines : 256; }
  uint32_t rawPixelStride() const { return rawStride() / (expectedDepth == 10 ? 2 : 1); }
  // An alternate firmware coded/crop representation is allowed only when it
  // describes the same active picture and fits every already reserved buffer.
  bool pscGeometry() const {
    const uint32_t w=psc.resolution>>16,h=psc.resolution&65535;
    const uint32_t left=psc.crop[0]>>16,top=psc.crop[0]&65535,right=psc.crop[1]>>16,bottom=psc.crop[1]&65535;
    if (!w || !h || ((w|h|left|top|right|bottom)&1) || left+right>=w || top+bottom>=h ||
        w-left-right!=activeWidth || h-top-bottom!=activeHeight) return false;
    if (!layout.general) return psc.resolution==0x014000f0 && !psc.crop[0] && !psc.crop[1];
    if ((psc.returned & 0x2a) != 0x2a) return false; // coded resolution, bit depth and firmware minimum are mandatory
    if (layout.hevc) {
      HevcGeometry g;
      if (!g.configure(w,h,expectedDepth) || w>layout.hevcGeometry.width || h>layout.hevcGeometry.height ||
          g.raw>layout.rawBytes() || g.dpb>layout.dpbBytes()) return false;
      for(unsigned i=0;i<layout.scratchCount();++i)if(g.scratchBytes(i,psc.n)>layout.scratchSize(i,psc.n))return false;
      return true;
    }
    AvcGeometry g;
    if (!g.configure(w,h) || w>layout.geometry.width || h>layout.geometry.height ||
        g.raw>layout.rawBytes() || g.dpb>layout.dpbBytes()) return false;
    for(unsigned i=0;i<layout.scratchCount();++i)if(g.scratchBytes(i,psc.n)>layout.scratchSize(i,psc.n))return false;
    return true;
  }
  bool adoptPscGeometry() {
    if (!layout.general) return true;
    if (layout.hevc) {
      HevcGeometry g;if(!g.configure(psc.resolution>>16,psc.resolution&65535,expectedDepth))return false;
      for(unsigned i=0;i<layout.used;++i) {
        auto &b=layout.buf[i];const uint32_t size=b.type==Raw?g.raw:b.type==Dpb?g.dpb:b.size;
        if(size>b.capacity)return false;
      }
      layout.hevcGeometry=g;
      for(unsigned i=0;i<layout.used;++i) {
        auto &b=layout.buf[i];if(b.type==Raw)b.size=g.raw;else if(b.type==Dpb)b.size=g.dpb;
      }
      o.hw.note("V9 HEVC PSC active %ux%u coded %ux%u crop %08x/%08x depth %u byte-stride %u pixel-stride %u scanlines %u UV %u RAW %u DPB %u N %u cap %u returned %08x",
        activeWidth,activeHeight,g.width,g.height,psc.crop[0],psc.crop[1],expectedDepth,g.stride,rawPixelStride(),g.scanlines,g.uvOffset,g.raw,g.dpb,psc.n,layout.maxN(),psc.returned);
      return true;
    }
    AvcGeometry g;if(!g.configure(psc.resolution>>16,psc.resolution&65535))return false;
    // Layout offsets/capacities stay unchanged. Only payload sizes may shrink.
    for(unsigned i=0;i<layout.used;++i) {
      auto &b=layout.buf[i];uint32_t size=b.type==Raw?g.raw:b.type==Dpb?g.dpb:b.size;
      if(size>b.capacity)return false;
    }
    layout.geometry=g;
    for(unsigned i=0;i<layout.used;++i) {
      auto &b=layout.buf[i];if(b.type==Raw)b.size=g.raw;else if(b.type==Dpb)b.size=g.dpb;
    }
    o.hw.note("V8 PSC geometry active %ux%u coded %ux%u crop %08x/%08x stride %u scanlines %u UV %u RAW %u DPB %u N %u cap %u",
      activeWidth,activeHeight,g.width,g.height,psc.crop[0],psc.crop[1],g.stride,g.scanlines,g.uvOffset,g.raw,g.dpb,psc.n,layout.maxN());
    return true;
  }
  stream::Ledger ledger;
  struct OutputFrame { uint64_t token = 0; unsigned index = 0; uint32_t bytes = 0; };
  OutputFrame outputs[stream::Credits] = {};
  unsigned outputHead = 0, outputCount = 0;
  diagnostic::History *history = nullptr; // storage belongs to the native owner, never the kernel stack
  void (*timeoutObserver)(void *, const SelfTest &) = nullptr;
  void *observerContext = nullptr;
  explicit SelfTest(Owner &owner, Layout &l) : o(owner), layout(l) {}
  Observation observation() const {
    Observation s; s.r=r;s.psc=psc;s.ledger=ledger;s.sid=sessionId;s.token=token;s.deadline=deadline;s.outputCount=outputCount;
    s.flags=unsigned(openSent)|(unsigned(inputStarted)<<1)|(unsigned(rawStarted)<<2)|(unsigned(drainSent)<<3)|
      (unsigned(closeSent)<<4)|(unsigned(cleanup)<<5);
    for(unsigned i=0;i<layout.used;++i){const auto &b=layout.buf[i];s.queued+=b.queued;s.releasing+=b.pendingRelease;s.readonly+=b.readonly;}
    return s;
  }
  bool pscSupported() const {
    if (!streaming) return psc.supported();
    if (layout.hevc) {
      // HEVC has no CODED_FRAMES/AVC POC subscription. Never promote seeded
      // profile/tier/level or buffer minimum to firmware-confirmed values.
      constexpr uint32_t required = (1u<<1)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10);
      return (psc.returned & required) == required && pscGeometry() && psc.n && psc.n <= layout.maxN() &&
        psc.depth == ((expectedDepth<<16)|expectedDepth) && psc.profile == expectedProfile &&
        psc.level == expectedLevel && psc.tier == expectedTier;
    }
    return pscGeometry() && psc.depth == 0x00080008 && psc.coded == 1 &&
      psc.n && psc.n <= layout.maxN() && (!(psc.returned & (1u << 6)) || psc.poc == expectedPoc) &&
      (!(psc.returned & (1u << 7)) || (expectedProfile == 0 ? psc.profile <= 1 : psc.profile == expectedProfile)) && (!(psc.returned & (1u << 8)) || psc.level == expectedLevel);
  }
  // The first AU may arrive after metadata-only input preparation. It cannot
  // change an opened session's identity or replace already submitted input.
  bool nativeInput(uint32_t sid, const uint8_t *bytes, uint32_t size, uint64_t callerToken)
  {
    if (r.error || cleanup || r.etb || !sid || !bytes || !size || size > kInputBytes ||
        (openSent && (sid != sessionId || !inputStarted || r.stage != Input))) return false;
    sessionId = sid; token = callerToken; accessUnit = bytes; accessUnitBytes = size; checkGolden = false;
    return true;
  }
  void stage(Stage s)
  {
    r.stage = s;
    o.hw.stage(60 + int(s), "V4 single-frame transaction");
    o.hw.note("V4 phase %u", unsigned(s));
  }
  int fail(int code, uint32_t type = 0, uint32_t flags = 0, uint32_t port = 0, uint32_t id = 0, uint32_t header = 0)
  {
    ++errorSerial;
    if (!r.error) {
      r.error = code;
      r.failedStage = r.stage;
      r.errorType = type;
      r.errorFlags = flags;
      r.errorPort = port;
      r.errorId = id;
      r.errorHeader = header;
      if(code==kErrHfiTimeout && streaming && !cleanup && timeoutObserver)
        timeoutObserver(observerContext,*this);
      o.hw.note("V4 first-error %d stage %u session %08x header %u type %08x flags %08x port %u packet %u", code, r.stage, sessionId,
                header, type, flags, port, id);
    }
    return code;
  }
  int tx(uint32_t type, uint32_t flags, uint32_t port, uint32_t payload, const uint32_t *data = nullptr, unsigned words = 0,
         bool systemOpen = false)
  {
    if (words > hfi::kBufferWords - 16 || o.ids.header == 0xffffffffu || o.ids.packet == 0xffffffffu)
      return fail(kErrHfi);
    hfi::begin(o.tx, systemOpen ? 0 : sessionId, o.ids);
    hfi::add(o.tx, type, flags, payload, data, words, o.ids, port);
    // Keep the reviewed OPB/output-START proof without formatting every command
    // twice (property and packet); the complete packet still enters history.
    if (qualificationTrace() || type == 0x03000184 || (type == Start && port == 2))
      o.hw.note("V4 TX stage %u session %08x header %u packet %u type %08x port %u flags %08x payload %u words %u", r.stage, o.tx[1], o.tx[2],
              o.tx[13], type, port, flags, payload, words);
    if(streaming&&history)history->commands.add(o.tx,o.tx[0],o.hw.nowUs());
    const int rc = o.send(o.tx[0], "V4-session");
    return rc ? fail(rc, type, flags, port) : 0;
  }
  int property(uint32_t id, uint32_t value, uint32_t port, uint32_t payload = 1)
  {
    if (qualificationTrace()) o.hw.note("V4 property %08x port %u payload %u value %08x", id, port, payload, value);
    return tx(id, 0, port, payload, &value, 1);
  }
  int pair(uint32_t id, uint32_t a, uint32_t b, uint32_t port)
  {
    const uint32_t v[2] = {a, b};
    if (qualificationTrace()) o.hw.note("V4 property %08x port %u payload 13 values %08x/%08x", id, port, a, b);
    return tx(id, 0, port, 13, v, 2);
  }
  int subscribe(unsigned port, bool properties)
  {
    uint32_t v[12] = {properties ? 2u : 1u};
    unsigned count = 1;
    if (properties) {
      v[count++] = port == 1 ? 0x0300016a : 0x03000162;
      if (port == 2)
        v[count++] = 0x03000121;
    } else
      for (unsigned i = 0; i < pscCount(); ++i)
        if(pscField(i))v[count++] = pscId(i);
    return tx(Subscribe, 3, port, 11, v, count);
  }
  int buffer(Buffer &b, bool release = false, uint32_t dataSize = 0)
  {
    if (!b.size || b.size > b.capacity || b.capacity > layout.arenaBytes() || b.off > layout.arenaBytes() - b.capacity || dataSize > b.size ||
        (release ? (!b.queued || b.pendingRelease || b.readonly) : (b.queued || b.pendingRelease || b.readonly)))
      return fail(kErrHfi);
    uint32_t v[16] = {b.type, b.index, b.iova(), 0, 0, b.size, 0, dataSize, dataSize ? uint32_t(token) : 0, dataSize ? uint32_t(token >> 32) : 0, release ? 0x201u : 0x200u,
                      0,      0,       0,        0, 0};
    // Update ownership before ringing: RX can only be processed by this worker, after this call returns.
    if (release)
      b.pendingRelease = true;
    else {
      b.queued = true;
      b.released = false;
    }
    o.hw.note("V4 buffer %s type %u index %u port %u size %u capacity %u iova %08x data %u timestamp %u", release ? "RELEASE" : "QUEUE",
              b.type, b.index, b.port, b.size, b.capacity, b.iova(), dataSize, v[8]);
    const int rc = tx(Buffers, release ? 3u : 1u, b.port, 5, v, 16);
    if (rc) {
      if (release)
        b.pendingRelease = false;
      else
        b.queued = false;
    }
    return rc;
  }
  bool validate(const uint32_t *b, uint32_t bytes)
  {
    if (!b || bytes < 64 || bytes > kHfiCorePacketBytes || (bytes & 3) || b[0] != bytes || !b[7] || b[7] > (bytes - 32) / 32)
      return false;
    uint32_t at = 32;
    for (unsigned i = 0; i < b[7]; ++i) {
      if (at > bytes || bytes - at < 32)
        return false;
      const uint32_t *p = b + at / 4;
      if (p[0] < 32 || (p[0] & 3) || p[0] > bytes - at || p[4] > 2)
        return false;
      // Minimum payload sizes from HFI's common payload enum; array/blob/string lengths remain bounded by this packet.
      unsigned need = 0;
      switch (p[3]) {
      case 0:
        break;
      case 1:
      case 2:
      case 8:
      case 9:
      case 10:
      case 11:
      case 12:
        need = 4;
        break;
      case 3:
      case 4:
      case 13:
        need = 8;
        break;
      case 5:
        need = p[1] == Buffers ? 64 : 4;
        break;
      case 6:
      case 7:
        need = 1;
        break;
      default:
        return false;
      }
      if (p[0] - 32 < need)
        return false;
      if (p[1] == Buffers && (p[3] != 5 || p[0] < 96))
        return false;
      at += p[0];
    }
    return at == bytes;
  }
  int receive(const uint32_t *b, uint32_t bytes)
  {
    if(b&&streaming&&history)history->messages.add(b,bytes,o.hw.nowUs());
    const uint32_t before = errorSerial;
    if (!validate(b, bytes))
      return fail(kErrHfi);
    // System failures remain actionable even when the header is for a session we do not own.
    for (unsigned at = 32; at < bytes; at += b[at / 4]) {
      const uint32_t *p = b + at / 4;
      if ((p[2] & 8) || (p[1] > 0x05000000 && p[1] < 0x05ffffff))
        return fail(kErrSysError, p[1], p[2], p[4], p[5], b[2]);
    }
    if (!b[1]) {
      for (unsigned at = 32; at < bytes; at += b[at / 4]) {
        const uint32_t *p = b + at / 4;
        if (p[2] & 4)
          return fail(kErrHfi, p[1], p[2], p[4], p[5], b[2]);
      }
      // No new system transaction is outstanding; errors were handled above. Do not reinterpret session SUCCESS as INIT.
      return 0;
    }
    if (b[1] != sessionId || r.closed)
      return fail(kErrHfi, 0, b[1], 0, 0, b[2]);
    bool hasPsc = false;
    for (unsigned at = 32; at < bytes; at += b[at / 4])
      if (b[at / 4 + 1] == Settings && b[at / 4 + 4] == 1)
        hasPsc = true;
    if (hasPsc) {
      if (r.psc || r.stage != FirstPsc)
        return fail(kErrHfi, Settings);
      psc = streaming ? initialPsc : Psc{};
      if(streaming){psc.poc=expectedPoc;psc.profile=expectedProfile;psc.level=expectedLevel;}
    }
    picture = 0;
    frameError = false;
    // Prefer the firmware's specific error code to the generic command carrying SESSION_ERROR.
    for (unsigned at = 32; at < bytes; at += b[at / 4]) {
      const uint32_t *p = b + at / 4;
      if (p[1] > 0x04000000 && p[1] < 0x04ffffff)
        fail(kErrHfi, p[1], p[2], p[4], p[5], b[2]);
    }
    for (unsigned pass = 0; pass < 4; ++pass)
      for (unsigned at = 32; at < bytes; at += b[at / 4]) {
        const uint32_t *p = b + at / 4;
        const uint32_t type = p[1], flags = p[2], port = p[4];
        if (pass == 0) {
          if (flags & 4)
            fail(kErrHfi, type, flags, port, p[5], b[2]);
          continue;
        }
        const bool kind = pass == 1   ? (type > 0x06000000 && type < 0x06ffffff)
                          : pass == 2 ? (type > 0x03000000 && type < 0x03ffffff)
                                      : (type > 0x01000000 && type < 0x01ffffff);
        if (!kind)
          continue;
        if (qualificationTrace()) o.hw.note("V4 RX pass %u stage %u session %08x header %u packet %u type %08x port %u flags %08x bytes %u", pass, r.stage, b[1],
                  b[2], p[5], type, port, flags, p[0]);
        if (pass == 1) {
          if (type == 0x06000001 || type == 0x06000002 || type == 0x06000004) {
            frameError = true;
            fail(kErrHfi, type, flags, port, p[5], b[2]);
          }
          if (type == 0x06000006) {
            if (!drainSent)
              fail(kErrHfi, type);
            else {
              r.last = true;
              o.hw.note("V4 drain LAST info");
            }
          }
          if (type == 0x06000007)
            fail(kErrHfi, type); // FIRST_IPSC must not enter running DRC
        } else if (pass == 2) {
          if (flags & 2)
            continue;
          if (p[0] < 36) {
            fail(kErrHfi, type);
            continue;
          }
          if (hasPsc && port == 1) {
            for (unsigned i = 1; i < pscCount(); ++i)
              if (pscField(i) && type == pscId(i)) {
                if (i == 2 && p[0] < 40) {
                  fail(kErrHfi, type);
                  break;
                }
                psc.returned |= 1u << i;
                switch (i) {
                case 1:
                  psc.resolution = p[8];
                  break;
                case 2:
                  psc.crop[0] = p[8];
                  psc.crop[1] = p[9];
                  break;
                case 3:
                  psc.depth = p[8];
                  break;
                case 4:
                  psc.coded = p[8];
                  break;
                case 5:
                  psc.n = p[8];
                  break;
                case 6:
                  psc.poc = p[8];
                  break;
                case 7:
                  psc.profile = p[8];
                  break;
                case 8:
                  psc.level = p[8];
                  break;
                case 9:
                  psc.color = p[8];
                  break;
                case 10:
                  psc.tier = p[8];
                  break;
                }
              }
          }
          if (type == 0x03000162 && port == 2)
            picture = p[8];
        } else {
          if (r.error && !cleanup)
            continue;
          if (type == Settings) {
            // First HEVC qualification must retain the complete firmware
            // response even when admission rejects it. RX payload0 alone
            // cannot preserve the second crop word or the returned mask.
            if(layout.hevc)o.hw.note("V9 HEVC PSC observed port %u returned %08x res %08x crop %08x/%08x depth %08x N %u profile %u level %u tier %u expected %u/%u/%u/%u",
              port,psc.returned,psc.resolution,psc.crop[0],psc.crop[1],psc.depth,psc.n,psc.profile,psc.level,psc.tier,
              expectedDepth,expectedProfile,expectedLevel,expectedTier);
            if (port != 1 || r.psc || !pscSupported()) {
              fail(kErrHfi, type, flags, port, p[5], b[2]);
              continue;
            }
            ++r.psc;
            o.hw.note("V4 PSC FIRST_IPSC 1 input-pause 1 returned %08x N %u", psc.returned, psc.n);
            for (unsigned i = 1; i < pscCount(); ++i)
              if(pscField(i))o.hw.note("V4 PSC property %08x value %08x source %s", pscId(i), psc.value(i),
                        (psc.returned & (1u << i)) ? "returned" : "seeded");
            o.hw.note("V4 PSC crop %08x/%08x supported 1", psc.crop[0], psc.crop[1]);
          } else if (type == Buffers)
            completion(p, b[2]);
          else if (type == Drain) {
            if (!drainSent || port != 1 || !(flags & 1))
              fail(kErrHfi, type, flags, port, p[5], b[2]);
            else {
              r.drainAck = true;
              o.hw.note("V4 drain ACK success 1");
            }
          } else if (type == Stop) {
            if (stopPending != port || !port || !(flags & 1))
              fail(kErrHfi, type, flags, port, p[5], b[2]);
            else {
              if (port == 2)
                r.stopRaw = true;
              else
                r.stopInput = true;
              stopPending = 0;
              o.hw.note("V4 STOP port %u success 1", port);
            }
          } else if (type == Close) {
            if (!closeSent || port || !(flags & 1))
              fail(kErrHfi, type, flags, port, p[5], b[2]);
            else {
              r.closed = true;
              o.hw.note("V4 CLOSE success 1 session %08x", sessionId);
            }
          }
          // OPEN/START/RESUME and subscriptions have no mandatory standalone ACK in the reference protocol.
        }
      }
    return errorSerial != before ? r.error : 0;
  }
  void completion(const uint32_t *p, uint32_t header)
  {
    const uint32_t *v = p + 8;
    o.hw.note("V4 completion type %u index %u iova %08x size %u addr-offset %u data-offset %u data-size %u timestamp %08x%08x flags %08x "
              "picture %08x error %u",
              v[0], v[1], v[2], v[5], v[4], v[6], v[7], v[9], v[8], v[10], picture, unsigned(frameError));
    Buffer *b = layout.find(v[0], v[1]);
    if (!b || !b->queued || b->port != p[4] || v[2] != b->iova() || v[3] || v[4] || v[5] > b->size || v[6] > v[5] ||
        v[7] > v[5] - v[6]) {
      fail(kErrHfi, Buffers, p[2], p[4], p[5], header);
      return;
    }

    if (b->pendingRelease) {
      if (!(v[10] & 1) || (v[10] & 0x10)) {
        fail(kErrHfi, Buffers);
        return;
      }
      b->pendingRelease = false;
      b->queued = false;
      b->released = true;
      b->readonly = false;
      ++r.releases;
      o.hw.note("V4 RELEASE_DONE type %u index %u iova %08x matched-pending 1", b->type, b->index, b->iova());
      return;
    }
    if (v[10] & 1) {
      fail(kErrHfi, Buffers);
      return;
    }
    b->readonly = (v[10] & 0x10) != 0;
    if (b->type == Bitstream) {
      if (streaming) {
        if (b->readonly || !ledger.ebd(b->index)) { fail(kErrHfi, Buffers); return; }
        b->queued = false; ++r.ebd;
        deadline = o.hw.nowUs() + 5000000;
        o.hw.note("V6 EBD session %08x index %u token %llu count %u", sessionId,b->index,
                  (unsigned long long)ledger.inputToken[b->index],r.ebd);
        return;
      }
      if (++r.ebd != 1) {
        fail(kErrHfi, Buffers);
        return;
      }
      b->queued = false;
      o.hw.note("V4 EBD count %u", r.ebd);
      if (!checkGolden) o.hw.note("V5 native EBD session %08x token %llu owned-input 1 count %u", sessionId,
                                 (unsigned long long)token, r.ebd);
      return;
    }
    if (b->type != Raw) {
      // Running firmware must keep split DPB/internal allocations. A return is
      // not permission to overwrite a reference; unsupported return fails closed.
      if (streaming && !cleanup) fail(kErrHfi, Buffers, p[2], p[4], p[5], header);
      return;
    }
    b->queued = false;
    ++r.rawReturns;
    if (v[10] & 0x20000000) {
      fail(kErrHfi, Buffers);
      return;
    }
    if (v[10] & 0x10000000) {
      if (!drainSent) {
        fail(kErrHfi, Buffers);
        return;
      }
      r.last = true;
      o.hw.note("V4 drain LAST raw index %u empty %u", b->index, unsigned(!v[7]));
    }
    const bool noShow = streaming && layout.hevc && (picture & 0x40);
    if (!v[7] && !noShow) {
      if (!(v[10] & 0x10000000) && !cleanup)
        fail(kErrHfi, Buffers);
      return;
    }
    if (streaming) {
      if (cleanup) return; // cancellation never delivers a late success
      const uint64_t returnedToken = uint64_t(v[8]) | (uint64_t(v[9]) << 32);
      // HEVC adds CRA/BLA and explicit non-display pictures. They still own a
      // token and a RAW return; pass a NULL image disposition to the consumer.
      const bool kind = (picture & (layout.hevc ? 0x3fu : 15u)) || noShow;
      if (!rawStarted || v[6] || (!noShow && v[7] < layout.rawBytes()) || b->readonly || frameError || !kind || (!layout.hevc && (picture & 0x40)) ||
          outputCount == stream::Credits || !ledger.picture(returnedToken,b->index)) {
        fail(kErrHfi, Buffers, p[2], p[4], p[5], header); return;
      }
      outputs[(outputHead+outputCount)%stream::Credits]={returnedToken,b->index,noShow?0:v[7]};++outputCount;++r.decoded;
      deadline = o.hw.nowUs()+5000000;
      o.hw.note("V6 FBD session %08x token %llu index %u picture %u count %u",sessionId,
                (unsigned long long)returnedToken,b->index,picture,r.decoded);
      return;
    }
    if (cleanup || !rawStarted || v[6] || v[8] != uint32_t(token) || v[9] != uint32_t(token >> 32) || frameError || !(picture & 1) || (picture & 0x40) ||
        ++r.decoded != 1) {
      fail(kErrHfi, Buffers, p[2], p[4], p[5], header);
      return;
    }
    o.hw.barrier();
    char hash[65];
    o.hw.hashActive(o.dma.arena + b->off, hash);
    o.hw.note("V4 active SHA256 %s", hash);
    r.checksum = sameText(hash, kGolden);
    o.hw.note("V4 RAW FBD decoded %u active 115200 checksum %s readonly %u", r.decoded, r.checksum ? "PASS" : "FAIL",
              unsigned(b->readonly));
    r.rawIndex = b->index;
    r.rawBytes = v[7];
    if (!checkGolden)
      o.hw.note("V5 native FBD session %08x token %llu index %u iova %08x bytes %u active 115200 SHA256 %s", sessionId,
                (unsigned long long)token, b->index, b->iova(), v[7], hash);
    if (checkGolden && !r.checksum)
      fail(kErrHfi, Buffers);
  }
  int pump()
  {
    int rc = 0;
    for (unsigned n = 0; n < 64; ++n) {
      uint32_t bytes = 0;
      const int read = hfi::read(o.mem, hfi::kMsgq, o.rx, sizeof o.rx, &bytes, [this] { o.hw.barrier(); });
      if (read == 1)
        break;
      if (read) {
        fail(kErrHfi);
        rc = kErrHfi;
        break;
      }
      o.packetTrace("RX", ++o.rxEvents, o.rx, bytes);
      const int got = receive(o.rx, bytes);
      if (got) {
        rc = got;
        if (failClosedCleanup) return rc;
      }
    }
    o.drainDebug();
    const uint32_t irq = o.ops.rd(kIris + kWrapperIntrStatus);
    ++o.watchdogSamples;
    o.watchdogSeen |= irq & (1u << 3);
    if (o.watchdogSeen) {
      rc = fail(kErrWatchdog);
      if (failClosedCleanup) return rc;
    }
    const vsmmu::Fault fault = vsmmu::fault(o.plan, o.ops);
    o.rep.fsr |= fault.fsr & vsmmu::kFsrFaults;
    if (fault.fsr & vsmmu::kFsrFaults)
      rc = fail(kErrFault);
    if (streaming) {
      ledger.ack = r.drainAck; ledger.last = r.last;
      if (!ledger.pending && !ledger.draining && ledger.inputsEmpty()) deadline = UINT64_MAX;
    }
    if (o.hw.nowUs() >= deadline && !cleanup)
      rc = fail(kErrHfiTimeout);
    return rc;
  }
  template <class Done> int wait(Done done, unsigned ms = 1000)
  {
    const uint64_t until = o.hw.nowUs() + uint64_t(ms) * 1000;
    for (;;) {
      const int rc = pump();
      if (rc && (!cleanup || failClosedCleanup))
        return rc;
      if (failClosedCleanup && r.error) return r.error;
      if (done())
        return 0;
      if (o.hw.nowUs() >= until)
        return fail(kErrHfiTimeout);
      o.hw.sleepMs(1);
    }
  }
  int consumed()
  {
    const uint32_t target = o.mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQWrite);
    return wait([this, target] { return o.mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQRead) == target; });
  }
  int scratch(unsigned set, unsigned n)
  {
    if (!n || n > layout.maxN() || property(0x03000193, n, 1))
      return r.error ? r.error : fail(kErrHfi);
    for (unsigned i = 0; i < layout.scratchCount(); ++i) {
      Buffer &b = *layout.find(scratchType[i], set);
      b.size = layout.scratchSize(i, n);
      if(!b.size || b.size>b.capacity)return fail(kErrHfi);
      if (buffer(b))
        return r.error;
    }
    o.hw.note("V8 scratch set %u N %u COMV %u count %u general %u",set,n,layout.scratchSize(1,n),layout.scratchCount(),unsigned(layout.general));
    return 0;
  }
  // Format/layout-only control path. No AU, frame credit or output port is
  // submitted here; native StartSession waits for this before reporting ready.
  int prepareInput()
  {
    if (r.error) return r.error;
    if (cleanup || openSent || inputStarted || r.etb) return kErrArgument;
    deadline = o.hw.nowUs() + 5000000;
    stage(Opening);
    const uint32_t sid = sessionId;
    if (tx(Open, 3, 0, 1, &sid, 1, true))
      return r.error;
    openSent = true;
    if (consumed())
      return r.error;
    // // acceptance is established by PSC downstream; this is only submission/consumption
    stage(Config);
    if (property(0x03000100, layout.hevc ? 3 : 1, 0, 9) || property(0x03000168, 0, 1) ||
        (streaming && property(0x0300015b, 1, 1)))
      return r.error;
    for (unsigned i = 1; i < pscCount(); ++i) {
      if(!pscField(i))continue;
      if (i == 2) {
        if (pair(pscId(i), psc.crop[0], psc.crop[1], 1))
          return r.error;
      } else if (property(pscId(i), psc.value(i), 1, (i == 7 || i == 8 || i == 10) ? 9 : i == 9 ? 10 : 1))
        return r.error;
    }
    if (property(0x03000107, streaming ? expectedProfile : 4, 2, 9) || property(0x03000108, streaming ? expectedLevel : 18, 2, 9) || property(0x03000169, 1, 1) || property(0x03000123, 64, 1) ||
        property(0x0300010a, 1, 0) || property(0x0300010b, 4, 0) || buffer(*layout.find(Persist, 0)) || scratch(0, 4) ||
        subscribe(1, false) || subscribe(1, true))
      return r.error;
    stage(Input);
    inputStarted = true;
    if (tx(Start, 3, 1, 0) || consumed())
      return r.error;
    deadline = UINT64_MAX; // an idle prepared session is not a missing picture
    return 0;
  }
  int transaction()
  {
    if (r.error) return r.error;
    if (cleanup || r.etb || rawStarted || !accessUnit || !accessUnitBytes) return kErrArgument;
    if (checkGolden) o.hw.note("V4 evidence contract vid-v4a: bounded one-AU kernel AVC no-consumer");
    if (checkGolden) o.hw.note("V4 fixture bytes %u SHA256 %s", unsigned(sizeof kAccessUnit), kAuSha);
    else o.hw.note("V5 transaction caller-input session %08x token %llu bytes %u embedded-fixture 0", sessionId,
                   (unsigned long long)token, accessUnitBytes);
    if (!inputStarted) {
      if (const int rc = prepareInput()) return rc;
    } else if (!openSent || r.stage != Input) return kErrArgument;
    deadline = o.hw.nowUs() + 5000000;
    Buffer &input = *layout.find(Bitstream, 0);
    for (unsigned i = 0; i < accessUnitBytes; ++i)
      o.dma.arena[input.off + i] = accessUnit[i];
    o.hw.barrier();
    if (streaming && (!ledger.accept(token) || !ledger.submit(token,0))) return fail(kErrHfi);
    ++r.etb;
    stage(FirstPsc); // RX may arrive immediately after ETB; the next pump sees this phase
    if (buffer(input, false, accessUnitBytes) || wait([this] { return r.psc == 1; }))
      return r.error;
    o.hw.note("V4 acceptance OPEN CONFIG START BITSTREAM downstream-PSC 1");
    stage(Reconfigure);
    if (!adoptPscGeometry()) return fail(kErrHfi,Settings);
    if (property(0x03000184, 1, 2) || property(0x03000101, expectedDepth == 10 ? 3 : 1, 2, 9) ||
        pair(0x03000104, (rawPixelStride()<<16)|rawScanlines(), (rawPixelStride()<<16)|((rawScanlines()/2+15)&~15u), 2))
      return r.error;
    for (unsigned i = 0; i < layout.scratchCount(); ++i)
      if (buffer(*layout.find(scratchType[i], 0), true))
        return r.error;
    if (scratch(1, psc.n) || property(0x0300010a, 1, 0) || property(0x0300010b, 4, 0))
      return r.error;
    const uint32_t cause = Settings;
    if (tx(Resume, 3, 1, 1, &cause, 1) || subscribe(2, false))
      return r.error;
    for (unsigned i = 1; i < pscCount(); ++i) {
      if(!pscField(i))continue;
      if (i == 2) {
        if (pair(pscId(i), psc.crop[0], psc.crop[1], 2))
          return r.error;
      } else if (property(pscId(i), psc.value(i), 2))
        return r.error;
    }
    if (subscribe(2, true))
      return r.error;
    stage(Output);
    rawStarted = true;
    if (tx(Start, 3, 2, 0) || consumed())
      return r.error;
    const unsigned count = psc.n < 2 ? 2 : psc.n;
    for (unsigned i = 0; i < psc.n; ++i)
      if (buffer(*layout.find(Dpb, i)))
        return r.error;
    for (unsigned i = 0; i < count; ++i) {
      Buffer &b = *layout.find(Raw, i);
      // General native output never checked the diagnostic sentinel. Firmware
      // owns and writes RAW after queueing; do not overwrite it just to discard
      // the check. Preserve the barrier and the exact descriptor/queue order.
      if (streaming && layout.general) { o.hw.barrier(); if (buffer(b)) return r.error; continue; }
      for (unsigned j = 0; j < layout.rawBytes(); ++j)
        o.dma.arena[b.off + j] = 0xa5;
      char hash[65];
      if(layout.general){o.hw.barrier();if(buffer(b))return r.error;continue;}
      o.hw.hashActive(o.dma.arena + b.off, hash);
      if (sameText(hash, kGolden))
        return fail(kErrHfi);
      o.hw.note("V4 sentinel RAW index %u active SHA256 %s differs-golden 1", i, hash);
      o.hw.barrier();
      if (buffer(b))
        return r.error;
    }
    if (streaming) {
      accessUnit = nullptr; accessUnitBytes = 0;
      o.hw.note("V6 persistent OPEN ready session %08x PSC %u N %u decode-order 1 aperture %u entries %u",sessionId,r.psc,psc.n,layout.bytes,layout.used);
      return 0;
    }
    stage(Draining);
    drainSent = true;
    if (tx(Drain, 7, 1, 0) ||
        wait([this] { return r.decoded == 1 && (!checkGolden || r.checksum) && r.ebd == 1 && r.drainAck && r.last && r.releases == 5; }, 5000))
      return r.error;
    o.hw.note("V4 frame gate PASS ETB %u EBD %u PSC %u decoded %u drain-ack %u LAST %u N %u", r.etb, r.ebd, r.psc, r.decoded,
              unsigned(r.drainAck), unsigned(r.last), psc.n);
    return 0;
  }
  int streamSubmit(const uint8_t *bytes, uint32_t size, uint64_t t) {
    if (!streaming || !rawStarted || cleanup || r.error || !bytes || !size || size > (1u<<20)) return -22;
    if (ledger.paused) {
      const uint32_t cause=Drain;
      if(tx(Resume,3,1,1,&cause,1)||tx(Resume,3,2,1,&cause,1)||!ledger.resume())return fail(kErrHfi);
      drainSent=r.drainAck=r.last=false;stage(Output);
      for(unsigned i=0;i<(psc.n<2?2:psc.n);++i){auto &b=*layout.find(Raw,i);if(!b.queued&&!ledger.rawHeld[i]&&buffer(b))return r.error;}
      o.hw.note("V6 resume session %08x BITSTREAM RAW cause DRAIN",sessionId);
    }
    const int index=ledger.freeInput();
    if(index<0 || ledger.pending==stream::Credits)return 1; // back-pressure, no mutation
    if(!ledger.accept(t)||!ledger.submit(t,unsigned(index)))return fail(kErrHfi);
    Buffer &b=*layout.find(Bitstream,unsigned(index));
    if(b.queued||b.readonly||b.pendingRelease)return fail(kErrHfi);
    for(unsigned i=0;i<size;++i)o.dma.arena[b.off+i]=bytes[i];
    o.hw.barrier();token=t;++r.etb;deadline=o.hw.nowUs()+5000000;
    o.hw.note("V6 ETB session %08x token %llu index %u bytes %u count %u input-reuse %u",sessionId,
              (unsigned long long)t,b.index,size,r.etb,ledger.inputReuse);
    return buffer(b,false,size);
  }
  const OutputFrame *nextOutput() const {
    return outputCount && !cleanup && !r.error ? &outputs[outputHead] : nullptr;
  }
  bool popOutput(OutputFrame &f) {
    const auto *next=nextOutput();
    if(!next)return false;f=*next;outputHead=(outputHead+1)%stream::Credits;--outputCount;return true;
  }
  int releaseOutput(unsigned index,uint64_t t) {
    if(!streaming||!ledger.release(index,t)||!ledger.retire(t))return fail(kErrHfi);
    Buffer *b=layout.find(Raw,index);
    if(!b||b->queued||b->readonly||b->pendingRelease)return fail(kErrHfi);
    o.hw.note("V6 RAW recycle session %08x token %llu index %u raw-reuse %u",sessionId,(unsigned long long)t,index,ledger.rawReuse);
    if(!cleanup && !r.error && !(r.drainAck&&r.last))return buffer(*b);
    return 0;
  }
  int beginDrain() {
    if(!streaming||r.error||cleanup)return -22;
    if(ledger.paused||ledger.draining)return 0;
    if(!ledger.beginDrain())return fail(kErrHfi);
    stage(Draining);drainSent=true;r.drainAck=r.last=false;deadline=o.hw.nowUs()+5000000;
    return tx(Drain,7,1,0);
  }
  int drainStatus() {
    if(r.error)return r.error;
    if(ledger.paused)return 1;
    if(!ledger.drainReady())return 0;
    // ACK+LAST fences all prior RAW events. Any accepted picture with no FBD
    // remains explicit no-output; caller retires it via the same completion path.
    return ledger.pending ? 2 : (ledger.drained()?1:0);
  }
  int retireNoOutput(uint64_t t) {
    auto *f=ledger.find(t);
    if(!ledger.drainReady()||!f||f->phase!=stream::Phase::Submitted)return -22;
    return ledger.retire(t)?0:-22;
  }
  int shutdown(bool failClosed = false)
  {
    cleanup = true;
    failClosedCleanup = failClosed;
    auto abortCleanup = [this](int error) {
      r.close = error;
      o.hw.note("V9c session cleanup stopped at first error %d no-further-HFI 1",error);
      stage(Finished);
      return error;
    };
    if (failClosed && r.error) return abortCleanup(r.error);
    int rc = 0;
    if (rawStarted) {
      stage(StopRaw);
      stopPending = 2;
      int got = tx(Stop, 7, 2, 0);
      if (!got)
        got = wait([this] { return r.stopRaw; });
      if (got && !rc)
        rc = got;
      if (failClosed && (got || r.error)) return abortCleanup(got ? got : r.error);
    }
    if (inputStarted) {
      stage(StopInput);
      stopPending = 1;
      int got = tx(Stop, 7, 1, 0);
      if (!got)
        got = wait([this] { return r.stopInput; });
      if (got && !rc)
        rc = got;
      if (failClosed && (got || r.error)) return abortCleanup(got ? got : r.error);
    }
    if (openSent) {
      stage(Closing);
      closeSent = true;
      int got = tx(Close, 7, 0, 0);
      if (!got)
        got = wait([this] { return r.closed; });
      if (got && !rc)
        rc = got;
      if (failClosed && (got || r.error)) return abortCleanup(got ? got : r.error);
    }
    // A bounded extra pump detects already queued late replies while backing
    // remains owned by the core.
    const int late = pump();
    if (late && !rc)
      rc = late;
    r.close = rc;
    const bool released = failClosed && !rc && r.closed;
    if (released && streaming) {
      ledger.closed();
      outputHead = outputCount = 0;
    }
    unsigned queued = 0, refs = 0, pending = 0;
    for (unsigned i = 0; i < layout.used; ++i) {
      Buffer &b = layout.buf[i];
      queued += b.queued;
      refs += b.readonly;
      pending += b.pendingRelease;
      if (b.queued || b.readonly || b.pendingRelease)
        o.hw.note("V4 ledger type %u index %u iova %08x queued %u readonly %u pending-release %u %s", b.type, b.index,
                  b.iova(), unsigned(b.queued), unsigned(b.readonly), unsigned(b.pendingRelease),
                  released ? "released-on-close 1" : "retained-until-PAS 1");
      if (released) b.queued = b.readonly = b.pendingRelease = false;
    }
    o.hw.note("V4 session end closed %u close-error %d queued %u readonly %u pending %u arena-retained 1", unsigned(r.closed), r.close,
              queued, refs, pending);
    stage(Finished);
    return rc;
  }
  int run()
  {
    const int rc = transaction();
    // Preserve all fields even when validation rejected PSC or an earlier stage failed (returned=0 means defaults).
    o.hw.note("V4 PSC-raw resolution %08x crop %08x/%08x depth %08x coded %08x N %u POC %u profile %u level %u color %08x returned %08x",
              psc.resolution, psc.crop[0], psc.crop[1], psc.depth, psc.coded, psc.n, psc.poc, psc.profile, psc.level, psc.color,
              psc.returned);
    shutdown();
    return rc ? rc : r.error ? r.error : r.close;
  }
};
} // namespace session
} // namespace pianovideo
