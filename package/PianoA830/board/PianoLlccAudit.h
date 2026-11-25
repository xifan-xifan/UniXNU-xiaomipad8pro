// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#pragma once
#include "PianoLlccIsland.h"

namespace pianollcc {

struct AuditRead {
  Block view;
  uint32_t offset;
  uint32_t mask;
  uint32_t expected;

  constexpr uint64_t physical() const { return kBlockPhys[view] + offset; }
  constexpr bool accepts(uint32_t word) const { return word != UINT32_MAX && (word & mask) == expected; }
};

// Register facts from third-party yzddmr6/linux-sm8750-piano at
// 1c21cb9895d69c1c5e83ceae68952fedf522644a. After the two probe reads,
// follow the v6 control-word order for SCID12. The boot's flag bits are data.
constexpr AuditRead kAuditReads[] = {
  {kOr,       kHwInfo,  0xff000000u, kVersion6},
  {kChannel0, kStatus0, 0xf0000000u, 4u << 28},
  {kOr, 0x42080, 0, 0},  // WRS_EN
  {kOr, 0x42088, 0, 0},  // WRS_CACHEABLE_EN
  {kOr, 0x42008, 0, 0},  // ALGO_STALE_EN
  {kOr, 0x42010, 0, 0},  // ALGO_STALE_CAP_EN
  {kOr, 0x42018, 0, 0},  // ALGO_MRU0
  {kOr, 0x42020, 0, 0},  // ALGO_MRU1
  {kOr, 0x42028, 0, 0},  // ALGO_ALLOC0
  {kOr, 0x42030, 0, 0},  // ALGO_ALLOC1
  {kOr, 0x42038, 0, 0},  // ALGO_ALLOC2
  {kOr, 0x42040, 0, 0},  // ALGO_ALLOC3
};
constexpr unsigned kAuditCount = sizeof kAuditReads / sizeof kAuditReads[0];
static_assert(kAuditCount == 12, "two metadata reads and ten SCID12 control reads");

struct Island3Access {
  unsigned seq;
  Block view;
  uint32_t offset;
  uint32_t mask = 0;
  uint32_t expected = 0;
  bool store = false;
  constexpr uint64_t physical() const { return kBlockPhys[view] + offset; }
};

constexpr uint32_t kIsland3Bit = 1u << kScid;
constexpr uint32_t kIsland3Alloc3 = 0x42040;
// Repeated sequence numbers identify groups. Each row gets its own host token.
constexpr Island3Access kIsland3Accesses[] = {
  {1, kOr, kHwInfo, UINT32_MAX, kVersion6},
  {2, kChannel0, kStatus0, 0xf0000000u, 4u << 28},
  {3, kOr, kTrpStatus, kActive},
  {4, kOr, kAttr0, UINT32_MAX},
  {4, kOr, kAttr1, UINT32_MAX},
  {4, kOr, kAttr2, UINT32_MAX},
  {4, kOr, kAttr3, UINT32_MAX},
  {5, kOr, 0x41788},  // SCID30 ATTR1, record only
  {6, kOr, kAttr0, 0, kReservedWays, true},
  {7, kOr, kAttr1, 0, kBonusWays, true},
  {8, kOr, kAttr2, 0, kAttr2Want, true},
  {9, kOr, kAttr3, 0, attr3For(4), true},
  {10, kOr, 0x42080, kIsland3Bit},
  {11, kOr, 0x42088, kIsland3Bit},
  {12, kOr, 0x42008, kIsland3Bit},
  {13, kOr, 0x42010, kIsland3Bit},
  {14, kOr, 0x42018, kIsland3Bit},
  {15, kOr, 0x42020, kIsland3Bit},
  {16, kOr, 0x42028, kIsland3Bit},
  {17, kOr, 0x42030, kIsland3Bit},
  {18, kOr, 0x42038, kIsland3Bit},
  {19, kOr, kIsland3Alloc3},
  {20, kOr, kIsland3Alloc3, 0, 0, true},
  {21, kOr, kAttr0, UINT32_MAX, kReservedWays},
  {21, kOr, kAttr1, UINT32_MAX, kBonusWays},
  {21, kOr, kAttr2, UINT32_MAX, kAttr2Want},
  {21, kOr, kAttr3, UINT32_MAX, attr3For(4)},
  {21, kOr, kIsland3Alloc3, UINT32_MAX},
};
constexpr unsigned kIsland3Count = sizeof kIsland3Accesses / sizeof kIsland3Accesses[0];
static_assert(kIsland3Count == 28, "up to 28 separately acknowledged accesses");

}  // namespace pianollcc
