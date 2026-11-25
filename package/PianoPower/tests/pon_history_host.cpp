// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. All rights reserved.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "PonHistory.h"
using namespace pianopower;

static std::vector<std::string> lines;
static void logLine(const char *fmt, ...)
{
  char buf[1024]; va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args); va_end(args); lines.emplace_back(buf);
}
static bool contains(const char *s)
{
  for (const auto &line : lines) if (line.find(s) != std::string::npos) return true;
  return false;
}
struct Model {
  uint8_t regs[2][256] = {};
  uint32_t cmd[2] = {};
  unsigned reads = 0, illegal = 0;
  bool missing = false, denied = false, moving = false, changed = false;
  Model()
  {
    for (auto &r : regs) { r[4] = 0x2e; r[5] = 1; r[0x40] = 0xa5; r[0x41] = 1; r[0x44] = 0x80; }
    regs[0][0x45] = 1; regs[0][0x46] = 0x4b;
    // Newest entry crosses ring end; bytes 230..233 with cursor zero.
    regs[1][0xbc] = 1; regs[1][0xbd] = 14;
  }
  static uint32_t rd(void *ctx, Window w, uint32_t off)
  {
    auto &m = *static_cast<Model *>(ctx);
    if (w == Window::core) {
      if (off == 0) return 0x70020000;
      if (off == 4) return m.missing ? 1 : 2;
      if (off == kSpmiApidMap) return 0x07400;
      if (off == kSpmiApidMap + 4) return 0x07500;
      return 0;
    }
    if (w == Window::cnfg) return 2; // observer access does not need owner EE0
    const unsigned bank = off / kObsvStride, reg = off % kObsvStride;
    assert(bank < 2);
    if (reg == kObsvStatus) return m.denied ? 5 : 1;
    assert(reg == kObsvData0 || reg == kObsvData1);
    const unsigned start = (m.cmd[bank] >> 4) & 255, len = (m.cmd[bank] & 7) + 1;
    uint32_t value = 0;
    for (unsigned i = reg == kObsvData0 ? 0 : 4; i < len && i < (reg == kObsvData0 ? 4u : 8u); ++i)
      value |= uint32_t(m.regs[bank][start + i]) << (8 * (i % 4));
    return value;
  }
  static void wr(void *ctx, Window w, uint32_t off, uint32_t value)
  {
    auto &m = *static_cast<Model *>(ctx);
    if (w != Window::obsv || off % kObsvStride || (value >> 27) != 1) { ++m.illegal; return; }
    const unsigned bank = off / kObsvStride, reg = (value >> 4) & 255, len = (value & 7) + 1;
    if (bank >= 2 || !((reg == 4 && len == 2) || (reg >= 0x40 && reg + len <= 0xc0))) ++m.illegal;
    assert(bank < 2); m.cmd[bank] = value; ++m.reads;
    if (m.moving && m.reads == 36) ++m.regs[0][0x46];
    if (m.changed && m.reads == 37) m.regs[1][0x70] ^= 1;
  }
  static void delay(void *, unsigned) {}
  Io io() { return {this, rd, wr, delay}; }
  bool run() { lines.clear(); const auto i = io(); return readPonHistory(i, spmiArbiter(i), logLine); }
};
static void ringBoundaries()
{
  uint8_t pages[2][128] = {};
  // Check ALL byte phases, both page crossings and the ring crossing.
  for (unsigned cursor = 0; cursor < kPonBytes; ++cursor) {
    for (auto &p : pages) memset(p, 0, sizeof p);
    pages[0][5] = 1; pages[0][6] = uint8_t(0x4b + cursor % 117); pages[0][7] = uint8_t(cursor / 117);
    assert(ponCursor(pages[0]) == int(cursor));
    for (unsigned j = 0; j < 58; ++j) {
      const uint8_t entry[4] = {uint8_t(j + 1), 6, 0, uint8_t(j + 0x80)};
      for (unsigned b = 0; b < 4; ++b) {
        const unsigned pos = (cursor + 2 + j * 4 + b) % 234;
        pages[pos / 117][11 + pos % 117] = entry[b];
      }
    }
    for (unsigned j = 0; j < 58; ++j) {
      const auto e = ponEntry(pages, cursor, j);
      assert(e.state == j + 1 && e.event == 6 && e.data() == j + 0x80);
    }
  }
  pages[0][5] = 2; assert(ponCursor(pages[0]) == -1);
  pages[0][5] = 1; pages[0][6] = 0xc0; assert(ponCursor(pages[0]) == -1);
  pages[0][6] = 0x4b; pages[0][7] = 2; assert(ponCursor(pages[0]) == -1);
}
int main()
{
  ringBoundaries();
  Model good; assert(good.run() && good.illegal == 0 && good.reads == 72);
  assert(contains("end valid=1 used=1 pon-success=1") && contains("entry=57 offset=230 raw=010e0000"));
  Model moving; moving.moving = true; assert(!moving.run() && !contains("entry="));
  Model changed; changed.changed = true; assert(!changed.run() && !contains("entry="));
  Model missing; missing.missing = true; assert(!missing.run() && missing.reads == 0);
  Model denied; denied.denied = true; assert(!denied.run() && denied.illegal == 0 && !contains("entry="));
  Model bad; bad.regs[0][0x45] = 0; assert(!bad.run() && contains("reason=layout"));
  Model empty; empty.regs[1][0xbc] = empty.regs[1][0xbd] = 0; assert(!empty.run() && contains("reason=empty"));
  char description[384];
  ponDescribe({0, 9, 0xb0, 0x60}, description, sizeof description);
  assert(strstr(description, "UVLO") && strstr(description, "OVLO") && strstr(description, "OTST3") && strstr(description, "PMIC_FAULT_WATCHDOG"));
  ponDescribe({6, 0x1c, 0x80, 0x40}, description, sizeof description); assert(strstr(description, "fault-sid=13"));
  ponDescribe({4, 6, 0, 0x83}, description, sizeof description); assert(strstr(description, "PMIC_WATCHDOG_S2"));
  ponDescribe({4, 6, 0, 0x84}, description, sizeof description); assert(strstr(description, "PS_HOLD"));
  ponDescribe({7, 8, 0x12, 0x34}, description, sizeof description); assert(strstr(description, "warm-count=4660"));
  ponDescribe({0xff, 0xff, 0x12, 0x34}, description, sizeof description); assert(strstr(description, "UNKNOWN_EVENT"));
  // Stable fixture is also a known-positive for the private capture validator.
  assert(good.run()); for (const auto &line : lines) printf("PianoPower: %s\n", line.c_str());
  puts("PASS PON history: 234 cursor phases, cross-page/ring entries, read-only bounds, unstable/denied/missing/empty gates and decode");
}
