// SPDX-License-Identifier: BSD-2-Clause-Patent
// The location-9 matrix and independent brightness correction from IOMobileFramebuffer on 27.3.
#pragma once
#include "DpuPcc.h"

namespace pianodpu::pcc {

constexpr unsigned kPanelDspps = 0x3;

class NightShift {
 public:
  enum class Input { invalid, blank, accepted };

  Input setMatrix(const uint64_t raw[9])
  {
    const Decoded d = decodeS3132(raw, false);
    if (!d.ok) return Input::invalid;
    bool blank = true, identityInput = true;
    for (unsigned k = 0; k < 9; ++k) {
      blank &= raw[k] == 0;
      identityInput &= raw[k] == (k % 4 == 0 ? 1ull << 32 : 0);
    }
    // M1 observed zero matrices around display blanking. A zero matrix would black out the panel.
    if (blank) return Input::blank;
    matrix_ = d.m;
    if (identityInput) correction_ = kOne;
    return Input::accepted;
  }

  void setCorrection(uint32_t gain) { correction_ = gain; }
  bool neutral() const { return isIdentity(matrix_) && correction_ == kOne; }

  Matrix corrected(const Matrix &base = identity()) const
  {
    // Each dot product has three terms with two inputs bounded by 4. Clamp to the PCC range after applying the gain.
    Matrix m = multiply(base, matrix_, 48 * kOne);
    for (auto &row : m.m)
      for (int32_t &q : row) {
        const int64_t scaled = int64_t(q) * correction_;
        const int64_t rounded = (scaled + (scaled >= 0 ? kOne / 2 : -kOne / 2)) / kOne;
        q = int32_t(rounded > 4 * kOne ? 4 * kOne : rounded < -4 * kOne ? -4 * kOne : rounded);
      }
    return m;
  }

 private:
  Matrix matrix_ = identity();
  uint32_t correction_ = kOne;
};

struct PanelPcc {
  Program dspp[kDsppCount]{};
};

inline bool same(const PanelPcc &a, const PanelPcc &b)
{
  for (unsigned d = 0; d < 2; ++d) {
    if (a.dspp[d].enabled != b.dspp[d].enabled) return false;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i)
        if (a.dspp[d].words.w[o][i] != b.dspp[d].words.w[o][i]) return false;
  }
  return true;
}

template <class Io>
bool capturePanelPcc(Io &io, const LivePath &p, PanelPcc &base)
{
  if (!p.ok || p.dspps != kPanelDspps) return false;
  for (unsigned d = 0; d < 2; ++d) {
    Program &program = base.dspp[d];
    program.enabled = io.rd(pccReg(d, 0)) & 1u;
    // Enabling PCC could activate offsets left in a bypassed block.
    if (!program.enabled && (io.rd(pccReg(d, 4)) || io.rd(pccReg(d, 8)) || io.rd(pccReg(d, 12)))) return false;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i)
        program.words.w[o][i] = io.rd(pccReg(d, kPccCoef[o][i])) & 0x3ffffu;
  }
  return true;
}

inline PanelPcc nightShiftPrograms(const PanelPcc &base, const NightShift &state)
{
  if (state.neutral()) return base;
  PanelPcc result;
  for (unsigned d = 0; d < 2; ++d) {
    Matrix baseMatrix = identity();
    if (base.dspp[d].enabled) {
      for (unsigned o = 0; o < 3; ++o)
        for (unsigned i = 0; i < 3; ++i) {
          const uint32_t w = base.dspp[d].words.w[o][i];
          const int32_t q = int32_t(w & 0x1ffffu) * 2;
          baseMatrix.m[o][i] = w & 0x20000u ? -q : q;
        }
    }
    result.dspp[d] = {pccWords(state.corrected(baseMatrix), Negative::signMagnitude), true};
  }
  return result;
}

struct NightShiftApply {
  Apply result;
  bool attempted = false, restored = false;
};

template <class Io>
NightShiftApply updateNightShift(Io &io, const LivePath &path, const PanelPcc &wanted, PanelPcc &last, bool replay)
{
  NightShiftApply update;
  if (!path.ok || path.dspps != kPanelDspps || (!replay && same(wanted, last))) return update;
  update.attempted = true;
  update.result = applyPrograms(io, path, wanted.dspp);
  if (update.result.ok) last = wanted;
  else update.restored = applyPrograms(io, path, last.dspp).ok;
  return update;
}

}  // namespace pianodpu::pcc
