// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -working-directory "${touch_stress_obj:=$(mktemp -d "${TMPDIR:-/tmp}/touch-stress.XXXXXXXX")}" -c "$PWD/../GeniSpi.c" && trap 'rm -r "$touch_stress_obj"' EXIT && c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. touch_mt_gesture_stress_host.cpp "$touch_stress_obj/GeniSpi.o" -o /tmp/touch_mt_gesture_stress_host && /tmp/touch_mt_gesture_stress_host
// Synthetic THP bursts through the real SE DMA transfer, matrix decoder, tracker and native HID router.
// The pointer gesture tests drive the same engine with exact pixel positions. Native mode bypasses that engine.
extern "C" {
#include "GeniSpi.h"
}
#include "TouchGesture.h"
#include "TouchNative.h"
#include "TouchPower.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <utility>

using namespace pianotouch;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

constexpr unsigned kRows = 40, kCols = 60;
constexpr unsigned kFingers = 10;
using Burst = std::array<uint8_t, kTransportBytes + kDefaultPayload>;
struct Peak { unsigned row, col; };

void seal(Burst &burst)
{
  uint8_t *p = burst.data() + kTransportBytes;
  uint32_t sum = 0;
  for (unsigned i = 20; i < kDefaultPayload; i += 2) sum += le16(p + i);
  const uint16_t checksum = uint16_t(0u - sum);
  const uint32_t words = (kDefaultPayload - 20) / 4;
  put16(p + 4, checksum);
  put32(p + 8, words);
  put16(p + 12, uint16_t(~checksum));
  put32(p + 16, ~words);
}

Burst scene(uint16_t frame, std::span<const Peak> peaks = {}, uint8_t type = 1)
{
  Burst b{};
  b[1] = 1;
  uint8_t *p = b.data() + kTransportBytes;
  p[0] = 0xa5;
  put16(p + 2, frame);
  put16(p + 28, frame);
  p[24] = 2;
  p[48] = kCols;
  p[49] = kRows;
  p[56] = type;
  for (unsigned n = 0; n < kRows * kCols; ++n) put16(p + 64 + 2 * n, 1000);
  for (const Peak &peak : peaks) {
    CHECK(peak.row < kRows && peak.col < kCols);
    put16(p + 64 + 2 * (peak.row * kCols + peak.col), 1600);
  }
  seal(b);
  return b;
}

std::array<Peak, kFingers> fingers(unsigned dy = 0)
{
  std::array<Peak, kFingers> p;
  for (unsigned i = 0; i < p.size(); ++i) p[i] = {4 + 19 * (i / 5) + dy, 4 + 12 * (i % 5)};
  return p;
}

// Only the running controller's event page and the SE DMA registers used for these transfers.
struct DmaPanel {
  std::array<uint32_t, 0x1000 / 4> regs{};
  std::array<uint8_t, kMaxFrame> tx{}, rx{};
  std::span<const uint8_t> event;
  geni_spi spi{};
  uint32_t page = 0;
  unsigned transfers = 0, cancels = 0, aborts = 0, resets = 0;
  int result = 0;
  bool penScanning = true;
  std::array<std::pair<uint8_t, uint16_t>, 3> commands{};
  unsigned commandCount = 0;

  DmaPanel()
  {
    reg(GENI_FW_REVISION_RO) = GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT;
    reg(GENI_IF_DISABLE_RO) = GENI_FIFO_IF_DISABLE;
    reg(SE_HW_PARAM_0) = (32u << 24) | (16u << 16);
    spi.se = {this, [](void *ctx, uint32_t r) { return static_cast<DmaPanel *>(ctx)->reg(r); },
              [](void *ctx, uint32_t r, uint32_t v) { static_cast<DmaPanel *>(ctx)->writeReg(r, v); }, nullptr};
    spi.wrapper.rd = [](void *, uint32_t r) { CHECK(r == QUPV3_HW_VER_REG); return 0x40010000u; };
    spi.use_dma = 1;
    spi.poll_limit = 8;
    spi.dma = {nullptr, nullptr, nullptr, nullptr, tx.data(), rx.data(),
               uintptr_t(tx.data()), uintptr_t(rx.data()), tx.size()};
    CHECK(geni_spi_init(&spi) == 0);
    CHECK(geni_spi_set_clock(&spi, 9600000, 100000000, 0) == 0);
    CHECK(geni_spi_setup(&spi, 0, 0) == 0);
  }

  static int transfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len)
  {
    auto &p = *static_cast<DmaPanel *>(ctx);
    ++p.transfers;
    p.result = geni_spi_transfer(&p.spi, tx, rx, len, 0);
    return p.result;
  }

  uint32_t &reg(uint32_t r)
  {
    CHECK(r % 4 == 0 && r / 4 < regs.size());
    return regs[r / 4];
  }

  void writeReg(uint32_t r, uint32_t value)
  {
    reg(r) = value;
    switch (r) {
    case SE_DMA_TX_IRQ_CLR: reg(SE_DMA_TX_IRQ_STAT) &= ~value; break;
    case SE_DMA_RX_IRQ_CLR: reg(SE_DMA_RX_IRQ_STAT) &= ~value; break;
    case SE_GENI_M_IRQ_CLEAR: reg(SE_GENI_M_IRQ_STATUS) &= ~value; break;
    case SE_DMA_TX_LEN: exchange(value); break;
    case SE_GENI_M_CMD_CTRL_REG:
      if (value == GENI_M_CMD_CANCEL) ++cancels;
      if (value == GENI_M_CMD_ABORT) {
        ++aborts;
        reg(SE_GENI_M_IRQ_STATUS) = GENI_M_CMD_ABORT_DONE;
      }
      break;
    case SE_DMA_TX_FSM_RST: case SE_DMA_RX_FSM_RST: ++resets; break;
    default: break;
    }
  }

  void exchange(unsigned len)
  {
    CHECK(len > 0 && len <= tx.size());
    CHECK(reg(SE_GENI_DMA_MODE_EN) & GENI_DMA_MODE_EN);
    CHECK(reg(SE_SPI_TX_TRANS_LEN) == len);
    reg(SE_DMA_TX_LEN_IN) = len;
    reg(SE_DMA_TX_IRQ_STAT) = GENI_DMA_DONE;
    const unsigned cmd = reg(SE_GENI_M_CMD0) >> GENI_M_OPCODE_SHIFT;
    if (tx[0] == 0xff) {
      CHECK(cmd == GENI_M_CMD_TX_ONLY && len == 3);
      page = uint32_t(tx[1]) << 15 | uint32_t(tx[2]) << 7;
      return;
    }
    const uint32_t addr = page + (tx[0] & 0x7f);
    if (cmd == GENI_M_CMD_TX_ONLY) {
      CHECK(addr == kEventCascadeE + kEvHostCmd && len == 6 && tx[1] == 0xbf && tx[3] == 0);
      CHECK(commandCount < commands.size());
      commands[commandCount++] = {tx[2], le16(tx.data() + 4)};
      if (tx[2] == kCmdStylus) penScanning = le16(tx.data() + 4) != 0;
      return;
    }

    CHECK(cmd == GENI_M_CMD_TX_RX && len >= 3 && reg(SE_DMA_RX_LEN) == len && reg(SE_SPI_RX_TRANS_LEN) == len);
    unsigned received = len;
    if (addr == kEventCascadeE) {
      const unsigned n = std::min<unsigned>(len - 2, event.size());
      rx[0] = rx[1] = 0;
      std::copy_n(event.begin(), n, rx.begin() + 2);
      received = n + 2;
    } else {
      std::fill_n(rx.begin(), len, 0);   // host command status is zero once accepted
      if (addr == kEventCascadeE + kEvFwInfo) {
        const uint8_t info[] = {0x21, 0xde, kCols, kRows};
        CHECK(len >= 2 + sizeof info);
        std::copy_n(info, sizeof info, rx.begin() + 2);
      } else if (addr == kEventCascadeE + kEvResetComplete) {
        rx[2] = kResetStateCalibrated;
      } else {
        CHECK(addr == kEventCascadeE + kEvHostCmd);
      }
    }
    reg(SE_DMA_RX_LEN_IN) = received;
    if (received == len) reg(SE_DMA_RX_IRQ_STAT) = GENI_DMA_DONE;
  }
};

enum class ReadResult { parked, transportError, badChecksum, skipped, learning, otherType, decoded };

struct NativeRig {
  DmaPanel panel;
  std::array<uint8_t, kMaxFrame> tx{}, rx{};
  Burst frame{};
  Chip chip;
  std::unique_ptr<Matrix> matrix = std::make_unique<Matrix>();
  Tracker tracker;
  NativeRouter native;
  NativeOut out;
  TouchPower power;
  PanelPark park;
  uint64_t now = 0;

  NativeRig()
  {
    chip.bus = {&panel, DmaPanel::transfer, nullptr, kMaxFrame - 2, kMaxFrame - 1};
    chip.tx = tx.data();
    chip.rx = rx.data();
    chip.txBytes = tx.size();
    chip.rxBytes = rx.size();
    chip.eventBuf = kEventCascadeE;
    CHECK(start(chip, true, 2) == kOk);
    CHECK(chip.calibrated && chip.fwInfoValid && frameBytes(chip) == frame.size());
    const std::array<std::pair<uint8_t, uint16_t>, 3> expected{{{0x04, 0}, {0x19, 1}, {0x01, 2}}};
    CHECK(chip.stylusRc == kOk && panel.commandCount == expected.size() && panel.commands == expected && !panel.penScanning);
    matrixReset(*matrix, {});
    trackerReset(tracker, {});
    nativeReset(native, true);
  }

  ReadResult poll(std::span<const uint8_t> burst)
  {
    now += 8;
    out = {};
    if (park.state() != PanelPark::released || !power.reading()) return ReadResult::parked;
    CHECK(!burst.empty());
    panel.event = burst.subspan(1);   // chip read inserts the frame's leading zero
    const int rc = readFrame(chip, frame.data());
    panel.event = {};
    if (rc != kOk) {
      lose();
      return ReadResult::transportError;
    }
    if (frameKind(frame.data()) != FrameKind::data) return ReadResult::skipped;
    switch (matrixFeed(*matrix, frame.data() + kTransportBytes, chip.payload)) {
    case Feed::badChecksum: return ReadResult::badChecksum;
    case Feed::skipped: return ReadResult::skipped;
    case Feed::learning: return ReadResult::learning;
    case Feed::otherType: return ReadResult::otherType;
    case Feed::decoded: break;
    }
    if (!power.reporting()) return ReadResult::decoded;

    trackerUpdate(tracker, matrix->points, matrix->pointCount, now);
    std::array<NativeContact, kMaxTracks> c;
    unsigned n = 0;
    for (const Track &t : tracker.t) {
      if (t.state == TrackState::active) c[n++] = {t.id, t.palm, t.x, t.y, t.x, t.y};
    }
    nativeUpdate(native, c.data(), n, kPanelW, kPanelH, kPanelW, kPanelH, out);
    return ReadResult::decoded;
  }

  void learn()
  {
    const auto quiet = scene(0);
    for (unsigned i = 0; i < kProbeFrames + kRefFrames; ++i) {
      CHECK(poll(quiet) == ReadResult::learning);
      CHECK(out.count == 0 && trackerLive(tracker) == 0);
    }
    CHECK(matrix->haveReference);
  }

  void lose()
  {
    trackerCancel(tracker);
    nativeCancel(native, out);
  }

  PowerStep powerPass(bool systemOff, int display)
  {
    out = {};
    if (park.state() == PanelPark::requested) {
      CHECK(power.step(false, 0, true) == PowerStep::mute);
      lose();
      CHECK(park.acknowledge());
      return PowerStep::mute;
    }
    if (park.state() == PanelPark::held && !systemOff) return PowerStep::none;
    const PowerStep step = power.step(systemOff, display, park.state() == PanelPark::held);
    if (step == PowerStep::suspend || step == PowerStep::mute) lose();
    if (step == PowerStep::resume || step == PowerStep::unmute) {
      bool kept = false;
      CHECK(probeState(chip, &kept) == kOk && kept);
      const Restart plan = restartPlan(step == PowerStep::resume, kept, 0);
      CHECK(plan == (step == PowerStep::resume ? Restart::relearn : Restart::keep));
      if (plan == Restart::relearn) matrixReset(*matrix, {});
      trackerReset(tracker, {});
    }
    return step;
  }
};

ScreenSlot slotAt(const NativeOut &out, unsigned report, unsigned slot)
{
  CHECK(report < out.count && out.count <= kNativeMaxReports && slot < kScreenSlots);
  CHECK(out.report[report][0] == kScreenReportId);
  const uint8_t *p = out.report[report] + 1 + slot * kScreenSlotBytes;
  const ScreenSlot s{p[0], p[1], le16(p + 2), le16(p + 4)};
  CHECK(s.contact == 0 || s.contact == slot + 1);
  CHECK(s.x <= kScreenMax && s.y <= kScreenMax);
  return s;
}

constexpr uint8_t kDownFlags = kSlotValid | kSlotTip | kSlotInRange;
using SlotIds = std::array<uint8_t, kScreenSlots>;

SlotIds slotIds(const NativeRouter &r)
{
  SlotIds ids{};
  for (unsigned i = 0; i < ids.size(); ++i) ids[i] = r.slot[i].track;
  return ids;
}

void checkFull(const NativeRig &r)
{
  CHECK(r.out.count == 1 && trackerLive(r.tracker) == kMaxTracks && trackerActive(r.tracker, true) == kMaxTracks);
  std::array<bool, 256> seen{};
  for (unsigned i = 0; i < kScreenSlots; ++i) {
    const ScreenSlot s = slotAt(r.out, 0, i);
    CHECK(s.flags == kDownFlags && s.contact == i + 1);
    const uint8_t id = r.native.slot[i].track;
    CHECK(id && !seen[id]);
    seen[id] = true;
  }
}

void checkLift(const NativeOut &out, unsigned report, const NativeOut &previous)
{
  for (unsigned i = 0; i < kScreenSlots; ++i) {
    const ScreenSlot lift = slotAt(out, report, i), was = slotAt(previous, 0, i);
    CHECK(lift.flags == kSlotValid && lift.contact == was.contact && lift.x == was.x && lift.y == was.y);
  }
}

void checkCancelled(const NativeRig &r, const NativeOut &previous)
{
  CHECK(r.out.count == 2 && nativeIdle(r.native) && trackerLive(r.tracker) == 0);
  CHECK(r.tracker.noteCount == kMaxTracks);
  for (unsigned i = 0; i < kMaxTracks; ++i) CHECK(r.tracker.notes[i].kind == TrackNoteKind::up);
  for (unsigned i = 0; i < kScreenSlots; ++i) {
    const ScreenSlot cancel = slotAt(r.out, 0, i), was = slotAt(previous, 0, i);
    CHECK(cancel.flags == (kDownFlags | kSlotUntouch) && cancel.contact == was.contact && cancel.x == was.x && cancel.y == was.y);
  }
  checkLift(r.out, 1, previous);
}

void testContactChurn()
{
  static_assert(kMaxTracks == 8 && kScreenSlots == 8);
  NativeRig r;
  r.learn();
  SlotIds previous{};
  bool wrapped = false;
  constexpr unsigned cycles = 40;   // enough new contacts to wrap the tracker's 8-bit IDs
  for (unsigned cycle = 0; cycle < cycles; ++cycle) {
    const uint16_t firstFrame = uint16_t(cycle * 5 + 1);
    const unsigned full = r.tracker.noSlot;
    CHECK(r.poll(scene(firstFrame, fingers())) == ReadResult::decoded);
    CHECK(r.matrix->pointCount == kFingers && r.tracker.noSlot == full + kFingers - kMaxTracks);
    checkFull(r);
    const auto down = r.out;
    const auto ids = slotIds(r.native);
    for (unsigned i = 0; i < ids.size(); ++i) CHECK(ids[i] != previous[i]);
    wrapped = wrapped || (cycle > 0 && ids[0] < previous[0]);
    previous = ids;

    CHECK(r.poll(scene(firstFrame + 1, fingers(1))) == ReadResult::decoded);
    checkFull(r);
    CHECK(r.matrix->pointCount == kFingers && slotIds(r.native) == ids);
    CHECK(r.tracker.noSlot == full + 2 * (kFingers - kMaxTracks));
    for (unsigned i = 0; i < kScreenSlots; ++i) {
      const ScreenSlot a = slotAt(down, 0, i), b = slotAt(r.out, 0, i);
      CHECK(a.contact == b.contact && a.x == b.x && b.y > a.y);
    }
    const auto moved = r.out;
    CHECK(r.tracker.cfg.hold == 3);
    for (unsigned gap = 2; gap < 4; ++gap) {
      CHECK(r.poll(scene(firstFrame + gap)) == ReadResult::decoded);
      checkFull(r);
      CHECK(slotIds(r.native) == ids);
    }
    CHECK(r.poll(scene(firstFrame + 4)) == ReadResult::decoded);
    CHECK(r.out.count == 1 && nativeIdle(r.native) && trackerLive(r.tracker) == 0);
    checkLift(r.out, 0, moved);
    // The very next frame can use every freed slot.
  }
  CHECK(wrapped && r.tracker.downs == cycles * kMaxTracks && r.tracker.ups == r.tracker.downs);
  CHECK(r.native.downs == cycles * kScreenSlots && r.native.ups == r.native.downs && r.native.cancels == 0);
  CHECK(r.poll(scene(cycles * 5 + 1)) == ReadResult::decoded && r.out.count == 0);
}

void testContendedSlotReuse()
{
  NativeRig r;
  r.learn();
  auto all = fingers();
  CHECK(r.poll(scene(1, all)) == ReadResult::decoded);
  checkFull(r);
  const SlotIds before = slotIds(r.native);
  const ScreenSlot first = slotAt(r.out, 0, 0);
  const MatrixPoint &leaving = r.matrix->points[0];   // the first decoded point occupied slot 0
  const auto removed = std::find_if(all.begin(), all.end(), [&](const Peak &p) {
    return p.row == leaving.row && p.col == leaving.col;
  });
  CHECK(removed != all.end());
  std::rotate(removed, removed + 1, all.end());
  const auto remaining = std::span(all).first(kFingers - 1);
  for (unsigned i = 1; i < r.tracker.cfg.hold; ++i) {
    CHECK(r.poll(scene(1 + i, remaining)) == ReadResult::decoded);
    checkFull(r);
    CHECK(slotIds(r.native) == before);
  }
  CHECK(r.poll(scene(4, remaining)) == ReadResult::decoded && r.out.count == 1);
  CHECK(trackerLive(r.tracker) == kMaxTracks && r.tracker.downs == kMaxTracks + 1 && r.tracker.ups == 1);
  CHECK(r.native.downs == kScreenSlots && r.native.ups == 1 && r.native.full == 1);
  const ScreenSlot lift = slotAt(r.out, 0, 0);
  CHECK(lift.flags == kSlotValid && lift.contact == first.contact && lift.x == first.x && lift.y == first.y);
  for (unsigned i = 1; i < kScreenSlots; ++i) CHECK(slotAt(r.out, 0, i).flags == kDownFlags);
  CHECK(r.poll(scene(5, remaining)) == ReadResult::decoded);
  checkFull(r);
  const SlotIds after = slotIds(r.native);
  CHECK(after[0] != before[0] && std::equal(before.begin() + 1, before.end(), after.begin() + 1));
  CHECK(r.native.downs == kScreenSlots + 1 && r.out.noteCount == 1 && r.out.notes[0].kind == NativeNoteKind::down);
}

void testBounds()
{
  for (bool panelAxes : {false, true}) {
    NativeRouter r;
    nativeReset(r, panelAxes);
    NativeOut out;
    NativeContact c{1, false, 0, 0, kPanelW - 1, kPanelH - 1};
    for (bool outside : {false, true}) {
      if (outside) {
        c.x = c.y = std::numeric_limits<int32_t>::min();
        c.px = c.py = std::numeric_limits<int32_t>::max();
      }
      nativeUpdate(r, &c, 1, kPanelW, kPanelH, kPanelW, kPanelH, out);
      const ScreenSlot s = slotAt(out, 0, 0);
      CHECK(s.flags == kDownFlags && s.contact == 1 && s.x == s.y && s.x == (panelAxes ? kScreenMax : 0));
      std::swap(c.x, c.px);
      std::swap(c.y, c.py);
      nativeUpdate(r, &c, 1, kPanelW, kPanelH, kPanelW, kPanelH, out);
      const ScreenSlot opposite = slotAt(out, 0, 0);
      CHECK(opposite.flags == kDownFlags && opposite.contact == 1 && opposite.x == opposite.y &&
            opposite.x == (panelAxes ? 0 : kScreenMax));
    }
    nativeUpdate(r, nullptr, 0, kPanelW, kPanelH, kPanelW, kPanelH, out);
    CHECK(slotAt(out, 0, 0).flags == kSlotValid && nativeIdle(r));
  }
}

void testSlop()
{
  for (int dx : {63, 64, 65}) {
    Gesture g;
    GestureOut out;
    GestureContact c{1, false, 1000, 1000, 2};
    CHECK(g.cfg.slopPx == 64 && g.cfg.dragDelayMs == 80);
    gestureUpdate(g, &c, 1, 0, out);
    CHECK(out.n == 1 && out.r[0].buttons == 0);
    c.x += dx;
    gestureUpdate(g, &c, 1, 8, out);
    CHECK(out.n == 0 && g.pending == (dx > 64));
    gestureUpdate(g, &c, 1, 87, out);
    CHECK(out.n == 0);
    gestureUpdate(g, &c, 1, 88, out);
    if (dx <= 64) {
      CHECK(out.n == 0 && g.mode == GestureMode::one);
      gestureUpdate(g, nullptr, 0, 96, out);
      CHECK(out.n == 2 && out.r[0].buttons == kButtonLeft && out.r[0].x == 1000 && out.r[1].x == 1000 && g.taps == 1);
    } else {
      CHECK(out.n == 2 && g.mode == GestureMode::drag && out.r[0].buttons == kButtonLeft && out.r[0].x == 1000);
      CHECK(out.r[1].buttons == kButtonLeft && out.r[1].x == 1065);
      gestureUpdate(g, nullptr, 0, 96, out);
      CHECK(out.n == 1 && out.r[0].x == 1065 && g.drags == 1 && !g.taps);
    }
    CHECK(out.r[out.n - 1].buttons == 0 && gestureIdle(g));
  }
}

struct PairGesture {
  Gesture g;
  GestureOut out;
  uint64_t now = 0;

  explicit PairGesture(int half)
  {
    GestureConfig cfg;
    cfg.pinch = 1;    // Command + wheel when pointer routing is selected
    gestureReset(g, cfg);
    step(half);
    step(half);
    CHECK(g.mode == GestureMode::two);
  }

  void step(int half, int dx = 0, int dy = 0, unsigned n = 2)
  {
    const GestureContact c[]{{1, false, 1600 + dx - half, 1068 + dy, 2}, {2, false, 1600 + dx + half, 1068 + dy, 2}};
    now += 8;
    gestureUpdate(g, c, n, now, out);
  }

  void checkReport(unsigned i, int wheel, int pan, uint8_t modifiers) const
  {
    CHECK(i < out.n);
    const PointerReport &p = out.r[i];
    CHECK(p.x == 1600 && p.y == 1068 && !p.buttons && p.wheel == wheel && p.pan == pan && p.modifiers == modifiers);
    uint8_t b[kMaxReportBytes];
    CHECK(pointerReport(p, kPanelW, kPanelH, true, b) == 8 && b[0] == kPointerReportId);
    CHECK(b[6] == uint8_t(wheel) && b[7] == uint8_t(pan));
  }
};

void testScroll()
{
  for (bool vertical : {true, false}) {
    PairGesture p(100);
    auto travel = [&](int main, int cross, unsigned n = 2) {
      p.step(100, vertical ? cross : main, vertical ? -main : cross, n);
    };
    travel(64, 0);
    CHECK(p.g.mode == GestureMode::two && p.out.n == 0);
    travel(69, 8);
    CHECK(p.g.mode == GestureMode::scroll && p.g.axis == (vertical ? 1 : 2) && p.out.n == 1);
    p.checkReport(0, vertical ? 17 : 0, vertical ? 0 : 17, 0);
    travel(72, 45);   // the residual pixel and 3 new pixels make one count; cross motion stays locked out
    CHECK(p.out.n == 1);
    p.checkReport(0, vertical ? 1 : 0, vertical ? 0 : 1, 0);
    travel(64, 45);
    CHECK(p.out.n == 1);
    p.checkReport(0, vertical ? -2 : 0, vertical ? 0 : -2, 0);
    travel(64, 45, 0);
    CHECK(gestureIdle(p.g) && p.out.n == 0 && p.out.noteCount == 1 && p.out.notes[0].kind == GestureKind::scrollEnd);
    CHECK(p.out.notes[0].a == (vertical ? 16 : 0) && p.out.notes[0].b == (vertical ? 0 : 16));
  }
}

void testPinch()
{
  for (unsigned survivors : {0u, 1u}) {
    PairGesture p(40);
    p.step(8);    // exactly 64 px of spread change
    CHECK(p.g.mode == GestureMode::two && p.out.n == 0);
    p.step(0);    // coincident fingers, zoom out
    CHECK(p.g.mode == GestureMode::pinch && p.out.n == 2 && p.g.lastD == 0);
    p.checkReport(0, 0, 0, kModifierCommand);
    p.checkReport(1, -5, 0, kModifierCommand);
    uint8_t mod[kMaxReportBytes];
    CHECK(modifierReport(p.out.r[0].modifiers, mod) == 2 && mod[0] == kKeyboardReportId && mod[1] == kModifierCommand);
    p.step(80);   // reverse into zoom in while keeping Command held
    CHECK(p.out.n == 1);
    p.checkReport(0, 10, 0, kModifierCommand);
    p.step(80, 0, 0, survivors);
    CHECK(p.out.n == 1 && p.out.noteCount == 1 && p.out.notes[0].kind == GestureKind::pinchEnd);
    CHECK(p.out.notes[0].a == 80 && p.out.notes[0].b == 5);
    p.checkReport(0, 0, 0, 0);
    CHECK(modifierReport(p.out.r[0].modifiers, mod) == 2 && mod[1] == 0);
    CHECK(p.g.modifiers == 0 && p.g.mode == (survivors ? GestureMode::inert : GestureMode::idle));
    p.step(80, 300, 0, survivors);
    CHECK(p.out.n == 0);
    p.step(80, 0, 0, 0);
    CHECK(gestureIdle(p.g) && p.out.n == 0);
    p.step(80, 0, 0, 1);
    CHECK(p.g.mode == GestureMode::one && p.out.n == 1 && !p.out.r[0].modifiers && !p.out.r[0].buttons);
  }
}

void testSleep()
{
  enum class Sleep { panel, system, both };
  for (const Sleep type : {Sleep::panel, Sleep::system, Sleep::both}) {
    NativeRig r;
    r.learn();
    const auto held = scene(1, fingers());
    CHECK(r.poll(held) == ReadResult::decoded);
    checkFull(r);
    const auto down = r.out;

    if (type == Sleep::system) {
      CHECK(r.powerPass(true, 1) == PowerStep::suspend);
    } else {
      r.park.request();
      CHECK(r.powerPass(false, 1) == PowerStep::mute && r.park.state() == PanelPark::held);
    }
    checkCancelled(r, down);
    const unsigned before = r.panel.transfers;
    for (unsigned i = 0; i < 16; ++i) CHECK(r.poll(held) == ReadResult::parked && r.out.count == 0);
    if (type == Sleep::both) {
      CHECK(r.powerPass(true, 0) == PowerStep::suspend && r.out.count == 0);
      CHECK(r.powerPass(false, 0) == PowerStep::none && r.poll(held) == ReadResult::parked);
    }
    CHECK(r.panel.transfers == before);
    r.park.release();
    if (type != Sleep::panel) CHECK(r.poll(held) == ReadResult::parked && r.panel.transfers == before);
    CHECK(r.powerPass(false, 1) == (type == Sleep::panel ? PowerStep::unmute : PowerStep::resume));
    CHECK(r.power.reporting() && r.power.reading() && r.out.count == 0 && !r.panel.penScanning && r.panel.commandCount == 3);
    if (type != Sleep::panel) r.learn();
    CHECK(r.poll(scene(2)) == ReadResult::decoded && r.out.count == 0);
    CHECK(r.poll(scene(3, fingers(1))) == ReadResult::decoded);
    checkFull(r);
    CHECK(r.out.noteCount == kScreenSlots);
    for (unsigned i = 0; i < r.out.noteCount; ++i) CHECK(r.out.notes[i].kind == NativeNoteKind::down);
  }
}

void testCorruptedBursts()
{
  NativeRig r;
  const auto pen = scene(1, fingers(), 6);
  for (unsigned i = 0; i < kProbeFrames; ++i) CHECK(r.poll(pen) == ReadResult::learning);
  CHECK(!r.panel.penScanning && r.matrix->probeCount == 0 && r.out.count == 0 && trackerLive(r.tracker) == 0);
  r.learn();
  const auto held = scene(2, fingers());
  CHECK(r.poll(held) == ReadResult::decoded);
  checkFull(r);
  const auto original = r.out;
  const auto ids = slotIds(r.native);
  const unsigned trackedFrames = r.tracker.frames;
  const unsigned decodedFrames = r.matrix->frames;
  const unsigned reports = r.native.reports;

  auto expectIgnored = [&](const Burst &b, ReadResult result) {
    CHECK(r.poll(b) == result && r.out.count == 0);
    CHECK(r.tracker.frames == trackedFrames && r.native.reports == reports && slotIds(r.native) == ids);
    CHECK(r.matrix->frames == decodedFrames && trackerLive(r.tracker) == kMaxTracks);
  };
  Burst bad = scene(3);
  bad[kTransportBytes + 64] ^= 1;
  expectIgnored(bad, ReadResult::badChecksum);
  bad.fill(0x00);
  expectIgnored(bad, ReadResult::badChecksum);
  bad.fill(0xff);
  expectIgnored(bad, ReadResult::badChecksum);
  bad = scene(4);
  std::fill(bad.begin() + kTransportBytes + 100, bad.end(), 0x5a);   // short content with a full-length DMA completion
  expectIgnored(bad, ReadResult::badChecksum);
  bad = scene(5);
  bad[kTransportBytes + 48] = 255;
  bad[kTransportBytes + 49] = 255;
  seal(bad);
  expectIgnored(bad, ReadResult::skipped);   // checksum valid, impossible matrix dimensions
  for (uint8_t penType : {6, 7, 9, 0x1d}) expectIgnored(scene(6, fingers(), penType), ReadResult::otherType);

  // THP carries a matrix rather than a contact-count field. A valid burst with 11 peaks reaches the tracker:
  // new unmatched peaks get no slots while the original contacts retain their IDs and coordinates.
  std::array<Peak, kFingers + 1> excess;
  const auto ten = fingers();
  std::copy(ten.begin(), ten.end(), excess.begin());
  excess.back() = {39, 59};
  const unsigned full = r.tracker.noSlot;
  CHECK(r.poll(scene(7, excess)) == ReadResult::decoded && r.matrix->pointCount == 11);
  CHECK(r.tracker.noSlot == full + excess.size() - kMaxTracks && slotIds(r.native) == ids);
  checkFull(r);
  for (unsigned i = 0; i < kScreenSlots; ++i) {
    const ScreenSlot s = slotAt(r.out, 0, i), was = slotAt(original, 0, i);
    CHECK(s.x == was.x && s.y == was.y);
  }
  CHECK(r.poll(scene(8, fingers(1))) == ReadResult::decoded && slotIds(r.native) == ids);
  checkFull(r);
  const auto down = r.out;
  CHECK(r.poll(std::span(held).first(kTransportBytes + 100)) == ReadResult::transportError);
  CHECK(r.panel.result == GENI_ERR_TIMEOUT && r.panel.spi.snap_valid && r.panel.spi.snap_dma_rxin == kTransportBytes + 101);
  CHECK(r.panel.cancels == 1 && r.panel.aborts == 1 && r.panel.resets == 2);
  checkCancelled(r, down);
  CHECK(r.poll(scene(9)) == ReadResult::decoded && r.out.count == 0);
  CHECK(r.poll(scene(10, ten)) == ReadResult::decoded);
  checkFull(r);
  CHECK(slotIds(r.native) != ids && r.out.noteCount == kScreenSlots && !r.panel.penScanning);
}

}  // namespace

int main()
{
  testContactChurn();
  testContendedSlotReuse();
  testBounds();
  testSlop();
  testScroll();
  testPinch();
  testSleep();
  testCorruptedBursts();
  std::puts("PASS: PianoTouch THP contact churn, native HID, pointer gestures, sleep and GENI DMA recovery");
}
