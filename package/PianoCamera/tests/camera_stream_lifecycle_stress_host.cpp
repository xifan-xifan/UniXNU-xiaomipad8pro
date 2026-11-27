// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I../../include -I../../PianoKeyboard -I"${camera_stress_gen:=$(mktemp -d)}$(python3 ../../../tooling/camera/sensor-tables.py header ../../../data/camera "${camera_stress_gen:?}/CameraTables.h")" camera_stream_lifecycle_stress_host.cpp -o /tmp/camera_stream_lifecycle_stress_host && /tmp/camera_stream_lifecycle_stress_host ; camera_rc=$?; rm -f "${camera_stress_gen:?}/CameraTables.h"; rmdir "$camera_stress_gen"; exit "$camera_rc"
// Virtual time and explicit worker completions drive the modeled CMIO clients and ABI 2 lifecycle.
// DMA reservation, RAW layout, SMMU programming, ISP unpacking and output helpers are production code.
#include "../CameraIsp.h"
#include "../CameraStream.h"
#include "../PianoCameraABI.h"
#include "../../../tooling/camera/extension/CameraOutput.h"

#include <array>
#include <cassert>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace {
using namespace pianocamera;

constexpr auto kFront = Sensor::front, kRear = Sensor::rear;
constexpr uint64_t kIntervalNs = (1000000000ull + kPianoNativeFps / 2) / kPianoNativeFps;
constexpr uint64_t kSourceAgeNs = PIANO_OUTPUT_MAX_SOURCE_AGE_NS;
constexpr unsigned kCycles = 128;

FrameLayout dailyRaw(Sensor sensor)
{
  FrameLayout layout;
  assert(frameLayout(sensor == kFront ? kFrontL3Width : kRearL3Width,
                     sensor == kFront ? kFrontL3Height : kRearL3Height, layout));
  return layout;
}

class DmaPath {
  auto read() const { return [this](uint32_t off) { return regs_.at(off / 4); }; }
  auto write()
  {
    return [this](uint32_t off, uint32_t value) {
      assert(smmuWritable(route_, off));
      regs_.at(off / 4) = value;
    };
  }

 public:
  DmaPath()
  {
    regs_[iova::kIdr0 / 4] = iova::kIdr0S1ts | 1;
    regs_[iova::kIdr1 / 4] = 2;
    regs_[iova::kIdr2 / 4] = iova::kIdr2Ptfs4k | 2u << 4;
    regs_[iova::s2crOffset(0) / 4] = kS2crFault;
    regs_[iova::cbarOffset(0) / 4] = iova::kCbarStage1;
    reserve_.install({kTargetPa, kPianoCameraSlotBytes, backing_.get(), kTablePa, tables_.data(), this});
  }

  void map(uint32_t arena)
  {
    expectUnmapped();
    assert(reserve_.take(arena, true, lease_));
    route_ = planRoute(sizeof regs_, read());
    assert(route_.route == Route::window && !route_.matches);
    assert(iova::buildWindow(window_, lease_.tables, lease_.tables + 512, lease_.tablePa,
                             lease_.pa, arena, kIovaWindowBase));
    assert(!iova::apply(route_.window, kCamStream, lease_.tablePa, read(), write(), [] { return true; }));
    assert(iova::translate(window_, kIovaWindowBase) == kTargetPa);
    assert(iova::translate(window_, kIovaWindowBase + arena - 1) == kTargetPa + arena - 1);
    assert(iova::translate(window_, kIovaWindowBase + arena) == 0);
    ++maps_;
  }

  uint8_t *address(size_t offset) const
  {
    assert(reserve_.busy() && offset < lease_.bytes);
    assert(regs_.at(route_.window.cb / 4) & iova::kSctlrM);
    assert(regs_.at(iova::smrOffset(route_.slot) / 4) == (0x80000000u | kCamStream));
    const uint64_t pa = iova::translate(window_, kIovaWindowBase + offset);
    assert(pa == kTargetPa + offset);
    return lease_.cpu + offset;
  }

  void unmap()
  {
    assert(reserve_.busy());
    assert(!releaseRoute(route_, read(), write(), [] { return true; }).error);
    assert(reserve_.giveBack(lease_));
    ++unmaps_;
    expectUnmapped();
  }

  void expectUnmapped() const
  {
    assert(maps_ == unmaps_ && !reserve_.busy() && !lease_.cpu && !lease_.tables);
    const RoutePlan idle = planRoute(sizeof regs_, read());
    assert(idle.route == Route::window && !idle.matches);
    assert(regs_.at(iova::s2crOffset(0) / 4) == kS2crFault);
    assert(!(regs_.at(idle.window.cb / 4) & iova::kSctlrM));
    assert(!regs_.at((idle.window.cb + iova::kCbTtbr0Lo) / 4));
    assert(!regs_.at((idle.window.cb + iova::kCbTtbr0Hi) / 4));
  }

 private:
  static constexpr uint64_t kTargetPa = 4ull << 30, kTablePa = 0x1000;
  std::array<uint32_t, 4096> regs_{};
  std::array<uint64_t, 1024> tables_{};
  std::unique_ptr<uint8_t[]> backing_{new uint8_t[kPianoCameraSlotBytes]};
  DmaReserve reserve_;
  Dma lease_;
  RoutePlan route_;
  iova::Window window_;
  unsigned maps_ = 0, unmaps_ = 0;
};

enum class Client : unsigned { quickTime, photoBooth, contender };
enum class Phase { idle, starting, streaming, draining };

struct Ticket {
  uint64_t generation, sequence;
  bool operator==(const Ticket &) const = default;
};

struct Sample {
  Ticket ticket;
  uint64_t sourceNs;
  uint32_t pixel;
};

struct Delivery {
  std::shared_ptr<const Sample> sample;
  bool discontinuity = false;
};

class Camera {
 public:
  bool start(Client client, Sensor sensor = kFront)
  {
    if (phase_ == Phase::draining || (phase_ != Phase::idle && sensor != sensor_)) return false;
    if (phase_ == Phase::idle) {
      ++generation_;
      sensor_ = sensor;
      layout_ = dailyRaw(sensor);
      ring_ = std::make_unique<Ring>();
      phase_ = Phase::starting;
    }
    clients_.set(unsigned(client));
    return true;
  }

  void streamOn()
  {
    assert(phase_ == Phase::starting);
    railOn_.at(unsigned(sensor_)) = true;
    mclk_ = true;
    dma_.map(layout_.arena);
    writer_ = true;
    phase_ = Phase::streaming;
  }

  void leave(Client client)
  {
    if (!clients_.test(unsigned(client))) return;
    clients_.reset(unsigned(client));
    if (clients_.none()) phase_ = Phase::draining;
  }

  bool finishStop()
  {
    if (phase_ != Phase::draining || pending_ || reading_) return false;
    if (writer_) {
      writer_ = false;
      assert(mclk_ && railOn_.at(unsigned(sensor_)));
      dma_.unmap();
      mclk_ = railOn_.at(unsigned(sensor_)) = false;
    }
    ring_.reset();
    latest_.reset();
    lastRead_ = lastPresentation_ = 0;
    stalled_ = false;
    phase_ = Phase::idle;
    return true;
  }

  Ticket queue(uint64_t sequence)
  {
    assert(phase_ == Phase::streaming && !pending_ && sequence);
    dmaTail_ = dma_.address(lastGroupOffset());
    return *(pending_ = Ticket{generation_, sequence});
  }

  bool complete(Ticket ticket, size_t bytes, uint8_t pixel)
  {
    if (pending_ != ticket) return false;
    assert(writer_ && dmaTail_ == dma_.address(lastGroupOffset()));
    auto *tail = dmaTail_;
    dmaTail_ = nullptr;
    pending_.reset();
    // The private DMA mapping holds the whole frame. Only the last RAW10 group is
    // materialized in the CPU ring, so the stress loop can retire frames cheaply.
    const bool full = bytes == layout_.payload;
    if (full) {
      std::memset(tail, pixel, 4);
      tail[4] = 0;
    }
    if (phase_ == Phase::draining) return false;
    for (auto &slot : *ring_) {
      if (slot.ticket.sequence) continue;
      slot = {ticket, bytes, full ? pianoRaw10(tail, 3) : 0};
      return true;
    }
    return false;
  }

  bool beginRead(Ticket ticket)
  {
    if (phase_ != Phase::streaming || reading_) return false;
    RawGroup *oldest = nullptr;
    for (auto &slot : *ring_)
      if (slot.ticket.sequence && (!oldest || slot.ticket.sequence < oldest->ticket.sequence)) oldest = &slot;
    if (!oldest || oldest->ticket != ticket) return false;
    reading_ = oldest;
    return true;
  }

  bool finishRead(Ticket ticket, uint8_t expected)
  {
    if (ticket.generation != generation_ || !reading_ || reading_->ticket != ticket) return false;
    // Release the lease even if the provider refuses its metadata.
    const bool valid = reading_->bytes == layout_.payload && ticket.sequence > lastRead_;
    if (valid) assert(reading_->pixel == uint32_t(expected) * 4);
    if (phase_ == Phase::streaming && valid) {
      lastRead_ = ticket.sequence;
      latest_ = std::make_shared<const Sample>(Sample{ticket, now_, reading_->pixel});
    }
    *reading_ = {};
    reading_ = nullptr;
    return valid;
  }

  Delivery tick()
  {
    if (phase_ != Phase::streaming || !latest_ || now_ <= lastPresentation_) return {};
    if (!pianoOutputSourceFresh(now_, latest_->sourceNs)) {
      stalled_ = true;
      return {};
    }
    const bool gap = pianoOutputDiscontinuity(stalled_, lastPresentation_, now_, kIntervalNs);
    lastPresentation_ = now_;
    stalled_ = false;
    return {latest_, gap};
  }

  void advance(uint64_t ns) { now_ += ns; }
  Sensor sensor() const { return sensor_; }
  size_t payload() const { return layout_.payload; }

  void expectOff() const
  {
    assert(phase_ == Phase::idle && !ring_ && !latest_ && !reading_ && !pending_ && !dmaTail_);
    assert(!railOn_[0] && !railOn_[1] && !mclk_ && !writer_);
    assert(clients_.none());
    dma_.expectUnmapped();
  }

  void expectPowered(Sensor sensor) const
  {
    assert(sensor_ == sensor && writer_ && mclk_);
    assert(railOn_[unsigned(kRear)] == (sensor == kRear) && railOn_[unsigned(kFront)] == (sensor == kFront));
    assert(dma_.address(layout_.payload - 1));
  }

 private:
  size_t lastGroupOffset() const { return layout_.payload - layout_.stride + layout_.packed - 5; }

  struct RawGroup {
    Ticket ticket{};
    size_t bytes = 0;
    uint32_t pixel = 0;
  };
  using Ring = std::array<RawGroup, kPianoCameraSlots>;
  std::unique_ptr<Ring> ring_;
  RawGroup *reading_ = nullptr;
  std::optional<Ticket> pending_;
  uint8_t *dmaTail_ = nullptr;
  std::shared_ptr<const Sample> latest_;
  std::bitset<3> clients_;
  std::array<bool, 2> railOn_{};
  DmaPath dma_;
  Phase phase_ = Phase::idle;
  Sensor sensor_ = kFront;
  FrameLayout layout_;
  uint64_t generation_ = 0, now_ = 1000000000, lastRead_ = 0, lastPresentation_ = 0;
  bool mclk_ = false, writer_ = false, stalled_ = false;
};

void stressSwitchAndClose()
{
  Camera camera;
  camera.expectOff();
  Ticket previousDma{}, previousRead{};
  for (unsigned cycle = 0; cycle < kCycles; ++cycle) {
    const auto sensor = cycle % 2 ? kRear : kFront;
    const auto other = cycle % 2 ? kFront : kRear;
    if (cycle % 4 == 0) {
      assert(camera.start(Client::quickTime, sensor));
      camera.leave(Client::quickTime);  // Aborted while the worker was still starting.
      assert(!camera.start(Client::contender, other));
      assert(camera.finishStop());
      camera.expectOff();
    }

    assert(cycle % 2 ? camera.start(Client::quickTime, kRear) : camera.start(Client::quickTime));
    assert(camera.sensor() == sensor && !camera.start(Client::contender, other));
    camera.streamOn();
    assert(camera.start(Client::photoBooth, sensor) && !camera.start(Client::contender, other));
    camera.expectPowered(sensor);
    assert(!camera.tick().sample);
    const auto first = camera.queue(1);
    assert(!camera.complete(previousDma, camera.payload(), 0));
    assert(camera.complete(first, camera.payload(), 37) && camera.beginRead(first) && camera.finishRead(first, 37));
    auto held = camera.tick();
    assert(held.sample && held.sample->ticket == first && held.sample->pixel == 148 && !held.discontinuity);
    std::weak_ptr<const Sample> retained = held.sample;

    camera.leave(Client::quickTime);
    camera.leave(Client::quickTime);  // Duplicate close cannot stop Photo Booth.
    assert(!camera.finishStop());
    camera.expectPowered(sensor);
    camera.advance(kIntervalNs);
    const auto read = camera.queue(2);
    assert(camera.complete(read, camera.payload(), 58) && camera.beginRead(read));
    assert(!camera.finishRead(previousRead, 0));
    const auto dma = camera.queue(3);
    if (cycle % 4 < 2) {
      // The last app disappears with both a RAW reader and a DMA write in flight.
      camera.leave(Client::photoBooth);
      assert(!camera.tick().sample && !camera.finishStop());
      assert(!camera.start(Client::contender, sensor) && !camera.start(Client::contender, other));
      camera.expectPowered(sensor);
      if (cycle % 2) {
        assert(camera.finishRead(read, 58) && !camera.finishStop());
        assert(!camera.complete(dma, camera.payload(), 99));
      } else {
        assert(!camera.complete(dma, camera.payload(), 99) && !camera.finishStop());
        assert(camera.finishRead(read, 58));
      }
    } else {
      assert(camera.complete(dma, camera.payload(), 99) && !camera.beginRead(dma));
      assert(camera.finishRead(read, 58) && camera.beginRead(dma) && camera.finishRead(dma, 99));
      const auto third = camera.tick();
      assert(third.sample && third.sample->ticket == dma && third.sample->pixel == 396 && !third.discontinuity);
      camera.leave(Client::photoBooth);
      assert(!camera.start(Client::contender, other));
    }
    assert(camera.finishStop() && !camera.finishStop());
    camera.expectOff();
    assert(!camera.finishRead(read, 58) && !camera.complete(dma, camera.payload(), 99));
    assert(held.sample->pixel == 148 && retained.use_count() == 1);
    held.sample.reset();
    assert(retained.expired());
    previousDma = first;
    previousRead = read;
  }
  // The last explicit rear selection never changes a fresh app's front default.
  assert(camera.start(Client::photoBooth) && camera.sensor() == kFront);
  camera.leave(Client::photoBooth);
  assert(camera.finishStop());
  camera.expectOff();
}

void testStallRecovery(Sensor sensor)
{
  Camera camera;
  assert(camera.start(Client::quickTime, sensor));
  camera.streamOn();
  const auto first = camera.queue(1);
  assert(camera.complete(first, camera.payload(), 41) && camera.beginRead(first) && camera.finishRead(first, 41));
  const auto original = camera.tick();
  assert(original.sample && !original.discontinuity && !camera.tick().sample);

  // No completion for CSI sequence 2. A fresh source can bridge the drop.
  camera.advance(kIntervalNs);
  const auto repeated = camera.tick();
  assert(repeated.sample == original.sample && !repeated.discontinuity);
  camera.advance(kIntervalNs);
  const auto third = camera.queue(3);
  assert(camera.complete(third, camera.payload(), 42) && camera.beginRead(third) && camera.finishRead(third, 42));
  const auto latest = camera.tick();
  assert(latest.sample && latest.sample->ticket == third && !latest.discontinuity);

  // A short DMA completion leaves the last good frame intact.
  camera.advance(kIntervalNs);
  const auto shortFrame = camera.queue(4);
  assert(camera.complete(shortFrame, camera.payload() - 1, 43) && camera.beginRead(shortFrame));
  assert(!camera.finishRead(shortFrame, 43));
  assert(camera.tick().sample == latest.sample);

  camera.advance(kSourceAgeNs - kIntervalNs);
  assert(camera.tick().sample == latest.sample);  // Exactly at the freshness limit.
  camera.advance(1);
  assert(!camera.tick().sample);
  const auto old = camera.queue(2);
  assert(camera.complete(old, camera.payload(), 43) && camera.beginRead(old));
  assert(!camera.finishRead(old, 43));
  const auto recovery = camera.queue(5);
  assert(!camera.complete(shortFrame, camera.payload(), 43) && !camera.tick().sample);
  camera.advance(kIntervalNs);
  assert(camera.complete(recovery, camera.payload(), 44) && camera.beginRead(recovery) && camera.finishRead(recovery, 44));
  const auto resumed = camera.tick();
  assert(resumed.sample && resumed.sample->ticket == recovery && resumed.sample->pixel == 176 && resumed.discontinuity);
  camera.advance(kIntervalNs);
  const auto steady = camera.tick();
  assert(steady.sample == resumed.sample && !steady.discontinuity);
  assert(original.sample->pixel == 164);
  camera.leave(Client::quickTime);
  assert(camera.finishStop());
  camera.expectOff();
}

void testGeometry()
{
  std::array<uint64_t, 1024> tables{};
  DmaReserve reserve;
  reserve.install({0, kPianoCameraSlotBytes, nullptr, 0, tables.data(), &reserve});
  const struct Mode {
    uint32_t width, height, stride, payload, arenaMiB;
    bool daily;
  } modes[] = {
    {kFrontL3Width, kFrontL3Height, 4080, 7490880, 8, true},
    {kRearL3Width, kRearL3Height, 5104, 15679488, 16, true},
    {6528, 4896, 8160, 39951360, 40, false},
    {8160, 6144, 10208, 62717952, 62, false},
  };
  for (const Mode &mode : modes) {
    FrameLayout layout;
    assert(frameLayout(mode.width, mode.height, layout));
    assert(layout.stride == mode.stride && layout.payload == mode.payload && layout.arena == (mode.arenaMiB << 20));
    assert(layout.stride % 16 == 0 && layout.stride - layout.packed < 16);
    assert((layout.payload <= kPianoCameraSlotBytes) == mode.daily);
    Dma lease, extra;
    assert(reserve.take(layout.arena, true, lease) == mode.daily);
    if (mode.daily) {
      assert(!reserve.take(1, true, extra));
      assert(reserve.giveBack(lease) && !reserve.giveBack(lease));
      PianoIspLayout isp;
      assert(!pianoIspLayout(mode.width, mode.height, layout.stride, layout.payload, 2, &isp));
      const size_t lastPacked = layout.payload - (layout.stride - layout.packed);
      assert(pianoIspLayout(mode.width, mode.height, layout.stride, lastPacked - 1, 2, &isp) == -1);
    }
    assert(!reserve.busy());
  }
  FrameLayout overflow;
  assert(!frameLayout(UINT32_MAX - 3, UINT32_MAX, overflow) && !overflow.arena);
  assert(!frameLayout(4, UINT32_MAX, overflow) && !overflow.payload);
  assert(!frameLayout(642, 480, overflow));
  PianoIspLayout isp;
  assert(pianoIspLayout(65536, 262144, 81920, SIZE_MAX, 1, &isp) == -1);

  // Four padded RAW10 rows, with an allocation ending one byte short of the last packed group.
  std::array<uint8_t, 62> shortRaw{};
  std::array<float, 36> work{};
  std::array<uint8_t, 36> rgb{};
  PianoIspStats stats{};
  assert(pianoIsp(shortRaw.data(), shortRaw.size(), 12, 4, 16, kPianoCameraBGGR, kRawBlack,
                  1, work.data(), work.size(), rgb.data(), rgb.size(), &stats) == -1);

  const auto format = pianoOutputFormat(kFrontL3Width / 4, kFrontL3Height / 4, 1);
  assert(format.width == 640 && format.height == 480 && format.minFps == 15);
  assert(pianoOutputDurationValid(1, 1, 15) && !pianoOutputDurationValid(1, INT64_MAX, INT32_MAX));
  const uint32_t width = format.width, height = format.height;
  const size_t yStride = width + 16, uvStride = width + 32, bgraStride = width * 4;
  std::vector<uint8_t> bgra(bgraStride * height, 255);
  std::vector<uint8_t> y(yStride * height, 0xa5), uv(uvStride * (height / 2), 0xa5);
  assert(pianoOutputBGRAto420v(bgra.data(), width, height, bgraStride, y.data(), yStride, uv.data(), uvStride));
  assert(y.front() == 235 && y[y.size() - yStride + width - 1] == 235 && y.back() == 0xa5);
  assert(uv.front() == 128 && uv[uv.size() - uvStride + width - 1] == 128 && uv.back() == 0xa5);
  for (size_t invalid : {size_t(width - 1), SIZE_MAX}) {
    assert(!pianoOutputBGRAto420v(bgra.data(), width, height, bgraStride, y.data(), invalid, uv.data(), uvStride));
    assert(!pianoOutputBGRAto420v(bgra.data(), width, height, bgraStride, y.data(), yStride, uv.data(), invalid));
  }
  assert(!pianoOutputBGRAto420v(bgra.data(), width - 1, height, bgraStride, y.data(), yStride, uv.data(), uvStride));
}
}  // namespace

int main()
{
  stressSwitchAndClose();
  testStallRecovery(kFront);
  testStallRecovery(kRear);
  testGeometry();
  std::printf("camera_stream_lifecycle_stress_host: %u alternating sessions, stall recovery and geometry passed\n", kCycles);
}
