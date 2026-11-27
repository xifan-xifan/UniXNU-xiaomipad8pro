// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "DpuSurvey.h"
#include "../PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.h"

namespace pianodpu::clkread {

// Register sources and voter scope are documented in docs/drivers/dpu-clkread.md.
constexpr uint32_t kUnread = 0xffffffffu;
constexpr uint64_t kCrmConfig = 0x0af29f00;
constexpr uint64_t kXoHz = 19200000;

struct ClockWords {
  uint32_t cmd, cfg, m, n, d;
  uint32_t pllMode, pllOpMode, pllStatus, pllL, pllAlpha;
};

struct CrmWords {
  uint32_t version = kUnread, cfg1 = kUnread, cfg2 = kUnread, enable = kUnread;
};

struct Clocks {
  ClockWords words{};
  uint64_t pllHz = 0, mdpHz = 0;
  const char *status = "gate-closed";
  const char *pllStatus = "gate-closed";
  CrmWords crm;
};

template <class Load>
ClockWords loadClocks(Load load)
{
  return {load(kDispCc + kCcMdpRcg), load(kDispCc + kCcMdpRcg + 4), load(kDispCc + kCcMdpRcg + 8),
          load(kDispCc + kCcMdpRcg + 12), load(kDispCc + kCcMdpRcg + 16),
          load(kDispCc), load(kDispCc + 4), load(kDispCc + 0xc), load(kDispCc + 0x10), load(kDispCc + 0x14)};
}

inline bool stableClocks(const ClockWords &a, const ClockWords &b)
{
  return a.cmd == b.cmd && a.cfg == b.cfg && a.pllMode == b.pllMode && a.pllOpMode == b.pllOpMode &&
         a.pllL == b.pllL && a.pllAlpha == b.pllAlpha;
}

constexpr unsigned source(uint32_t cfg) { return (cfg >> 8) & 7; }
constexpr unsigned hid(uint32_t cfg) { return cfg & 31; }
constexpr unsigned dividerX2(uint32_t cfg) { return hid(cfg) ? hid(cfg) + 1 : 2; }
constexpr uint64_t pllRate(uint32_t l, uint32_t alpha)
{
  return kXoHz * (l & 0xffff) + ((kXoHz * (alpha & 0xffff)) >> 16);
}

template <class Load>
Clocks capture(const ClockGate &gate, Load load)
{
  Clocks clocks;
  if (!gate.readable) return clocks;
  clocks.words = loadClocks(load);
  const auto &w = clocks.words;
  if (branchOn(gate.nonGdscAhb)) {
    clocks.crm = {load(kCrmConfig), load(kCrmConfig + 4), load(kCrmConfig + 8), load(kCrmConfig + 12)};
  }
  // Recheck rate inputs around the sample. M/N/D have zero hardware width on MDP.
  const ClockWords after = loadClocks(load);
  if (!stableClocks(w, after)) {
    clocks.status = clocks.pllStatus = "changing";
    return clocks;
  }
  if (w.pllMode == kUnread || w.pllOpMode == kUnread || w.pllL == kUnread || w.pllAlpha == kUnread) {
    clocks.pllStatus = "unreadable";
  } else if ((w.pllOpMode & 1) && (w.pllMode & 0x80000001u) == 0x80000001u && (w.pllL & 0xffff)) {
    clocks.pllStatus = "ok";
    clocks.pllHz = pllRate(w.pllL, w.pllAlpha);
  } else {
    clocks.pllStatus = "not-running";
  }

  if (w.cmd == kUnread || w.cfg == kUnread) clocks.status = "unreadable";
  else if (w.cmd & 1) clocks.status = "updating";
  else if (w.cmd >> 31) clocks.status = "root-off";
  else if (source(w.cfg) == 1 && !clocks.pllHz) clocks.status = "pll-unavailable";
  else if (source(w.cfg) <= 1) {
    clocks.status = "ok";
    const uint64_t parentHz = source(w.cfg) == 1 ? clocks.pllHz : kXoHz;
    clocks.mdpHz = parentHz * 2 / dividerX2(w.cfg);
  } else clocks.status = "unsupported-source";
  return clocks;
}

constexpr unsigned bcmX(uint32_t word) { return (word >> 14) & Sm8750RpmhBcmVoteMask; }
constexpr unsigned bcmY(uint32_t word) { return word & Sm8750RpmhBcmVoteMask; }
constexpr bool bcmValid(uint32_t word) { return word & (1u << 29); }

inline int arcLevel(const uint16_t *levels, unsigned count, uint32_t word)
{
  return word < count ? levels[word] : -1;
}

template <class Out>
void format(const Clocks &clocks, Out &out)
{
  const auto &w = clocks.words;
  out.line("clkread_mdp status=%s cmd=%08x cfg=%08x m=%08x n=%08x d=%08x mnd_width=0 mode=%u source=%u div_x2=%u hz=%llu mhz=%llu.%06u",
           clocks.status, w.cmd, w.cfg, w.m, w.n, w.d, unsigned((w.cfg >> 12) & 3), source(w.cfg), dividerX2(w.cfg),
           (unsigned long long)clocks.mdpHz, (unsigned long long)(clocks.mdpHz / 1000000), unsigned(clocks.mdpHz % 1000000));
  out.line("clkread_pll0 status=%s mode=%08x opmode=%08x pll_status=%08x l=%08x alpha=%08x hz=%llu mhz=%llu.%06u",
           clocks.pllStatus, w.pllMode, w.pllOpMode, w.pllStatus, w.pllL, w.pllAlpha,
           (unsigned long long)clocks.pllHz, (unsigned long long)(clocks.pllHz / 1000000), unsigned(clocks.pllHz % 1000000));
  const auto &crm = clocks.crm;
  const unsigned major = (crm.version >> 16) & 0xff;
  const bool readable = crm.version != kUnread && crm.cfg1 != kUnread && crm.cfg2 != kUnread && crm.enable != kUnread;
  const char *status = !readable ? "unavailable" : major == 2 ? "ok" : "unsupported-version";
  out.line("clkread_cesta status=%s version=%08x cfg1=%08x cfg2=%08x enable=%08x enabled=%u sw_drvs=%u bw_vcds=%u owner=unavailable",
           status, crm.version, crm.cfg1, crm.cfg2, crm.enable, unsigned(readable && crm.enable != 0),
           readable && major == 2 ? unsigned((crm.cfg1 >> 20) & 15) : 0, readable && major == 2 ? unsigned((crm.cfg1 >> 24) & 7) : 0);
}

template <class Out, class ReadVotes>
void formatReadback(const Clocks &clocks, bool snapshotOn, Out &out, ReadVotes readVotes)
{
  format(clocks, out);
  if (snapshotOn) out.line("clkread_rpmh status=unavailable (ACTIVE transport requires stores)");
  else readVotes(out);
}

}  // namespace pianodpu::clkread
