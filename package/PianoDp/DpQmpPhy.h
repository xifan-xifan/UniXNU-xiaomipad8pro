// SPDX-License-Identifier: BSD-2-Clause-Patent
// Data-only PHY selection for piano. The host runner generates DpQmpTables.h.
#pragma once
#include "DpLinkTrain.h"
#include "DpQmpTables.h"

namespace pianodp {

static_assert(piano_dp_qmp_piano_max_mbps * 100u == kPianoDpMaxRateKhz);

struct DpQmpPhySettings {
  const PianoDpQmpRate *rate = nullptr;
  uint8_t swing = 0, preemphasis = 0;
};

constexpr bool lookupDpQmpPhy(uint32_t rateKhz, DpDrive drive, DpQmpPhySettings &out)
{
  out = {};
  if (!validDpLink({rateKhz, 1}) || rateKhz > kPianoDpMaxRateKhz ||
      drive.swing > 3 || drive.preemphasis > 3 - drive.swing)
    return false;

  const bool highRate = rateKhz == kDpHbr2Khz;
  const auto &swingTable = highRate ? piano_dp_qmp_swing_hbr3_hbr2 : piano_dp_qmp_swing_hbr_rbr;
  const auto &preTable = highRate ? piano_dp_qmp_pre_emphasis_hbr3_hbr2 : piano_dp_qmp_pre_emphasis_hbr_rbr;
  const uint8_t swing = swingTable[drive.swing][drive.preemphasis];
  const uint8_t preemphasis = preTable[drive.swing][drive.preemphasis];
  if (swing > 0x3f || preemphasis > 0x3f) return false;

  for (const auto &rate : piano_dp_qmp_rates) {
    if (rate.link_clk_hz != rateKhz * 1000u) continue;
    out = {&rate, swing, preemphasis};
    return true;
  }
  return false;
}

} // namespace pianodp
