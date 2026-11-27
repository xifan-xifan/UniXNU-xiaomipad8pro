// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all cam_switch_privacy_host.cpp -o /tmp/cam_switch_privacy_host && /tmp/cam_switch_privacy_host
// Deterministic interleavings of ABI 2 ownership and CMIO's last-stream-client stop.
// App clients, device enumeration, sensor power and the privacy LED are modeled.
#include "../CameraIsp.h"
#include "../CameraSmmu.h"
#include "../PianoCameraABI.h"
#include "../../../tooling/camera/extension/CameraOutput.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <memory>

namespace {
using namespace pianocamera;

constexpr std::array<uint32_t, 2> kDiscoveryOrder{kPianoCameraFront, kPianoCameraRear};
enum class Format { bgra, video420, unsupported };
struct Mode {
  unsigned index;
  Format format;
  uint32_t width, height;
  int32_t fps;
  size_t stride;
};
constexpr Mode kDaily{1, Format::video420, 640, 480, 15, 640};

bool supportsMode(uint32_t sensor, const Mode &mode)
{
  const bool front = sensor == kPianoCameraFront;
  const auto f = pianoOutputFormat((front ? kFrontL3Width : kRearL3Width) / 4,
                                   (front ? kFrontL3Height : kRearL3Height) / 4, mode.index);
  const Format format = mode.index == 0 ? Format::bgra : Format::video420;
  const uint64_t rowBytes = uint64_t(f.width) * (format == Format::bgra ? 4 : 1);
  return pianoOutputDurationValid(mode.index, 1, mode.fps) && mode.format == format &&
         mode.width == f.width && mode.height == f.height && pianoOutputRowsValid(mode.stride, rowBytes, f.height);
}

// One mock context bank and one private DMA target. Use the real SMMU helpers
// for programming, translation and release, with register accesses in memory.
class DmaPath {
  auto read() const { return [this](uint32_t off) { return regs_.at(off / 4); }; }
  auto write()
  {
    return [this](uint32_t off, uint32_t val) {
      assert(smmuWritable(route_, off));
      regs_.at(off / 4) = val;
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
  }

  void map()
  {
    assert(!target_);
    route_ = planRoute(sizeof regs_, read());
    assert(route_.route == Route::window && !route_.matches);
    assert(iova::buildWindow(window_, tables_.data(), tables_.data() + 512, kTablePa,
                             kTargetPa, 2u << 20, kIovaWindowBase));
    assert(!iova::apply(route_.window, kCamStream, kTablePa, read(), write(), [] { return true; }));
    target_ = std::make_unique<uint8_t>();
  }

  uint8_t *target() const
  {
    assert(target_ && (regs_[route_.window.cb / 4] & iova::kSctlrM));
    assert(regs_[iova::smrOffset(route_.slot) / 4] == (0x80000000u | kCamStream));
    assert(iova::translate(window_, kIovaWindowBase) == kTargetPa);
    return target_.get();
  }

  bool unmap(bool tlbStuck)
  {
    assert(target_);
    regs_[iova::kTlbGStatus / 4] = tlbStuck;
    if (releaseRoute(route_, read(), write(), [] { return true; }).error) return false;
    const auto free = planRoute(sizeof regs_, read());
    assert(free.route == Route::window && !free.matches && free.bank == route_.bank);
    target_.reset();
    return true;
  }

  bool leased() const { return bool(target_); }

 private:
  static constexpr uint64_t kTargetPa = 4ull << 30, kTablePa = 0x1000;
  std::array<uint32_t, 4096> regs_{};
  std::array<uint64_t, 1024> tables_{};
  RoutePlan route_;
  iova::Window window_;
  std::unique_ptr<uint8_t> target_;
};

struct Client {
  uint64_t id;
  bool attached = true, started = false;
  std::shared_ptr<const uint8_t> surface{};
};

struct FrameTicket {
  uint64_t owner = 0, sequence = 0;
  bool operator==(const FrameTicket &) const = default;
};

// The CPU ring and independently retained output are separate from private
// DMA. A borrowed pointer stays live until its completion callback drains.
class Camera {
 public:
  enum class StartResult { started, invalid, busy };
  Client attach() { return {++nextClient_}; }

  StartResult start(Client &client, uint32_t sensor = kDiscoveryOrder.front(), const Mode &mode = kDaily)
  {
    if ((sensor != kPianoCameraFront && sensor != kPianoCameraRear) || !supportsMode(sensor, mode))
      return StartResult::invalid;
    if (owner_ || !client.attached || client.started) return StartResult::busy;
    client.started = true;
    owner_ = client.id;
    status_ = {};
    status_.version = kPianoCameraLiveVersion;
    status_.sensor = sensor;
    status_.state = kPianoCameraStarting;
    ring_ = std::make_unique<Ring>();
    return StartResult::started;
  }

  void streamOn()
  {
    assert(owner_ && !stopping_ && status_.state == kPianoCameraStarting);
    led_ = true;
    rails_ = mclk_ = true;
    dma_.map();
    const auto sensor = status_.sensor == kPianoCameraFront ? Sensor::front : Sensor::rear;
    phy_ = int(kSensors[unsigned(sensor)].csiphy);
    writer_ = true;
    status_.state = kPianoCameraStreaming;
  }

  FrameTicket queueDma()
  {
    if (stopping_ || !writer_ || pending_.owner) return {};
    assert(phy_ >= 0 && rails_ && mclk_ && led_);
    dmaTarget_ = dma_.target();
    return pending_ = {owner_, ++nextSequence_};
  }

  bool completeDma(FrameTicket ticket, uint8_t value)
  {
    if (!ticket.owner || ticket != pending_) return false;
    assert(dmaTarget_ == dma_.target() && writer_ && led_);
    *dmaTarget_ = value;
    dmaTarget_ = nullptr;
    pending_ = {};
    if (!stopping_ && (!reading_.owner || slot(reading_) != slot(ticket))) {
      (*ring_)[slot(ticket)] = value;
      ready_ = ticket;
      ++status_.produced;
    }
    return true;
  }

  FrameTicket beginRead()
  {
    if (stopping_ || reading_.owner || !ready_.owner) return {};
    raw_ = &(*ring_)[slot(ready_)];
    reading_ = ready_;
    ready_ = {};
    return reading_;
  }

  bool finishRead(FrameTicket ticket, uint8_t expected)
  {
    if (!ticket.owner || ticket != reading_) return false;
    assert(*raw_ == expected);
    if (!stopping_) latest_ = std::make_shared<const uint8_t>(*raw_);
    raw_ = nullptr;
    reading_ = {};
    return true;
  }

  // Stop represents CMIO's last streaming client's stop notification.
  // Connection loss requests cancellation without joining either worker.
  void stop(const Client &client)
  {
    if (client.id == owner_) stopping_ = true;
  }

  void disconnect(Client &client)
  {
    stop(client);
    client.surface.reset();
    client.attached = false;
  }

  bool quiesce()
  {
    if (!stopping_ || pending_.owner) return false;
    writer_ = false;
    phy_ = -1;
    return true;
  }

  bool finishStop(bool tlbStuck = false)
  {
    if (!stopping_ || writer_ || reading_.owner || status_.state == kPianoCameraFailed) return false;
    assert(phy_ == -1 && !pending_.owner && !dmaTarget_ && !raw_);
    bool released = true;
    if (dma_.leased()) {
      assert(rails_ && mclk_ && led_);
      released = dma_.unmap(tlbStuck);
    }
    latest_.reset();
    ring_.reset();
    ready_ = {};
    if (!released) {
      status_.state = kPianoCameraFailed;
      return false;
    }
    rails_ = mclk_ = led_ = false;
    status_.state = kPianoCameraStopped;
    status_.cleanOff = 1;
    stopping_ = false;
    owner_ = nextSequence_ = 0;
    return true;
  }

  const PianoCameraLiveStatus &status() const { return status_; }
  std::shared_ptr<const uint8_t> latest() const { return latest_; }
  bool captureActive() const { return owner_ && status_.state != kPianoCameraFailed; }
  bool railsOn() const { return rails_; }
  bool mclkOn() const { return mclk_; }
  bool ledOn() const { return led_; }
  int phy() const { return phy_; }
  bool dmaLeased() const { return dma_.leased(); }
  bool ringMapped() const { return bool(ring_); }

 private:
  using Ring = std::array<uint8_t, kPianoCameraSlots>;
  static size_t slot(FrameTicket ticket) { return (ticket.sequence - 1) % kPianoCameraSlots; }

  DmaPath dma_;
  PianoCameraLiveStatus status_{};
  uint64_t nextClient_ = 0, owner_ = 0, nextSequence_ = 0;
  bool rails_ = false, mclk_ = false, led_ = false, writer_ = false, stopping_ = false;
  int phy_ = -1;
  std::unique_ptr<Ring> ring_;
  std::shared_ptr<const uint8_t> latest_;
  FrameTicket pending_, ready_, reading_;
  uint8_t *dmaTarget_ = nullptr;
  const uint8_t *raw_ = nullptr;
};

using Start = Camera::StartResult;

void expectOff(const Camera &camera)
{
  assert(!camera.captureActive() && !camera.ledOn() && !camera.railsOn() && !camera.mclkOn());
  assert(camera.phy() == -1 && !camera.dmaLeased() && !camera.ringMapped() && !camera.latest());
}

void expectPowered(const Camera &camera)
{
  assert(camera.ledOn() && camera.railsOn() && camera.mclkOn() && camera.dmaLeased());
}

void testRapidSwitch()
{
  Camera camera;
  auto client = camera.attach();
  expectOff(camera);
  for (unsigned round = 0; round < 128; ++round) {
    const auto sensor = kDiscoveryOrder[round % 2], other = kDiscoveryOrder[(round + 1) % 2];
    auto idle = camera.attach();
    auto next = camera.attach();
    assert(camera.start(client, sensor) == Start::started);
    assert(camera.status().state == kPianoCameraStarting && !camera.ledOn());
    assert(camera.start(next, other) == Start::busy);
    camera.streamOn();
    const int phy = sensor == kPianoCameraFront ? 4 : 1;
    assert(camera.phy() == phy && camera.status().sensor == sensor && camera.status().state == kPianoCameraStreaming);
    assert(camera.start(next, sensor) == Start::busy && camera.start(next, other) == Start::busy);
    expectPowered(camera);
    camera.disconnect(idle);
    const auto pending = camera.queueDma();
    assert(pending.owner == client.id);

    camera.stop(client);
    assert(!camera.queueDma().owner && !camera.quiesce() && !camera.finishStop());
    assert(camera.start(next, other) == Start::busy && camera.phy() == phy);
    expectPowered(camera);
    assert(camera.completeDma(pending, 42) && camera.status().produced == 0);
    assert(camera.quiesce() && camera.phy() == -1);
    assert(camera.start(next, other) == Start::busy);
    expectPowered(camera);
    assert(camera.finishStop() && camera.status().state == kPianoCameraStopped && camera.status().cleanOff);
    expectOff(camera);
    assert(camera.start(client, other) == Start::busy);
    camera.disconnect(client);
    client = next;
  }
}

void testClientLoss()
{
  // Each fresh app request falls back to the front after the preceding crash.
  Camera camera;
  FrameTicket previousDma{}, previousRead{};
  for (const char *app : {"QuickTime", "Photo Booth", "FaceTime"}) {
    auto client = camera.attach();
    auto next = camera.attach();
    assert(camera.start(client) == Start::started && camera.status().sensor == kPianoCameraFront);
    camera.streamOn();
    const auto sample = camera.queueDma();
    assert(!camera.completeDma(previousDma, 0));
    assert(camera.completeDma(sample, 77) && camera.beginRead() == sample);
    assert(camera.finishRead(sample, 77));
    client.surface = camera.latest();
    auto surface = client.surface;  // Another consumer retains the same delivered frame.
    assert(surface && *surface == 77);
    std::weak_ptr<const uint8_t> delivered = surface;

    const auto raw = camera.queueDma();
    assert(camera.completeDma(raw, 89) && camera.beginRead() == raw);
    assert(!camera.finishRead(previousRead, 0));
    const auto pending = camera.queueDma();
    assert(pending.owner == client.id);
    camera.disconnect(client);
    assert(!client.attached && !client.surface && camera.captureActive());
    assert(!camera.beginRead().owner && !camera.queueDma().owner);
    assert(!camera.quiesce() && !camera.finishStop());
    expectPowered(camera);
    assert(camera.completeDma(pending, 99) && camera.quiesce());
    assert(camera.status().produced == 2 && !camera.finishStop() && camera.ringMapped());
    assert(camera.start(next, kPianoCameraRear) == Start::busy);
    assert(camera.finishRead(raw, 89) && camera.latest() == surface);
    assert(camera.finishStop() && camera.status().state == kPianoCameraStopped && camera.status().cleanOff);
    expectOff(camera);
    assert(!camera.finishRead(raw, 89) && !camera.completeDma(pending, 99));
    assert(*surface == 77);
    surface.reset();
    assert(delivered.expired());
    previousDma = sample;
    previousRead = raw;
    std::printf("%s model: opened front and drained after disconnect\n", app);
  }
}

void testReleaseTimeout()
{
  Camera camera;
  auto client = camera.attach();
  auto next = camera.attach();
  assert(camera.start(client) == Start::started);
  camera.streamOn();
  camera.disconnect(client);
  assert(camera.quiesce() && !camera.finishStop(true));
  assert(camera.status().state == kPianoCameraFailed && !camera.status().cleanOff && !camera.captureActive());
  assert(!camera.ringMapped());
  expectPowered(camera);
  assert(camera.start(next, kPianoCameraRear) == Start::busy && !camera.finishStop());
}

void testModes()
{
  const Mode invalid[] = {
    {2, Format::video420, 640, 480, 15, 640},
    {1, Format::unsupported, 640, 480, 15, 640},
    {1, Format::bgra, 640, 480, 15, 2560},
    {1, Format::video420, 1280, 720, 15, 1280},
    {1, Format::video420, 640, 0, 15, 640},
    {1, Format::video420, 640, 480, 14, 640},
    {1, Format::video420, 640, 480, 31, 640},
    {1, Format::video420, 640, 480, 15, 639},
    {1, Format::video420, 640, 480, 15, SIZE_MAX},
  };
  for (uint32_t sensor : kDiscoveryOrder) {
    Camera camera;
    auto client = camera.attach();
    for (const auto &mode : invalid) {
      assert(camera.start(client, sensor, mode) == Start::invalid);
      expectOff(camera);
    }
    assert(camera.start(client, sensor) == Start::started);
    assert(camera.status().sensor == sensor);
    camera.disconnect(client);   // Cancellation before stream-on.
    assert(camera.quiesce() && camera.finishStop() && camera.status().cleanOff);
    expectOff(camera);
  }

  assert(kDiscoveryOrder.front() == kPianoCameraFront && kDiscoveryOrder.back() == kPianoCameraRear);
  assert(kPianoNativeFps == 15);
  assert(supportsMode(kPianoCameraFront, {0, Format::bgra, 816, 459, 15, 3264}));
  assert(supportsMode(kPianoCameraRear, {0, Format::bgra, 1020, 768, 15, 4080}));
  assert(supportsMode(kPianoCameraFront, {1, Format::video420, 640, 480, 30, 640}));

  PianoIspLayout isp{};
  assert(!pianoIspLayout(kFrontL3Width, kFrontL3Height, 4080, 7490880, 2, &isp));
  assert(pianoIspLayout(kFrontL3Width, kFrontL3Height, UINT32_MAX, kPianoCameraSlotBytes, 2, &isp) == -1);
  assert(!pianoIspLayout(kRearL3Width, kRearL3Height, 5104, 15679488, 2, &isp));
  assert(pianoIspLayout(kRearL3Width, kRearL3Height, 5104, 15679483, 2, &isp) == -1);
  uint8_t sentinel = 0xa5;
  assert(!pianoOutputBGRAto420v(&sentinel, 640, 480, 2560, &sentinel, SIZE_MAX, &sentinel, 640));
  assert(!pianoOutputBGRAto420v(&sentinel, 640, 480, 2560, &sentinel, 640, &sentinel, SIZE_MAX));
  assert(sentinel == 0xa5);
}
}  // namespace

int main()
{
  testRapidSwitch();
  testClientLoss();
  testReleaseTimeout();
  testModes();
  std::puts("cam_switch_privacy_host: all checks passed");
}
