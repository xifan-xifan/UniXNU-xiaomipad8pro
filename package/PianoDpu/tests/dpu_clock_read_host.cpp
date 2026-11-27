// SPDX-License-Identifier: BSD-2-Clause-Patent
// c++ -std=c++20 -Wall -Wextra -Werror dpu_clock_read_host.cpp -o /tmp/dpu_clock_read_host && /tmp/dpu_clock_read_host
#include "../DpuClockRead.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using namespace pianodpu;

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

struct Out {
  std::string text;
  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char row[480];
    va_list args;
    va_start(args, fmt);
    const int len = std::vsnprintf(row, sizeof row, fmt, args);
    va_end(args);
    CHECK(len >= 0 && static_cast<unsigned>(len) < sizeof row);
    text.append(row).append("\n");
  }
};

int main()
{
  namespace c = clkread;
  // 20261126-runs/v131y12p-m3-120/console.out:236–241. PLL and CRM words in
  // this fixture are synthetic. The console records only the RCG and gates.
  const uint64_t rcg = kDispCc + kCcMdpRcg;
  std::map<uint64_t, uint32_t> words = {
    {kGcc + kGccDispAhb, 0x88000003}, {kGcc + kGccDispHfAxi, 0x08200001},
    {kDispCc + kCcMdssGdsc, 0xfa22f800}, {kDispCc + kCcAhb, 0x08000001},
    {kDispCc + kCcNonGdscAhb, 0x08000000}, {kDispCc + kCcMdp, 0x08000001},
    {rcg, 0}, {rcg + 4, 0x00100105}, {rcg + 8, 0x1234}, {rcg + 12, 0xfedc}, {rcg + 16, 0xfdcb},
    {kDispCc, 0x80000005}, {kDispCc + 4, 1}, {kDispCc + 0xc, 0},
    {kDispCc + 0x10, 0x00440059}, {kDispCc + 0x14, 0xd800},
    {c::kCrmConfig, 0x00020100}, {c::kCrmConfig + 4, 0x02602000}, {c::kCrmConfig + 8, 0x04000000},
    {c::kCrmConfig + 12, 1},
  };
  unsigned loads = 0;
  auto load = [&](uint64_t at) -> uint32_t {
    ++loads;
    auto it = words.find(at);
    return it == words.end() ? c::kUnread : it->second;
  };
  unsigned surveyLoads = 0;
  CHECK(!surveyClocks(load, &surveyLoads).readable);
  CHECK(loads == 2);
  const auto gate = surveyClocks(load, &surveyLoads, true);
  CHECK(gate.readable && gate.rcgCfg == 0x00100105);
  auto clocks = c::capture(gate, load);
  CHECK(c::source(clocks.words.cfg) == 1 && c::dividerX2(clocks.words.cfg) == 6);
  CHECK(clocks.pllHz == 1725000000 && clocks.mdpHz == 575000000);
  Out out;
  c::format(clocks, out);
  CHECK(out.text.find("cfg=00100105 m=00001234 n=0000fedc d=0000fdcb mnd_width=0") != std::string::npos);
  CHECK(out.text.find("div_x2=6 hz=575000000 mhz=575.000000") != std::string::npos);
  CHECK(out.text.find("clkread_cesta status=ok version=00020100") != std::string::npos);
  CHECK(out.text.find("enabled=1 sw_drvs=6 bw_vcds=2") != std::string::npos);

  const auto before = loads;
  CHECK(!c::capture(ClockGate{}, load).mdpHz && loads == before);
  auto noCrm = gate;
  noCrm.nonGdscAhb = 0x80000000;
  auto checked = [&](uint64_t at) -> uint32_t { CHECK(at < c::kCrmConfig); return load(at); };
  CHECK(c::capture(noCrm, checked).crm.version == c::kUnread);
  words[kDispCc] = 0x5;
  CHECK(!c::capture(gate, load).mdpHz);
  words[rcg + 4] = 0;
  CHECK(c::capture(gate, load).mdpHz == 19200000);
  words[rcg + 4] = 2;
  CHECK(c::capture(gate, load).mdpHz == 12800000);
  words[rcg + 4] = 31;
  CHECK(c::capture(gate, load).mdpHz == 1200000);
  static_assert(c::dividerX2(0) == 2 && c::dividerX2(1) == 2 && c::dividerX2(UINT32_MAX) == 32);
  static_assert(c::pllRate(0xabcdffff, 0x1234ffff) == 1'258'291'199'707);
  words[rcg] = 1;
  CHECK(!c::capture(gate, load).mdpHz);
  words[rcg] = 0x80000000;
  CHECK(!c::capture(gate, load).mdpHz);
  words[rcg] = 0;
  words[rcg + 4] = 4 << 8;
  CHECK(!c::capture(gate, load).mdpHz);
  words[rcg + 4] = c::kUnread;
  CHECK(!c::capture(gate, load).mdpHz);
  unsigned cfgReads = 0;
  CHECK(!c::capture(gate, [&](uint64_t at) { return at == rcg + 4 ? cfgReads++ : load(at); }).mdpHz);

  // Synthetic RPMh responses and cmd-db aux. ACV's Y is a mask.
  static_assert(c::bcmX(0x60024012) == 9 && c::bcmY(0x60024012) == 18 && c::bcmValid(0x60024012));
  static_assert(c::bcmX(0x20000005) == 0 && c::bcmY(0x20000005) == 5 && c::bcmValid(0x20000005));
  static_assert(!c::bcmValid(0));
  const uint16_t levels[] = {0, 64, 128, 256, 320};
  CHECK(c::arcLevel(levels, 5, 4) == 320 && c::arcLevel(levels, 5, 0) == 0);
  CHECK(c::arcLevel(levels, 5, 5) == -1 && c::arcLevel(levels, 5, c::kUnread) == -1);
  std::puts("PASS: recorded SM8750 display gate and RCG, PLL/CRM and RPMh response decoding");
}
