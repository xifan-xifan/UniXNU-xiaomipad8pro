// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Independent scripted firmware peer: asynchronous commands, reordered PSC/FBD packets, ownership and fault injection.
// c++ -std=c++20 -O2 -Wall -Wextra -Werror -I .. -I ../../include session_host.cpp -o /tmp/session_host && /tmp/session_host
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <CommonCrypto/CommonDigest.h>
#include "VideoSession.h"
#include "VideoArena.h"
#include "VideoNativeState.h"
#include "vt/PianoVideoVTContract.h"
using namespace pianovideo;
using namespace pianovideo::session;
static unsigned failures = 0;
#define CHECK(x)                                                                                                                           \
  do {                                                                                                                                     \
    if (!(x)) {                                                                                                                            \
      std::printf("FAIL line %u: %s\n", __LINE__, #x);                                                                                     \
      ++failures;                                                                                                                          \
    }                                                                                                                                      \
  } while (0)
struct TestHw {
  uint64_t clock = 0;
  std::string log;
  void barrier() {}
  void stage(int, const char *) {}
  void sleepMs(unsigned n) { clock += uint64_t(n) * 1000; }
  uint64_t nowUs() { return clock; }
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
    if (std::getenv("VIDEO_SESSION_ECHO"))
      std::printf("PianoVideo: %s\n", b);
  }
  void hashActive(const volatile uint8_t *base, char hash[65])
  {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    activeRows(base, [&](const uint8_t *p, unsigned n) { CC_SHA256_Update(&ctx, p, n); });
    uint8_t h[32];
    CC_SHA256_Final(h, &ctx);
    hexDigest(h, hash);
  }
};
struct TestOps {
  bool fault = false, watchdog = false;
  uint32_t rd(uint64_t pa) { return pa == kIris + kWrapperIntrStatus ? (watchdog ? 8 : 0) : (fault && (pa & 4095) == vsmmu::kFsr ? 2 : 0); }
};
struct Peer {
  TestHw hw;
  TestOps ops;
  vsmmu::Plan plan;
  hfi::Mem mem;
  hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords] = {}, rx[hfi::kBufferWords] = {};
  struct {
    volatile uint8_t *arena = nullptr;
  } dma;
  struct {
    uint32_t fsr = 0;
  } rep;
  unsigned watchdogSamples = 0, watchdogSeen = 0, rxEvents = 0;
  unsigned n = 4, inputCount = 0, dpbs = 0, partial = 0, releaseCount = 0, stops = 0, closes = 0, hostErrors = 0;
  uint32_t activeSid = 0;
  uint64_t expectedToken = kTimestamp;
  std::vector<uint8_t> expectedInput = std::vector<uint8_t>(kAccessUnit, kAccessUnit + sizeof kAccessUnit);
  bool dropPsc = false, badPixels = false, badPsc = false, badRelease = false, dropClose = false, dropStop = false, dropLast = false,
       dropDrainAck = false;
  bool wrongTimestamp = false, wrongIova = false, corrupt = false, sessionError = false, readonly = false, late = false,
       systemError = false;
  bool opened = false, input = false, raw = false, resumed = false;
  bool hevc = false;
  uint32_t depth = 8, firmwareMinimum = 4, hevcPicture = 1;
  HevcGeometry hg;
  std::vector<std::vector<uint32_t>> properties, subscriptions;
  std::vector<uint32_t> uc;
  std::vector<uint8_t> arena;
  std::vector<std::vector<uint32_t>> raws, pendingReleases;
  std::vector<uint32_t> traceTypes, comvSizes, comvCounts, headers, packets;
  Peer() : uc(kUcBytes / 4), arena(kAvcArenaBytes, 0)
  {
    mem.uc = uc.data();
    dma.arena = arena.data();
    hfi::initQueues(mem);
    plan.bank = 3;
    plan.pages = 128;
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
    for (const auto &p : packets)
      b.insert(b.end(), p.begin(), p.end());
    b[0] = uint32_t(4 * b.size());
    CHECK(hfi::write(mem, hfi::kMsgq, b.data(), b[0], [] {}) == 0);
  }
  void packetTrace(const char *, unsigned, const uint32_t *, unsigned) {}
  void drainDebug() {}
  int send(unsigned bytes, const char *)
  {
    // Exercise the real ring, including wrap. The peer consumes it before responding, like the firmware owner.
    CHECK(hfi::write(mem, hfi::kCmdq, tx, bytes, [] {}) == 0);
    uint32_t data[hfi::kBufferWords], size = 0;
    CHECK(hfi::read(mem, hfi::kCmdq, data, sizeof data, &size, [] {}) == 0);
    CHECK(size == bytes);
    CHECK(data[7] == 1 && data[0] == bytes);
    const uint32_t *p = data + 8;
    const uint32_t type = p[1], flags = p[2], port = p[4], payload = p[3];
    headers.push_back(data[2]);
    packets.push_back(p[5]);
    traceTypes.push_back(type);
    if (type == 0x01000003) {
      CHECK(!data[1] && port == 0 && flags == 3 && payload == 1 && p[8] != 0);
      activeSid = p[8];
      opened = true;
      return 0;
    }
    CHECK(opened && data[1] == activeSid);
    if (type == 0x01000005) {
      CHECK(flags == 3 && payload == 0);
      if (port == 1)
        input = true;
      else
        raw = true;
      return 0;
    }
    if (type == 0x01000008) {
      CHECK(port == 1 && flags == 3 && payload == 1 && p[8] == 0x0100000c && releaseCount == (hevc ? 4u : 5u));
      resumed = true;
      for (const auto &b : pendingReleases)
        reply({packet(0x01000009, 0, 1, 5, b)});
      pendingReleases.clear();
      return 0;
    }
    if (type == 0x03000193)
      comvCounts.push_back(p[8]);
    if (type == 0x01000009) {
      CHECK(payload == 5 && p[0] == 96);
      std::vector<uint32_t> b(p + 8, p + 24);
      const uint32_t bt = b[0];
      CHECK((b[10] & 0x200) != 0 && b[3] == 0 && b[4] == 0 && b[6] == 0);
      CHECK(b[2] >= kArenaBase && uint64_t(b[2]) + b[5] <= kArenaBase + (hevc ? kArenaBytes : 64*1024*1024));
      if (b[10] & 1) {
        CHECK(flags == 3);
        ++releaseCount;
        b[10] = badRelease ? 0 : 1;
        pendingReleases.push_back(b);
        return 0;
      }
      CHECK(flags == 1);
      if (bt == 10)
        comvSizes.push_back(b[5]);
      if (bt == 5) {
        CHECK(b[5] == 196608);
        ++partial;
      }
      if (bt == 6) {
        CHECK(raw && resumed && b[5] == (hevc ? hg.dpb : 155648));
        ++dpbs;
      }
      if (bt == 2) {
        CHECK(raw && resumed && b[5] == (hevc ? hg.raw : 147456) && b[7] == 0);
        raws.push_back(b);
        if(hevc && raws.size()==firmwareMinimum) {
          b[7]=(hevcPicture&0x40)?0:hg.raw;b[8]=uint32_t(expectedToken);b[9]=uint32_t(expectedToken>>32);b[10]=0;
          reply({packet(0x01000009,0,2,5,b),packet(0x03000162,0,2,1,{hevcPicture})});
        }
      }
      if (bt == 1) {
        CHECK(input && !raw && b[5] == 7077888 && b[7] == expectedInput.size() && b[8] == uint32_t(expectedToken) && b[9] == uint32_t(expectedToken >> 32));
        CHECK(std::memcmp(arena.data() + b[2] - kArenaBase, expectedInput.data(), expectedInput.size()) == 0);
        ++inputCount;
        b[7] = 0;
        b[8] = b[9] = 0; // input completion metadata is not an echoed-timestamp contract; RAW is correlated strictly
        reply({packet(type, 0, 1, 5, b)});
        if (!dropPsc && hevc)
          reply({packet(0x0100000c,0,1,0),packet(0x03000103,0,1,1,{(hg.width<<16)|hg.height}),
            packet(0x03000105,0,1,13,{0,0}),packet(0x0300010f,0,1,1,{(depth<<16)|depth}),
            packet(0x03000124,0,1,1,{firmwareMinimum}),packet(0x03000107,0,1,9,{depth==10?2u:0u}),
            packet(0x03000108,0,1,9,{4}),packet(kHevcTier,0,1,9,{0}),packet(0x03000155,0,1,10,{0x14000003})});
        else if (!dropPsc)
          reply({packet(0x0100000c, 0, 1, 0), packet(0x03000103, 0, 1, 1, {badPsc ? 0x01400100u : 0x014000f0u}),
                 packet(0x03000105, 0, 1, 13, {0, 0}), packet(0x0300010f, 0, 1, 1, {0x00080008}), packet(0x03000120, 0, 1, 1, {1}),
                 packet(0x03000124, 0, 1, 1, {n}), packet(0x03000128, 0, 1, 1, {2}), packet(0x03000107, 0, 1, 1, {0}),
                 packet(0x03000108, 0, 1, 1, {4}), packet(0x03000155, 0, 1, 1, {0x14000003})});
      }
      return 0;
    }
    if (type == 0x01000007) {
      CHECK(port == 1 && flags == 7 && raw && resumed && inputCount == 1 && dpbs == n && raws.size() == (n < 2 ? 2 : n));
      if (!dropDrainAck)
        reply({packet(type, 1, 1, 0)});
      auto b = raws.back();
      b[7] = 147456;
      b[8] = wrongTimestamp ? 42 : uint32_t(expectedToken);
      b[9] = uint32_t(expectedToken >> 32);
      b[10] = readonly ? 0x10 : 0;
      uint8_t *base = arena.data() + b[2] - kArenaBase;
      for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 320; ++x)
          base[y * 384 + x] = 64;
      for (unsigned y = 0; y < 120; ++y)
        for (unsigned x = 0; x < 320; x += 2) {
          base[98304 + y * 384 + x] = 96;
          base[98304 + y * 384 + x + 1] = 160;
        }
      if (badPixels)
        base[0] = 0;
      if (wrongIova)
        b[2] += 4096;
      std::vector<std::vector<uint32_t>> ps = {packet(0x01000009, 0, 2, 5, b), packet(0x03000162, 0, 2, 1, {1})};
      if (corrupt)
        ps.push_back(packet(0x06000002, 2, 2, 0));
      if (sessionError)
        ps.push_back(packet(0x04000001, 4, 0, 0));
      if (systemError)
        ps.push_back(packet(0x05000001, 8, 0, 0));
      reply(ps);
      if (!dropLast) {
        b = raws.front();
        b[10] = 0x10000000;
        reply({packet(0x01000009, 0, 2, 5, b)});
      }
      return 0;
    }
    if (type == 0x01000006) {
      CHECK(flags == 7);
      ++stops;
      if (!dropStop)
        reply({packet(type, 1, port, 0)});
      return 0;
    }
    if (type == 0x01000004) {
      CHECK(flags == 7 && !port);
      ++closes;
      if (!dropClose) {
        reply({packet(type, 1, 0, 0)});
        if (late)
          reply({packet(0x03000162, 0, 2, 1, {1})});
      }
      return 0;
    }
    // Verify every payload category of the pinned property/subscription contract independently of the implementation's builder.
    if (type == 0x0100000b) {
      CHECK(flags == 3 && payload == 11 && (p[8] == 1 || p[8] == 2));
      subscriptions.emplace_back(p+8,p+p[0]/4);
    } else {
      CHECK(type > 0x03000000 && type < 0x03ffffff && flags == 0);
      properties.push_back({type,port,payload,p[8],p[0]>=40?p[9]:0});
    }
    return 0;
  }
};
static void layoutTests()
{
  Layout l;
  CHECK(l.init());
  CHECK(l.used == 141 && l.bytes == 56623104 && l.bytes < kArenaBytes);
  CHECK(scratchBytes(1, 4) == 172544 && scratchBytes(1, 64) == 2753024);
  std::vector<uint64_t> words(vsmmu::kSessionTablePages * 512), pages(kArenaPages), uc(vsmmu::kUcPages);
  for (unsigned i = 0; i < pages.size(); ++i)
    pages[i] = 0x900000000ull + uint64_t((i * 7907) % kArenaPages) * 4096;
  for (unsigned i = 0; i < uc.size(); ++i)
    uc[i] = 0x8d0000000ull + uint64_t(i) * 4096;
  vsmmu::Tables t{words.data(), 0x8e0000000ull, vsmmu::kSessionTablePages};
  CHECK(vsmmu::build(t, uc.data(), unsigned(uc.size()), 0x8e1000000ull));
  CHECK(buildArena(t, l, pages.data()));
  for (unsigned i = 0; i < l.used; ++i) {
    const auto &b = l.buf[i];
    CHECK(vsmmu::walk(t, b.iova() + b.capacity - 1) == pages[(b.off + b.capacity - 1) / 4096] + ((b.capacity - 1) & 4095));
    CHECK(vsmmu::walk(t, b.iova() + ((b.capacity + 16383) & ~16383u)) == 0);
  }
  CHECK(vsmmu::walk(t, kArenaBase - 1) == 0x8e1000fffull);
  CHECK(vsmmu::walk(t, kArenaBase + kArenaBytes) == 0);
  auto original = pages[0];
  pages[0] = 1;
  CHECK(!buildArena(t, l, pages.data()));
  pages[0] = original;
  // Capacity, geometry and count boundaries fail before any DMA use.
  Psc p;
  CHECK(p.supported());
  p.n = 0;
  CHECK(!p.supported());
  p.n = 65;
  CHECK(!p.supported());
  p.n = 64;
  CHECK(p.supported());
  p.crop[1] = 1;
  CHECK(!p.supported());
}
static void happy(unsigned n)
{
  Peer p;
  p.n = n;
  p.readonly = true;
  Layout l;
  CHECK(l.init());
  SelfTest<Peer> s(p, l);
  CHECK(s.run() == 0);
  CHECK(!s.r.error && !s.r.close && s.r.closed && s.r.checksum && s.r.decoded == 1);
  CHECK(s.r.etb == 1 && s.r.ebd == 1 && s.r.psc == 1 && s.r.releases == 5 && s.r.last && s.r.drainAck);
  CHECK(p.partial == 2 && p.dpbs == n && p.inputCount == 1 && p.stops == 2 && p.closes == 1 && p.hw.clock < 5000000);
  CHECK(p.comvCounts == std::vector<uint32_t>({4, n}));
  CHECK(p.comvSizes == std::vector<uint32_t>({172544, 43008 * n + 512}));
  CHECK(l.find(Raw, n < 2 ? 1 : n - 1)->readonly);
  CHECK(l.find(Bin, 0)->released && l.find(Bin, 1)->queued);
  CHECK(l.find(Bin, 0)->iova() != l.find(Bin, 1)->iova());
  for (unsigned i = 0; i < p.headers.size(); ++i) {
    CHECK(p.headers[i] == i);
    CHECK(p.packets[i] == i);
  }
}
static void negative(unsigned kind)
{
  Peer p;
  Layout l;
  CHECK(l.init());
  switch (kind) {
  case 0:
    p.dropPsc = true;
    break;
  case 1:
    p.badPixels = true;
    break;
  case 2:
    p.n = 65;
    break;
  case 3:
    p.badPsc = true;
    break;
  case 4:
    p.badRelease = true;
    break;
  case 5:
    p.dropClose = true;
    break;
  case 6:
    p.dropStop = true;
    break;
  case 7:
    p.dropLast = true;
    break;
  case 8:
    p.wrongTimestamp = true;
    break;
  case 9:
    p.wrongIova = true;
    break;
  case 10:
    p.corrupt = true;
    break;
  case 11:
    p.sessionError = true;
    break;
  case 12:
    p.ops.fault = true;
    break;
  case 13:
    p.ops.watchdog = true;
    break;
  case 14:
    p.late = true;
    break;
  case 15:
    p.systemError = true;
    break;
  case 16:
    p.dropDrainAck = true;
    break;
  }
  SelfTest<Peer> s(p, l);
  CHECK(s.run() != 0);
  CHECK(s.r.error && p.hw.clock <= 8000000);
  CHECK(!s.r.checksum || kind == 5 || kind == 6 || kind == 7 || kind == 14 || kind == 16);
  // Failure never frees a buffer/arena, even after an unacknowledged STOP/CLOSE. Only ownership flags change on RELEASE_DONE.
  CHECK(p.dma.arena == p.arena.data());
  CHECK(l.find(Bitstream, 0)->capacity == 7077888);
  CHECK(p.hw.log.find("V4 PSC-raw resolution ") != std::string::npos);
  if (kind == 11)
    CHECK(s.r.errorType == 0x04000001);
  if (kind == 15)
    CHECK(s.r.error == kErrSysError);
  if (kind == 4)
    CHECK(l.find(Bin, 0)->pendingRelease && !l.find(Bin, 0)->released);
  if (kind == 5)
    CHECK(!s.r.closed && s.r.close != 0);
}
static void malformed()
{
  Peer p;
  Layout l;
  CHECK(l.init());
  SelfTest<Peer> s(p, l);
  s.deadline = 100000;
  auto pkt = Peer::packet(0x03000162, 0, 2, 1, {1});
  std::vector<uint32_t> b = {68, kSessionId, 0, 0, 0, 0, 0, 1};
  b.insert(b.end(), pkt.begin(), pkt.end());
  CHECK(s.validate(b.data(), 68));
  for (unsigned i = 0; i < 68; ++i)
    CHECK(!s.validate(b.data(), i));
  auto bad = b;
  bad[7] = 0;
  CHECK(!s.validate(bad.data(), 68));
  bad = b;
  bad[8] = 35;
  CHECK(!s.validate(bad.data(), 68));
  bad = b;
  bad[11] = 13;
  CHECK(!s.validate(bad.data(), 68));
  bad = b;
  bad[12] = 3;
  CHECK(!s.validate(bad.data(), 68));
  bad = b;
  bad[1] = 42;
  CHECK(s.receive(bad.data(), 68) != 0);
  // Wrong bank/offset/session, duplicate buffer return and unsanctioned release cannot mutate a live entry.
  Peer q;
  SelfTest<Peer> u(q, l);
  u.deadline = 100000;
  u.rawStarted = true;
  auto &raw = *l.find(Raw, 0);
  raw.queued = true;
  std::vector<uint32_t> v = {2, 0, raw.iova(), 0, 0, 147456, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
  auto bp = Peer::packet(0x01000009, 0, 2, 5, v);
  std::vector<uint32_t> bb = {128, kSessionId, 0, 0, 0, 0, 0, 1};
  bb.insert(bb.end(), bp.begin(), bp.end());
  CHECK(u.receive(bb.data(), 128) != 0 && !raw.released);
}
static void nativeTests()
{
  // Unlike the fixed V4 fixture, the native worker consumes a copied, normalized
  // external AU and correlates both words of an arbitrary caller token. Holding
  // the descriptor does not STOP/CLOSE or change any DMA ownership flags.
  for (uint64_t token : {0x1020304050607080ull, 0x55ull}) {
    Peer p; p.n = 9;
    uint8_t sample[252], normalized[287];
    vtwire::put32(sample, 248); memcpy(sample + 4, kAccessUnit + 38, 248);
    CHECK(vtcontract::normalize(sample, sizeof sample, normalized, sizeof normalized) == sizeof normalized);
    p.expectedInput.assign(normalized, normalized + sizeof normalized); p.expectedToken = token;
    Layout l; CHECK(l.init()); SelfTest<Peer> tx(p, l);
    CHECK(tx.nativeInput(0x56351234, normalized, sizeof normalized, token));
    CHECK(tx.transaction() == 0 && tx.r.decoded == 1 && tx.r.drainAck && tx.r.last);
    CHECK(p.activeSid == 0x56351234 && p.stops == 0 && p.closes == 0);
    CHECK(tx.r.rawIndex < 9 && tx.r.rawBytes == kRawBytes);
    CHECK(!tx.nativeInput(0x56350002, normalized, sizeof normalized, token + 1));
    CHECK(tx.shutdown() == 0 && p.closes == 1 && p.stops == 2);
    CHECK(p.dma.arena == p.arena.data()); // only the PAS/domain owner may free it
  }
  // Descriptor loan/worker lifetime and failed-quiesce fencing: neither close
  // while decoding nor close while a consumer holds RAW can free the owner.
  native::State st; uint32_t sid = 0, other = 0;
  CHECK(st.acquire(&sid) == 0 && sid != kSessionId);
  CHECK(st.acquire(&other) == -16 && st.begin(sid + 1, 44) == -22);
  CHECK(st.begin(sid, 0xdeadbeef01234567ull) == 0);
  CHECK(st.closing(sid) == -16); st.decoded(0);
  CHECK(st.closing(sid) == -16 && st.begin(sid, 8) == -45);
  CHECK(st.release(sid, 7) == -22 && st.release(sid, 0xdeadbeef01234567ull) == 0);
  CHECK(st.release(sid, 0xdeadbeef01234567ull) == -22 && st.finish(sid) == 0);
  CHECK(st.closing(sid) == 0); st.closed(false);
  const auto old = sid; CHECK(st.acquire(&sid) == 0 && sid != old);
  CHECK(st.begin(sid, 8) == 0); st.decoded(-5);
  CHECK(st.finish(sid) == -5 && st.closing(sid) == 0); st.closed(true);
  CHECK(st.phase == native::Phase::Poisoned && st.acquire(&sid) == -16);
  native::State preInput; CHECK(preInput.acquire(&sid) == 0 && preInput.closing(sid) == 0);
  preInput.closed(false); CHECK(preInput.acquire(&other) == 0 && other != sid);
}
static void generalGeometryTests() {
  Peer p;Layout layout;CHECK(layout.init(1920,1088));SelfTest<Peer> tx(p,layout);
  tx.streaming=true;tx.activeWidth=1920;tx.activeHeight=1080;
  tx.expectedProfile=4;tx.expectedLevel=11;
  tx.psc.resolution=0x07800440;tx.psc.crop[0]=0;tx.psc.crop[1]=8;
  tx.psc.returned=0x2a;
  tx.psc.poc=tx.expectedPoc;tx.psc.profile=4;tx.psc.level=11;tx.psc.n=16;
  CHECK(tx.pscSupported());
  tx.psc.returned=0x0a;CHECK(!tx.pscSupported());tx.psc.returned=0x2a;
  tx.psc.n=17;CHECK(!tx.pscSupported());tx.psc.n=16;
  tx.psc.crop[1]=7;CHECK(!tx.pscSupported());tx.psc.crop[1]=10;CHECK(!tx.pscSupported());
  tx.psc.crop[1]=8;CHECK(tx.adoptPscGeometry());
  tx.psc.resolution=0x07800438;tx.psc.crop[1]=0;
  CHECK(tx.pscSupported() && tx.adoptPscGeometry());
  CHECK(layout.geometry.stride==1920 && layout.geometry.scanlines==1088 && layout.rawBytes()==3133440);
  CHECK(layout.find(Raw,15)->size==3133440 && layout.find(Dpb,15)->size==3153920);
  std::vector<uint64_t> words(vsmmu::kSessionTablePages*512),pages(kArenaPages),uc(vsmmu::kUcPages);
  for(unsigned i=0;i<pages.size();++i)pages[i]=0x900000000ull+4096ull*i;
  for(unsigned i=0;i<uc.size();++i)uc[i]=0x8d0000000ull+4096ull*i;
  vsmmu::Tables tables{words.data(),0x8e0000000ull,vsmmu::kSessionTablePages};
  CHECK(vsmmu::build(tables,uc.data(),unsigned(uc.size()),0x8e1000000ull));
  CHECK(buildArena(tables,layout,pages.data()));
  // Exercise the newly added L3 tables beyond the old 64 MiB aperture.
  const auto *last=layout.find(Raw,15);CHECK(last && last->off>64*1024*1024);
  CHECK(vsmmu::walk(tables,last->iova()+last->capacity-1)==pages[(last->off+last->capacity-1)/4096]+((last->capacity-1)&4095));
  CHECK(!vsmmu::walk(tables,last->iova()+((last->capacity+16383)&~16383u)));
  CHECK(!vsmmu::walk(tables,kArenaBase+kArenaBytes));
}
static void hevcProtocolTests() {
  // The independent peer returns HEVC-only PSC fields and firmware N, then an
  // owned RAW without DRAIN. It is a protocol test, not firmware decode proof.
  for(uint32_t depth : {8u,10u})for(uint32_t kind : {1u,0x10u,0x20u,0x40u}) {
    Peer p;p.hevc=true;p.depth=depth;p.hevcPicture=kind;p.firmwareMinimum=6;
    CHECK(p.hg.configure(320,256,depth));
    Layout layout;CHECK(layout.initHevc(320,256,depth));SelfTest<Peer> tx(p,layout);
    for(unsigned i=0;i<layout.maxN();++i) {
      const auto &raw=*layout.find(Raw,i);
      std::fill(p.arena.begin()+raw.off,p.arena.begin()+raw.off+raw.capacity,0x5c);
    }
    tx.streaming=true;tx.activeWidth=320;tx.activeHeight=256;tx.expectedDepth=depth;
    tx.expectedProfile=depth==10?2:0;tx.expectedLevel=4;tx.expectedTier=0;
    tx.psc.resolution=0x01400100;tx.psc.depth=(depth<<16)|depth;tx.psc.profile=tx.expectedProfile;
    tx.psc.level=4;tx.psc.tier=0;tx.initialPsc=tx.psc;
    tx.sessionId=0x5635abcd;tx.checkGolden=false;tx.accessUnit=nullptr;tx.accessUnitBytes=0;
    CHECK(tx.prepareInput()==0 && p.inputCount==0 && !p.raw && tx.ledger.pending==0);
    p.hw.clock+=10000000;
    CHECK(tx.pump()==0 && tx.deadline==UINT64_MAX);
    CHECK(tx.nativeInput(0x5635abcd,kAccessUnit,sizeof kAccessUnit,kTimestamp));
    CHECK(tx.transaction()==0 && tx.pump()==0);
    bool pristine=true;
    for(unsigned i=0;i<layout.maxN();++i) {
      const auto &raw=*layout.find(Raw,i);
      for(unsigned j=0;j<raw.capacity;++j)pristine&=p.arena[raw.off+j]==0x5c;
    }
    CHECK(pristine); // this peer returns FBD metadata without writing pixels
    CHECK(tx.psc.n==6 && tx.psc.returned==0x7ae && tx.r.psc==1 && tx.r.decoded==1);
    CHECK(p.partial==0 && p.releaseCount==4 && p.dpbs==6 && p.raws.size()==6);
    CHECK(p.comvCounts==std::vector<uint32_t>({4,6}));
    SelfTest<Peer>::OutputFrame output;CHECK(tx.popOutput(output));
    CHECK(output.token==kTimestamp && output.bytes==((kind&0x40)?0:p.hg.raw));
    CHECK(tx.releaseOutput(output.index,output.token)==0);
    bool codec=false,format=false,stride=false,tier=false;
    for(const auto &v:p.properties) {
      if(v[0]==0x03000100)codec=v[3]==3 && v[2]==9;
      if(v[0]==0x03000101)format=v[3]==(depth==10?3u:1u);
      if(v[0]==0x03000104)stride=v[3]==((p.hg.stride/(depth==10?2:1))<<16|p.hg.scanlines);
      if(v[0]==kHevcTier)tier=true;
      CHECK(v[0]!=0x03000120 && v[0]!=0x03000128);
    }
    CHECK(codec&&format&&stride&&tier);
    for(const auto &v:p.subscriptions)for(unsigned i=1;i<v.size();++i)CHECK(v[i]!=0x03000120 && v[i]!=0x03000128);
    CHECK(tx.shutdown()==0 && tx.r.stopRaw && tx.r.stopInput && tx.r.closed);
  }
  // A 4K10 returned PSC may shrink coded2176 to2160 only if all capacities
  // still fit. Missing profile/tier/level/crop/minimum is never a seeded pass.
  Peer p;Layout layout;CHECK(layout.initHevc(3840,2176,10));SelfTest<Peer> tx(p,layout);
  tx.streaming=true;tx.activeWidth=3840;tx.activeHeight=2160;tx.expectedDepth=10;tx.expectedProfile=2;tx.expectedLevel=11;tx.expectedTier=1;
  tx.psc.resolution=(3840u<<16)|2160;tx.psc.depth=0x000a000a;tx.psc.profile=2;tx.psc.level=11;tx.psc.tier=1;tx.psc.n=10;tx.psc.returned=0x7ae;
  CHECK(tx.pscSupported());
  for(unsigned bit:{1u,2u,3u,5u,7u,8u,10u}) {tx.psc.returned^=1u<<bit;CHECK(!tx.pscSupported());tx.psc.returned^=1u<<bit;}
  tx.psc.n=11;CHECK(!tx.pscSupported());tx.psc.n=10;
  tx.psc.depth=0x00080008;CHECK(!tx.pscSupported());tx.psc.depth=0x000a000a;
  tx.psc.tier=0;CHECK(!tx.pscSupported());tx.psc.tier=1;
  CHECK(tx.adoptPscGeometry() && tx.rawPixelStride()==3840 && tx.rawStride()==7680 && layout.rawBytes()==25067520);
  // Rejected PSC still preserves both crop words and the full returned mask
  // before the first-error line; this is the evidence needed on the first boot.
  Peer bad;Layout small;CHECK(small.initHevc(320,256,8));SelfTest<Peer> rejected(bad,small);
  rejected.streaming=true;rejected.activeWidth=320;rejected.activeHeight=256;
  rejected.r.stage=FirstPsc;rejected.deadline=UINT64_MAX;
  bad.reply({Peer::packet(Settings,0,1,0),Peer::packet(0x03000103,0,1,1,{0x01400100}),
    Peer::packet(0x03000105,0,1,13,{0,2}),Peer::packet(0x0300010f,0,1,1,{0x00080008}),
    Peer::packet(0x03000124,0,1,1,{6}),Peer::packet(0x03000107,0,1,9,{0}),
    Peer::packet(0x03000108,0,1,9,{4}),Peer::packet(kHevcTier,0,1,9,{0})},rejected.sessionId);
  CHECK(rejected.pump()==kErrHfi && rejected.r.psc==0);
  const auto observed=bad.hw.log.find("V9 HEVC PSC observed port 1 returned 000005ae res 01400100 crop 00000000/00000002 depth 00080008 N 6 profile 0 level 4 tier 0");
  CHECK(observed!=std::string::npos && observed<bad.hw.log.find("V4 first-error"));
}
int main()
{
  hevcProtocolTests();
  generalGeometryTests();
  nativeTests();
  layoutTests();
  for (unsigned n : {1u, 4u, 7u, 64u})
    happy(n);
  for (unsigned i = 0; i < 17; ++i)
    negative(i);
  malformed();
  std::printf("V4 host: %s (scripted firmware, never hardware evidence)\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
