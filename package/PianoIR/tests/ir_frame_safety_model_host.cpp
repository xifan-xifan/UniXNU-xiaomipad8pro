// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I../../PianoTouch -x c++ ir_frame_safety_model_host.cpp -x c ../../PianoTouch/GeniSpi.c -o /tmp/ir_frame_safety_model_host && /tmp/ir_frame_safety_model_host
// The RAM qualification model holds emission even after loopback passes. Its suspend
// and disconnect events exercise teardown of an internal command using host memory.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
extern "C" {
#include "GeniSpi.h"
}
#include "../IrEngine.h"
#include "../PianoIRClient.h"

namespace {
using namespace pianoir;

unsigned checks = 0;
#define CHECK(condition)                                                     \
  do {                                                                       \
    ++checks;                                                                \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);      \
      std::exit(EXIT_FAILURE);                                               \
    }                                                                        \
  } while (false)

constexpr size_t kLoopBytes = 64;
constexpr uint32_t kPollLimit = 4;
constexpr uint64_t kDmaPa = 0x1ffff8000ull;  // The RX half crosses the low address word.
constexpr uint32_t kFound = kEgpioPresent | kEgpioEnable;

enum class Reply { Inactive, BadArgument, NotReady, Held };
enum class Outcome { Echo, Corrupt, ShortRx, NoCompletion, BusError };
enum class Event { Suspend, Disconnect };
enum class Bank { Pin, Engine };

struct Store {
  Bank bank;
  uint32_t reg, value;
};

struct Qualification {
  explicit Qualification(uint64_t bootLevel) : level(bootLevel)
  {
    reg(GENI_FW_REVISION_RO) = GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT | 2;
    reg(GENI_IF_DISABLE_RO) = GENI_FIFO_IF_DISABLE;
    reg(SE_HW_PARAM_0) = 32u << 24 | 16u << 16;
    spi.se = {
      this,
      [](void *ctx, uint32_t off) { return static_cast<Qualification *>(ctx)->readSe(off); },
      [](void *ctx, uint32_t off, uint32_t value) { static_cast<Qualification *>(ctx)->writeSe(off, value); },
      [](void *ctx, unsigned) { ++static_cast<Qualification *>(ctx)->waits; },
    };
    spi.wrapper = {this, [](void *ctx, uint32_t off) {
      ++static_cast<Qualification *>(ctx)->loads;
      CHECK(off == QUPV3_HW_VER_REG);
      return 0x30090000u;
    }, nullptr, nullptr};
  }

  const uint64_t level;
  uint32_t gpioCtl = kFound, gpioLatch = 2;
  unsigned loads = 0, waits = 0;
  bool surveyed = false, verified = false, suspended = false, disconnected = false;
  bool dmaOwned = false, dmaReusable = true, resetRx = true, stuckPad = false;
  Outcome outcome = Outcome::Echo;
  geni_spi spi = {};
  std::vector<Store> stores;
  alignas(4096) uint8_t dma[2 * IR_MAX_WAVE_BYTES] = {};
  std::array<uint32_t, kSe15Bytes / sizeof(uint32_t)> registers = {};

  uint32_t &reg(uint32_t off) { return registers.at(off / sizeof(uint32_t)); }

  uint32_t pinIo() const
  {
    return gpioLatch | (stuckPad || (pinOutput(gpioCtl) && gpioLatch != 0) ? 1u : 0u);
  }

  bool parked() const { return pinOutput(gpioCtl) && mosiDark(gpioCtl, pinIo()); }

  bool park()
  {
    return parkEmitter(kFound, [this](uint32_t off) {
      ++loads;
      return off == kCtl ? gpioCtl : pinIo();
    }, [this](uint32_t off, uint32_t value) {
      stores.push_back({Bank::Pin, off, value});
      (off == kCtl ? gpioCtl : gpioLatch) = value;
    });
  }

  uint32_t readSe(uint32_t off)
  {
    ++loads;
    return reg(off);
  }

  void writeSe(uint32_t off, uint32_t value)
  {
    stores.push_back({Bank::Engine, off, value});
    reg(off) = value;
    switch (off) {
    case SE_GENI_M_CMD0:
      CHECK(parked());
      reg(SE_GENI_STATUS) = GENI_STATUS_M_ACTIVE;
      break;
    case SE_DMA_TX_LEN:
      completeDma(value);
      break;
    case SE_GENI_M_CMD_CTRL_REG:
      CHECK(parked());
      if (value == GENI_M_CMD_ABORT) {
        reg(SE_GENI_STATUS) = 0;
        reg(SE_GENI_M_IRQ_STATUS) |= GENI_M_CMD_ABORT_DONE;
      }
      break;
    case SE_GENI_M_IRQ_CLEAR: reg(SE_GENI_M_IRQ_STATUS) &= ~value; break;
    case SE_DMA_TX_IRQ_CLR: reg(SE_DMA_TX_IRQ_STAT) &= ~value; break;
    case SE_DMA_RX_IRQ_CLR: reg(SE_DMA_RX_IRQ_STAT) &= ~value; break;
    case SE_DMA_TX_FSM_RST: reg(SE_DMA_TX_IRQ_STAT) |= GENI_DMA_RESET_DONE; break;
    case SE_DMA_RX_FSM_RST:
      if (resetRx) reg(SE_DMA_RX_IRQ_STAT) |= GENI_DMA_RESET_DONE;
      break;
    default: break;
    }
  }

  void completeDma(uint32_t bytes)
  {
    CHECK(parked() && dmaOwned && !dmaReusable);
    CHECK(reg(SE_SPI_LOOPBACK) == GENI_LOOPBACK_ENABLE);
    CHECK(reg(SE_GENI_DMA_MODE_EN) & GENI_DMA_MODE_EN);
    CHECK(reg(SE_GENI_M_CMD0) == GENI_M_CMD_TX_RX << GENI_M_OPCODE_SHIFT);
    CHECK(bytes == kLoopBytes && reg(SE_DMA_RX_LEN) == bytes);
    CHECK(reg(SE_SPI_TX_TRANS_LEN) == bytes && reg(SE_SPI_RX_TRANS_LEN) == bytes);
    CHECK((uint64_t(reg(SE_DMA_TX_PTR_H)) << 32 | reg(SE_DMA_TX_PTR_L)) == kDmaPa);
    CHECK((uint64_t(reg(SE_DMA_RX_PTR_H)) << 32 | reg(SE_DMA_RX_PTR_L)) == kDmaPa + IR_MAX_WAVE_BYTES);
    if (outcome == Outcome::NoCompletion) return;
    if (outcome == Outcome::BusError) {
      reg(SE_DMA_TX_IRQ_STAT) = GENI_DMA_AHB_ERR;
      return;
    }
    const size_t received = bytes - (outcome == Outcome::ShortRx ? 1 : 0);
    auto *rx = dma + IR_MAX_WAVE_BYTES;
    std::copy_n(dma, received, rx);
    if (outcome == Outcome::Corrupt) rx[bytes - 1] ^= 1;
    reg(SE_DMA_TX_LEN_IN) = bytes;
    reg(SE_DMA_RX_LEN_IN) = uint32_t(received);
    reg(SE_DMA_TX_IRQ_STAT) = reg(SE_DMA_RX_IRQ_STAT) = GENI_DMA_DONE;
    reg(SE_GENI_STATUS) = 0;
  }

  void survey()
  {
    if (!piano_ir_enabled(level)) return;
    geni_spi probe = {};
    probe.se = {this, spi.se.rd, nullptr, nullptr};
    probe.wrapper = spi.wrapper;
    surveyed = geni_spi_read_info(&probe) == 0 && probe.info.protocol == GENI_PROTO_SPI &&
               probe.info.fifo_disabled && mosiDark(gpioCtl, pinIo());
  }

  TransferEnd finish(int status)
  {
    const auto end = finishTransfer(spi, status, kLoopBytes, true, false,
                                    [this](uint32_t off) { return readSe(off); },
                                    [this](uint32_t off, uint32_t value) { writeSe(off, value); },
                                    [this] { ++waits; }, [this] { return park(); });
    dmaReusable = end.reusable;
    return end;
  }

  int loopback()
  {
    if (level != 2 || !surveyed || suspended || disconnected || !dmaReusable || !park()) return GENI_ERR_IO;
    spi.use_dma = spi.poll_only = 1;
    spi.poll_limit = kPollLimit;
    CHECK(setDmaBuffers(spi.dma, dma, kDmaPa, sizeof dma));
    CHECK(spi.dma.tx_virt == dma && spi.dma.rx_virt == dma + IR_MAX_WAVE_BYTES);
    dmaOwned = true;
    CHECK(geni_spi_init(&spi) == 0);
    constexpr uint32_t source = 19200000;
    ir_clock_pick clock = {};
    CHECK(ir_pick_clock(&source, 1, &clock) && clock.rate == IR_TARGET_RATE);
    CHECK(setSampleClock(spi, clock, source) == 0);
    CHECK(beginLoopback(spi, [this] { return park(); }));

    std::array<uint8_t, kLoopBytes> tx = {}, rx = {};
    for (size_t i = 0; i + 1 < tx.size(); ++i) tx[i] = uint8_t(0xa5 ^ (i * 37));
    dmaReusable = false;
    const int status = geni_spi_transfer(&spi, tx.data(), rx.data(), tx.size(), 0);
    const auto end = finish(status);
    if (end.reusable) CHECK(geni_spi_setup(&spi, 0, 0) == 0);
    verified = !end.status && tx == rx;
    if (end.status) return end.status;
    return verified ? 0 : GENI_ERR_IO;
  }

  Reply request(const uint32_t *durations, size_t bytes, uint64_t carrier = IR_DEFAULT_CARRIER,
                uint64_t duty = IR_DEFAULT_DUTY) const
  {
    if (level != 2) return Reply::Inactive;
    const size_t count = bytes / sizeof(uint32_t);
    if (!durations || bytes % sizeof(uint32_t) || !(count & 1) || count > IR_MAX_DURATIONS ||
        !ir_carrier_valid(carrier) || !ir_duty_valid(duty)) return Reply::BadArgument;
    if (!verified || suspended || disconnected) return Reply::NotReady;
    ir_wave_plan plan = {};
    const int result = ir_wave_check(durations, unsigned(count), uint32_t(carrier), uint32_t(duty), IR_TARGET_RATE, &plan);
    return result == IR_OK ? Reply::Held : Reply::BadArgument;
  }

  void interrupt(Event event)
  {
    suspended |= event == Event::Suspend;
    disconnected |= event == Event::Disconnect;
    if (dmaOwned && !dmaReusable) finish(GENI_ERR_IO);
    if (dmaReusable) {
      spi.dma = {};
      dmaOwned = false;
    }
  }
};

unsigned storesOf(const Qualification &q, Bank bank, uint32_t reg, uint32_t value)
{
  return unsigned(std::count_if(q.stores.begin(), q.stores.end(), [=](const Store &s) {
    return s.bank == bank && s.reg == reg && s.value == value;
  }));
}

void checkEmissionHold(const Qualification &q, unsigned loopbacks)
{
  unsigned commands = 0;
  CHECK(kGpio[kMosi] == 29 && pinFunction(q.gpioCtl) == kFunctionGpio);
  for (const auto &s : q.stores) {
    if (s.bank == Bank::Pin && s.reg == kCtl) CHECK(pinFunction(s.value) == kFunctionGpio);
    if (s.bank == Bank::Pin && s.reg == kIo) CHECK(s.value == 0);
    if (s.bank == Bank::Engine && s.reg == SE_GENI_M_CMD0) {
      ++commands;
      CHECK(s.value == GENI_M_CMD_TX_RX << GENI_M_OPCODE_SHIFT);
    }
  }
  CHECK(commands == loopbacks);
}

void testLevels()
{
  constexpr uint32_t raw[] = {562, 562, 562}, zero[] = {562, 0, 562};
  CHECK(piano_ir_boot_test_count(0) == 0);
  Qualification off(0);
  off.survey();
  CHECK(off.request(raw, sizeof raw) == Reply::Inactive && off.loopback() == GENI_ERR_IO);
  CHECK(!off.surveyed && off.loads == 0 && off.stores.empty() && !off.dmaOwned);
  checkEmissionHold(off, 0);

  Qualification survey(1);
  survey.survey();
  CHECK(survey.request(raw, sizeof raw) == Reply::Inactive && survey.loopback() == GENI_ERR_IO);
  CHECK(survey.surveyed && survey.loads > 0 && survey.stores.empty() && !survey.dmaOwned);
  CHECK(survey.gpioCtl == kFound && survey.gpioLatch == 2);
  checkEmissionHold(survey, 0);

  Qualification ram(2);
  CHECK(ram.request(raw, sizeof raw) == Reply::NotReady && ram.stores.empty());
  ram.survey();
  CHECK(ram.surveyed && ram.stores.empty());
  CHECK(ram.request(raw, sizeof raw - 1) == Reply::BadArgument);
  CHECK(ram.request(raw, sizeof raw) == Reply::NotReady && ram.stores.empty());
  CHECK(ram.loopback() == 0 && ram.verified && ram.parked());
  CHECK(ram.spi.last_tx_in == 64 && ram.spi.last_rx_in == 64 && ram.dmaReusable);
  CHECK(ram.reg(SE_SPI_LOOPBACK) == 0);
  const size_t afterLoopback = ram.stores.size();
  CHECK(ram.request(raw, sizeof raw) == Reply::Held);
  CHECK(ram.request(zero, sizeof zero) == Reply::BadArgument);
  CHECK(ram.request(raw, 2 * sizeof(uint32_t)) == Reply::BadArgument);
  CHECK(ram.request(raw, sizeof raw, (1ull << 32) + 38000) == Reply::BadArgument);
  CHECK(ram.request(raw, sizeof raw, 38000, (1ull << 32) + 40) == Reply::BadArgument);
  CHECK(ram.stores.size() == afterLoopback && ram.parked());
  checkEmissionHold(ram, 1);
}

void testWaveLimits()
{
  constexpr uint32_t mark = 1000;
  ir_wave_plan plan = {};
  for (uint32_t hz : {30000u, 38000u, 56000u}) {
    for (uint32_t duty : {10u, 40u, 70u}) {
      CHECK(ir_wave_check(&mark, 1, hz, duty, 1920000, &plan) == IR_OK);
      // Whole carrier periods round the high time down to a whole sample.
      CHECK(plan.high * 100 <= plan.period * duty && (plan.high + 1) * 100 > plan.period * duty);
    }
  }
  for (uint32_t hz : {0u, 20000u, 29999u, 56001u, 60000u, UINT32_MAX})
    CHECK(ir_wave_check(&mark, 1, hz, 40, 1920000, &plan) == IR_ERR_CARRIER);
  for (uint32_t duty : {0u, 9u, 71u, 100u, UINT32_MAX})
    CHECK(ir_wave_check(&mark, 1, 38000, duty, 1920000, &plan) == IR_ERR_DUTY);

  const auto check = [&](const uint32_t *d, unsigned n) { return ir_wave_check(d, n, 38000, 40, 1000000, &plan); };
  std::array<uint32_t, 1025> many;
  many.fill(1);
  CHECK(check(many.data(), 0) == IR_ERR_COUNT);
  CHECK(check(many.data(), 1023) == IR_OK && plan.marks == 512);
  CHECK(check(many.data(), 1024) == IR_ERR_COUNT && check(many.data(), 1025) == IR_ERR_COUNT);
  for (size_t i = 0; i < 3; ++i) {
    uint32_t durations[] = {1, 1, 1};
    durations[i] = 0;
    CHECK(check(durations, 3) == IR_ERR_ZERO);
    durations[i] = UINT32_MAX;
    CHECK(check(durations, 3) == IR_ERR_LONG);
  }
  uint32_t frame[] = {200000, 100000, 200000};
  CHECK(check(frame, 3) == IR_ERR_SIZE);  // Exactly 500 ms, but too large for one DMA transfer.
  ++frame[2];
  CHECK(check(frame, 3) == IR_ERR_LONG);
  CHECK(piano_ir_burst_valid(1) && piano_ir_burst_valid(500));
  CHECK(!piano_ir_burst_valid(0) && !piano_ir_burst_valid(501) && !piano_ir_burst_valid((1ull << 32) + 1));
  for (const uint32_t rate : {1000000u, 1920000u, 4000000u}) {
    uint32_t left = 500;
    const uint32_t limit = rate == 4000000 ? 65 : 120;
    while (left) {
      const uint32_t part = ir_burst_part_ms(left, rate);
      CHECK(part == std::min(left, limit));
      const uint32_t duration = part * 1000;
      CHECK(ir_wave_check(&duration, 1, 38000, 40, rate, &plan) == IR_OK);
      left -= part;
    }
  }
}

void testPacking()
{
  constexpr uint32_t raw[] = {9, 23, 1};  // At 1 MHz, the last mark starts on the second word.
  constexpr std::array<uint8_t, 12> expected = {0xff, 0x80, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
  std::array<uint8_t, IR_MAX_WAVE_BYTES + 1> bytes;
  bytes.fill(0xa5);
  ir_wave_plan plan = {};
  const auto encode = [&](const uint32_t *d, unsigned count, size_t cap) {
    return ir_wave_encode(d, count, 40000, 40, 1000000, bytes.data(), cap, &plan);
  };
  CHECK(encode(raw, 3, 11) == IR_ERR_SIZE && bytes.front() == 0xa5);
  CHECK(encode(raw, 3, 12) == 12 && plan.samples == 33 && plan.high_samples == 10);
  CHECK(std::equal(expected.begin(), expected.end(), bytes.begin()) && bytes[12] == 0xa5);
  constexpr uint32_t burst = 1000;
  CHECK(ir_wave_encode(&burst, 1, 50000, 70, 1000000, bytes.data(), bytes.size(), &plan) == 132);
  CHECK(plan.period == 20 && plan.high == 14 && plan.high_samples == 700);
  CHECK(ir_wave_bit(bytes.data(), 13) && !ir_wave_bit(bytes.data(), 14) && ir_wave_bit(bytes.data(), 20));
  uint32_t mark = 262112;  // 32 KiB less the final 32-bit zero word, at one sample per µs.
  CHECK(encode(&mark, 1, IR_MAX_WAVE_BYTES) == 32768 && plan.samples == mark);
  CHECK(std::all_of(bytes.end() - 5, bytes.end() - 1, [](uint8_t byte) { return byte == 0; }));
  CHECK(bytes.back() == 0xa5);
  ++mark;
  CHECK(encode(&mark, 1, bytes.size()) == IR_ERR_SIZE && bytes.back() == 0xa5);

  uint32_t nec[IR_NEC_ENTRIES];
  CHECK(ir_nec_frame(0x00, 0x45, nec, IR_NEC_ENTRIES - 1) == IR_ERR_COUNT);
  CHECK(ir_nec_frame(0x10000, 0x45, nec, IR_NEC_ENTRIES) == IR_ERR_COUNT);
  CHECK(ir_nec_frame(0x00, 0x45, nec, IR_NEC_ENTRIES) == 67);
  CHECK(ir_wave_encode(nec, 67, 38000, 40, 1920000, bytes.data(), bytes.size(), &plan) == 16332);
  CHECK(plan.total_us == 68030 && plan.marks == 34 && plan.period == 51 && plan.high == 20);
  CHECK(ir_nec_repeat(nec, IR_NEC_REPEAT_ENTRIES) == 3);
  CHECK(ir_wave_encode(nec, 3, 38000, 40, 1920000, bytes.data(), bytes.size(), &plan) == 2840);
  CHECK(plan.total_us == 11812 && plan.marks == 2);
}

void testLoopbackFailures()
{
  constexpr uint32_t mark = 562;
  for (const Outcome outcome : {Outcome::Corrupt, Outcome::ShortRx, Outcome::NoCompletion, Outcome::BusError}) {
    Qualification ram(2);
    ram.survey();
    ram.outcome = outcome;
    CHECK(ram.loopback() == (outcome == Outcome::NoCompletion ? GENI_ERR_TIMEOUT : GENI_ERR_IO));
    CHECK(!ram.verified && ram.parked() && ram.dmaReusable);
    CHECK(ram.request(&mark, sizeof mark) == Reply::NotReady);
    if (outcome == Outcome::Corrupt)
      CHECK(ram.spi.last_tx_in == 64 && ram.spi.last_rx_in == 64);
    if (outcome == Outcome::ShortRx)
      CHECK(ram.spi.last_tx_in == 64 && ram.spi.last_rx_in == 63);
    if (outcome == Outcome::NoCompletion || outcome == Outcome::BusError) {
      CHECK(ram.spi.snap_valid && (ram.spi.snap_status & GENI_STATUS_M_ACTIVE));
      CHECK(storesOf(ram, Bank::Engine, SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT) == 1);
      CHECK(ram.waits == (outcome == Outcome::NoCompletion ? 2 * kPollLimit : kPollLimit));
    }
    checkEmissionHold(ram, 1);
  }

  Qualification highPad(2);
  highPad.survey();
  highPad.stuckPad = true;
  CHECK(highPad.loopback() == GENI_ERR_IO && !highPad.verified && !highPad.dmaOwned);
  CHECK(highPad.stores.size() == 2);
  checkEmissionHold(highPad, 0);
}

void testInterrupt(Event event, bool resetRx)
{
  Qualification ram(2);
  ram.survey();
  CHECK(ram.loopback() == 0);

  // Place the modeled engine at an in-flight internal DMA command. The event
  // handler must abort it before polling for idle or recycling its pages.
  ram.reg(SE_GENI_STATUS) = GENI_STATUS_M_ACTIVE;
  ram.reg(SE_DMA_RX_IRQ_STAT) = GENI_DMA_RESET_DONE;  // stale acknowledgment
  ram.dmaReusable = false;
  ram.resetRx = resetRx;
  const size_t first = ram.stores.size();
  ram.interrupt(event);
  CHECK(ram.stores.size() == first + 7);
  const auto &latch = ram.stores[first], &mux = ram.stores[first + 1], &abort = ram.stores[first + 2];
  CHECK(latch.bank == Bank::Pin && latch.reg == kIo && latch.value == 0);
  CHECK(mux.bank == Bank::Pin && mux.reg == kCtl && pinOutput(mux.value));
  CHECK(abort.bank == Bank::Engine && abort.reg == SE_GENI_M_CMD_CTRL_REG && abort.value == GENI_M_CMD_ABORT);
  CHECK(ram.parked() && !(ram.reg(SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE));
  CHECK(ram.waits == (resetRx ? 0u : 5000u));
  CHECK(ram.dmaReusable == resetRx && ram.dmaOwned == !resetRx);
  CHECK(ram.spi.dma.tx_virt == (resetRx ? nullptr : ram.dma));
  CHECK(ram.spi.dma.rx_virt == (resetRx ? nullptr : ram.dma + IR_MAX_WAVE_BYTES));
  const uint32_t mark = 562;
  CHECK(ram.request(&mark, sizeof mark) == Reply::NotReady);
  checkEmissionHold(ram, 1);
}
}  // namespace

int main()
{
  testLevels();
  testWaveLimits();
  testPacking();
  testLoopbackFailures();
  for (const Event event : {Event::Suspend, Event::Disconnect}) {
    testInterrupt(event, true);
    testInterrupt(event, false);
  }
  std::printf("PianoIR frame safety model: %u checks, 0 failures\n", checks);
}
