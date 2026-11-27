// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Runs the VT selector/worker, resident SMMU remapper and E1 HFI transaction.
// Apple transport, native decode completions and Iris registers are scripted.
#include "vt_cancel_kernel_shim.h"
#include "HEVCFixture.h"
#include "../VideoBringup.h"
#include <algorithm>
#include <array>
#include <deque>
#include <map>

#define CHECK(x) assert(x)
#include "VideoEncodePeer.h"
#undef CHECK

#define PIANO_VIDEO_VT_HOST_TEST
#include "../vt/PianoVideoVTHost.cpp"
#undef PIANO_VIDEO_VT_HOST_TEST

using namespace pianovideo;

namespace {
struct ResidentHw {
  std::map<uint64_t, uint32_t> registers;
  std::vector<uint64_t> cleanTable, deviceTable;
  uint64_t bank = 0;
  unsigned bankSyncs = 0, globalSyncs = 0;

  uint32_t rd(uint64_t pa) {
    const auto it = registers.find(pa);
    return it == registers.end() ? 0 : it->second;
  }
  void wr(uint64_t pa, uint32_t value) {
    if (pa == bank + vsmmu::kTlbiAll || pa == kSmmu + vsmmu::kTlbiAllNsnh) return;
    if (pa == bank + vsmmu::kTlbSync) {
      assert(!cleanTable.empty());
      ++bankSyncs;
    } else if (pa == kSmmu + vsmmu::kTlbGsync) {
      assert(bankSyncs > globalSyncs);
      deviceTable = cleanTable;
      ++globalSyncs;
    } else {
      registers[pa] = value;
    }
  }
  void cleanToPoc(const uint64_t *p, size_t bytes) { cleanTable.assign(p, p + bytes / sizeof(*p)); }
  void delayUs(uint32_t) {}
  void note(const char *, ...) {}
};

struct Resident {
  ResidentHw hw;
  Bringup<ResidentHw> core{hw};
  stream::Owner owner;
  std::vector<uint64_t> words, pages, avcTable;
  std::vector<uint32_t> queues;
  std::vector<uint8_t> arena;
  std::map<uint64_t, uint32_t> residentRegisters;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<uint64_t> pendingDecode;
  unsigned submits = 0, closes = 0, pictures = 0, loans = 0;
  bool cancelled = false;

  Resident() : words(vsmmu::kSessionTablePages * 512), pages(session::kAvcArenaBytes / 4096), queues(kUcBytes / 4) {
    // Host bytes are needed only for E1. All 256 MiB of the resident aperture
    // have modeled physical page metadata, including pages that must fault.
    session::Layout encLayout;
    enc::Geometry geometry;
    assert(geometry.configure(640, 480) && enc::buildLayout(encLayout, geometry));
    arena.resize(encLayout.bytes);
    core.dma.arena = arena.data();
    core.dma.arenaPages = pages.data();
    core.dmaHeld = core.firstHfi = core.fst.loaded = true;
    core.opt.level = 6;
    auto &p = core.pst;
    p.on = p.railsUp = p.busOn = p.axi0 = p.axi1 = true;
    p.ctrlGdsc = p.ctrlFreerun = p.ctrlClk = p.codecGdsc = p.codecFreerun = p.codecClk = true;
    for (size_t i = 0; i < pages.size(); ++i)
      pages[i] = kXnuDramBase + 0x10000000 + ((7907 * i) % pages.size()) * 4096;
    for (unsigned i = 0; i < vsmmu::kUcPages; ++i)
      core.dma.pages[i] = kXnuDramBase + i * 4096;
    core.dma.scratch = kXnuDramBase + 0x3000000;
    core.tables = {words.data(), kXnuDramBase + 0x2000000, vsmmu::kSessionTablePages};
    assert(vsmmu::build(core.tables, core.dma.pages, vsmmu::kUcPages, core.dma.scratch));
    core.mem.uc = queues.data();
    hfi::initQueues(core.mem);
    hw.registers = {{kSmmu + vsmmu::kIdr0, vsmmu::kIdr0S1ts | 8},
                    {kSmmu + vsmmu::kIdr1, 0x60000008},
                    {kSmmu + vsmmu::kIdr2, vsmmu::kIdr2Ptfs4k | (2 << 4)},
                    {kSmmu + vsmmu::smrOffset(0), 0x80000800},
                    {kSmmu + 128 * 4096 + vsmmu::kSctlr, vsmmu::kSctlrM}};
    core.plan = vsmmu::plan([&](uint32_t off) { return hw.rd(kSmmu + off); });
    assert(!core.plan.error);
    hw.bank = kSmmu + core.plan.cb();
    hw.cleanToPoc(words.data(), words.size() * sizeof(words[0]));
    assert(!vsmmu::apply(core.plan, core.tables.pa, core.ops, core.routed));
    residentRegisters = hw.registers;
  }

  int acquire(uint32_t *sid) {
    std::lock_guard lock(mutex);
    return owner.acquire(sid);
  }

  void remap() {
    const auto bankSyncs = hw.bankSyncs, globalSyncs = hw.globalSyncs;
    assert(!core.prepareResidentSession());
    assert(hw.bankSyncs == bankSyncs + 1 && hw.globalSyncs == globalSyncs + 1);
    assert(hw.registers == residentRegisters && words == hw.deviceTable);
    vsmmu::Tables device = core.tables;
    device.cpu = hw.deviceTable.data();
    for (unsigned i = 0; i < vsmmu::kWindowPages; ++i) {
      const uint64_t pa = i < vsmmu::kUcPages ? core.dma.pages[i] : core.dma.scratch;
      assert(vsmmu::walk(device, kIovaBase + 4096 * i) == pa);
    }
    std::vector<uint64_t> expected(pages.size());
    for (unsigned slot = 0; slot < core.layout.used; ++slot) {
      const auto &b = core.layout.buf[slot];
      for (unsigned off = 0; off < b.capacity; off += 4096) {
        const unsigned page = (b.off + off) / 4096;
        assert(!expected.at(page));
        expected[page] = pages[page];
      }
    }
    // Every former slot and guard page outside the new layout must fault.
    for (size_t i = 0; i < pages.size(); ++i)
      assert(vsmmu::walk(device, session::kArenaBase + 4096 * i) == expected[i]);
    assert(!vsmmu::walk(device, session::kArenaBase + 4096 * pages.size()));
  }

  void prepareDecode(uint32_t sid, const codec::Config &format) {
    assert(owner.owns(sid) && !core.encoder && pendingDecode.empty() && !loans && !pictures);
    submits = 0;
    cancelled = false;
    core.hevc = format.isHevc();
    core.bitdepth = format.depth;
    core.avcWidth = format.s.codedWidth;
    core.avcHeight = format.s.codedHeight;
    remap();
    if (format.isHevc()) {
      assert(core.layout.hevc && core.layout.hevcGeometry.bitDepth == 10 && words != avcTable);
    } else if (avcTable.empty()) {
      avcTable = words;
    } else {
      assert(words == avcTable);
    }
  }

  void waitForSubmits(unsigned count) {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return submits == count; }));
  }
  void waitForClose(unsigned count) {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return closes == count; }));
  }
  void returnPicture() {
    std::lock_guard lock(mutex);
    assert(!pictures && !pendingDecode.empty());
    ++pictures;
  }

  void encode(const char *bootArgs, bool dropClose = false) {
    if (!enc::bootEnabled(bootArgs)) return;
    const unsigned serial = owner.serial;
    const auto decodeTable = words;
    uint32_t sid = 0;
    assert(!acquire(&sid) && sid && owner.serial == serial + 1);
    core.encoder = true;
    assert(core.encGeometry.configure(640, 480));
    remap();
    assert(words != decodeTable);
    auto *arp = core.layout.find(enc::BufArp, 0);
    assert(arp && vsmmu::walk(core.tables, arp->iova()) == pages[arp->off / 4096]);
    const auto encodeTable = words;
    const unsigned syncs = hw.globalSyncs;

    encfixture::Peer peer;
    peer.bind(core.encGeometry, core.layout, core.dma.arena, core.mem);
    peer.rawBatch = 4;
    peer.reorderFirstBatch = peer.zeroRawTimestamps = peer.reportArpOnStop = true;
    peer.dropCloseAck = dropClose;
    enc::EncodeSelfTest tx(peer, core.layout);
    tx.g = core.encGeometry;
    tx.set.frames = 8;
    tx.sessionId = sid;
    std::array<uint8_t, 16384> output{};
    tx.stream = output.data();
    tx.streamCap = output.size();
    const int rc = tx.run();
    assert(peer.peakRaw == 4 && peer.rawSlots == 0xf && peer.patternOk == 8 && !peer.patternBad);
    assert(tx.r.etb == 8 && tx.r.ebd == 8 && tx.r.fbd == 8);
    assert(tx.frames[0].ebdUs < tx.frames[2].ebdUs && tx.frames[2].ebdUs < tx.frames[1].ebdUs &&
           tx.frames[1].ebdUs < tx.frames[3].ebdUs);
    assert(tx.r.stopRaw && tx.r.stopBitstream && tx.r.drainAck && tx.r.last && tx.closeSent);
    assert(words == encodeTable && hw.deviceTable == words && hw.globalSyncs == syncs);

    core.encoder = false;
    assert(!owner.closing(sid, false));
    owner.closed(rc && core.dmaHeld, rc);
    if (dropClose) {
      assert(rc == kErrHfiTimeout && !tx.r.closed && arp->queued && !peer.arp.empty());
      assert(peer.activeSid == sid && peer.hw.clock >= 1000000 && peer.hw.clock < 2000000);
      assert(owner.phase == stream::Owner::Poisoned && core.dmaHeld);
      uint32_t next = 0;
      assert(acquire(&next) == -16 && !next && words == encodeTable && hw.registers == residentRegisters);
    } else {
      assert(!rc && tx.r.pass() && !arp->queued && peer.arp.empty() && owner.phase == stream::Owner::Idle);
    }
  }
};

Resident *resident = nullptr;
}

extern "C" {
bool piano_video_native_ready() { return true; }
int piano_video_native_open(uint32_t *sid) { return resident->acquire(sid); }
int piano_video_native_prepare(uint32_t sid, const codec::Config *cfg) {
  resident->prepareDecode(sid, *cfg);
  return 0;
}
int piano_video_native_submit(uint32_t sid, const uint8_t *p, uint32_t n, uint64_t token, const codec::Config *cfg) {
  auto &r = *resident;
  std::lock_guard lock(r.mutex);
  assert(r.owner.owns(sid) && !r.cancelled && codec::normalized(p, n, *cfg));
  if (!r.submits) assert(r.owner.start(sid));
  r.pendingDecode.push_back(token);
  ++r.submits;
  r.cv.notify_one();
  return 0;
}
int piano_video_native_poll(uint32_t sid, native::Frame *frame) {
  auto &r = *resident;
  std::lock_guard lock(r.mutex);
  assert(r.owner.owns(sid));
  if (!r.pictures || r.pendingDecode.empty() || frame->memory) return 1;
  --r.pictures;
  ++r.loans;
  frame->memory = new IOMemoryDescriptor;
  frame->token = r.pendingDecode.front();
  frame->noOutput = true;
  r.pendingDecode.pop_front();
  return 0;
}
int piano_video_native_frame_release(uint32_t sid, native::Frame *frame) {
  auto &r = *resident;
  std::lock_guard lock(r.mutex);
  assert(r.owner.owns(sid) && r.loans == 1 && frame->memory);
  --r.loans;
  frame->memory->release();
  *frame = {};
  return 0;
}
void piano_video_native_cancel(uint32_t sid) {
  std::lock_guard lock(resident->mutex);
  assert(resident->owner.owns(sid));
  resident->cancelled = true;
}
int piano_video_native_close(uint32_t sid) {
  auto &r = *resident;
  std::lock_guard lock(r.mutex);
  assert(r.cancelled && !r.loans && !r.pictures && !r.owner.closing(sid, false));
  r.pendingDecode.clear();
  r.owner.closed(false);
  ++r.closes;
  r.cv.notify_one();
  return 0;
}
}

namespace {
using Bytes = std::vector<uint8_t>;
using Uuid = std::array<uint8_t, 16>;

void atom(Bytes &out, uint32_t kind, const void *data, uint32_t size, uint32_t key = 0, uint32_t type = 0) {
  const auto at = out.size();
  const uint32_t prefix = key ? 16 : 8;
  out.resize(at + prefix + size);
  auto *p = out.data() + at;
  put32(p, prefix + size);
  put32(p + 4, kind);
  if (key) {
    put32(p + 8, key);
    put32(p + 12, type);
  }
  std::memcpy(p + prefix, data, size);
}

struct DecodeFixture {
  uint32_t kind, width, height;
  Bytes config, sample;
  explicit DecodeFixture(bool hevc) : kind(hevc ? cc("hvc1") : cc("avc1")),
    width(hevc ? 1920 : 320), height(hevc ? 1080 : 240) {
    if (hevc) {
      const hevcfixture::Fixture h(10);
      config = h.hvcc;
      hevcfixture::append(sample, h.slice());
    } else {
      const auto *au = session::kAccessUnit;
      // The existing AVC fixture is Annex B SPS, PPS and a single IDR.
      config = {1, 66, 0xc0, 13, 0xff, 0xe1, 0, 22};
      config.insert(config.end(), au + 4, au + 26);
      config.insert(config.end(), {1, 0, 5});
      config.insert(config.end(), au + 30, au + 35);
      hevcfixture::append(sample, Bytes(au + 38, au + sizeof(session::kAccessUnit)));
    }
  }

  Bytes format() const {
    Bytes fds(86);
    put32(fds.data() + 4, kind);
    fds[32] = width >> 8;
    fds[33] = width;
    fds[34] = height >> 8;
    fds[35] = height;
    atom(fds, kind == cc("hvc1") ? cc("hvcC") : cc("avcC"), config.data(), config.size());
    put32(fds.data(), fds.size());
    return fds;
  }

  Bytes sampleBuffer() const {
    const uint32_t count = 1, bytes = sample.size();
    uint8_t timing[72] = {};
    timing[32] = timing[36] = 1;
    Bytes body, out;
    atom(body, cc("nsmp"), &count, sizeof count);
    atom(body, cc("ssiz"), &bytes, sizeof bytes);
    atom(body, cc("stia"), timing, sizeof timing);
    atom(body, cc("sdat"), sample.data(), bytes);
    atom(out, cc("sbuf"), body.data(), body.size());
    return out;
  }
};

struct Message {
  Header header;
  int32_t error = 0, info = 0;
  int64_t token = 0;
  explicit Message(const Bytes &b) {
    assert(vtwire::header(b.data(), b.size(), header));
    if (header.type == cc("Dpba")) return;
    assert(paramS32(b.data(), b.size(), cc(",err"), error));
    if (header.type == cc("Demt")) {
      Param surface;
      assert(header.flags == FlagHostRequest && b.size() == 112);
      assert(paramS64(b.data(), b.size(), cc(",frm"), token));
      assert(paramS32(b.data(), b.size(), cc(",inf"), info) && info == 3);
      assert(!param(b.data(), b.size(), cc(",uui"), surface));
    } else {
      assert(header.flags == FlagReply);
    }
  }
};

struct Transport {
  std::mutex queueLock;
  std::condition_variable cv;
  std::deque<Bytes> queue;
  Client *client = nullptr;
  std::thread worker;
  Uuid uuid = {};
  uint64_t lastHostRequest = 0;

  Transport() {
    client = static_cast<Client *>(droidvm_vthost_client_open(this, enqueue, nullptr, 1));
    assert(client);
  }
  ~Transport() {
    assert(!worker.joinable() && !client->session && empty());
    droidvm_vthost_client_close(client);
  }
  static bool enqueue(void *context, const uint8_t *b, uint32_t n) {
    auto &t = *static_cast<Transport *>(context);
    std::lock_guard lock(t.queueLock);
    t.queue.emplace_back(b, b + n);
    t.cv.notify_one();
    return true;
  }
  bool empty() {
    std::lock_guard lock(queueLock);
    return queue.empty();
  }
  Message nextMessage() {
    std::unique_lock lock(queueLock);
    assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return !queue.empty(); }));
    const Message m(queue.front());
    queue.pop_front();
    assert(!std::memcmp(m.header.uuid, uuid.data(), uuid.size()));
    if (m.header.type == cc("Demt")) {
      assert(m.header.request > lastHostRequest);
      lastHostRequest = m.header.request;
    }
    return m;
  }
  Message next(uint32_t type) {
    auto m = nextMessage();
    assert(m.header.type == type);
    return m;
  }
  void reply(uint32_t type, uint64_t request, int error) {
    const auto m = next(type);
    assert(m.header.request == request && m.error == error);
  }
  int send(const Bytes &b) { return droidvm_vthost_message(client, b.data(), b.size(), nullptr, 0); }
  void request(uint32_t type, uint64_t id, uint32_t kind = 0, const Bytes &format = {}) {
    uint8_t b[128];
    Writer w(b, sizeof b);
    w.header(type, FlagRequest, uuid.data(), id);
    if (kind) w.s32(cc(",cod"), kind);
    Bytes message(b, b + w.size());
    if (!format.empty()) atom(message, cc("para"), format.data(), format.size(), cc(",fds"), cc("=fds"));
    assert(!send(message));
  }
  void start(const DecodeFixture &fixture) {
    request(cc("+dec"), 1, fixture.kind);
    reply(cc("+dec"), 1, 0);
    kernelThreadJoinTarget = &worker;
    request(cc("dsta"), 2, 0, fixture.format());
    kernelThreadJoinTarget = nullptr;
    next(cc("Dpba"));
    reply(cc("dsta"), 2, 0);
    assert(worker.joinable());
  }
  void frame(uint64_t token, unsigned index, const Bytes &sample) {
    uint8_t b[128];
    Writer w(b, sizeof b);
    w.header(cc("dfrm"), FlagRequest, uuid.data(), 10 + index);
    w.s64(cc(",frm"), token);
    w.u32(cc(",dfl"), 9);
    Bytes message(b, b + w.size());
    atom(message, cc("para"), sample.data(), sample.size(), cc(",sbf"), cc("=sbf"));
    stubAdmissionHash = true;
    assert(!send(message));
    stubAdmissionHash = false;
  }
  int ack(const Header &h) {
    uint8_t b[128];
    Writer w(b, sizeof b);
    w.header(h.type, FlagGuestReply, h.uuid, h.request);
    w.s32(cc(",err"), 0);
    return send(Bytes(b, b + w.size()));
  }
  Header complete(uint64_t token, int error) {
    const auto m = next(cc("Demt"));
    assert(uint64_t(m.token) == token && m.error == error);
    return m.header;
  }
  void joined(int error, unsigned accepted, unsigned retired, const Header &last) {
    reply(cc("dinv"), 90, error);
    auto *s = client->session;
    IOLockLock(s->lock);
    assert(!s->workerRunning && !s->native && !s->invalidateRequest && !s->guest.type);
    assert(s->frames.accepted == accepted && !s->frames.pending);
    assert(s->frames.retired == retired && s->frames.cancelled == accepted - retired);
    for (const auto &in : s->inputs) assert(!in.bytes && !in.reply && !in.deadline);
    IOLockUnlock(s->lock);
    assert(ack(last) == -1);
    request(cc("-dec"), 93);
    worker.join();
    assert(!client->session && empty());
    assert(resident->owner.phase == stream::Owner::Idle && resident->pendingDecode.empty() && !resident->loans);
  }
};

Header cancelEpoch(Transport &vt, const DecodeFixture &fixture, unsigned epoch, unsigned inFlight,
                   bool successInFlight, bool dropTerminal, const Header *stale = nullptr) {
  assert(inFlight >= 2 && inFlight <= stream::Credits);
  const uint64_t base = uint64_t(epoch) << 32;
  const auto sample = fixture.sampleBuffer();
  const auto priorCloses = resident->closes;
  testClockMs = 1000 * (epoch + 1);
  vt.start(fixture);
  const unsigned retired = successInFlight ? 1 : 0;
  const unsigned first = retired + 1, last = retired + inFlight;
  Header success{};
  if (retired) {
    vt.frame(base + 1, 1, sample);
    vt.reply(cc("dfrm"), 11, 0);
    resident->waitForSubmits(1);
    resident->returnPicture();
    success = vt.complete(base + 1, 0);
    assert(!vt.ack(success));
    vt.request(cc("dfin"), 8);
    vt.reply(cc("dfin"), 8, 0);
  }
  for (unsigned i = first; i <= last; ++i) vt.frame(base + i, i, sample);
  const unsigned published = retired + std::min(inFlight, stream::PublishedWindow);
  for (unsigned i = first; i <= published; ++i) vt.reply(cc("dfrm"), 10 + i, 0);
  resident->waitForSubmits(published);
  uint32_t busy = 0;
  assert(resident->acquire(&busy) == -16 && !busy);

  if (successInFlight) {
    resident->returnPicture();
    const auto pending = vt.complete(base + first, 0);
    assert(vt.ack(success) == -1);
    success = pending;
  }
  vt.request(cc("dinv"), 90);
  vt.request(cc("dinv"), 91);
  vt.request(cc("+dec"), 92, fixture.kind);
  // Held original admissions and the duplicate-request replies can race on enqueue.
  std::map<uint64_t, int> replies{{91, kBadState}, {92, kUnsupported}};
  for (unsigned i = published + 1; i <= last; ++i) replies.emplace(10 + i, kDecodeError);
  std::deque<Header> terminals;
  if (successInFlight) {
    resident->waitForClose(priorCloses + 1);
    assert(!vt.ack(success));
  }
  for (size_t n = replies.size(); n; --n) {
    // The first terminal can be queued while the duplicate requests are in
    // flight. Keep it unacknowledged as we check the other replies.
    auto m = vt.nextMessage();
    if (m.header.type == cc("Demt")) {
      assert(terminals.empty() && !successInFlight);
      assert(uint64_t(m.token) == base + first && m.error == kDecodeError);
      terminals.push_back(m.header);
      m = vt.nextMessage();
    }
    assert((m.header.type == cc("dfrm") && m.header.request < 90) ||
           (m.header.type == cc("dinv") && m.header.request == 91) ||
           (m.header.type == cc("+dec") && m.header.request == 92));
    const auto it = replies.find(m.header.request);
    assert(it != replies.end() && it->second == m.error);
    replies.erase(it);
  }
  Header previous = success;
  for (unsigned i = first + successInFlight; i <= published; ++i) {
    const auto demt = terminals.empty() ? vt.complete(base + i, kDecodeError) : terminals.front();
    terminals.clear();
    assert(vt.empty() && resident->closes == priorCloses + 1);
    if (previous.request) assert(demt.request == previous.request + 1);
    auto future = demt;
    ++future.request;
    assert(vt.ack(future) == -1);
    if (previous.request) assert(vt.ack(previous) == -1);
    if (stale) assert(vt.ack(*stale) == -1);
    auto *s = vt.client->session;
    IOLockLock(s->lock);
    assert(s->invalidateRequest == 90 && s->invalidateDeadline == testClockMs + 900);
    assert(s->guest.matches(demt.request, demt.type) && s->workerRunning);
    IOLockUnlock(s->lock);
    if (dropTerminal && i == published) {
      // Lose the guest response and run right up to the dinv watchdog.
      testClockMs += 899;
      assert(vt.ack(future) == -1 && vt.empty());
      ++testClockMs;
    } else {
      assert(!vt.ack(demt));
      assert(vt.ack(demt) == -1);
    }
    previous = demt;
  }
  vt.joined(dropTerminal ? kDecodeError : 0, last, retired, previous);
  assert(vt.ack(previous) == -1 && resident->closes == priorCloses + 1 && vt.empty());
  return previous;
}

void handoffs() {
  Resident boot;
  resident = &boot;
  Transport vt;
  const DecodeFixture avc(false), main10(true);
  Header stale = cancelEpoch(vt, avc, 1, stream::Credits, true, false);
  const auto snapshot = boot.words;
  const unsigned serial = boot.owner.serial, syncs = boot.hw.globalSyncs;
  for (const char *args : {"piano-video=6", "piano-video=6 piano-video-enc=0"}) {
    boot.encode(args);
    assert(boot.owner.serial == serial && boot.words == snapshot && boot.hw.globalSyncs == syncs && !boot.core.encoder);
  }
  boot.encode("piano-video=6 piano-video-enc=1");
  stale = cancelEpoch(vt, main10, 2, 7, true, false, &stale);
  stale = cancelEpoch(vt, avc, 3, 6, false, false, &stale);
  for (unsigned epoch = 4; epoch <= 24; ++epoch) {
    const auto &fixture = epoch % 3 == 1 ? main10 : avc;
    // Reuse UUIDs across several epochs, then rotate the transport UUID.
    vt.uuid[0] = epoch / 4;
    stale = cancelEpoch(vt, fixture, epoch, 2 + epoch % 7, epoch % 2, epoch % 5 == 0, &stale);
  }
  assert(boot.words == snapshot && boot.hw.registers == boot.residentRegisters);
}

void quarantine() {
  Resident boot;
  resident = &boot;
  Transport vt;
  const DecodeFixture avc(false), main10(true);
  cancelEpoch(vt, avc, 1, stream::Credits, true, false);
  boot.encode("piano-video=6 piano-video-enc=1", true);
  vt.request(cc("+dec"), 1, main10.kind);
  vt.reply(cc("+dec"), 1, 0);
  const auto words = boot.words;
  const unsigned syncs = boot.hw.globalSyncs;
  vt.request(cc("dsta"), 2, 0, main10.format());
  vt.reply(cc("dsta"), 2, kDecodeError);
  vt.request(cc("-dec"), 93);
  assert(boot.words == words && boot.hw.deviceTable == words && boot.hw.globalSyncs == syncs);
  assert(boot.core.layout.find(enc::BufArp, 0)->queued && boot.core.dmaHeld && boot.owner.phase == stream::Owner::Poisoned);
}
}

int main() {
  handoffs();
  quarantine();
  std::puts("PianoVideo VT cancel/E1 handoff: 25 epochs, bounded Demt timeout, Main10/AVC remaps and ARP quarantine passed");
}
