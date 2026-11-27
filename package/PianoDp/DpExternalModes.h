// SPDX-License-Identifier: BSD-2-Clause-Patent
// D6 timings for a future IOAV/WindowServer publisher. No allocation or I/O.
#pragma once
#include "DpFake.h"
#include "DpLinkTrain.h"

namespace pianodp {

enum class DpPixelFormat : uint8_t { Rgb8, Rgb10, Ycbcr420 };

struct DpExternalAxis {
  uint16_t total, active, syncWidth, backPorch, frontPorch;
  bool positiveSync;
  constexpr bool operator==(const DpExternalAxis &) const = default;
};

struct DpExternalTiming {
  DpExternalAxis horizontal, vertical;
  uint32_t pixelHz, refresh16_16;
  constexpr bool operator==(const DpExternalTiming &) const = default;
};

struct DpExternalModes {
  static constexpr size_t kMaxModes = 16;
  static constexpr uint32_t kFirstTimingId = 1000;
  size_t count = 0;
  DpExternalTiming modes[kMaxModes]{};

  constexpr uint32_t timingId(size_t index) const { return index < count ? kFirstTimingId + uint32_t(index) : 0; }
};

constexpr uint32_t kDpExternalMaxPixelKhz = 594000;  // CTA UHD at 60 Hz

// Keep detailed timings (including the base preferred timing) before VICs, in EDID
// order within each group. Identical rasters, clocks and syncs share an ID.
inline DpExternalModes buildDpExternalModes(const EdidInfo &edid, DpLink link, DpPixelFormat format)
{
  DpExternalModes table;
  if (format != DpPixelFormat::Rgb8 || !validDpLink(link) || link.rateKhz > kPianoDpMaxRateKhz ||
      edid.modeCount > EdidInfo::kMaxModes || edid.tile.multiTile()) return table;

  for (unsigned source = 0; source < 2; ++source) {
    for (size_t i = 0; i < edid.modeCount; ++i) {
      const EdidMode &mode = edid.modes[i];
      const DpTiming &t = mode.timing;
      if (unsigned(mode.vic != 0) != source || !mode.rgb || !validFakeRaster(t.hActive, t.vActive) ||
          t.pixelKhz > kDpExternalMaxPixelKhz)
        continue;
      DpMsaTu stream;
      if (!computeDpMsaTu(t, link, stream)) continue;

      const uint32_t pixelHz = t.pixelKhz * 1000;
      const uint32_t framePixels = uint32_t(stream.hTotal) * stream.vTotal;
      if (pixelHz < uint64_t(framePixels) * kFakeMinRefreshHz || pixelHz > uint64_t(framePixels) * kFakeMaxRefreshHz)
        continue;
      const uint32_t refresh = uint32_t(((uint64_t(pixelHz) << 16) + framePixels / 2) / framePixels);
      const DpExternalTiming timing{
        {stream.hTotal, t.hActive, t.hSyncWidth, uint16_t(stream.hStart - t.hSyncWidth), t.hSyncOffset, t.hSyncPositive},
        {stream.vTotal, t.vActive, t.vSyncWidth, uint16_t(stream.vStart - t.vSyncWidth), t.vSyncOffset, t.vSyncPositive},
        pixelHz, refresh,
      };
      size_t index = 0;
      while (index < table.count && table.modes[index] != timing) ++index;
      if (index != table.count) continue;
      table.modes[table.count++] = timing;
      if (table.count == DpExternalModes::kMaxModes) return table;
    }
  }
  return table;
}

} // namespace pianodp
