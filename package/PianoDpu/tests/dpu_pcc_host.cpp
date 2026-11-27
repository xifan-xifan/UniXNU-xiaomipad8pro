// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu B1d and M4 (DpuPcc.h and DpuNightShift.h):
//   - macOS's fixed matrix: S31.32 words, both sign conventions, the +-4 limit, rounding to Q16.16;
//   - the matrix product, the linear-to-encoded gain table against pow(x, 1 / 2.2), the diagonal kept exact, others flagged;
//   - the PCC's S3.15 words: rounding, saturation, negative coefficients clamped unless allowed;
//   - the stores on the register model: both DSPPs of the path identical, enable, CTL_DSPP0/1_FLUSH bit 4, CTL_FLUSH bit 29,
//     latched at the next frame, read back, nothing else written; the survey's decoder reads the words back as written;
//   - what macOS has set: one matrix per function, their product, the identity clearing a slot, the gamma fit (scaled ramps =
//     a diagonal; curves, non-monotonic or empty tables refused).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_pcc_host.cpp -o /tmp/dpu_pcc_host && /tmp/dpu_pcc_host
#include "DpuPcc.h"
#include "DpuNightShift.h"
#include "dpu_model.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianodpu;
using namespace pianodpu::pcc;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint64_t s3132(double v)   // two's complement
{
  return uint64_t(int64_t(std::llround(v * 4294967296.0)));
}
static uint64_t signMagnitude(double v)
{
  const uint64_t m = uint64_t(std::llround(std::fabs(v) * 4294967296.0));
  return v < 0 ? m | (1ull << 63) : m;
}

static void testDecode()
{
  uint64_t raw[9] = {1ull << 32, 0, 0, 0, 1ull << 32, 0, 0, 0, 1ull << 32};
  Decoded d = decodeS3132(raw);
  CHECK(d.ok && isIdentity(d.m));
  raw[4] = s3132(0.5);
  raw[8] = s3132(0.45);
  d = decodeS3132(raw);
  CHECK(d.ok && d.m.m[1][1] == 32768 && d.m.m[2][2] == 29491 && isDiagonal(d.m));   // 0.45 * 65536 = 29491.2
  raw[1] = s3132(-0.25);                          // output R from input G, two's complement
  raw[5] = signMagnitude(-0.125);                 // output G from input B, sign-magnitude (DRM's CTM)
  d = decodeS3132(raw);
  CHECK(d.ok && d.m.m[0][1] == -16384 && d.m.m[1][2] == -8192 && d.negativeTwos == 1 && d.negativeSignMagnitude == 1);
  CHECK(!isDiagonal(d.m));
  raw[1] = s3132(4.0);
  CHECK(decodeS3132(raw).ok);                     // exactly 4.0 is in range
  raw[1] = s3132(4.5);
  CHECK(!decodeS3132(raw).ok);
  raw[1] = s3132(-4.5);
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x8000000500000000ull;                 // -5 sign-magnitude
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x7fffffffffffffffull;
  CHECK(!decodeS3132(raw).ok);
  raw[1] = 0x8000;                                // half a Q16.16 step: rounds up
  CHECK(decodeS3132(raw).ok && decodeS3132(raw).m.m[0][1] == 1);
  raw[1] = 0x7fff;
  CHECK(decodeS3132(raw).m.m[0][1] == 0);
  raw[1] = 1ull << 63;                            // -0 in sign-magnitude
  CHECK(decodeS3132(raw).ok && decodeS3132(raw).m.m[0][1] == 0);
  const uint64_t invalidWords[] = {kS3132Limit + 1, 0ull - kS3132Limit - 1,
                                   0x7ff8000000000000ull, 0xfff8000000000000ull};
  for (uint64_t invalid : invalidWords) {
    raw[1] = invalid;
    CHECK(!decodeS3132(raw).ok);
  }
}

static void testMath()
{
  const Matrix a = diagonal(kOne, fromMilli(730), fromMilli(450)), b = diagonal(fromMilli(500), kOne, fromMilli(500));
  const Matrix ab = multiply(a, b);
  CHECK(std::abs(ab.m[0][0] - fromMilli(500)) <= 1 && std::abs(ab.m[2][2] - fromMilli(225)) <= 1 && isDiagonal(ab));
  CHECK(same(multiply(identity(), a), a) && same(multiply(a, identity()), a));
  Matrix p{{{kOne, kOne / 2, 0}, {0, kOne, 0}, {0, 0, kOne}}}, q{{{kOne, 0, 0}, {kOne / 4, kOne, 0}, {0, 0, 2 * kOne}}};
  const Matrix pq = multiply(p, q);   // [[1.125, .5, 0], [.25, 1, 0], [0, 0, 2]]
  CHECK(pq.m[0][0] == kOne + kOne / 8 && pq.m[0][1] == kOne / 2 && pq.m[1][0] == kOne / 4 && pq.m[2][2] == 2 * kOne);
  CHECK(milli(fromMilli(730)) == 730 && milli(-kOne / 2) == -500);
  // the gain table: pow(d, 1/2.2), monotonic, ends exact
  CHECK(encodeGain(0) == 0 && encodeGain(kOne) == kOne && encodeGain(kOne / 2) == 47824 && encodeGain(-5) == 0);
  CHECK(encodeGain(2 * kOne) == 2 * kOne);   // a gain above 1 is not a white-point reduction: kept
  int32_t last = -1;
  double worst = 0;
  for (int32_t d = 0; d <= kOne; d += 37) {
    const int32_t e = encodeGain(d);
    CHECK(e >= last);
    last = e;
    if (d >= kOne / 50) {
      const double want = std::pow(double(d) / kOne, 1 / 2.2), got = double(e) / kOne;
      worst = std::fmax(worst, std::fabs(got - want) / want);
    }
  }
  CHECK(worst < 0.005);
  // a Night Shift diagonal, linear 1.0 / 0.73 / 0.45 -> encoded 1.0 / 0.867 / 0.696, exact
  bool approximate = true;
  const Matrix e = encode(diagonal(kOne, fromMilli(730), fromMilli(450)), &approximate);
  CHECK(!approximate && e.m[0][0] == kOne && std::abs(milli(e.m[1][1]) - 867) <= 1 && std::abs(milli(e.m[2][2]) - 696) <= 1);
  const Matrix f = encode(p, &approximate);
  CHECK(approximate && same(f, p));
  encode(identity(), &approximate);
  CHECK(!approximate);
}

static void testWords()
{
  Words w = pccWords(identity(), false);
  CHECK(w.w[0][0] == 0x8000 && w.w[1][1] == 0x8000 && w.w[2][2] == 0x8000 && w.w[0][1] == 0 && w.w[2][0] == 0);
  CHECK(w.clampedNegative == 0 && w.saturated == 0);
  w = pccWords(diagonal(kOne / 2, 3, 4 * kOne), false);
  CHECK(w.w[0][0] == 0x4000 && w.w[1][1] == 2 && w.w[2][2] == 0x1ffff && w.saturated == 1);   // 3/2 rounds to 2; 4.0 saturates
  Matrix n = identity();
  n.m[0][1] = -kOne / 2;
  w = pccWords(n, false);
  CHECK(w.w[0][1] == 0 && w.clampedNegative == 1);
  w = pccWords(n, true);
  CHECK(w.w[0][1] == 0x3c000 && w.clampedNegative == 0 && pccMilli(w.w[0][1]) == -500);
  n.m[0][1] = -8 * kOne;
  CHECK(pccWords(n, true).w[0][1] == 0x20000 && pccWords(n, true).saturated == 1);
  w = pccWords(diagonal(INT32_MAX, INT32_MIN, -1), true);
  CHECK(w.w[0][0] == 0x1ffff && w.w[1][1] == 0x20000 && w.w[2][2] == 0x3ffff && w.saturated == 2);

  const Matrix minRow{{{INT32_MIN, INT32_MIN, INT32_MIN}, {}, {}}};
  const Matrix extremes{{{INT32_MIN, INT32_MAX, INT32_MAX},
                         {INT32_MIN, INT32_MAX, 1},
                         {INT32_MAX, INT32_MAX, INT32_MIN}}};
  const Matrix product = multiply(minRow, extremes);
  CHECK(product.m[0][0] == 8 * kOne && product.m[0][1] == -8 * kOne && product.m[0][2] == 0);
  const Matrix rounded = multiply(diagonal(1, -1, 1), diagonal(kOne / 2, kOne / 2, kOne / 2 - 1));
  CHECK(same(rounded, diagonal(1, -1, 0)));
  Matrix extremeCrossTerm = identity();
  extremeCrossTerm.m[0][1] = INT32_MIN;
  CHECK(!isDiagonal(extremeCrossTerm, INT32_MIN));
  CHECK(same(multiply(identity(), identity(), INT32_MIN), Matrix{}));
}

static void testApply()
{
  model::Dpu m;
  m.sleepMs(3);
  const LivePath p = readPath(m);
  CHECK(p.ok && p.dspps == 0x3);
  const size_t before = m.stores.size();
  Apply a = apply(m, p, identity(), false);
  CHECK(a.ok && a.refused == 0 && a.dspps == 0x3 && a.latchUs >= 0 && a.latchUs < 9000);
  CHECK(m.stores.size() - before == 2 * 10 + 3 && a.stores == 23);
  for (unsigned d = 0; d < 2; ++d) {
    CHECK(m.pccActive[d][0] == 1 && m.pccActive[d][0x10 / 4] == 0x8000 && m.pccActive[d][0x20 / 4] == 0x8000 &&
          m.pccActive[d][0x30 / 4] == 0x8000 && m.pccActive[d][0x14 / 4] == 0);
    CHECK(m.pccActive[d][1] == 0 && m.pccActive[d][2] == 0 && m.pccActive[d][3] == 0);   // the constants are not written
  }
  CHECK(m.storesTo(pccReg(2, 0)) == 0 && m.storesTo(pccReg(3, 0x10)) == 0);             // DSPP2/3 untouched
  CHECK(m.firstStore(ctlReg(0, kCtlDspp0Flush), kDsppFlushPcc) >= 0 && m.firstStore(ctlReg(0, kCtlDspp0Flush + 4), kDsppFlushPcc) >= 0);
  CHECK(m.firstStore(ctlReg(0, kCtlFlush), kFlushDspp) > m.firstStore(ctlReg(0, kCtlDspp0Flush + 4), kDsppFlushPcc));
  CHECK(m.firstStore(pccReg(0, 0), 1) > m.firstStore(pccReg(0, 0x30), 0x8000));   // the matrix before the enable
  // a warm white point with an off-diagonal term: both halves the same, read back by the survey's decoder as written
  Matrix warm = diagonal(kOne, fromMilli(867), fromMilli(696));
  warm.m[0][1] = kOne / 8;
  a = apply(m, p, warm, false);
  CHECK(a.ok);
  for (unsigned d = 0; d < 2; ++d) {
    DsppRegs r{};
    for (unsigned k = 0; k < 13; ++k) r.pcc[k] = m.pccActive[d][k];
    const PccView v = viewPcc(r);
    CHECK(v.enabled && !v.identity && v.m[0][0] == 1000 && v.m[0][1] == 125 && v.m[1][0] == 0 && v.m[1][1] == 867 && v.m[2][2] == 696);
  }
  CHECK(std::memcmp(m.pccActive[0], m.pccActive[1], sizeof m.pccActive[0]) == 0);
  CHECK(m.glitches == 0 && m.refused.empty());
  CHECK(!m.anyStoreIn(kTlmm, 0x200000) && !m.anyStoreIn(kDsi[0], 0x3000) && !m.anyStoreIn(kMdp + kIntfBase, 0x4000));
  {   // engines stopped: the flush cannot latch (the caller keeps the matrix for the wake)
    model::Dpu s;
    s.sleepMs(2);
    const LivePath q = readPath(s);
    s.wr(intfReg(1, kIntfEn), 0);
    s.wr(intfReg(2, kIntfEn), 0);
    s.sleepMs(20);
    const Apply b = apply(s, q, identity(), false);
    CHECK(!b.ok && std::strstr(b.why, "bit 29"));
    s.wr(intfReg(1, kIntfEn), 1);   // the start latches what was pending
    s.wr(intfReg(2, kIntfEn), 1);
    CHECK(s.pccActive[0][0] == 1 && s.ctlFlush == 0);
  }
  {   // a path whose LMs have no DSPP: refused, nothing written
    model::Dpu s;
    s.regs[ctlReg(0, kCtlLayerActive)] = 0x30;   // LM4 + LM5
    s.sleepMs(2);
    const LivePath q = readPath(s);
    CHECK(q.ok && q.dspps == 0);
    const Apply b = apply(s, q, identity(), false);
    CHECK(!b.ok && s.stores.empty());
  }
  {   // LM2 + LM3: DSPP2 + DSPP3
    model::Dpu s;
    s.regs[ctlReg(0, kCtlLayerActive)] = 0xc;
    s.sleepMs(2);
    const LivePath q = readPath(s);
    CHECK(q.dspps == 0xc && apply(s, q, identity(), false).ok && s.storesTo(pccReg(2, 0)) == 1 && s.storesTo(pccReg(0, 0)) == 0);
  }
}

struct ReadbackFault : model::Dpu {
  bool corruptOnce = false;
  uint32_t rd(uint64_t at)
  {
    const uint32_t value = model::Dpu::rd(at);
    if (corruptOnce && at == pccReg(1, 0x1c)) {
      corruptOnce = false;
      return value ^ 1;
    }
    return value;
  }
};

static void testNightShift()
{
  uint64_t warm[9] = {s3132(1), s3132(0.125), 0, s3132(-0.25), s3132(0.75), 0, 0, 0, s3132(0.5)};
  const uint64_t unity[9] = {s3132(1), 0, 0, 0, s3132(1), 0, 0, 0, s3132(1)};
  const uint64_t blank[9] = {};
  NightShift state;
  CHECK(state.neutral());
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted && state.neutral());
  CHECK(state.setMatrix(warm) == NightShift::Input::accepted && !state.neutral());
  CHECK(state.corrected().m[1][0] == -kOne / 4 && state.corrected().m[0][1] == kOne / 8);
  warm[3] = signMagnitude(-0.25);
  CHECK(state.setMatrix(warm) == NightShift::Input::invalid && state.corrected().m[1][0] == -kOne / 4);
  warm[3] = s3132(-0.25);
  CHECK(state.setMatrix(blank) == NightShift::Input::blank && state.corrected().m[1][0] == -kOne / 4);

  const auto words = pccWords(diagonal(-4 * kOne, 0, 4 * kOne), true);
  CHECK(words.w[0][0] == 0x20000 && words.w[2][2] == 0x1ffff && words.saturated == 1);

  ReadbackFault io;
  io.pccShadow[0][4] = io.pccActive[0][4] = 0x1234;   // disabled firmware state with different dormant coefficients
  io.pccShadow[1][4] = io.pccActive[1][4] = 0x5678;
  const LivePath path = readPath(io);
  PanelPcc base;
  CHECK(capturePanelPcc(io, path, base) && io.stores.empty());
  PanelPcc last = base;
  PanelPcc wanted = nightShiftPrograms(base, state);
  io.corruptOnce = true;
  auto update = updateNightShift(io, path, wanted, last, false);
  CHECK(update.attempted && !update.result.ok && update.restored && same(last, base));
  CHECK(io.pccActive[0][4] == 0x1234 && io.pccActive[1][4] == 0x5678);
  const unsigned flushes = io.storesTo(ctlReg(0, kCtlFlush));
  update = updateNightShift(io, path, wanted, last, false);
  CHECK(update.result.ok && update.result.stores == 23 && same(last, wanted));
  CHECK(io.storesTo(ctlReg(0, kCtlFlush)) == flushes + 1);
  CHECK(io.storesTo(pccReg(2, 0)) == 0 && io.storesTo(pccReg(3, 0)) == 0);
  for (unsigned d = 0; d < 2; ++d) {
    const uint32_t *pcc = io.pccActive[d];
    CHECK(pcc[0] == 1 && pcc[4] == 0x8000 && pcc[7] == 0x1000);
    CHECK(pcc[5] == 0x3e000 && pcc[8] == 0x6000 && pcc[12] == 0x4000);
  }

  // A slider operation with no callbacks leaves the last colour in place. A repeat also needs no stores.
  CHECK(!updateNightShift(io, path, nightShiftPrograms(base, state), last, false).attempted);
  state.setCorrection(kOne);
  CHECK(!updateNightShift(io, path, nightShiftPrograms(base, state), last, false).attempted);
  CHECK(state.setMatrix(warm) == NightShift::Input::accepted);
  CHECK(!updateNightShift(io, path, nightShiftPrograms(base, state), last, false).attempted);
  // Slot 314 alone updates the existing matrix. A later slot 309 update uses the retained correction.
  state.setCorrection(kOne / 2);
  CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, false).result.ok);
  CHECK(io.pccActive[0][4] == 0x4000 && io.pccActive[1][5] == 0x3f000);
  warm[8] = s3132(0.75);
  CHECK(state.setMatrix(warm) == NightShift::Input::accepted);
  wanted = nightShiftPrograms(base, state);
  CHECK(updateNightShift(io, path, wanted, last, false).result.ok && io.pccActive[1][12] == 0x3000);

  // Power blanking keeps the colour. Both wake and mode-set replay it after a hardware reset.
  CHECK(state.setMatrix(blank) == NightShift::Input::blank);
  for (unsigned event = 0; event < 2; ++event) {
    for (unsigned d = 0; d < 2; ++d) io.pccShadow[d][0] = io.pccActive[d][0] = 0;
    CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, true).result.ok);
    CHECK(io.pccActive[0][0] == 1 && io.pccActive[1][12] == 0x3000);
  }
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted && state.neutral());
  CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, false).result.ok && same(last, base));
  CHECK(io.pccActive[0][0] == 0 && io.pccActive[1][0] == 0);
  CHECK(io.pccActive[0][4] == 0x1234 && io.pccActive[1][4] == 0x5678);

  // An enabled base keeps its matrix when Night Shift is off. Active colour composes with the base.
  base.dspp[0] = {pccWords(diagonal(kOne / 2, kOne, kOne), true), true};
  base.dspp[0].words.w[2][0] = 0x3e000;   // firmware blue from red = -0.25
  CHECK(nightShiftPrograms(base, state).dspp[0].words.w[0][0] == 0x4000);
  CHECK(state.setMatrix(warm) == NightShift::Input::accepted);
  CHECK(nightShiftPrograms(base, state).dspp[0].words.w[0][1] == 0x0800);
  CHECK(nightShiftPrograms(base, state).dspp[0].words.w[2][1] == 0x3fc00);
  state.setCorrection(6 * kOne);
  CHECK(nightShiftPrograms(base, state).dspp[0].words.w[0][0] == 0x18000);
  state.setCorrection(UINT32_MAX);
  CHECK(state.corrected().m[0][0] == 4 * kOne && state.corrected().m[1][0] == -4 * kOne);
  state.setCorrection(0);
  CHECK(same(state.corrected(), Matrix{}) && !state.neutral());
  CHECK(state.setMatrix(blank) == NightShift::Input::blank && !state.neutral());
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted && state.neutral());
  CHECK(same(state.corrected(), identity()));

  model::Dpu enabled;
  const LivePath enabledPath = readPath(enabled);
  CHECK(apply(enabled, enabledPath, identity(), true).ok);
  PanelPcc enabledBase;
  CHECK(capturePanelPcc(enabled, enabledPath, enabledBase));
  PanelPcc enabledLast = enabledBase;
  CHECK(state.setMatrix(warm) == NightShift::Input::accepted);
  CHECK(updateNightShift(enabled, enabledPath, nightShiftPrograms(enabledBase, state), enabledLast, false).result.ok);
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted);
  CHECK(updateNightShift(enabled, enabledPath, nightShiftPrograms(enabledBase, state), enabledLast, false).result.ok);
  for (unsigned d = 0; d < 2; ++d) {
    CHECK(enabled.pccActive[d][0] == 1);
    for (unsigned k = 0; k < 9; ++k)
      CHECK(enabled.pccActive[d][4 + k] == (k % 4 == 0 ? 0x8000u : 0u));
  }

  LivePath incomplete = path;
  incomplete.dspps = 1;
  CHECK(!capturePanelPcc(io, incomplete, base));
  CHECK(!updateNightShift(io, incomplete, wanted, last, true).attempted);
}

static void testNightShiftRegisterWords()
{
  // y12q qual2. #1 has seven intact S31.32 words. The last two use the exact
  // magnitudes from the matching PCC snapshot, lifted back to S31.32.
  const uint64_t first[9] = {
    0x00000000e93fd100, 0x00000000135c95a0, 0x0000000003639998,
    0xffffffffff149e71, 0x00000000d906a900, 0x0000000000719d4f,
    0xfffffffffe28c2ca, 0xfffffffff94e0000, 0x00000000b7520000,
  };
  // The stronger post-login sample is reconstructed from both DSPPs' read-back
  // at PCC precision, undoing y12q's sign-magnitude packing.
  const uint64_t stronger[9] = {
    0x00000000a6920000, 0x000000004fb20000, 0x0000000009bc0000,
    0xfffffffffc2a0000, 0x0000000061aa0000, 0x0000000000ce0000,
    0xfffffffffb600000, 0xfffffffff1860000, 0x00000000224a0000,
  };
  const uint64_t unity[9] = {1ull << 32, 0, 0, 0, 1ull << 32, 0, 0, 0, 1ull << 32};
  const uint32_t identityWords[9] = {0x8000, 0, 0, 0, 0x8000, 0, 0, 0, 0x8000};
  // Physical order +0x10..+0x30, independent of kPccCoef.
  const uint32_t firstWords[9] = {
    0x074a0, 0x3ff8a, 0x3ff14, 0x009af, 0x06c84, 0x3fca7, 0x001b2, 0x00039, 0x05ba9,
  };
  const uint32_t strongerWords[9] = {
    0x05349, 0x3fe15, 0x3fdb0, 0x027d9, 0x030d5, 0x3f8c3, 0x004de, 0x00067, 0x01125,
  };
  const uint32_t oldWords[9] = {
    0x05349, 0x027d9, 0x004de, 0x201eb, 0x030d5, 0x00067, 0x20250, 0x2073d, 0x01125,
  };
  auto grey = [](const uint32_t (&regs)[9], unsigned output) {
    return pccSignedQ15(regs[output]) + pccSignedQ15(regs[output + 3]) + pccSignedQ15(regs[output + 6]);
  };
  CHECK(grey(oldWords, 0) < 0 && grey(oldWords, 1) < 0 && grey(oldWords, 2) > 0);

  model::Dpu io;
  const LivePath path = readPath(io);
  for (unsigned d = 0; d < 2; ++d) {
    io.pccActive[d][0] = io.pccShadow[d][0] = 1;
    for (unsigned k = 0; k < 9; ++k)
      io.pccActive[d][4 + k] = io.pccShadow[d][4 + k] = identityWords[k];
  }
  PanelPcc base;
  CHECK(capturePanelPcc(io, path, base));
  PanelPcc last = base;
  NightShift state;
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted);   // neutral boot reference for the serialization fixtures
  const struct {
    const uint64_t (&raw)[9];
    const uint32_t (&regs)[9];
  } cases[] = {{first, firstWords}, {stronger, strongerWords}, {unity, identityWords}};
  for (const auto &fixture : cases) {
    CHECK(state.setMatrix(fixture.raw) == NightShift::Input::accepted);
    state.setCorrection(0x10000);   // the logged slot 314 value
    const PanelPcc wanted = nightShiftPrograms(base, state);
    const NightShiftApply update = updateNightShift(io, path, wanted, last, false);
    CHECK(update.attempted && update.result.ok);
    for (unsigned d = 0; d < 2; ++d) {
      CHECK(update.result.enable[d] == 1);
      for (unsigned k = 0; k < 9; ++k) {
        CHECK(io.pccActive[d][4 + k] == fixture.regs[k]);
        CHECK(update.result.readback[d][k % 3][k / 3] == fixture.regs[k]);
      }
    }
    CHECK(fixture.regs[0] >= fixture.regs[8]);   // red gain >= blue gain
    CHECK(grey(fixture.regs, 0) >= grey(fixture.regs, 1) && grey(fixture.regs, 1) >= grey(fixture.regs, 2));
    CHECK(grey(fixture.regs, 2) > 0);
  }
  CHECK(state.neutral() && same(last, base));
}

static void testNightShiftBootReference(const char *outputDirectory)
{
  // y12s-qual1 console:5780, ns-{base,off,half,warm2}-dpu.txt.
  // Boot has seven intact raw words; its last two, and the other matrices,
  // are reconstructed at S3.15 precision from physical +0x10..+0x30 words.
  const uint64_t boot[9] = {
    0xe93ec600, 0x135d7980, 0x0363c088,
    0xffffffffff1493b5, 0xd904d800, 0x0071a262,
    0xfffffffffe28adb0, 0xfffffffff94e0000, 0xb74e0000,
  };
  const uint64_t off[9] = {
    0xe9300000, 0x136c0000, 0x03660000,
    0xffffffffff140000, 0xd8ea0000, 0x00720000,
    0xfffffffffe280000, 0xfffffffff94c0000, 0xb72c0000,
  };
  const uint64_t half[9] = {
    0xc5fa0000, 0x322c0000, 0x07dc0000,
    0xfffffffffd9a0000, 0x9b780000, 0x00ee0000,
    0xfffffffffbe00000, 0xfffffffff1860000, 0x54a60000,
  };
  const uint64_t warm[9] = {
    0xa69c0000, 0x4fa80000, 0x09bc0000,
    0xfffffffffc2a0000, 0x61be0000, 0x00ce0000,
    0xfffffffffb600000, 0xfffffffff1840000, 0x22540000,
  };
  const uint64_t unity[9] = {1ull << 32, 0, 0, 0, 1ull << 32, 0, 0, 0, 1ull << 32};
  const uint64_t blank[9] = {};
  // Reproduce the old failure through the real decode/serializer/programmer,
  // before exercising the fix. Checking read-back equality alone missed it.
  model::Dpu oldIo;
  const LivePath oldPath = readPath(oldIo);
  CHECK(apply(oldIo, oldPath, decodeS3132(boot, false).m, true).ok);
  auto gray = [](const model::Dpu &io, unsigned d, unsigned output) {
    if (!(io.pccActive[d][0] & 1u)) return 1.0;
    const auto *regs = io.pccActive[d] + 4;
    return (pccSignedQ15(regs[output]) + pccSignedQ15(regs[output + 3]) + pccSignedQ15(regs[output + 6])) / 32768.0;
  };
  CHECK(std::abs(gray(oldIo, 0, 0) - 1) < .001);
  CHECK(std::abs(gray(oldIo, 0, 1) - .846) < .002 && std::abs(gray(oldIo, 0, 2) - .683) < .002);

  model::Dpu io;
  const LivePath path = readPath(io);
  PanelPcc base;
  CHECK(capturePanelPcc(io, path, base));
  PanelPcc last = base;
  NightShift state;
  CHECK(state.setMatrix(blank) == NightShift::Input::blank);   // not a reference
  uint64_t bad[9] = {s3132(1), 0, 0, 0, s3132(1), 0, 0, 0, 0};
  CHECK(state.setMatrix(bad) == NightShift::Input::invalid && state.neutral());   // singular
  bad[8] = s3132(.001);
  CHECK(state.setMatrix(bad) == NightShift::Input::invalid && state.neutral());
  bad[8] = s3132(.1);
  CHECK(state.setMatrix(bad) == NightShift::Input::invalid && state.neutral());   // inverse exceeds 4
  CHECK(state.setMatrix(boot) == NightShift::Input::accepted && state.neutral());
  CHECK(same(nightShiftPrograms(base, state), base));
  CHECK(!updateNightShift(io, path, nightShiftPrograms(base, state), last, false).attempted && io.stores.empty());

  const struct {
    const char *name;
    const uint64_t (&raw)[9];
    bool neutral;
    double expectedGray[3];   // independent double-precision M * inverse(boot), tolerance covers capture quantization
  } cases[] = {
    {"off", off, true, {1.000, .999, .999}},
    {"half", half, false, {1.033, .713, .397}},
    {"warm", warm, false, {1.059, .441, .110}},
    {"off-again", off, true, {1.000, .999, .999}},
  };
  for (const auto &fixture : cases) {
    CHECK(state.setMatrix(fixture.raw) == NightShift::Input::accepted);
    state.setCorrection(kOne);   // every observed slot 314 correction is unity
    CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, false).result.ok);
    FILE *snapshot = nullptr;
    if (outputDirectory) {
      char filename[1024];
      std::snprintf(filename, sizeof filename, "%s/ns-%s-dpu.txt", outputDirectory, fixture.name);
      snapshot = std::fopen(filename, "w");
      CHECK(snapshot);
    }
    for (unsigned d = 0; d < 2; ++d) {
      const double r = gray(io, d, 0), g = gray(io, d, 1), b = gray(io, d, 2);
      for (unsigned o = 0; o < 3; ++o) {
        CHECK(std::abs(gray(io, d, o) - fixture.expectedGray[o]) < .002);
        if (fixture.neutral) CHECK(std::abs(gray(io, d, o) - 1) <= .02);
      }
      if (!fixture.neutral) CHECK(r >= g && g >= b && r >= .9);
      if (snapshot) {
        std::fprintf(snapshot, "dspp%u: pcc en %08x c", d, io.pccActive[d][0]);
        for (unsigned k = 0; k < 9; ++k) std::fprintf(snapshot, "%s %08x", k && k % 3 == 0 ? " |" : "", io.pccActive[d][4 + k]);
        std::fprintf(snapshot, " (+00000000 00000000 00000000); gc en 00000000\n");
      }
      std::printf("ns-base %s dspp%u gray R %.3f G %.3f B %.3f\n", fixture.name, d, r, g, b);
    }
    if (snapshot) std::fclose(snapshot);
    const PanelPcc beforeReplay = last;
    CHECK(state.setMatrix(blank) == NightShift::Input::blank);
    for (unsigned d = 0; d < 2; ++d) io.pccActive[d][0] = io.pccShadow[d][0] = 0;
    CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, true).result.ok && same(last, beforeReplay));
  }
  // Wake/mode replay and the OFF callback must not replace the boot reference.
  CHECK(state.setMatrix(boot) == NightShift::Input::accepted && state.neutral());
  CHECK(updateNightShift(io, path, nightShiftPrograms(base, state), last, false).result.ok && same(last, base));
  CHECK(state.setMatrix(unity) == NightShift::Input::accepted && state.neutral());
  CHECK(state.setMatrix(half) == NightShift::Input::accepted && std::abs(milli(state.corrected().m[0][0]) - 850) <= 1);

  // A noncommuting reference/relative pair distinguishes right cancellation
  // from inverse(reference) * M or elementwise coefficient division.
  NightShift shear;
  const uint64_t shearBase[9] = {s3132(1), s3132(.125), 0, 0, s3132(1), s3132(.25), 0, 0, s3132(1)};
  const uint64_t composed[9] = {s3132(1), s3132(.25), s3132(.03125), s3132(-.25), s3132(.71875), s3132(.1875), 0, 0, s3132(.5)};
  CHECK(shear.setMatrix(shearBase) == NightShift::Input::accepted);
  CHECK(shear.setMatrix(composed) == NightShift::Input::accepted);
  CHECK(shear.corrected().m[0][1] == kOne / 8 && shear.corrected().m[1][0] == -kOne / 4);
  CHECK(shear.corrected().m[1][1] == 3 * kOne / 4 && shear.corrected().m[0][2] == 0 && shear.corrected().m[1][2] == 0);
  // Independent scalar correction also works when it arrives before the reference.
  NightShift pending;
  pending.setCorrection(kOne / 2);
  CHECK(pending.setMatrix(boot) == NightShift::Input::accepted && same(pending.corrected(), diagonal(kOne / 2, kOne / 2, kOne / 2)));

  // Exercise negative determinants and the largest decode inputs through
  // the same integer inverse (also run under the UB sanitizer).
  NightShift permuted;
  const uint64_t swap[9] = {0, s3132(1), 0, s3132(1), 0, 0, 0, 0, s3132(1)};
  const uint64_t scaledSwap[9] = {0, s3132(.5), 0, s3132(.75), 0, 0, 0, 0, s3132(1)};
  CHECK(permuted.setMatrix(swap) == NightShift::Input::accepted);
  CHECK(permuted.setMatrix(scaledSwap) == NightShift::Input::accepted);
  CHECK(same(permuted.corrected(), diagonal(kOne / 2, 3 * kOne / 4, kOne)));
  NightShift maximum;
  const uint64_t four[9] = {s3132(4), 0, 0, 0, s3132(4), 0, 0, 0, s3132(4)};
  CHECK(maximum.setMatrix(four) == NightShift::Input::accepted);
  CHECK(maximum.setMatrix(half) == NightShift::Input::accepted);
  CHECK(std::abs(maximum.corrected().m[0][0] - decodeS3132(half, false).m.m[0][0] / 4) <= 1);
}

static void testState()
{
  State s;
  bool approximate = true;
  CHECK(isIdentity(s.linear()) && isIdentity(s.pcc(true, &approximate)) && !approximate);
  CHECK(s.setMatrix(2, diagonal(kOne, fromMilli(730), fromMilli(450))));   // Night Shift (say)
  Matrix filter = identity();
  filter.m[1][1] = kOne / 2;
  CHECK(s.setMatrix(5, filter) && s.functions() == ((1u << 2) | (1u << 5)));
  CHECK(std::abs(milli(s.linear().m[1][1]) - 365) <= 1 && milli(s.linear().m[2][2]) == 450);
  const Matrix pc = s.pcc(true, &approximate);
  CHECK(!approximate && std::abs(milli(pc.m[1][1]) - 633) <= 1);   // 0.365 ^ (1 / 2.2)
  CHECK(s.setMatrix(5, identity()) && s.functions() == (1u << 2));
  CHECK(!s.setMatrix(16, filter));
  Matrix grey{{{kOne / 3, kOne / 3, kOne / 3}, {kOne / 3, kOne / 3, kOne / 3}, {kOne / 3, kOne / 3, kOne / 3}}};
  s.setMatrix(7, grey);
  s.pcc(true, &approximate);
  CHECK(approximate);
  s.setMatrix(7, identity());
  s.setMatrix(2, identity());
  s.setGamma(diagonal(kOne, fromMilli(800), fromMilli(500)));
  const Matrix g = s.pcc(true, &approximate);
  CHECK(!approximate && milli(g.m[1][1]) == 800 && milli(g.m[2][2]) == 500);   // the gamma fit is encoded already
}

static std::vector<uint32_t> table(double r, double g, double b, bool curveB = false)
{
  std::vector<uint32_t> t(kGammaWords, 0);
  const double gain[3] = {r, g, b};
  for (unsigned c = 0; c < 3; ++c) {
    t[c * kGammaChannelWords] = 0x100;   // the channel's first word (unknown meaning): ignored
    for (unsigned k = 0; k < kGammaEntries; ++k) {
      double x = double(k) / 255;
      if (curveB && c == 2) x = x * x;
      t[c * kGammaChannelWords + 1 + k] = uint32_t(std::lround(x * gain[c] * 1023));
    }
  }
  return t;
}

static void testGamma()
{
  std::vector<uint32_t> t = table(1.0, 0.8, 0.5);
  GammaFit f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && f.reference == 0 && milli(f.m.m[0][0]) == 1000 && std::abs(milli(f.m.m[1][1]) - 800) <= 2 &&
        std::abs(milli(f.m.m[2][2]) - 500) <= 2 && isDiagonal(f.m));
  t = table(0.6, 0.9, 0.3);   // the brightest channel is the reference
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && f.reference == 1 && milli(f.m.m[1][1]) == 1000 && std::abs(milli(f.m.m[0][0]) - 667) <= 2);
  t = table(1.0, 1.0, 1.0, true);
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(!f.ok && std::strstr(f.why, "scaled copies"));
  t = table(1.0, 1.0, 1.0);
  t[1 + 100] = 0;
  CHECK(!fitGamma(t.data(), unsigned(t.size())).ok);   // not monotonic
  t.assign(kGammaWords, 0);
  CHECK(!fitGamma(t.data(), unsigned(t.size())).ok);
  CHECK(!fitGamma(t.data(), 64).ok && !fitGamma(nullptr, kGammaWords).ok);
  t = table(1.0, 1.0, 1.0);
  f = fitGamma(t.data(), unsigned(t.size()));
  CHECK(f.ok && isIdentity(f.m));
}

int main(int argc, char **argv)
{
  testDecode();
  testMath();
  testWords();
  testApply();
  testNightShift();
  testNightShiftRegisterWords();
  testNightShiftBootReference(argc == 2 ? argv[1] : nullptr);
  testState();
  testGamma();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu PCC host tests: all passed\n");
  return 0;
}
