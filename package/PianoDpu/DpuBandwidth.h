// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "DpuHw.h"
#include "../PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h"

namespace pianodpu::modes {

// SM8750 DPU perf for the bonded panel and one linear 32-bpp surface at unity scale.
// The 35-line prefill fits into VBP + VSYNC, so the base fetch sets the vote.
constexpr uint32_t scanoutAvgKBps(const Mode &mode)
{
  return uint32_t(uint64_t(kPanelWidth) * 4 * modeVtotal(mode) * mode.hz * 120 / 100 / 1000);
}

// Keep the reference splash peak when taking over the same DRV-2 resource.
constexpr uint32_t kKmsPeakKBps = 800000;
constexpr uint32_t kMm0PeakKBps = kSplashPeakKBps > kKmsPeakKBps ? kSplashPeakKBps : kKmsPeakKBps;

constexpr uint64_t bcmDivide(uint64_t value, uint32_t divisor)
{
  return value && value < divisor ? 1 : value / divisor;
}

// qns_mem_noc_hf is two 32-byte channels. The Qualcomm BCM voter divides AB
// across channels and IB by the bus width alone, then scales by command-db unit.
constexpr uint32_t mm0Field(uint32_t kbps, uint32_t unit, uint16_t auxWidth, unsigned channels)
{
  const uint64_t scaled = bcmDivide(uint64_t(kbps) * auxWidth, 32 * channels) * 1000;
  const uint64_t vote = bcmDivide(scaled, unit);
  return vote > Sm8750RpmhBcmVoteMask ? Sm8750RpmhBcmVoteMask : uint32_t(vote);
}

struct Mm0Vote {
  uint32_t average = 0, peak = 0;
};

constexpr Mm0Vote mm0Vote(const Mode &mode, uint32_t unit, uint16_t auxWidth)
{
  if (!unit || !auxWidth) return {};
  return {mm0Field(scanoutAvgKBps(mode), unit, auxWidth, 2), mm0Field(kMm0PeakKBps, unit, auxWidth, 1)};
}

}  // namespace pianodpu::modes
