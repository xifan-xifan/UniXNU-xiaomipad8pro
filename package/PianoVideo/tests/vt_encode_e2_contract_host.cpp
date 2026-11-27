// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer vt_encode_e2_contract_host.cpp -o /tmp/vt_encode_e2_contract_host && /tmp/vt_encode_e2_contract_host
// Deterministic host model for the bounded E2 lifecycle in video-encode-e2-analysis.md.
// Iris, IOSurface, the VT guest and the worker scheduler are scripted. The sample
// model covers AVC framing and the observed sbuf timing/data fields.
#include "../VideoEncode.h"
#include "../VideoStreamState.h"
#include "../vt/PianoVideoVTWire.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {
using namespace pianovideo;
using namespace vtwire;

using Bytes = std::vector<uint8_t>;
using Uuid = std::array<uint8_t, 16>;
constexpr uint32_t kAvc = cc("avc1"), kHevc = cc("hvc1"), kNv12 = cc("420v");
constexpr uint32_t kArenaBytes = 256 * 1024 * 1024;
constexpr uint32_t kTask = 7;
constexpr size_t kFrameCredits = 8;

struct Time {
  int64_t value;
  int32_t scale;
  uint32_t flags = 1; // finite, valid CMTime
  int64_t epoch = 0;
};

std::optional<int64_t> nanoseconds(Time time) {
  constexpr int64_t second = enc::kNsPerSecond;
  if (time.value < 0 || time.scale <= 0 || time.flags != 1 || time.epoch != 0) return {};
  const int64_t seconds = time.value / time.scale;
  const int64_t fraction = (time.value % time.scale) * second / time.scale;
  if (seconds > (INT64_MAX - fraction) / second) return {};
  return seconds * second + fraction;
}

struct Plane {
  size_t offset = 0, stride = 0;
};

struct Surface {
  uint32_t task = kTask, format = kNv12, width = 0, height = 0;
  std::vector<Plane> planes;
  Bytes bytes;
  unsigned pins = 0;
  bool readLocked = false;

  bool valid(uint32_t client, const enc::Geometry &geometry) const {
    if (task != client || format != kNv12 || width != geometry.width || height != geometry.height ||
        planes.size() != 2 || readLocked) return false;
    size_t end = 0;
    for (size_t i = 0; i < planes.size(); ++i) {
      const auto [offset, stride] = planes[i];
      const size_t rows = i == 0 ? height : height / 2;
      if (offset < end || offset > bytes.size() || stride < width || stride > (bytes.size() - offset) / rows)
        return false;
      end = offset + stride * rows;
    }
    return true;
  }

  void copyToRaw(const enc::Geometry &g, std::span<uint8_t> raw) {
    assert(pins && !readLocked && raw.size() == g.rawBytes);
    readLocked = true;
    std::ranges::fill(raw, 0);
    for (size_t i = 0; i < planes.size(); ++i) {
      const auto [offset, stride] = planes[i];
      const size_t dest = i == 0 ? 0 : g.uvOffset;
      const size_t rows = i == 0 ? height : height / 2;
      for (size_t y = 0; y < rows; ++y)
        std::copy_n(bytes.data() + offset + y * stride, width, raw.data() + dest + y * g.stride);
    }
    readLocked = false;
  }
};

void atom(Bytes &out, uint32_t type, std::span<const uint8_t> body) {
  const size_t offset = out.size();
  out.resize(offset + 8);
  put32(out.data() + offset, uint32_t(body.size() + 8));
  put32(out.data() + offset + 4, type);
  out.insert(out.end(), body.begin(), body.end());
}

void appendLittle(Bytes &out, uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

struct SampleBuffer {
  Bytes avcc, data;
  int64_t pts = 0, duration = 0;
  bool sync = false;

  Bytes serializeTimingAndData() const {
    Bytes timing, sizes, body, sample;
    // Nested CMTime fields and sample sizes use the arm64e native byte order.
    for (int64_t value : {duration, pts, pts}) { // zero B frames: DTS = PTS
      appendLittle(timing, uint64_t(value), 8);
      appendLittle(timing, enc::kNsPerSecond, 4);
      appendLittle(timing, 1, 4);
      appendLittle(timing, 0, 8);
    }
    appendLittle(sizes, data.size(), 4);
    const std::array<uint8_t, 4> oneSample{1, 0, 0, 0};
    atom(body, cc("stia"), timing);
    atom(body, cc("nsmp"), oneSample);
    atom(body, cc("ssiz"), sizes);
    atom(body, cc("sdat"), data);
    atom(sample, cc("sbuf"), body);
    return sample;
  }
};

std::optional<SampleBuffer> makeSample(std::span<const uint8_t> annexB, int64_t pts, int64_t duration, const Bytes &format) {
  std::array<enc::Nal, 32> nals;
  bool bad = false;
  const auto count = enc::scanAnnexB(annexB.data(), uint32_t(annexB.size()), nals.data(), nals.size(), &bad);
  if (bad || !count || count > nals.size()) return {};
  std::span<const uint8_t> sps, pps;
  SampleBuffer result{format, {}, pts, duration};
  unsigned slices = 0;
  for (const auto &nal : std::span(nals.data(), count)) {
    const auto payload = annexB.subspan(nal.offset, nal.size);
    if ((payload.front() & 0x80) || payload.size() < 2) return {};
    if (nal.type == 7 || nal.type == 8) {
      auto &parameter = nal.type == 7 ? sps : pps;
      if (!parameter.empty() || payload.size() > UINT16_MAX || slices) return {};
      parameter = payload;
      continue;
    }
    if (nal.type != 1 && nal.type != 5) return {};
    if (slices && result.sync != (nal.type == 5)) return {};
    ++slices;
    result.sync = nal.type == 5;
    const auto offset = result.data.size();
    result.data.resize(offset + 4);
    put32(result.data.data() + offset, nal.size);
    result.data.insert(result.data.end(), payload.begin(), payload.end());
  }
  if (!slices) return {};
  if (!sps.empty() || !pps.empty()) {
    if (!result.sync || sps.size() < 4 || pps.empty()) return {};
    result.avcc = {1, sps[1], sps[2], sps[3], 0xff, 0xe1};
    auto appendParameter = [&](std::span<const uint8_t> param) {
      result.avcc.push_back(uint8_t(param.size() >> 8));
      result.avcc.push_back(uint8_t(param.size()));
      result.avcc.insert(result.avcc.end(), param.begin(), param.end());
    };
    appendParameter(sps);
    result.avcc.push_back(1); // PPS count
    appendParameter(pps);
    if (!format.empty() && result.avcc != format) return {};
  }
  if (result.avcc.empty()) return {};
  return result;
}

struct Resident {
  stream::Owner owner;
  session::Layout layout;
  Bytes arena;

  void releaseArena() {
    layout = {};
    Bytes{}.swap(arena);
  }

  bool decode(uint32_t codec) {
    uint32_t id = 0;
    if (owner.acquire(&id)) return false;
    assert(layout.used == 0 && arena.empty());
    assert(codec == kAvc ? layout.init(640, 480) : layout.initHevc(1920, 1088, 10));
    assert(layout.hevc == (codec == kHevc) && !layout.find(enc::BufArp, 0));
    assert(layout.mapped(0) && owner.start(id));
    releaseArena();
    assert(owner.closing(id, false) == 0);
    owner.closed(false);
    return true;
  }

  void resetAfterDmaStopped() {
    assert(owner.phase == stream::Owner::Poisoned && layout.find(enc::BufArp, 0)->queued);
    releaseArena();
    owner = stream::Owner{.serial = owner.serial}; // Keep stale-event identities distinct in this model.
  }
};

enum class Status { Ok, Cancelled, SessionError, Watchdog };
enum class Phase { New, Created, Configured, Running, Stopping, Joined, Finalized };

struct Frame {
  int64_t token, pts, duration;
  Uuid pixel;
  Surface &surface;
  bool submitted = false, rawReturned = false;
  std::optional<SampleBuffer> sample;
};

struct Completion {
  int64_t token;
  Uuid pixel;
  Status status;
  bool delivered;
  std::optional<SampleBuffer> sample;
};

struct Encoder {
  Resident &core;
  Phase phase = Phase::New;
  enc::Geometry geometry;
  enc::Settings settings;
  uint32_t sid = 0;
  std::array<Frame *, enc::kRaws> rawSlots{};
  std::list<Frame> pending;
  std::vector<Completion> completed;
  Bytes format;
  Frame *awaiting = nullptr;
  uint64_t lastRequest = 0;
  std::optional<int64_t> flushCutoff;
  std::optional<Status> flushReply, invalidateReply;
  Status stopReason = Status::Cancelled;
  bool bitstreamStopped = false, rawStopped = false, closed = false, abandoned = false;

  explicit Encoder(Resident &resident) : core(resident) {}

  bool create(uint32_t codec, uint32_t version) {
    if (phase != Phase::New || codec != kAvc || version != 6) return false;
    phase = Phase::Created;
    return true;
  }

  bool configure(uint32_t width, uint32_t height, int64_t bitrate, uint32_t fps) {
    const bool supported = (width == 640 && height == 480) || (width == 1280 && height == 720) ||
                           (width == 1920 && height == 1080);
    if (phase != Phase::Created || !supported || bitrate < 100000 || bitrate > 40000000) return false;
    enc::Settings candidate;
    candidate.bitrate = uint32_t(bitrate);
    candidate.fps = fps;
    if (!enc::admitted(candidate) || !geometry.configure(width, height)) return false;
    settings = candidate;
    phase = Phase::Configured;
    return true;
  }

  bool prepare() {
    if (phase != Phase::Configured || core.owner.acquire(&sid)) return false;
    assert(enc::buildLayout(core.layout, geometry));
    assert(core.layout.arenaBytes() == kArenaBytes && core.layout.bytes <= kArenaBytes);
    core.arena.resize(core.layout.bytes);
    for (auto &b : std::span(core.layout.buf, core.layout.used))
      b.queued = b.type != enc::BufRaw;
    assert(core.owner.start(sid));
    phase = Phase::Running;
    return true;
  }

  // efrm admission only. The worker later copies and queues RAW.
  bool admit(int64_t token, Uuid pixel, Surface &surface, Time pts, Time duration) {
    if (phase != Phase::Running || pending.size() == kFrameCredits || !surface.valid(kTask, geometry)) return false;
    const auto ns = nanoseconds(pts), length = nanoseconds(duration);
    if (!ns || !length || !*length || std::ranges::any_of(pending, [&](const Frame &frame) {
          return frame.token == token || frame.pixel == pixel || frame.pts == *ns;
        })) return false;
    pending.push_back(Frame{token, *ns, *length, pixel, surface, false, false, {}});
    ++surface.pins;
    return true;
  }

  unsigned submitRaw() {
    if (phase != Phase::Running) return 0;
    unsigned queued = 0;
    for (auto &frame : pending) {
      if (frame.submitted) continue;
      const auto slot = std::ranges::find(rawSlots, nullptr);
      if (slot == rawSlots.end()) break;
      auto &b = *core.layout.find(enc::BufRaw, unsigned(slot - rawSlots.begin()));
      assert(!b.queued);
      frame.surface.copyToRaw(geometry, std::span(core.arena).subspan(b.off, b.capacity));
      *slot = &frame;
      b.queued = frame.submitted = true;
      ++queued;
    }
    return queued;
  }

  bool rawDone(uint32_t id, unsigned index, uint64_t /* timestamp */) {
    // Iris EBD reports slot ownership. Firmware can zero the PTS (6716eceeb).
    if (id != sid || phase != Phase::Running || index >= rawSlots.size() || !rawSlots[index]) return false;
    rawSlots[index]->rawReturned = true;
    rawSlots[index] = nullptr;
    core.layout.find(enc::BufRaw, index)->queued = false;
    return true;
  }

  bool bitstreamDone(uint32_t id, unsigned index, int64_t pts, std::span<const uint8_t> payload) {
    if (id != sid || phase != Phase::Running || index >= enc::kBitstreams) return false;
    auto &b = *core.layout.find(enc::BufBitstream, index);
    if (!b.queued || payload.size() > b.capacity) return false;
    const auto frame = std::ranges::find(pending, pts, &Frame::pts);
    if (frame == pending.end() || !frame->submitted || frame->sample || &*frame == awaiting) return false;
    auto buffer = std::span(core.arena).subspan(b.off, payload.size());
    std::ranges::copy(payload, buffer.begin());
    b.queued = false;
    auto sample = makeSample(buffer, pts, frame->duration, format);
    b.queued = true; // Requeue after copying. The guest owns independent sample bytes.
    if (!sample) return false;
    format = sample->avcc;
    frame->sample = std::move(sample);
    return true;
  }

  bool flush(std::optional<Time> cutoff = {}) {
    if (phase != Phase::Running || flushCutoff) return false;
    const auto ns = cutoff ? nanoseconds(*cutoff) : INT64_MAX;
    if (!ns) return false;
    flushCutoff = ns;
    flushReply.reset();
    return true;
  }

  bool stop(Status reason, bool clientDied = false) {
    if (phase != Phase::Running || reason == Status::Ok) return false;
    phase = Phase::Stopping;
    stopReason = reason;
    abandoned = clientDied;
    assert(core.owner.closing(sid, false) == 0);
    if (flushCutoff) {
      flushCutoff.reset();
      flushReply = reason;
    }
    return true;
  }

  bool stopAck(uint32_t port) {
    if (phase != Phase::Stopping || stopReason == Status::Watchdog || closed) return false;
    if (port == enc::PortBitstream && !bitstreamStopped) bitstreamStopped = true;
    else if (port == enc::PortRaw && bitstreamStopped && !rawStopped) {
      rawStopped = true;
      rawSlots.fill(nullptr);
    } else return false;
    for (auto &b : std::span(core.layout.buf, core.layout.used))
      if (b.port == port) b.queued = false;
    return true;
  }

  bool closeAck() {
    if (phase != Phase::Stopping || !bitstreamStopped || !rawStopped || closed) return false;
    assert(core.layout.find(enc::BufArp, 0)->queued);
    closed = true;
    core.releaseArena();
    return true;
  }

  void retire(Frame &frame) {
    assert(frame.surface.pins && !frame.surface.readLocked);
    --frame.surface.pins;
    pending.erase(std::ranges::find(pending, frame.token, &Frame::token));
  }

  void publish(Frame &frame, Status status) {
    completed.push_back({frame.token, frame.pixel, status, !abandoned,
                         status == Status::Ok ? std::move(frame.sample) : std::nullopt});
    if (abandoned) {
      retire(frame);
    } else {
      awaiting = &frame;
      ++lastRequest;
    }
  }

  bool ack(uint32_t id, uint64_t request) {
    if (id != sid || request != lastRequest || !awaiting || abandoned) return false;
    retire(*awaiting);
    awaiting = nullptr;
    return true;
  }

  void workerStep() {
    if (phase == Phase::Running) {
      if (flushCutoff && std::ranges::none_of(pending, [&](const Frame &frame) { return frame.pts <= *flushCutoff; })) {
        flushCutoff.reset();
        flushReply = Status::Ok;
      }
      if (awaiting) return;
      for (auto &frame : pending)
        if (frame.sample && frame.rawReturned) {
          publish(frame, Status::Ok);
          return;
        }
      return;
    }
    const bool watchdog = stopReason == Status::Watchdog;
    if (phase != Phase::Stopping || (!closed && !watchdog)) return;
    if (awaiting) {
      if (!abandoned) return;
      // The client already received this completion before its connection
      // closed. Retire its loan without duplicating the callback.
      retire(*awaiting);
      awaiting = nullptr;
    }
    if (!pending.empty()) {
      // A watchdog may leave Iris holding our copied RAW buffers. The input
      // surfaces are separate, so terminal retirement can still release them.
      if (watchdog) rawSlots.fill(nullptr);
      publish(pending.front(), stopReason);
      return;
    }
    // A confirmed CLOSE contains a session fault in this model. Loss of
    // device progress blocks decode and retains the arena until a reset.
    core.owner.closed(watchdog);
    phase = Phase::Joined;
    if (!abandoned) invalidateReply = stopReason == Status::Cancelled ? Status::Ok : stopReason;
  }

  bool finalize() {
    if (phase != Phase::Joined) return false;
    phase = Phase::Finalized;
    return true;
  }
};

Uuid pixelUuid(unsigned i) {
  Uuid uuid{};
  uuid[15] = uint8_t(i + 1);
  return uuid;
}

Time presentation(unsigned frame) { return {270000 + 3003 * int64_t(frame), 90000}; }
constexpr Time kDuration{1001, 30000};
constexpr std::array<int64_t, 5> kTokens{INT64_MIN, 71, -42, 0, INT64_MAX};

// High-profile 720p SPS/PPS from a generated black frame, followed by a
// synthetic slice payload. The model tests framing, not slice decoding.
const Bytes kIdr{0, 0, 0, 1, 0x67, 0x64, 0, 0x32, 0xac, 0xd9, 0x40, 0x50, 5, 0xbb, 1, 0x10, 0, 0, 3, 0, 0x10,
                 0, 0, 3, 3, 0xc0, 0xf1, 0x83, 0x19, 0x60, 0, 0, 1, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0,
                 0, 0, 0, 1, 0x65, 0x88, 0x80};
const Bytes kPFrame{0, 0, 1, 0x61, 0x9a, 0x80};

Surface makeSurface(const enc::Geometry &g) {
  Surface surface;
  surface.width = g.width;
  surface.height = g.height;
  surface.planes = {{19, g.width + 16}, {19 + size_t(g.width + 16) * g.height + 23, g.width + 48}};
  surface.bytes.resize(surface.planes[1].offset + surface.planes[1].stride * (g.height / 2), 0xee);
  for (unsigned i = 0; i < 2; ++i) {
    const auto [offset, stride] = surface.planes[i];
    for (unsigned row = 0; row < (i ? g.height / 2 : g.height); ++row)
      for (unsigned col = 0; col < g.width; ++col)
        surface.bytes[offset + row * stride + col] = uint8_t(17 * i + row + 3 * col);
  }
  return surface;
}

void open720p(Encoder &e) {
  assert(e.create(kAvc, 6));
  assert(e.configure(1280, 720, 2000000, 30));
  assert(e.prepare());
}

void closeFirmware(Encoder &e) {
  assert(!e.closeAck() && !e.stopAck(enc::PortRaw));
  assert(e.stopAck(enc::PortBitstream) && !e.stopAck(enc::PortBitstream));
  assert(e.core.layout.find(enc::BufArp, 0)->queued);
  assert(!e.closeAck() && e.stopAck(enc::PortRaw));
  assert(e.core.layout.find(enc::BufArp, 0)->queued && !e.core.arena.empty());
  assert(e.closeAck() && !e.closeAck());
  assert(!e.core.layout.used && e.core.arena.empty());
}

void allocationCases() {
  struct Expected {
    uint32_t width, height, codedHeight, raw, bitstream, guarded;
    std::array<uint32_t, 7> internals; // ARP, VPSS, BIN, COMV, NON_COMV, LINE, DPB
  };
  constexpr Expected cases[] = {
      {640, 480, 480, 462848, 921600, 32980992, {204800, 0, 23787008, 147456, 313856, 700928, 1921024}},
      {1280, 720, 720, 1413120, 708608, 27557888, {204800, 1449984, 9563648, 436224, 392448, 834048, 5804032}},
      {1920, 1080, 1088, 3133440, 1568768, 56180736, {204800, 0, 21661184, 983040, 542720, 984576, 12636160}},
  };
  Resident core;
  Encoder invalid(core);
  assert(!invalid.create(kHevc, 6) && !invalid.create(cc("vp09"), 6) && !invalid.create(kAvc, 5));
  assert(!invalid.configure(640, 480, 2000000, 30) && !invalid.prepare());
  assert(invalid.create(kAvc, 6));
  for (const auto &[w, h] : {std::pair{320u, 240u}, {0u, 480u}, {641u, 480u}, {1920u, 1088u}, {3840u, 2160u}, {UINT32_MAX, 480u}})
    assert(!invalid.configure(w, h, 2000000, 30));
  for (const int64_t bitrate : {INT64_MIN, int64_t(-1), int64_t(0), int64_t(99999), int64_t(40000001), INT64_MAX})
    assert(!invalid.configure(640, 480, bitrate, 30));
  assert(!invalid.configure(640, 480, 2000000, 0) && !invalid.configure(640, 480, 2000000, 61));
  assert(!invalid.prepare() && core.owner.phase == stream::Owner::Idle && core.layout.used == 0);
  assert(core.decode(kAvc));

  constexpr uint32_t types[]{enc::BufArp, enc::BufVpss, enc::BufBin, enc::BufComv, enc::BufNonComv, enc::BufLine, enc::BufDpb};
  constexpr uint32_t ports[]{0, 2, 1, 1, 1, 1, 1};
  for (const auto &c : cases) {
    for (const auto &[bitrate, fps] : {std::pair{100000, 30}, {40000000, 60}}) {
      Encoder e(core);
      assert(e.create(kAvc, 6) && e.configure(c.width, c.height, bitrate, fps) && e.prepare());
      assert(e.settings.bitrate == uint32_t(bitrate) && e.settings.fps == uint32_t(fps));
      assert(!e.prepare() && !e.configure(c.width, c.height, bitrate, fps));
      assert(e.geometry.codedHeight == c.codedHeight && e.geometry.codedCrop() == c.codedHeight - c.height);
      auto &layout = core.layout;
      assert(layout.used == (c.internals[1] ? 15u : 14u) && layout.bytes == c.guarded && layout.arenaBytes() == kArenaBytes);
      for (unsigned slot = 0; slot < 4; ++slot) {
        const auto *raw = layout.find(enc::BufRaw, slot), *bits = layout.find(enc::BufBitstream, slot);
        assert(raw && raw->port == 2 && raw->capacity == c.raw && !raw->queued);
        assert(bits && bits->port == 1 && bits->capacity == c.bitstream && bits->queued);
      }
      assert(!layout.find(enc::BufRaw, 4) && !layout.find(enc::BufBitstream, 4));
      for (unsigned i = 0; i < c.internals.size(); ++i) {
        const auto *buffer = layout.find(types[i], 0);
        if (c.internals[i]) assert(buffer && buffer->port == ports[i] && buffer->capacity == c.internals[i] && buffer->queued);
        else assert(!buffer);
      }
      uint32_t end = 0;
      for (const auto &buffer : std::span(layout.buf, layout.used)) {
        assert(buffer.off == end && !(buffer.off % 16384) && buffer.size == buffer.capacity);
        const auto guard = buffer.off + enc::align(buffer.capacity, 16384);
        assert(layout.mapped(buffer.off / 4096) && layout.mapped((buffer.off + buffer.capacity - 1) / 4096));
        end = guard + 16384;
        for (uint32_t offset = guard; offset < end; offset += 4096) assert(!layout.mapped(offset / 4096));
      }
      assert(end == c.guarded);
      assert(!core.decode(kAvc) && !core.decode(kHevc));
      assert(e.stop(Status::Cancelled));
      closeFirmware(e);
      e.workerStep();
      assert(e.invalidateReply == Status::Ok && e.finalize());
    }
  }
  assert(core.decode(kHevc) && core.decode(kAvc));
}

uint64_t little(const uint8_t *p, unsigned length) {
  uint64_t value = 0;
  for (unsigned i = 0; i < length; ++i) value |= uint64_t(p[i]) << (8 * i);
  return value;
}

void checkSample(const SampleBuffer &s, int64_t pts, bool sync) {
  assert(s.sync == sync && s.pts == pts && s.duration == 33366666);
  assert(s.avcc == (Bytes{1, 0x64, 0, 0x32, 0xff, 0xe1, 0, 26, 0x67, 0x64, 0, 0x32, 0xac, 0xd9, 0x40, 0x50, 5, 0xbb, 1,
                          0x10, 0, 0, 3, 0, 0x10, 0, 0, 3, 3, 0xc0, 0xf1, 0x83, 0x19, 0x60, 1, 0, 6, 0x68, 0xeb, 0xe3, 0xcb,
                          0x22, 0xc0}));
  assert(s.data == (sync ? Bytes{0, 0, 0, 3, 0x65, 0x88, 0x80} : Bytes{0, 0, 0, 3, 0x61, 0x9a, 0x80}));
  const auto wire = s.serializeTimingAndData();
  const auto *at = wire.data();
  const auto *end = at + wire.size();
  Atom sbuf, field;
  assert(next(at, end, sbuf) && sbuf.type == cc("sbuf") && at == end);
  at = sbuf.body;
  assert(next(at, end, field) && field.type == cc("stia") && field.size == 72);
  const std::array<int64_t, 3> times{33366666, pts, pts};
  for (unsigned i = 0; i < times.size(); ++i) {
    const auto *time = field.body + i * 24;
    assert(little(time, 8) == uint64_t(times[i]) && little(time + 8, 4) == 1000000000 &&
           little(time + 12, 4) == 1 && little(time + 16, 8) == 0);
  }
  assert(next(at, end, field) && field.type == cc("nsmp") && field.size == 4 && little(field.body, 4) == 1);
  assert(next(at, end, field) && field.type == cc("ssiz") && field.size == 4 && little(field.body, 4) == s.data.size());
  assert(next(at, end, field) && field.type == cc("sdat") && field.size == s.data.size() && at == end);
  assert(std::ranges::equal(s.data, std::span(field.body, field.size)));
}

void frameAndFlushCase() {
  Resident core;
  Encoder e(core);
  open720p(e);
  auto input = makeSurface(e.geometry);

  auto rejected = [&](auto change) {
    auto bad = input;
    change(bad);
    assert(!e.admit(8, pixelUuid(8), bad, presentation(0), kDuration) && !bad.pins);
  };
  rejected([](Surface &s) { s.task = 8; });
  rejected([](Surface &s) { s.format = cc("BGRA"); });
  rejected([](Surface &s) { s.format = cc("420f"); });
  rejected([](Surface &s) { ++s.width; });
  rejected([](Surface &s) { s.planes.pop_back(); });
  rejected([](Surface &s) { s.planes[0].stride = s.width - 1; });
  rejected([](Surface &s) { s.planes[1].offset = SIZE_MAX; });
  rejected([](Surface &s) { s.planes[1].stride = SIZE_MAX; });
  rejected([](Surface &s) { s.planes[1].offset = s.planes[0].offset; });
  rejected([](Surface &s) { s.bytes.pop_back(); });
  rejected([](Surface &s) { s.readLocked = true; });
  for (const Time bad : {Time{-1, 1}, {1, 0}, {1, 1, 0}, {1, 1, 1, 2}, {INT64_MAX, 1}})
    assert(!e.admit(8, pixelUuid(8), input, bad, kDuration) && !e.flush(bad) && !input.pins);
  assert(!e.admit(8, pixelUuid(8), input, presentation(0), Time{0, 1}));
  const std::array<int64_t, 5> pts{3000000000, 3033366666, 3066733333, 3100100000, 3133466666};
  for (unsigned i = 0; i < pts.size(); ++i) assert(e.admit(kTokens[i], pixelUuid(i), input, presentation(i), kDuration));
  assert(!e.admit(kTokens[0], pixelUuid(8), input, presentation(8), kDuration));
  assert(!e.admit(8, pixelUuid(0), input, presentation(8), kDuration));
  assert(!e.admit(8, pixelUuid(8), input, presentation(0), kDuration));
  assert(e.completed.empty() && input.pins == 5 && e.submitRaw() == 4 && e.submitRaw() == 0);

  const auto *raw = core.arena.data() + core.layout.find(enc::BufRaw, 0)->off;
  for (unsigned i = 0; i < 2; ++i) {
    const auto &g = e.geometry;
    const auto [offset, stride] = input.planes[i];
    const auto *plane = raw + (i ? g.uvOffset : 0);
    for (unsigned row = 0; row < (i ? g.height / 2 : g.height); ++row)
      assert(std::equal(plane + row * g.stride, plane + row * g.stride + g.width, input.bytes.data() + offset + row * stride));
  }
  assert(!raw[720 * 1280] && !raw[e.geometry.uvOffset + 360 * 1280] && !input.readLocked);

  assert(e.flush(presentation(1)) && !e.flush());
  assert(!e.bitstreamDone(e.sid, 0, pts[4], kIdr)); // admitted but not submitted
  assert(!e.bitstreamDone(e.sid, 0, pts[0] + 1, kIdr));
  assert(!e.bitstreamDone(e.sid, 4, pts[0], kIdr));
  assert(!e.bitstreamDone(e.sid, 0, pts[0], kPFrame)); // format is not established yet
  assert(!e.bitstreamDone(e.sid, 0, pts[0], Bytes{0, 0, 1}));
  assert(e.bitstreamDone(e.sid, 0, pts[0], kIdr));
  assert(!e.bitstreamDone(e.sid, 0, pts[0], kIdr));
  e.workerStep();
  assert(e.completed.empty() && !e.flushReply && input.pins == 5); // FBD precedes RAW EBD

  assert(!e.rawDone(e.sid, 4, 0) && !e.rawDone(e.sid + 1, 2, 0));
  assert(e.rawDone(e.sid, 2, 0) && !e.rawDone(e.sid, 2, 0));
  assert(e.submitRaw() == 1); // Reuse slot 2 before the previous occupant's FBD.
  assert(e.rawDone(e.sid, 0, 0));
  e.workerStep();
  assert(e.completed.size() == 1 && e.completed[0].token == kTokens[0] && input.pins == 5);
  assert(!e.ack(e.sid + 1, e.lastRequest) && !e.ack(e.sid, e.lastRequest + 1));
  assert(e.ack(e.sid, e.lastRequest) && !e.ack(e.sid, e.lastRequest) && input.pins == 4);

  assert(e.bitstreamDone(e.sid, 2, pts[2], kPFrame) && e.bitstreamDone(e.sid, 3, pts[3], kPFrame));
  assert(e.rawDone(e.sid, 3, 0));
  e.workerStep();
  assert(e.completed.back().token == kTokens[2] && e.ack(e.sid, e.lastRequest));
  e.workerStep();
  assert(e.completed.back().token == kTokens[3] && e.ack(e.sid, e.lastRequest));
  assert(!e.flushReply && input.pins == 2);

  assert(e.rawDone(e.sid, 1, 0) && e.bitstreamDone(e.sid, 1, pts[1], kPFrame));
  e.workerStep();
  assert(e.completed.back().token == kTokens[1] && !e.flushReply && input.pins == 2);
  assert(e.ack(e.sid, e.lastRequest));
  e.workerStep();
  assert(e.flushReply == Status::Ok && input.pins == 1 && e.phase == Phase::Running);
  assert(e.flush());
  assert(e.bitstreamDone(e.sid, 0, pts[4], kPFrame) && e.rawDone(e.sid, 2, 0));
  e.workerStep();
  assert(e.completed.back().token == kTokens[4] && e.ack(e.sid, e.lastRequest));
  e.workerStep();
  assert(e.flushReply == Status::Ok && !input.pins && e.pending.empty() && e.completed.size() == 5);
  for (const auto &done : e.completed) {
    const auto index = unsigned(std::ranges::find(kTokens, done.token) - kTokens.begin());
    assert(index < pts.size() && done.pixel == pixelUuid(index) && done.status == Status::Ok && done.delivered && done.sample);
    checkSample(*done.sample, pts[index], index == 0);
  }
  assert(e.stop(Status::Cancelled));
  closeFirmware(e);
  e.workerStep();
  assert(e.invalidateReply == Status::Ok && e.finalize() && core.decode(kAvc) && core.decode(kHevc));
}

void teardownCase(Status reason, bool clientDied) {
  Resident core;
  Encoder e(core);
  open720p(e);
  auto input = makeSurface(e.geometry);
  for (unsigned i = 0; i < kFrameCredits; ++i)
    assert(e.admit(i, pixelUuid(i), input, presentation(i), kDuration));
  assert(!e.admit(9, pixelUuid(9), input, presentation(9), kDuration) && input.pins == kFrameCredits);
  assert(e.submitRaw() == 4);
  assert(e.bitstreamDone(e.sid, 3, 3000000000, kIdr) && e.rawDone(e.sid, 0, 0));
  e.workerStep();
  assert(e.completed.size() == 1 && e.completed[0].sample && e.awaiting && e.flush());
  // Stop with one unacknowledged success, active DMA, a copied sample awaiting
  // EBD, and software-queued frames. Preserve the already emitted success.
  assert(e.bitstreamDone(e.sid, 2, 3033366666, kPFrame));
  assert(e.stop(reason, clientDied));
  const uint32_t oldSid = e.sid;
  assert(e.flushReply == reason && !e.invalidateReply && !e.finalize() && !e.stop(reason));
  assert(!e.admit(9, pixelUuid(9), input, presentation(9), kDuration) && e.submitRaw() == 0);
  assert(!e.rawDone(oldSid, 1, 0) && !e.bitstreamDone(oldSid, 0, 3033366666, kPFrame));
  e.workerStep();
  assert(e.completed.size() == 1 && input.pins == kFrameCredits && !core.decode(kAvc));

  if (reason == Status::Watchdog) {
    assert(!e.stopAck(enc::PortBitstream) && !e.closeAck());
  } else {
    closeFirmware(e);
    if (!clientDied) e.workerStep();
    assert(e.completed.size() == 1 && !e.invalidateReply && !core.decode(kHevc));
  }
  assert(e.ack(oldSid, e.lastRequest) == !clientDied);
  for (unsigned i = 1; i < kFrameCredits; ++i) {
    e.workerStep();
    assert(e.completed.size() == i + 1 && !e.invalidateReply);
    if (!clientDied) {
      const auto held = e.lastRequest;
      e.workerStep();
      assert(e.completed.size() == i + 1 && !e.invalidateReply && e.ack(oldSid, held) && !e.ack(oldSid, held));
    }
  }
  e.workerStep();
  assert(e.phase == Phase::Joined && !input.pins && e.pending.empty() && !e.awaiting);
  assert(e.completed.size() == kFrameCredits && e.finalize());
  for (unsigned i = 1; i < kFrameCredits; ++i) {
    const auto &done = e.completed[i];
    assert(done.token == i && done.pixel == pixelUuid(i) && done.status == reason && !done.sample && done.delivered == !clientDied);
  }
  assert(!e.rawDone(oldSid, 1, 0) && !e.bitstreamDone(oldSid, 0, 3033366666, kPFrame) && !e.ack(oldSid, e.lastRequest));
  e.workerStep();
  assert(e.completed.size() == kFrameCredits);
  if (clientDied) assert(!e.invalidateReply);
  else assert(e.invalidateReply == (reason == Status::Cancelled ? Status::Ok : reason));

  if (reason == Status::Watchdog) {
    assert(core.owner.phase == stream::Owner::Poisoned && core.layout.find(enc::BufArp, 0)->queued);
    assert(!core.arena.empty() && !core.decode(kAvc) && !core.decode(kHevc));
    core.resetAfterDmaStopped();
  }
  assert(!core.layout.used && core.arena.empty() && core.decode(kAvc) && core.decode(kHevc));

  Encoder next(core);
  open720p(next);
  assert(next.sid != oldSid && next.admit(1, pixelUuid(1), input, presentation(1), kDuration) && next.submitRaw() == 1);
  assert(!next.rawDone(oldSid, 0, 0) && !next.ack(oldSid, e.lastRequest));
  assert(!next.bitstreamDone(oldSid, 0, 3033366666, kIdr) && input.pins == 1);
  assert(next.stop(Status::Cancelled, true));
  closeFirmware(next);
  next.workerStep();
  next.workerStep();
  assert(next.finalize() && !input.pins && next.completed.size() == 1 && !next.completed[0].delivered);
}
} // namespace

int main() {
  assert(!enc::bootEnabled("") && !enc::bootEnabled("piano-video=6") && !enc::bootEnabled("piano-video=6 piano-video-enc=0"));
  allocationCases();
  frameAndFlushCase();
  teardownCase(Status::Cancelled, false);
  teardownCase(Status::Cancelled, true);
  teardownCase(Status::SessionError, false);
  teardownCase(Status::Watchdog, false);
  std::puts("VT E2 host contract PASS (ASan/UBSan): arena admission, RAW/FBD and NV12 leases, AVC samples, flush/cancel join, quarantine and decode handoff");
}
