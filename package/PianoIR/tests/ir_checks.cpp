// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoIR's portable parts: IrWave.h (checks, the waveform bit by bit, NEC, the parser, the clock pick), IrHw.h (the
// RCG/DFS decoders, the pin words) and the SE DMA programming of a TX-only send through package/PianoTouch/GeniSpi.c against a
// register model (documented register behaviour, not hardware).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
extern "C" {
#include "GeniSpi.h"
}
#include "IrEngine.h"
#include "PianoIRClient.h"

static int gChecks, gFails;
#define CHECK(c)                                                         \
  do {                                                                   \
    ++gChecks;                                                           \
    if (!(c)) {                                                          \
      ++gFails;                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
    }                                                                    \
  } while (0)

using namespace pianoir;

static void testPlanBounds()
{
  ir_wave_plan p;
  uint32_t one[1] = {1000};
  CHECK(ir_wave_check(one, 0, 38000, 40, 1920000, &p) == IR_ERR_COUNT);
  CHECK(ir_wave_check(nullptr, 0, 38000, 40, 1920000, &p) == IR_ERR_COUNT);
  CHECK(ir_wave_check(nullptr, 1, 38000, 40, 1920000, &p) == IR_ERR_COUNT);
  CHECK(ir_wave_check(one, 1, 38000, 40, 1920000, nullptr) == IR_ERR_COUNT);
  uint32_t two[2] = {1000, 1000};
  CHECK(ir_wave_check(two, 2, 38000, 40, 1920000, &p) == IR_ERR_COUNT);   // even: must end on a mark
  std::vector<uint32_t> many(1025, 10);
  CHECK(ir_wave_check(many.data(), 1025, 38000, 40, 1920000, &p) == IR_ERR_COUNT);
  std::vector<uint32_t> max(1023, 10);
  CHECK(ir_wave_check(max.data(), 1023, 38000, 40, 1920000, &p) == IR_OK);
  uint32_t zero[3] = {100, 0, 100};
  CHECK(ir_wave_check(zero, 3, 38000, 40, 1920000, &p) == IR_ERR_ZERO);
  uint32_t lng[3] = {300000, 100000, 100001};
  CHECK(ir_wave_check(lng, 3, 38000, 40, 1000000, &p) == IR_ERR_LONG);
  lng[2]--;
  CHECK(ir_wave_check(lng, 3, 38000, 40, 1000000, &p) == IR_ERR_SIZE);
  lng[0] = UINT32_MAX;
  CHECK(ir_wave_check(lng, 3, 38000, 40, 1000000, &p) == IR_ERR_LONG);
  CHECK(ir_wave_check(one, 1, 29999, 40, 1920000, &p) == IR_ERR_CARRIER);
  CHECK(ir_wave_check(one, 1, 56001, 40, 1920000, &p) == IR_ERR_CARRIER);
  CHECK(ir_wave_check(one, 1, 30000, 40, 1920000, &p) == IR_OK);
  CHECK(ir_wave_check(one, 1, 56000, 40, 1920000, &p) == IR_OK);
  CHECK(ir_wave_check(one, 1, 38000, 9, 1920000, &p) == IR_ERR_DUTY);
  CHECK(ir_wave_check(one, 1, 38000, 71, 1920000, &p) == IR_ERR_DUTY);
  CHECK(ir_wave_check(one, 1, 38000, 40, 999999, &p) == IR_ERR_RATE);
  CHECK(ir_wave_check(one, 1, 38000, 40, 4000001, &p) == IR_ERR_RATE);
  // 32 KiB: 136 ms fits at 1.92 MHz (261120 samples -> 32644 bytes), 140 ms does not.
  uint32_t m136[1] = {136000}, m140[1] = {140000};
  CHECK(ir_wave_check(m136, 1, 38000, 40, 1920000, &p) == IR_OK && p.bytes == 32644);
  CHECK(ir_wave_check(m140, 1, 38000, 40, 1920000, &p) == IR_ERR_SIZE);
  // the burst slices the kext uses (120 ms) fit
  uint32_t m120[1] = {120000};
  CHECK(ir_wave_check(m120, 1, 38000, 40, 1920000, &p) == IR_OK && p.bytes <= IR_MAX_WAVE_BYTES);
  for (int e = 0; e >= -7; --e) CHECK(std::strcmp(ir_error_name(e), "unknown") != 0);
}

static void testEncodeMark()
{
  std::vector<uint8_t> w(IR_MAX_WAVE_BYTES, 0xee);
  ir_wave_plan p;
  uint32_t d[1] = {1000};
  const int n = ir_wave_encode(d, 1, 38000, 40, 1920000, w.data(), w.size(), &p);
  CHECK(n == 244);                                // 1920 samples -> 60 words + the zero word
  CHECK(p.period == 51 && p.high == 20 && p.samples == 1920 && p.marks == 1 && p.total_us == 1000);
  unsigned bad = 0, highs = 0;
  for (uint32_t i = 0; i < p.samples; ++i) {
    const int want = (i % 51) < 20;
    bad += ir_wave_bit(w.data(), i) != want;
    highs += want;
  }
  CHECK(bad == 0);
  CHECK(p.high_samples == highs);
  for (uint32_t i = p.samples; i < uint32_t(n) * 8; ++i) CHECK(ir_wave_bit(w.data(), i) == 0);   // low after the end
  CHECK(w[n - 1] == 0 && w[n - 2] == 0 && w[n - 3] == 0 && w[n - 4] == 0);
  CHECK(w[0] == 0xff && w[1] == 0xff && w[2] == 0xf0);   // MSB first: samples 0..19 high
  // a refused list leaves no partial transfer: the error, nothing claimed
  CHECK(ir_wave_encode(d, 1, 38000, 40, 1920000, w.data(), 100, &p) == IR_ERR_SIZE);
}

static void testEncodePhaseAndBoundaries()
{
  std::vector<uint8_t> w(IR_MAX_WAVE_BYTES);
  ir_wave_plan p;
  // mark 100 us, space 50 us, mark 100 us at 1 MHz, 40 kHz (period 25, high 10): the phase restarts at the second mark
  uint32_t d[3] = {100, 50, 100};
  CHECK(ir_wave_encode(d, 3, 40000, 40, 1000000, w.data(), w.size(), &p) > 0);
  CHECK(p.samples == 250 && p.period == 25 && p.high == 10);
  for (uint32_t i = 0; i < 100; ++i) CHECK(ir_wave_bit(w.data(), i) == ((i % 25) < 10));
  for (uint32_t i = 100; i < 150; ++i) CHECK(ir_wave_bit(w.data(), i) == 0);
  for (uint32_t i = 150; i < 250; ++i) CHECK(ir_wave_bit(w.data(), i) == (((i - 150) % 25) < 10));
  // cumulative rounding: 7 entries of 1 us at 1.5 MHz end at round(k * 1.5): 2, 3, 5, 6, 8, 9, 11 (no drift)
  uint32_t e[7] = {1, 1, 1, 1, 1, 1, 1};
  CHECK(ir_wave_encode(e, 7, 56000, 70, 1500000, w.data(), w.size(), &p) > 0);
  CHECK(p.samples == 11);
  // marks are entries 0, 2, 4, 6 -> samples [0,2) [3,5) [6,8) [9,11); period round(1.5e6/56e3) = 27, high 18: all high
  const int expect[11] = {1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1};
  for (int i = 0; i < 11; ++i) CHECK(ir_wave_bit(w.data(), uint32_t(i)) == expect[i]);
  // rate scaling: 2.4 MHz, 38 kHz -> period 63, high 25
  uint32_t m[1] = {10000};
  CHECK(ir_wave_encode(m, 1, 38000, 40, 2400000, w.data(), w.size(), &p) > 0);
  CHECK(p.period == 63 && p.high == 25 && p.samples == 24000);
  // the measured carrier: rate / period within 1 % of the request for the default case
  CHECK(ir_wave_check(m, 1, 38000, 40, 1920000, &p) == IR_OK);
  const double carrier = 1920000.0 / p.period;
  CHECK(carrier > 38000 * 0.99 && carrier < 38000 * 1.01);
}

static void testEncodeBufferEdges()
{
  constexpr uint8_t canary = 0xa5;
  constexpr uint32_t fit = (IR_MAX_WAVE_BYTES - 4) * 8;
  for (const uint32_t rate : {IR_MIN_RATE, IR_MAX_RATE}) {
    uint32_t duration = fit / (rate / IR_MIN_RATE);
    std::vector<uint8_t> bytes(IR_MAX_WAVE_BYTES + 2, canary);
    ir_wave_plan p;
    const auto encode = [&](size_t cap) {
      return ir_wave_encode(&duration, 1, IR_MAX_CARRIER, IR_MAX_DUTY, rate, bytes.data() + 1, cap, &p);
    };
    CHECK(encode(IR_MAX_WAVE_BYTES - 1) == IR_ERR_SIZE);
    ++duration;
    CHECK(encode(IR_MAX_WAVE_BYTES) == IR_ERR_SIZE);
    duration = 0;
    CHECK(encode(IR_MAX_WAVE_BYTES) == IR_ERR_ZERO);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == canary; }));
    duration = fit / (rate / IR_MIN_RATE);
    CHECK(encode(IR_MAX_WAVE_BYTES) == IR_MAX_WAVE_BYTES);
    CHECK(bytes.front() == canary && bytes.back() == canary && p.samples == fit);
    CHECK(std::all_of(bytes.end() - 5, bytes.end() - 1, [](uint8_t b) { return b == 0; }));
  }
  uint8_t bytes[12];
  ir_wave_plan p;
  for (uint32_t us : {31u, 32u, 33u}) {
    const int n = ir_wave_encode(&us, 1, IR_DEFAULT_CARRIER, IR_DEFAULT_DUTY, IR_MIN_RATE, bytes, sizeof bytes, &p);
    CHECK(n == (us <= 32 ? 8 : 12) && p.samples == us);
    CHECK(std::all_of(bytes + n - 4, bytes + n, [](uint8_t b) { return b == 0; }));
  }
}

static void testNec()
{
  uint32_t f[IR_NEC_ENTRIES];
  CHECK(ir_nec_frame(0x00, 0x45, f, IR_NEC_ENTRIES - 1) == IR_ERR_COUNT);
  CHECK(ir_nec_frame(0x10000, 0x45, f, IR_NEC_ENTRIES) == IR_ERR_COUNT);
  CHECK(ir_nec_frame(0x00, 0x45, f, IR_NEC_ENTRIES) == 67);
  CHECK(f[0] == 9000 && f[1] == 4500 && f[66] == 562);
  // word 0xba45ff00, LSB first: address 0x00 -> eight 0 bits, ~address -> eight 1 bits, then 0x45, then 0xba
  const uint32_t word = 0xba45ff00u;
  uint64_t total = 0;
  for (unsigned b = 0; b < 32; ++b) {
    CHECK(f[2 + 2 * b] == 562);
    CHECK(f[3 + 2 * b] == ((word >> b) & 1 ? 1687u : 562u));
  }
  for (unsigned i = 0; i < 67; ++i) total += f[i];
  CHECK(total == 68030);
  ir_wave_plan p;
  CHECK(ir_wave_check(f, 67, 38000, 40, 1920000, &p) == IR_OK && p.bytes < IR_MAX_WAVE_BYTES / 2 + 1024);
  // extended address 0x1234: low byte then high byte
  CHECK(ir_nec_frame(0x1234, 0x01, f, IR_NEC_ENTRIES) == 67);
  uint32_t got = 0;
  for (unsigned b = 0; b < 32; ++b) got |= (f[3 + 2 * b] == 1687 ? 1u : 0u) << b;
  CHECK(got == (0x34u | 0x12u << 8 | 0x01u << 16 | 0xfeu << 24));
  uint32_t guarded[IR_NEC_ENTRIES + 2];
  std::fill_n(guarded, IR_NEC_ENTRIES + 2, UINT32_MAX);
  CHECK(ir_nec_frame(0xffff, 0xff, guarded + 1, IR_NEC_ENTRIES - 1) == IR_ERR_COUNT);
  CHECK(std::all_of(guarded, guarded + IR_NEC_ENTRIES + 2, [](uint32_t d) { return d == UINT32_MAX; }));
  CHECK(ir_nec_frame(0xffff, 0xff, guarded + 1, IR_NEC_ENTRIES) == IR_NEC_ENTRIES);
  CHECK(guarded[0] == UINT32_MAX && guarded[IR_NEC_ENTRIES + 1] == UINT32_MAX);
  CHECK(ir_nec_frame(0xffff, 0xff, nullptr, IR_NEC_ENTRIES) == IR_ERR_COUNT);
  uint32_t r[3];
  CHECK(ir_nec_repeat(r, 2) == IR_ERR_COUNT);
  CHECK(ir_nec_repeat(nullptr, IR_NEC_REPEAT_ENTRIES) == IR_ERR_COUNT);
  CHECK(ir_nec_repeat(r, 3) == 3 && r[0] == 9000 && r[1] == 2250 && r[2] == 562);
  CHECK(IR_NEC_PERIOD_US == 108000);
}

static void testParse()
{
  uint32_t v = 7;
  CHECK(ir_parse_duration("9000", &v) && v == 9000);
  CHECK(ir_parse_duration("+562", &v) && v == 562);
  CHECK(ir_parse_duration("-4500", &v) && v == 4500);
  CHECK(ir_parse_duration("-500000", &v) && v == IR_MAX_TOTAL_US);
  CHECK(!ir_parse_duration("184467440737095516160", &v) && v == IR_MAX_TOTAL_US);
  CHECK(!ir_parse_duration("", &v));
  CHECK(!ir_parse_duration("+", &v));
  CHECK(!ir_parse_duration("12a", &v));
  CHECK(!ir_parse_duration("pulse", &v));
  CHECK(!ir_parse_duration("500001", &v));
  CHECK(!ir_parse_duration(nullptr, &v));
}

static void testClockPick()
{
  ir_clock_pick c;
  // the SM8750 QUP table's first entries (DFS levels as firmware may program them): 19.2 MHz / 10 exact
  const uint32_t t1[8] = {7372800, 14745600, 19200000, 29491200, 32000000, 48000000, 64000000, 80000000};
  CHECK(ir_pick_clock(t1, 8, &c) && c.exact && c.level == 2 && c.divider == 10 && c.rate == 1920000);
  // 48 MHz / 25 is exact too, but 19.2 MHz is the lower source
  const uint32_t t2[2] = {48000000, 19200000};
  CHECK(ir_pick_clock(t2, 2, &c) && c.level == 1 && c.divider == 10);
  const uint32_t t3[1] = {48000000};
  CHECK(ir_pick_clock(t3, 1, &c) && c.exact && c.divider == 25);
  // no exact divider: the nearest rate within 1..4 MHz
  const uint32_t t4[2] = {7372800, 14745600};
  CHECK(ir_pick_clock(t4, 2, &c) && !c.exact && c.rate == 1843200);
  const uint32_t t5[1] = {100000000};
  CHECK(ir_pick_clock(t5, 1, &c) && c.divider == 52 && c.rate == 1923076);
  const uint32_t t6[3] = {0, 0, 0};
  CHECK(!ir_pick_clock(t6, 3, &c));
  const uint32_t t7[1] = {500000};   // too slow for any divider
  CHECK(!ir_pick_clock(t7, 1, &c));
  // the speed the kext hands geni_spi_set_clock gives back the picked divider (ceil(src / ceil(src / d)) == d)
  for (const uint32_t src : {7372800u, 14745600u, 19200000u, 29491200u, 51200000u, 100000000u, 117964800u, 128000000u}) {
    const uint32_t list[1] = {src};
    if (!ir_pick_clock(list, 1, &c)) continue;
    const uint32_t speed = (c.source_hz + c.divider - 1) / c.divider;
    CHECK((c.source_hz + speed - 1) / speed == c.divider);
  }
}

static void testRcg()
{
  CHECK(rcgRate(0x0, 0, 0) == 19200000);                       // XO, no divider
  CHECK(rcgRate(0x0 | 19, 0, 0) == 1920000);                   // XO half divider 19 = divide by 10
  // GPLL0 even (source 6) M/N 4/25 (dual edge): 48 MHz
  CHECK(rcgRate(6u << 8 | 2u << 12, 4, ~(25u - 4u) & 0xffff) == 48000000);
  CHECK(rcgRate(2u << 8, 0, 0) == 0);                          // unknown source
  CHECK(rcgRate(6u << 8 | 2u << 12, 0, 0) == 0);               // M/N degenerate
  std::map<uint32_t, uint32_t> g;
  g[kS7Rcg + kRcgDfsCmd] = 1;
  for (unsigned l = 0; l < 8; ++l) g[kS7Rcg + kRcgDfsPerf + 4 * l] = l == 2 ? 0u : (6u << 8 | 2u << 12);
  for (unsigned l = 0; l < 8; ++l) g[kS7Rcg + kRcgDfsM + 4 * l] = 4, g[kS7Rcg + kRcgDfsN + 4 * l] = ~(25u - 4u) & 0xffff;
  const S7Clock c = s7Clock([&](uint32_t off) { return g[off]; });
  CHECK(c.dfsOn && c.levels == 8 && c.hz[2] == 19200000 && c.hz[0] == 48000000);
  g.clear();
  g[kS7Rcg + kRcgCfg] = 19;
  const S7Clock d = s7Clock([&](uint32_t off) { return g[off]; });
  CHECK(!d.dfsOn && d.levels == 1 && d.hz[0] == 1920000);
}

static void testPins()
{
  CHECK(kGpio[kMosi] == 29 && kGpio[kMiso] == 28 && kGpio[kClk] == 30 && kGpio[kCs] == 31);
  // as found: function 1, 6 mA, eGPIO present: parked = GPIO, 2 mA, no pull, output, eGPIO claimed
  const uint32_t found = kEgpioPresent | 1u << 2 | 2u << 6 | 1u;
  const uint32_t park = pinCtl(found, kFunctionGpio, kDrive2mA, kNoPull, true);
  CHECK(pinFunction(park) == 0 && pinDriveMa(park) == 2 && pinOutput(park) && (park & kEgpioEnable) && (park & 3) == 0);
  const uint32_t eng = pinCtl(found, kFunctionQup, kDrive6mA, kNoPull, false);
  CHECK(pinFunction(eng) == 1 && pinDriveMa(eng) == 6 && !pinOutput(eng));
  CHECK(mosiDark(park, 0));
  CHECK(!mosiDark(park, 2));    // driven high
  CHECK(!mosiDark(eng, 0));     // with the engine: not dark by itself
  const uint32_t input = pinCtl(0, 0, 0, 1, false);
  CHECK(mosiDark(input, 0));
  CHECK(!mosiDark(input, 1) && !mosiDark(park, 1));   // the pad is high
  CHECK(!mosiDark(park & ~kEgpioEnable, 0));
  CHECK((kVoteBits & (1u << 17)) && (kVoteBits & 0xfu) == 0xf && kGccQup2Vote == 0x52010);
  for (const Branch &b : kBranches) CHECK(b.cbcr);
  CHECK(branchOn(0x0) && branchOn(kCbcrClkOff | kCbcrHwCtl) && !branchOn(kCbcrClkOff));

  uint32_t ctl = found, io = 3;
  std::vector<uint32_t> writes;
  const auto read = [&](uint32_t reg) { return reg == kCtl ? ctl : io; };
  const auto write = [&](uint32_t reg, uint32_t value) {
    writes.push_back(reg);
    (reg == kCtl ? ctl : io) = value;
  };
  CHECK(parkEmitter(ctl, read, write));
  CHECK((writes == std::vector<uint32_t>{kIo, kCtl}));
  CHECK(ctl == park && io == 0);
  const auto ignoreWrites = [](uint32_t, uint32_t) {};
  ctl = eng;
  CHECK(!parkEmitter(found, read, ignoreWrites));  // mux stuck at GENI
  ctl = park;
  io = 1;
  CHECK(!parkEmitter(found, read, ignoreWrites));  // latch low, pad high
  io = 2;
  CHECK(!parkEmitter(found, read, ignoreWrites));  // latch still high
  io = 0;
  ctl &= ~kOe;
  CHECK(!parkEmitter(found, read, ignoreWrites));  // input is insufficient for loopback
  ctl = park & ~kEgpioEnable;
  CHECK(!parkEmitter(found, read, ignoreWrites));  // owned by another controller
}

static void testClockVotes()
{
  uint32_t vote = 1u << 20;
  bool locked = false;
  unsigned writes = 0, waits = 0;
  const auto read = [&](uint32_t) { return vote; };
  const auto write = [&](uint32_t reg, uint32_t value) {
    CHECK(reg == kGccQup2Vote && locked);
    ++writes;
    vote = value;
  };
  const auto unlock = [&] { locked = false; };
  const auto wait = [&] { ++waits; };
  VoteResult r = addClockVotes(read, write, [] { return false; }, unlock, wait);
  CHECK(!r.locked && waits == 5000 && writes == 0 && r.before == r.after);
  r = addClockVotes(read, write, [&] { return locked = true; }, unlock, wait);
  CHECK(r.locked && !locked && writes == 1 && r.after == (kVoteBits | (1u << 20)));
  r = addClockVotes(read, write, [&] { return locked = true; }, unlock, wait);
  CHECK(!r.locked && !locked && writes == 1 && r.after == vote);
}

static void testDmaTeardown()
{
  CHECK(dmaSpanValid(0x10000, 0x20000, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(0x10001, 0x20000, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(0x10000, 0x20001, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(0x10000, 0x20000, 2 * IR_MAX_WAVE_BYTES - 1));
  CHECK(!dmaSpanValid(0x10000, UINT64_MAX & ~0xfffull, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(UINTPTR_MAX & ~uintptr_t(0xfff), 0x20000, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(0, 0x20000, 2 * IR_MAX_WAVE_BYTES));
  CHECK(!dmaSpanValid(0x10000, 0, 2 * IR_MAX_WAVE_BYTES));

  std::map<uint32_t, uint32_t> reg;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  bool abortWorks = true, rxResetWorks = true;
  const auto read = [&](uint32_t off) { return reg[off]; };
  const auto write = [&](uint32_t off, uint32_t value) {
    writes.emplace_back(off, value);
    if (off == SE_GENI_M_CMD_CTRL_REG && abortWorks) reg[SE_GENI_STATUS] = 0;
    if (off == SE_DMA_TX_IRQ_CLR) reg[SE_DMA_TX_IRQ_STAT] &= ~value;
    if (off == SE_DMA_RX_IRQ_CLR) reg[SE_DMA_RX_IRQ_STAT] &= ~value;
    if (off == SE_DMA_TX_FSM_RST) reg[SE_DMA_TX_IRQ_STAT] |= GENI_DMA_RESET_DONE;
    if (off == SE_DMA_RX_FSM_RST && rxResetWorks) reg[SE_DMA_RX_IRQ_STAT] |= GENI_DMA_RESET_DONE;
  };
  unsigned waits = 0;
  const auto wait = [&] { ++waits; };

  reg[SE_GENI_STATUS] = GENI_STATUS_M_ACTIVE;
  CHECK(!waitForIdle(read, wait) && waits == 5000);
  CHECK(resetTransfer(true, read, write, wait));
  CHECK((writes == std::vector<std::pair<uint32_t, uint32_t>>{
    {SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT},
    {SE_DMA_TX_IRQ_CLR, GENI_DMA_RESET_DONE}, {SE_DMA_RX_IRQ_CLR, GENI_DMA_RESET_DONE},
    {SE_DMA_TX_FSM_RST, 1}, {SE_DMA_RX_FSM_RST, 1},
  }));
  CHECK(waitForIdle(read, wait));
  rxResetWorks = false;
  // A stale RX acknowledgment from the previous reset must not make the buffer recyclable.
  CHECK(!resetTransfer(true, read, write, wait));
  writes.clear();
  abortWorks = false;
  reg[SE_GENI_STATUS] = GENI_STATUS_M_ACTIVE;
  CHECK(!resetTransfer(true, read, write, wait) && writes.size() == 1);
  writes.clear();
  abortWorks = true;
  CHECK(resetTransfer(false, read, write, wait) && writes.size() == 1);
}

// ---- a register model of the serial engine in SE DMA mode, enough for a TX-only send and the loopback
struct Model {
  std::map<uint32_t, uint32_t> reg;
  uint32_t cmd0 = 0, txLen = 0, txTrans = 0, rxLen = 0, txAttr = 0;
  uint64_t txPtr = 0, rxPtr = 0;
  unsigned rxWrites = 0, writes = 0;
  const uint8_t *txVirt = nullptr;
  uint8_t *rxVirt = nullptr;
  std::vector<uint8_t> wire;
  bool loop = false, failDma = false, shortTx = false, shortRx = false, abortWorks = true;
};
static Model gM;
static uint32_t mRd(void *, uint32_t r)
{
  switch (r) {
  case GENI_FW_REVISION_RO: return GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT | 0x02;
  case GENI_IF_DISABLE_RO: return GENI_FIFO_IF_DISABLE;
  case SE_HW_PARAM_0: return 32u << 24 | 16u << 16;
  case SE_DMA_TX_LEN_IN: return gM.txLen - (gM.txLen && gM.shortTx ? 1 : 0);
  case SE_DMA_RX_LEN_IN: return gM.rxLen - (gM.rxLen && gM.shortRx ? 1 : 0);
  default: return gM.reg[r];
  }
}
static void mWr(void *, uint32_t r, uint32_t v)
{
  ++gM.writes;
  gM.reg[r] = v;
  switch (r) {
  case SE_GENI_M_CMD0: gM.cmd0 = v; break;
  case SE_SPI_TX_TRANS_LEN: gM.txTrans = v; break;
  case SE_SPI_LOOPBACK: gM.loop = v; break;
  case SE_GENI_M_CMD_CTRL_REG:
    if (v == GENI_M_CMD_ABORT && gM.abortWorks) gM.reg[SE_GENI_STATUS] = 0;
    break;
  case SE_DMA_TX_PTR_L: gM.txPtr = (gM.txPtr & ~0xffffffffull) | v; break;
  case SE_DMA_TX_PTR_H: gM.txPtr = (gM.txPtr & 0xffffffffull) | uint64_t(v) << 32; break;
  case SE_DMA_RX_PTR_L: gM.rxPtr = (gM.rxPtr & ~0xffffffffull) | v; break;
  case SE_DMA_RX_PTR_H: gM.rxPtr = (gM.rxPtr & 0xffffffffull) | uint64_t(v) << 32; break;
  case SE_DMA_TX_ATTR: gM.txAttr = v; break;
  case SE_DMA_RX_LEN:
    gM.rxLen = v;
    ++gM.rxWrites;
    break;
  case SE_DMA_TX_LEN:
    gM.txLen = v;
    gM.wire.assign(gM.txVirt, gM.txVirt + mRd(nullptr, SE_DMA_TX_LEN_IN));
    if (gM.failDma) {
      gM.reg[SE_DMA_TX_IRQ_STAT] = GENI_DMA_AHB_ERR;
      break;
    }
    if (gM.loop && gM.rxVirt) {
      std::memcpy(gM.rxVirt, gM.txVirt, mRd(nullptr, SE_DMA_RX_LEN_IN));
      gM.reg[SE_DMA_RX_IRQ_STAT] = GENI_DMA_DONE;
    }
    gM.reg[SE_DMA_TX_IRQ_STAT] = GENI_DMA_DONE;
    gM.reg[SE_GENI_M_IRQ_STATUS] = GENI_M_CMD_DONE;
    break;
  case SE_DMA_TX_IRQ_CLR: gM.reg[SE_DMA_TX_IRQ_STAT] &= ~v; break;
  case SE_DMA_RX_IRQ_CLR: gM.reg[SE_DMA_RX_IRQ_STAT] &= ~v; break;
  case SE_DMA_TX_FSM_RST: gM.reg[SE_DMA_TX_IRQ_STAT] |= GENI_DMA_RESET_DONE; break;
  case SE_DMA_RX_FSM_RST: gM.reg[SE_DMA_RX_IRQ_STAT] |= GENI_DMA_RESET_DONE; break;
  case SE_GENI_M_IRQ_CLEAR: gM.reg[SE_GENI_M_IRQ_STATUS] &= ~v; break;
  default: break;
  }
}
static uint32_t wRd(void *, uint32_t r) { return r == QUPV3_HW_VER_REG ? 0x30090000u : 0; }
static void wWr(void *, uint32_t, uint32_t) {}

static void testGeniSend()
{
  alignas(4096) static uint8_t buffer[2 * IR_MAX_WAVE_BYTES];
  constexpr uint64_t pa = 0x1ffff8000ull;   // the RX half crosses the low address word
  gM = Model{};
  gM.txVirt = buffer;
  gM.rxVirt = buffer + IR_MAX_WAVE_BYTES;
  geni_spi probe = {};
  probe.se = {nullptr, mRd, nullptr, nullptr};
  probe.wrapper = {nullptr, wRd, nullptr, nullptr};
  CHECK(geni_spi_read_info(&probe) == 0 && probe.info.protocol == GENI_PROTO_SPI && gM.writes == 0);
  geni_spi s = {};
  s.se = {nullptr, mRd, mWr, nullptr};
  s.wrapper = {nullptr, wRd, wWr, nullptr};
  s.use_dma = 1;
  CHECK(!setDmaBuffers(s.dma, buffer, pa, sizeof buffer - 1) && !s.dma.tx_virt && !s.dma.size);
  CHECK(setDmaBuffers(s.dma, buffer, pa, sizeof buffer));
  CHECK(s.dma.tx_virt == gM.txVirt && s.dma.rx_virt == gM.rxVirt && s.dma.size == IR_MAX_WAVE_BYTES);
  s.poll_only = 1;
  CHECK(geni_spi_init(&s) == 0);
  CHECK(s.info.fifo_disabled && s.oversampling == 1);
  CHECK(geni_spi_setup(&s, 0, 0) == 0);
  // the kext's clock: DFS level 2 = 19.2 MHz, divider 10
  ir_clock_pick c;
  const uint32_t t[8] = {7372800, 14745600, 19200000, 29491200, 32000000, 48000000, 64000000, 80000000};
  CHECK(ir_pick_clock(t, 8, &c));
  CHECK(setSampleClock(s, c, t[c.level]) == 0);
  CHECK(gM.reg[SE_GENI_CLK_SEL] == 2 && gM.reg[GENI_SER_M_CLK_CFG] == (10u << GENI_CLK_DIV_SHIFT | GENI_SER_CLK_EN));
  const uint32_t oversampled[] = {19200000 / 2};
  ir_clock_pick over = {};
  s.oversampling = 2;
  CHECK(ir_pick_clock(oversampled, 1, &over) && over.rate == IR_TARGET_RATE);
  CHECK(setSampleClock(s, over, 19200000) == 0);
  CHECK(gM.reg[GENI_SER_M_CLK_CFG] == (5u << GENI_CLK_DIV_SHIFT | GENI_SER_CLK_EN));
  s.oversampling = 1;
  CHECK(setSampleClock(s, c, t[c.level]) == 0);
  uint32_t ctl = 1u << 2, io = 3;
  const auto pinRead = [&](uint32_t reg) { return reg == kCtl ? ctl : io; };
  const auto pinWrite = [&](uint32_t reg, uint32_t value) { (reg == kCtl ? ctl : io) = value; };
  const auto park = [&] { return parkEmitter(ctl, pinRead, pinWrite); };
  const auto finish = [&](int rc, size_t len, bool loopback, bool muxed) {
    return finishTransfer(s, rc, len, loopback, muxed, [](uint32_t reg) { return mRd(nullptr, reg); },
                          [&](uint32_t reg, uint32_t value) {
                            CHECK(mosiDark(ctl, io) && pinOutput(ctl));
                            mWr(nullptr, reg, value);
                          }, [] {}, park);
  };
  // The pad stays parked for internal loopback, with TX and RX in distinct DMA halves.
  const unsigned beforeLoopback = gM.writes;
  CHECK(!beginLoopback(s, [] { return false; }) && gM.writes == beforeLoopback && !gM.loop);
  CHECK(beginLoopback(s, park) && gM.loop && mosiDark(ctl, io));
  uint8_t lt[64], lr[64];
  for (unsigned i = 0; i < 63; ++i) lt[i] = uint8_t(0xa5 ^ (i * 37));
  lt[63] = 0;
  s.poll_limit = 500;
  auto end = finish(geni_spi_transfer(&s, lt, lr, sizeof lt, 0), sizeof lt, true, false);
  CHECK(!end.status && end.parked && end.reusable && std::memcmp(lt, lr, sizeof lt) == 0 && gM.loop);
  CHECK(gM.txPtr == pa && gM.rxPtr == pa + IR_MAX_WAVE_BYTES && mosiDark(ctl, io));
  gM.shortRx = true;
  const int shortLoop = geni_spi_transfer(&s, lt, lr, sizeof lt, 0);
  CHECK(shortLoop == 0 && gM.loop && s.last_rx_in == sizeof lt - 1);
  end = finish(shortLoop, sizeof lt, true, false);
  CHECK(end.status == GENI_ERR_IO && end.parked && end.reusable);
  gM.shortRx = false;
  geni_spi_setup(&s, 0, 0);
  CHECK(!gM.loop);
  // a NEC frame, TX only
  uint32_t f[IR_NEC_ENTRIES];
  ir_nec_frame(0x00, 0x45, f, IR_NEC_ENTRIES);
  static uint8_t wave[IR_MAX_WAVE_BYTES];
  ir_wave_plan p;
  const int n = ir_wave_encode(f, 67, 38000, 40, c.rate, wave, sizeof wave, &p);
  CHECK(n > 0);
  gM.rxWrites = 0;
  ctl = 1u << 2;
  end = finish(geni_spi_transfer(&s, wave, nullptr, uint32_t(n), 0), uint32_t(n), false, true);
  CHECK(!end.status && end.parked && end.reusable);
  CHECK((gM.cmd0 >> GENI_M_OPCODE_SHIFT) == GENI_M_CMD_TX_ONLY);
  CHECK((gM.cmd0 & GENI_M_PARAM_FRAGMENTATION) == 0);
  CHECK(gM.txTrans == uint32_t(n) && gM.txLen == uint32_t(n));
  CHECK(gM.txPtr == pa && gM.txAttr == GENI_DMA_TX_ATTR_EOT_BUF);
  CHECK(gM.rxWrites == 0);                                    // TX only: no RX DMA armed
  CHECK(gM.reg[SE_SPI_WORD_LEN] == 8 - GENI_WORD_LEN_MIN);
  // MSB first packing: the first vector starts at bit 7 with the direction bit set
  CHECK(((gM.reg[SE_GENI_TX_PACKING_CFG0] >> GENI_PACK_START_SHIFT) & 0x1f) == 7 && (gM.reg[SE_GENI_TX_PACKING_CFG0] >> GENI_PACK_DIR_SHIFT & 1));
  CHECK(gM.wire.size() == size_t(n) && std::memcmp(gM.wire.data(), wave, size_t(n)) == 0);
  CHECK(gM.wire[size_t(n) - 1] == 0);                        // the line ends low
  CHECK(s.last_tx_in == uint32_t(n));
  ctl = 1u << 2;
  gM.shortTx = true;
  const int shortSend = geni_spi_transfer(&s, wave, nullptr, uint32_t(n), 0);
  CHECK(shortSend == 0 && s.last_tx_in == uint32_t(n) - 1);
  end = finish(shortSend, uint32_t(n), false, true);
  CHECK(end.status == GENI_ERR_IO && end.parked && end.reusable && mosiDark(ctl, io));
  CHECK(gM.wire.size() == size_t(n) - 1);
  gM.shortTx = false;
  // a DMA error comes back as an error (the kext parks the pins either way)
  gM.failDma = true;
  ctl = 1u << 2;
  end = finish(geni_spi_transfer(&s, wave, nullptr, uint32_t(n), 0), uint32_t(n), false, true);
  CHECK(end.status == GENI_ERR_IO && end.parked && end.reusable);
  gM.failDma = false;
  ctl = 1u << 2;
  gM.reg[SE_GENI_STATUS] = GENI_STATUS_M_ACTIVE;
  gM.abortWorks = false;
  end = finish(GENI_ERR_TIMEOUT, uint32_t(n), false, true);
  CHECK(end.status == GENI_ERR_TIMEOUT && end.parked && !end.reusable && mosiDark(ctl, io));
  gM.abortWorks = true;
  end = finish(GENI_ERR_TIMEOUT, uint32_t(n), false, false);
  CHECK(end.status == GENI_ERR_TIMEOUT && end.parked && end.reusable && mosiDark(ctl, io));
  // a send larger than the DMA buffer is refused before any register write
  buffer[IR_MAX_WAVE_BYTES] = 0xa5;
  CHECK(geni_spi_transfer(&s, wave, nullptr, IR_MAX_WAVE_BYTES, 0) == 0);
  CHECK(gM.txLen == IR_MAX_WAVE_BYTES && buffer[IR_MAX_WAVE_BYTES] == 0xa5 && gM.rxWrites == 0);
  const unsigned before = gM.writes;
  CHECK(geni_spi_transfer(&s, wave, nullptr, 0, 0) == GENI_ERR_PARAM);
  CHECK(geni_spi_transfer(&s, wave, nullptr, IR_MAX_WAVE_BYTES + 1, 0) == GENI_ERR_PARAM && gM.writes == before);
}

static void testClient()
{
  CHECK(std::strcmp(kPianoIRSend, "piano-ir-send") == 0);
  CHECK(!piano_ir_enabled(0) && piano_ir_enabled(1) && piano_ir_enabled(2));
  CHECK(!piano_ir_enabled(3) && !piano_ir_enabled(UINT64_MAX) && !piano_ir_enabled((1ull << 32) + 2));
  CHECK(piano_ir_boot_test_count(1) == 1 && piano_ir_boot_test_count(20) == 20);
  CHECK(!piano_ir_boot_test_count(0) && !piano_ir_boot_test_count(21) && !piano_ir_boot_test_count(UINT64_MAX));
  CHECK(piano_ir_burst_valid(1) && piano_ir_burst_valid(500));
  CHECK(!piano_ir_burst_valid(0) && !piano_ir_burst_valid(501) && !piano_ir_burst_valid((1ull << 32) + 500));
  CHECK(!ir_carrier_valid((1ull << 32) + IR_DEFAULT_CARRIER) && !ir_duty_valid((1ull << 32) + IR_DEFAULT_DUTY));
  CHECK(!ir_carrier_valid(UINT64_MAX) && !ir_duty_valid(UINT64_MAX));
  CHECK(ir_burst_part_ms(500, 0) == 0 && ir_burst_part_ms(500, IR_MAX_RATE + 1) == 0);
  for (uint32_t rate : {IR_MIN_RATE, IR_TARGET_RATE, IR_MAX_RATE}) {
    unsigned parts = 0, sum = 0;
    for (uint32_t left = kPianoIRMaxBurstMs; left;) {
      const uint32_t part = ir_burst_part_ms(left, rate);
      CHECK(part && part <= 120 && part <= left);
      if (!part || part > left) break;
      ir_wave_plan p;
      const uint32_t mark = part * 1000;
      CHECK(ir_wave_check(&mark, 1, IR_DEFAULT_CARRIER, IR_DEFAULT_DUTY, rate, &p) == IR_OK);
      sum += part;
      ++parts;
      left -= part;
    }
    CHECK(parts == (rate == IR_MAX_RATE ? 8u : 5u) && sum == kPianoIRMaxBurstMs);
  }
}

int main()
{
  testPlanBounds();
  testEncodeMark();
  testEncodePhaseAndBoundaries();
  testEncodeBufferEdges();
  testNec();
  testParse();
  testClockPick();
  testRcg();
  testPins();
  testClockVotes();
  testDmaTeardown();
  testGeniSend();
  testClient();
  std::printf("PianoIR host tests: %d checks, %d failures\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
