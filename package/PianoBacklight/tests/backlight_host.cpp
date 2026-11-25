// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoBacklight's portable parts: BitI2c.h against a bit-level model of a KTZ8866 (address 0x11, register pointer with
// auto-increment, repeated-START reads, NACK for another address, clock stretching); Ktz8866.h's steps, levels and log limit; the
// Controller's register sequences on two register models (survey writes nothing; boot level; display off/on; steps and the floor;
// the lid; failures, retries and giving up; a missing IC; only 0x04/0x05/0x08 ever written, BL_EN bit 7 never, bits 5:0 kept); the
// whole stack (Controller over BitI2c over two bit-level ICs); PianoBacklightClient.h's KeyWatch.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. backlight_host.cpp -o /tmp/backlight_host && /tmp/backlight_host
#include "BitI2c.h"
#include "Ktz8866.h"
#include "NativeBrightness.h"
#include "PianoBacklightClient.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianobacklight;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The datasheet's reset values (Table 5), with the ID's reserved bits 0b001.
static void resetRegs(uint8_t *r)
{
  std::memset(r, 0, kRegCount);
  r[kRegId] = 0x21; r[kRegCfg1] = 0xfa; r[kRegCfg2] = 0x8d; r[kRegBrtLsb] = 0x07; r[kRegBrtMsb] = 0xff; r[kRegEn] = 0x3f;
  r[kRegLcd1] = 0x18; r[kRegLcd2] = 0x11; r[kRegBoost] = 0x28; r[kRegOutp] = 0x1e; r[kRegOutn] = 0x1c; r[kRegOpt1] = 0x80;
  r[kRegOpt2] = 0x77; r[kRegRamp] = 0x44; r[kRegIfs] = 0xf8;
}

// ------------------------------------------------------------------------------------------------------------- bit-level IC
struct Chip {
  uint8_t address = kAddress;
  uint8_t reg[kRegCount] = {};
  uint8_t pointer = 0;
  bool sdaLow = false, sclLow = false, mSda = true, mScl = true, lastSda = true, lastScl = true;
  enum { idle, addr, ackAddr, wr, ackWr, rd, rdAck, ignore } st = idle;
  int bits = 0, stretch = 0, holding = 0;
  uint8_t shift = 0, byte = 0;
  bool reading = false, masterAck = false, firstByte = false;
  std::vector<std::pair<uint8_t, uint8_t>> writes;   // (register, value)
  unsigned starts = 0, stops = 0;
  bool sda() const { return mSda && !sdaLow; }
  bool scl() const { return mScl && !sclLow; }
  void load() { byte = pointer < kRegCount ? reg[pointer] : 0; ++pointer; sdaLow = !(byte & 0x80); }
  void rising()
  {
    if (st == addr || st == wr) { shift = uint8_t(shift << 1 | (sda() ? 1 : 0)); ++bits; }
    else if (st == rdAck) masterAck = !sda();
  }
  void falling()
  {
    switch (st) {
    case addr:
      if (bits == 8) {
        if ((shift >> 1) == address) { st = ackAddr; reading = shift & 1; sdaLow = true; }
        else st = ignore;
      }
      break;
    case ackAddr:
      sdaLow = false;
      if (stretch) { sclLow = true; holding = stretch; }
      if (reading) { st = rd; bits = 0; load(); }
      else { st = wr; bits = 0; shift = 0; firstByte = true; }
      break;
    case wr:
      if (bits == 8) {
        if (firstByte) pointer = shift;
        else {
          writes.push_back({pointer, shift});
          if (pointer < kRegCount) reg[pointer] = shift;
          ++pointer;
        }
        firstByte = false;
        st = ackWr;
        sdaLow = true;
      }
      break;
    case ackWr: sdaLow = false; st = wr; bits = 0; shift = 0; break;
    case rd:
      if (++bits < 8) sdaLow = !((byte >> (7 - bits)) & 1);
      else { sdaLow = false; st = rdAck; }
      break;
    case rdAck:
      if (masterAck) { st = rd; bits = 0; load(); }
      else { st = ignore; sdaLow = false; }
      break;
    default: break;
    }
  }
  void update()
  {
    const bool s = sda(), c = scl();
    if (c && lastScl && lastSda && !s) { st = addr; bits = 0; shift = 0; ++starts; }   // START or repeated START
    else if (c && lastScl && !lastSda && s) { st = idle; ++stops; }                    // STOP
    else if (c && !lastScl) rising();
    else if (!c && lastScl) falling();
    lastSda = sda();
    lastScl = scl();
  }
};
static Chip gChip[2];
static unsigned gDelays;
static void cSda(void *ctx, bool release) { Chip *c = static_cast<Chip *>(ctx); c->mSda = release; c->update(); }
static void cScl(void *ctx, bool release) { Chip *c = static_cast<Chip *>(ctx); c->mScl = release; c->update(); }
static bool cReadSda(void *ctx) { return static_cast<Chip *>(ctx)->sda(); }
static bool cReadScl(void *ctx) { return static_cast<Chip *>(ctx)->scl(); }
static void cDelay(void *ctx, unsigned)
{
  ++gDelays;
  Chip *c = static_cast<Chip *>(ctx);
  if (c->holding && !--c->holding) { c->sclLow = false; c->update(); }
}

static void testI2c()
{
  Chip &c = gChip[0];
  resetRegs(c.reg);
  BitI2c bus;
  bus.configure({&c, cSda, cScl, cReadSda, cReadScl, cDelay}, 5, 2000);
  uint8_t v = 0;
  const uint8_t id = kRegId;
  CHECK(bus.writeRead(kAddress, &id, 1, &v, 1) == I2c::ok && v == 0x21);
  CHECK(c.writes.empty());
  const uint8_t w[2] = {kRegBrtMsb, 0x73};
  CHECK(bus.write(kAddress, w, 2) == I2c::ok);
  CHECK(c.writes.size() == 1 && c.writes[0].first == kRegBrtMsb && c.writes[0].second == 0x73 && c.reg[kRegBrtMsb] == 0x73);
  // Auto-increment: two bytes read from 0x04.
  uint8_t two[2] = {};
  const uint8_t lsb = kRegBrtLsb;
  CHECK(bus.writeRead(kAddress, &lsb, 1, two, 2) == I2c::ok && two[0] == 0x07 && two[1] == 0x73);
  // Clock stretching after the address; nobody at 0x12; a stuck clock.
  c.stretch = 30;
  CHECK(bus.writeRead(kAddress, &id, 1, &v, 1) == I2c::ok && v == 0x21);
  c.stretch = 0;
  CHECK(bus.write(0x12, w, 2) == I2c::nackAddress);
  CHECK(c.writes.size() == 1);
  CHECK(c.stops >= 5);
}

// ------------------------------------------------------------------------------------------------------------- steps, levels, log limit
static void testSteps()
{
  Steps s;
  s.build(128, false);
  const uint16_t expected[] = {128, 161, 203, 256, 322, 406, 512, 645, 812, 1024, 1290, 1625, 2047};
  CHECK(s.count() == sizeof expected / sizeof expected[0]);
  for (unsigned i = 0; i < s.count() && i < 13; ++i) CHECK(s.at(i) == expected[i]);
  for (unsigned i = 1; i < s.count(); ++i) CHECK(s.at(i) > s.at(i - 1));
  CHECK(s.up(128) == 161 && s.up(150) == 161 && s.up(1500) == 1625 && s.up(1625) == 2047 && s.up(2047) == 2047);
  CHECK(s.down(2047) == 1625 && s.down(1500) == 1290 && s.down(161) == 128 && s.down(128) == 128 && s.down(140) == 128);
  CHECK(s.down(100) == 100);   // under the floor (found so): "down" never raises it
  CHECK(s.up(100) == 128);
  s.build(64, false);
  CHECK(s.at(0) == 64 && s.at(1) == 81 && s.at(s.count() - 1) == 2047);
  s.build(128, true);
  CHECK(s.count() == 17 && s.at(0) == 128 && s.at(16) == 2047 && s.at(8) == 128 + (1919 * 8 + 8) / 16);
  CHECK(codeForPercent(45, 128) == 921 && codeForPercent(0, 128) == 128 && codeForPercent(100, 128) == 2047 && codeForPercent(250, 128) == 2047);
  CHECK(codeForPercent(3, 128) == 128);
  CHECK(percentOf(2047) == 100 && percentOf(921) == 45 && percentOf(0) == 0);
  CHECK(rampUs(0) == 512 && rampUs(1) == 1000 && rampUs(4) == 8000 && rampUs(15) == 16384000);
  CHECK(idIsKtz8866(0x21) && idIsKtz8866(0x20) && !idIsKtz8866(0xff) && !idIsKtz8866(0x00) && !idIsKtz8866(0x61));
  CHECK(enableValue(0x3f, true) == 0x7f && enableValue(0x7f, false) == 0x3f && enableValue(0xff, true) == 0x7f && enableValue(0x15, false) == 0x15);
  CHECK(writable(0x04) && writable(0x05) && writable(0x08) && !writable(0x09) && !writable(0x0f) && !writable(0x15) && !writable(0x02));

  LogLimit l(3, 1000);
  uint32_t d = 0;
  CHECK(l.allow(0, &d) && d == 0);
  CHECK(l.allow(10, &d) && l.allow(20, &d));
  CHECK(!l.allow(30, &d) && !l.allow(40, &d));
  CHECK(l.allow(1000, &d) && d == 2);
  CHECK(l.allow(1001, &d) && d == 0);
}

// ------------------------------------------------------------------------------------------------------------- register-level model
struct Model final : Ics {
  uint8_t reg[kIcs][kRegCount];
  bool present[kIcs] = {true, true};
  bool deaf[kIcs] = {false, false};   // writes acknowledged, not taken
  int failWrites[kIcs] = {0, 0};   // the next N writes of the IC are not acknowledged (-1: all)
  struct Access {
    unsigned ic;
    bool write;
    uint8_t reg, value;
    bool operator==(const Access &) const = default;
  };
  std::map<std::pair<unsigned, uint8_t>, std::vector<int>> readReplies;   // per-register overrides, -1 for NACK
  std::vector<Access> trace;
  std::vector<std::string> lines;
  bool inBurst = false;
  unsigned bursts = 0;
  uint64_t now = 0;
  Model() { resetRegs(reg[0]); resetRegs(reg[1]); }
  bool read(unsigned ic, uint8_t r, uint8_t *v) override
  {
    inBurst = true;
    if (!present[ic]) return false;
    *v = reg[ic][r];
    auto reply = readReplies.find({ic, r});
    if (reply != readReplies.end() && !reply->second.empty()) {
      const int next = reply->second.front();
      reply->second.erase(reply->second.begin());
      if (next < 0) return false;
      *v = uint8_t(next);
    }
    if (r == kRegFlag) reg[ic][r] = 0;   // cleared by the read
    trace.push_back({ic, false, r, *v});
    return true;
  }
  bool write(unsigned ic, uint8_t r, uint8_t v) override
  {
    inBurst = true;
    if (!present[ic]) return false;
    if (failWrites[ic]) {
      if (failWrites[ic] > 0) --failWrites[ic];
      return false;
    }
    trace.push_back({ic, true, r, v});
    if (deaf[ic]) return true;
    if (r == kRegEn && (v & kEnSoftReset)) resetRegs(reg[ic]);
    else reg[ic][r] = v;
    return true;
  }
  void done() override { if (inBurst) ++bursts; inBurst = false; }
  void log(const char *line) override { lines.push_back(line); if (std::getenv("BACKLIGHT_VERBOSE")) std::printf("  | %s\n", line); }
  uint64_t nowMs() override { return now += 5; }
  std::vector<Access> writes() const { std::vector<Access> w; for (const Access &a : trace) if (a.write) w.push_back(a); return w; }
  uint16_t code(unsigned ic) const { return codeOf(reg[ic][kRegBrtLsb], reg[ic][kRegBrtMsb]); }
  bool lit(unsigned ic) const { return reg[ic][kRegEn] & kEnBacklight; }
  void setFound(unsigned ic, uint16_t code, bool lit, uint8_t sinks = 0x3f)
  {
    reg[ic][kRegBrtLsb] = uint8_t(code & 7);
    reg[ic][kRegBrtMsb] = uint8_t(code >> 3);
    reg[ic][kRegEn] = uint8_t(sinks | (lit ? kEnBacklight : 0));
  }
  bool logged(const char *needle) const
  {
    for (const std::string &l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

// Every write ever made: one of the three registers, BL_EN without bit 7, an MSB at or above the floor's.
static void checkWhitelist(const Model &m, uint16_t floor)
{
  for (const Model::Access &a : m.trace) {
    if (!a.write) continue;
    CHECK(writable(a.reg));
    if (a.reg == kRegEn) CHECK(!(a.value & kEnSoftReset) && (a.value & kEnSinks) == 0x3f);
    if (a.reg == kRegBrtMsb) CHECK(a.value >= (floor >> 3));
  }
}

static Config config(uint32_t percent = 45, bool force = false, bool lid = false)
{
  Config c;
  c.floor = 128;
  c.bootPercent = percent;
  c.bootForce = force;
  c.lidWatch = lid;
  return c;
}

static void testSurvey()
{
  Model m;
  m.setFound(0, 1500, true, 0x3f);
  m.setFound(1, 1500, true, 0x3f);
  m.reg[0][kRegFlag] = 0x02;
  Controller c(m);
  c.configure(config());
  CHECK(c.survey());
  CHECK(m.writes().empty());
  CHECK(m.bursts == 1 && !m.inBurst);
  CHECK(c.ic(0).foundCode == 1500 && c.ic(0).foundLit && c.ic(1).foundCode == 1500);
  CHECK(m.reg[0][kRegFlag] == 0x02);   // FLAG not read by default
  for (const Model::Access &a : m.trace) CHECK(a.reg != kRegFlag);
  CHECK(m.logged("survey primary: ID 0x21 (KTZ8866, device field 4)"));
  CHECK(m.logged("OVP 40.0 V") && m.logged("linear mapping") && m.logged("code 1500 (73 %"));
  CHECK(m.logged("full scale 30.0 mA per sink") && m.logged("ramps 0x14 0x44 (on 8000 us, off 8000 us)"));

  Model f;
  f.reg[1][kRegFlag] = 0x82;
  Controller cf(f);
  Config cfgFlags = config();
  cfgFlags.readFlags = true;
  cf.configure(cfgFlags);
  CHECK(cf.survey());
  CHECK(f.reg[1][kRegFlag] == 0 && f.logged("FLAG 0x82 LED short BL OVP (now cleared)"));

  // A missing secondary: survey false, start refused, nothing ever written whatever happens.
  Model g;
  g.present[1] = false;
  Controller cg(g);
  cg.configure(config());
  CHECK(!cg.survey());
  CHECK(!cg.start());
  CHECK(g.logged("control: OFF, the secondary did not answer"));
  cg.display(true, 1, 1);
  cg.display(false, 0, 1);
  cg.step(3, "test");
  cg.percent(80, "test");
  cg.retry();
  CHECK(g.writes().empty());
  // Something that is not a KTZ8866 at 0x11.
  Model h;
  h.reg[0][kRegId] = 0xff;
  Controller ch(h);
  ch.configure(config());
  CHECK(!ch.survey() && !ch.start() && h.writes().empty());
}

static void testBootLevel()
{
  // Found high (1500 = 73 %): lowered to 45 % (921), LSB then MSB, primary then secondary; BL_EN already set: not written.
  Model m;
  m.setFound(0, 1500, true);
  m.setFound(1, 1500, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  CHECK(c.start());
  auto w = m.writes();
  CHECK(w.size() == 4);
  if (w.size() == 4) {
    CHECK(w[0].ic == 0 && w[0].reg == kRegBrtLsb && w[0].value == (921 & 7));
    CHECK(w[1].ic == 0 && w[1].reg == kRegBrtMsb && w[1].value == (921 >> 3));
    CHECK(w[2].ic == 1 && w[2].reg == kRegBrtLsb && w[3].ic == 1 && w[3].reg == kRegBrtMsb);
  }
  CHECK(m.code(0) == 921 && m.code(1) == 921 && m.lit(0) && m.lit(1));
  CHECK(m.logged("boot level: found code 1500 (73 %), set 921 (45 %"));
  CHECK(m.logged("write primary: BL_BRT_LSB 0x04 = 0x01, BL_BRT_MSB 0x05 = 0x73: code 921 (45 %), was 1500; read back ok"));
  CHECK(!m.inBurst && !c.pending());
  checkWhitelist(m, 128);

  // Found moderate (600): kept, nothing written.
  Model k;
  k.setFound(0, 600, true);
  k.setFound(1, 600, true);
  Controller ck(k);
  ck.configure(config());
  ck.survey();
  CHECK(ck.start() && k.writes().empty() && ck.code() == 600);
  CHECK(k.logged("not above 921 (45 %): kept"));

  // piano-backlight-level=30 given: set even though lower... and higher.
  Model g;
  g.setFound(0, 300, true);
  g.setFound(1, 300, true);
  Controller cg(g);
  cg.configure(config(30, true));
  cg.survey();
  CHECK(cg.start() && g.code(0) == codeForPercent(30, 128) && g.code(1) == codeForPercent(30, 128));

  // piano-backlight-level=0: as found, even when high.
  Model a;
  a.setFound(0, 2047, true);
  a.setFound(1, 2047, true);
  Controller ca(a);
  ca.configure(config(0));
  ca.survey();
  CHECK(ca.start() && a.writes().empty() && a.logged("boot level: as found, code 2047"));

  // Different codes on the two ICs (both under the cap): both get the primary's.
  Model d;
  d.setFound(0, 700, true);
  d.setFound(1, 650, true);
  Controller cd(d);
  cd.configure(config());
  cd.survey();
  CHECK(cd.start() && d.code(1) == 700 && d.writes().size() == 2 && d.logged("the ICs hold different codes (700, 650)"));

  // Found dark (BL_EN clear): turned on, code first, then BL_EN (bits 5:0 kept).
  Model o;
  o.setFound(0, 800, false);
  o.setFound(1, 800, false);
  Controller co(o);
  co.configure(config());
  co.survey();
  CHECK(co.start() && o.lit(0) && o.lit(1) && o.reg[0][kRegEn] == 0x7f);
  auto wo = o.writes();
  CHECK(wo.size() == 2 && wo[0].reg == kRegEn && wo[0].value == 0x7f && wo[1].reg == kRegEn);
  CHECK(o.logged("found the backlight OFF"));
  checkWhitelist(o, 128);

  // Found code 0 with BL_EN set (the IC's own off): raised to the boot level.
  Model z;
  z.setFound(0, 0, true);
  z.setFound(1, 0, true);
  Controller cz(z);
  cz.configure(config());
  cz.survey();
  CHECK(cz.start() && z.code(0) == 921 && z.code(1) == 921);
}

static void testDisplayAndSteps()
{
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  CHECK(c.start() && m.writes().empty());
  // Off before the first on (boot): ignored.
  c.display(false, 0, 0);
  CHECK(m.writes().empty() && m.lit(0) && m.logged("before the first on: ignored"));
  c.display(true, 1, 1);
  CHECK(m.writes().empty());
  // Display sleep: BL_EN cleared on both (sinks kept), the code untouched.
  const size_t before = m.trace.size();
  c.display(false, 1, 0);
  auto w = m.writes();
  CHECK(w.size() == 2 && w[0].ic == 0 && w[0].reg == kRegEn && w[0].value == 0x3f && w[1].ic == 1 && w[1].value == 0x3f);
  CHECK(!m.lit(0) && !m.lit(1) && m.code(0) == 812 && m.code(1) == 812);
  CHECK(m.trace.size() - before == 6);   // per IC: read BL_EN, write it, read it back
  CHECK(m.logged("write primary: BL_EN 0x08 = 0x3f (was 0x7f): backlight OFF, sinks 0x3f kept; read back ok"));
  // A second off changes nothing.
  c.display(false, 1, 0);
  CHECK(m.writes().size() == 2);
  // A step while dark: remembered, not written.
  c.step(1, "test keys");
  CHECK(m.writes().size() == 2 && c.code() == 1024 && m.logged("(applied when the backlight is on again)"));
  // Wake: the new code first, then BL_EN, on each IC.
  c.display(true, 1, 1);
  w = m.writes();
  CHECK(w.size() == 8);
  if (w.size() == 8) {
    CHECK(w[2].reg == kRegBrtLsb && w[3].reg == kRegBrtMsb && w[4].reg == kRegEn && w[4].value == 0x7f && w[4].ic == 0);
    CHECK(w[5].ic == 1 && w[5].reg == kRegBrtLsb && w[7].ic == 1 && w[7].reg == kRegEn);
  }
  CHECK(m.lit(0) && m.lit(1) && m.code(0) == 1024 && m.code(1) == 1024);
  // Steps: up to full scale and no further, down to the floor and no further.
  for (int i = 0; i < 10; ++i) c.step(1, "test keys");
  CHECK(m.code(0) == 2047 && m.code(1) == 2047 && m.logged("(full scale)"));
  c.step(-20, "test keys");
  CHECK(m.code(0) == 128 && m.code(1) == 128);
  m.now += 20000;   // a new log window (the steps above used this one up)
  c.step(-1, "test keys");
  CHECK(m.code(0) == 128 && m.logged("(the floor)") && m.logged("write/step lines not logged)"));
  c.step(2, "test keys");
  CHECK(m.code(0) == 203);
  c.percent(100, "test");
  CHECK(m.code(0) == 2047 && m.code(1) == 2047);
  c.percent(1, "test");
  CHECK(m.code(0) == 128);
  CHECK(!m.inBurst);
  checkWhitelist(m, 128);
  // The log limit: many steps in a short time leave a count, not a flood.
  Model q;
  q.setFound(0, 812, true);
  q.setFound(1, 812, true);
  Controller cq(q);
  cq.configure(config());
  cq.survey();
  cq.start();
  const size_t lines = q.lines.size();
  for (int i = 0; i < 40; ++i) cq.step(i % 2 ? 1 : -1, "flood");
  CHECK(q.lines.size() - lines <= 24);
}

static void testLid()
{
  // Not watched: nothing written on a closed lid.
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  c.start();
  c.lid(0);
  CHECK(m.writes().empty() && m.logged("lid closed (not acted on: piano-backlight-lid=0)"));
  // Watched: closed -> off, open -> on; with the display off the open lid does not light it.
  Model n;
  n.setFound(0, 812, true);
  n.setFound(1, 812, true);
  Controller d(n);
  d.configure(config(45, false, true));
  d.survey();
  d.start();
  d.lid(1);
  CHECK(n.writes().empty());
  d.lid(0);
  CHECK(!n.lit(0) && !n.lit(1));
  d.lid(1);
  CHECK(n.lit(0) && n.lit(1));
  d.display(true, 1, 1);
  d.display(false, 1, 0);
  CHECK(!n.lit(0));
  d.lid(0);
  d.lid(1);
  CHECK(!n.lit(0) && !n.lit(1));
  d.display(true, 1, 1);
  CHECK(n.lit(0) && n.lit(1));
  d.lid(-1);   // unknown counts as open
  CHECK(n.lit(0));
  checkWhitelist(n, 128);
}

// A system sleep (setPowerState off / on, docs/power.md 4): dark whatever the display says, then back to what the display and the
// lid want; the code kept or the newest one; a failed off retried; nothing but BL_EN and the code ever written.
static void testSleep()
{
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config(45, false, true));
  c.survey();
  c.start();
  c.display(true, 1, 1);
  CHECK(m.lit(0) && m.lit(1));
  // off: BL_EN cleared on both, the code kept
  c.sleep(true);
  CHECK(!m.lit(0) && !m.lit(1) && m.code(0) == 812 && m.code(1) == 812 && c.asleep() && !c.pending());
  CHECK(m.logged("system sleep: the backlight off on both ICs (BL_EN cleared, the code kept; HWEN untouched)"));
  const size_t writes = m.writes().size();
  c.sleep(true);   // a second off: nothing
  CHECK(m.writes().size() == writes);
  // while asleep: the display path, a step and the lid light nothing (the step's code is kept for the wake)
  c.display(true, 1, 1);
  c.lid(1);
  c.step(1, "test keys");
  CHECK(!m.lit(0) && !m.lit(1) && m.writes().size() == writes && c.code() == 1024);
  // on: the newest code first, then BL_EN, on both
  c.sleep(false);
  CHECK(m.lit(0) && m.lit(1) && m.code(0) == 1024 && m.code(1) == 1024 && !c.asleep());
  CHECK(m.logged("system wake: the backlight as before the sleep"));
  // the display went off during the sleep (IOMFB's system power state): the wake leaves it dark until the display's on
  c.sleep(true);
  c.display(false, 0, 0);
  c.sleep(false);
  CHECK(!m.lit(0) && !m.lit(1) && m.logged("system wake: the backlight stays off (the display or the lid wants it so)"));
  c.display(true, 1, 1);
  CHECK(m.lit(0) && m.lit(1));
  // the lid closed during the sleep (piano-backlight-lid=1): dark after the wake too
  c.sleep(true);
  c.lid(0);
  c.sleep(false);
  CHECK(!m.lit(0) && !m.lit(1));
  c.lid(1);
  CHECK(m.lit(0) && m.lit(1));
  // an IC that refuses the off write: pending, a retry takes it (the kext retries twice before acknowledging)
  m.failWrites[1] = 1;
  c.sleep(true);
  CHECK(!m.lit(0) && m.lit(1) && c.pending());
  c.retry();
  CHECK(!m.lit(1) && !c.pending());
  c.sleep(false);
  CHECK(m.lit(0) && m.lit(1));
  // the wake before the first display on (a sleep during boot): the boot screen's rule holds, lit as found
  Model b;
  b.setFound(0, 812, true);
  b.setFound(1, 812, true);
  Controller cb(b);
  cb.configure(config());
  cb.survey();
  cb.start();
  cb.sleep(true);
  CHECK(!b.lit(0) && !b.lit(1));
  cb.display(false, 0, 0);   // before the first on: ignored
  cb.sleep(false);
  CHECK(b.lit(0) && b.lit(1));
  checkWhitelist(m, 128);
  checkWhitelist(b, 128);
  // not controlling (a missing IC): a sleep writes nothing
  Model x;
  x.present[1] = false;
  Controller cx(x);
  cx.configure(config());
  cx.survey();
  cx.start();
  cx.sleep(true);
  cx.sleep(false);
  CHECK(x.writes().empty());
}

static void testFailures()
{
  // The secondary refuses one write: the primary goes dark, the secondary stays lit until a retry.
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Controller c(m);
  c.configure(config());
  c.survey();
  c.start();
  c.display(true, 1, 1);
  m.failWrites[1] = 1;
  c.display(false, 1, 0);
  CHECK(!m.lit(0) && m.lit(1) && c.pending());
  CHECK(m.logged("write secondary: BL_EN 0x08 = 0x3f (was 0x7f): backlight OFF, sinks 0x3f kept; WRITE FAILED"));
  c.retry();
  CHECK(!m.lit(1) && !c.pending());
  // A bus that stays dead: control stops after the error limit; no write afterwards.
  m.failWrites[1] = -1;
  c.display(true, 1, 1);
  for (int i = 0; i < 20; ++i) c.retry();
  CHECK(!c.controlling() && m.logged("control: OFF after 8 failed attempts in a row"));
  const size_t writes = m.writes().size();
  m.failWrites[1] = 0;
  c.display(false, 1, 0);
  c.step(1, "test");
  c.retry();
  CHECK(m.writes().size() == writes);
  // A read-back that differs (the IC acknowledged but did not take the code) counts as a failure and is retried.
  Model r;
  r.setFound(0, 812, true);
  r.setFound(1, 812, true);
  Controller cr(r);
  cr.configure(config(30, true));
  cr.survey();
  r.deaf[1] = true;
  cr.start();
  CHECK(r.code(0) == codeForPercent(30, 128) && r.code(1) == 812 && cr.pending() && r.logged("READ BACK DIFFERS"));
  r.deaf[1] = false;
  cr.retry();
  CHECK(r.code(1) == codeForPercent(30, 128) && !cr.pending() && cr.controlling());
}

// ------------------------------------------------------------------------------------------------------------- the whole stack
struct Wire final : Ics {
  BitI2c bus[2];
  bool muxed[2] = {false, false};
  unsigned restores = 0;
  std::vector<std::string> lines;
  uint64_t now = 0;
  Wire()
  {
    for (unsigned i = 0; i < 2; ++i) bus[i].configure({&gChip[i], cSda, cScl, cReadSda, cReadScl, cDelay}, 5, 2000);
  }
  bool read(unsigned ic, uint8_t reg, uint8_t *v) override
  {
    muxed[ic] = true;
    return bus[ic].writeRead(kAddress, &reg, 1, v, 1) == I2c::ok;
  }
  bool write(unsigned ic, uint8_t reg, uint8_t v) override
  {
    muxed[ic] = true;
    const uint8_t b[2] = {reg, v};
    return bus[ic].write(kAddress, b, 2) == I2c::ok;
  }
  void done() override
  {
    for (bool &m : muxed) if (m) { m = false; ++restores; }
  }
  void log(const char *line) override { lines.push_back(line); }
  uint64_t nowMs() override { return now += 3; }
};

static void testStack()
{
  for (Chip &c : gChip) {
    c = Chip();
    resetRegs(c.reg);
    c.reg[kRegBrtLsb] = 1500 & 7;
    c.reg[kRegBrtMsb] = 1500 >> 3;
    c.reg[kRegEn] = 0x7f;
  }
  gChip[1].stretch = 3;   // the secondary stretches the clock after its address
  Wire w;
  Controller c(w);
  c.configure(config());
  CHECK(c.survey());
  CHECK(gChip[0].writes.empty() && gChip[1].writes.empty() && w.restores == 2);
  CHECK(c.start());
  CHECK(codeOf(gChip[0].reg[kRegBrtLsb], gChip[0].reg[kRegBrtMsb]) == 921 && codeOf(gChip[1].reg[kRegBrtLsb], gChip[1].reg[kRegBrtMsb]) == 921);
  c.display(true, 1, 1);
  c.display(false, 1, 0);
  CHECK(gChip[0].reg[kRegEn] == 0x3f && gChip[1].reg[kRegEn] == 0x3f);
  c.display(true, 1, 1);
  CHECK(gChip[0].reg[kRegEn] == 0x7f && gChip[1].reg[kRegEn] == 0x7f);
  c.step(-1, "stack");
  CHECK(codeOf(gChip[1].reg[kRegBrtLsb], gChip[1].reg[kRegBrtMsb]) == 812);
  for (const Chip &chip : gChip)
    for (auto [reg, value] : chip.writes) {
      CHECK(writable(reg));
      if (reg == kRegEn) CHECK(!(value & kEnSoftReset));
    }
  CHECK(gChip[0].lastSda && gChip[0].lastScl && gChip[1].lastSda && gChip[1].lastScl);   // both buses idle
  CHECK(!w.muxed[0] && !w.muxed[1]);
}

// ------------------------------------------------------------------------------------------------------------- the producers' side
static void testKeys()
{
  KeyWatch k;
  CHECK(k.consumer(kUsageBrightnessUp) == 1);
  CHECK(k.consumer(kUsageBrightnessUp) == 0);   // the same report again: no second step
  CHECK(k.consumer(0) == 0);
  CHECK(k.consumer(kUsageBrightnessDown) == -1);
  CHECK(k.consumer(0xe9) == 0 && k.consumer(0) == 0);
  CHECK(k.consumer(kUsageBrightnessUp) == 1 && k.consumer(kUsageBrightnessDown) == -1);
  uint8_t r[9] = {5, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(k.keyboard(r) == 0);
  r[3] = kUsageKey2;
  CHECK(k.keyboard(r) == 0);   // 2 without Right Ctrl
  r[1] = kModRightCtrl;
  CHECK(k.keyboard(r) == 1);   // the combo became true (Ctrl after the key counts too)
  CHECK(k.keyboard(r) == 0);
  r[3] = 0;
  CHECK(k.keyboard(r) == 0);
  r[4] = kUsageKey2;
  CHECK(k.keyboard(r) == 1);
  CHECK(k.keyboard(r) == 0);
  r[5] = kUsageKey1;
  CHECK(k.keyboard(r) == -1);
  r[4] = r[5] = 0;
  CHECK(k.keyboard(r) == 0);
  r[1] = 0x01;   // left Ctrl is not the combo
  r[3] = kUsageKey1;
  CHECK(k.keyboard(r) == 0);
}

// piano-backlight-auto=1 (Config::autoAllowed): ambient targets through PianoAuto's AmbientBacklight.h. Without it nothing changes.
static void testAmbient()
{
  using pianoauto::Ambient;
  {
    Model m;
    m.setFound(0, 812, true);
    m.setFound(1, 812, true);
    Controller c(m);
    c.configure(config());
    c.survey();
    CHECK(c.start());
    CHECK(!c.ambient(500, 400, "test light") && !c.automatic(true, "test") && m.writes().empty() && !c.ambientState().automatic());
    c.step(1, "test keys");
    CHECK(m.code(0) == 1024 && !m.logged("automatic"));
  }
  Model m;
  m.setFound(0, 812, true);
  m.setFound(1, 812, true);
  Config cf = config();
  cf.autoAllowed = true;
  Controller c(m);
  c.configure(cf);
  c.survey();
  CHECK(c.start());
  c.display(true, 1, 1);
  // The first target: automatic mode, the curve's code on both ICs, its writes not logged one by one.
  CHECK(c.ambient(370, 300, "test light") && c.ambientState().automatic());
  CHECK(m.code(0) == 757 && m.code(1) == 757 && m.logged("automatic brightness: on (test light): 300 lux, target 370 permille"));
  CHECK(!m.logged("code 757 (37 %)"));
  // The keys in automatic mode shift the curve by the ladder's step; the offset stays as the room changes.
  c.step(1, "test keys");
  CHECK(c.ambientState().offset() == 1 && m.code(0) == 954 && m.logged("in automatic mode: offset 0 -> 1, code 757 -> 954"));
  CHECK(c.ambient(500, 400, "test light") && m.code(0) == 1290 && m.code(1) == 1290);   // 1024 one step up: the ladder's 1290
  // Dark (display asleep): a target is kept, nothing written; the wake lights the newest.
  c.display(false, 1, 0);
  const size_t before = m.writes().size();
  CHECK(!c.ambient(240, 100, "test light") && m.writes().size() == before && !m.lit(0) &&
        !c.ambientState().valid() && c.ambientState().requested());
  c.display(true, 1, 1);
  CHECK(m.lit(0) && !c.autoActive());
  CHECK(c.ambient(240, 100, "fresh wake sample") && c.autoActive() &&
        m.code(0) == Ambient::shift(Ambient::codeForPermille(240, 128), 1, 128, false));
  // One summary line per 10 s while targets come (retry(), every second in the kext, flushes it).
  m.now += 11000;
  c.retry();
  CHECK(m.logged("ambient: 3 targets in "));
  // An explicit level: manual, targets only recorded; the property re-enters with the last target and the offset.
  c.percent(50, "setProperties");
  CHECK(!c.ambientState().automatic() && m.code(0) == codeForPercent(50, 128) &&
        m.logged("automatic brightness: off, a level was set by setProperties"));
  CHECK(c.ambient(800, 3000, "test light") && m.code(0) == codeForPercent(50, 128));
  CHECK(c.automatic(true, "setProperties") && !c.ambientState().automatic() && c.ambientState().requested());
  CHECK(c.ambient(800, 3000, "fresh auto sample") && c.autoActive() &&
        m.code(0) == Ambient::shift(Ambient::codeForPermille(800, 128), 1, 128, false));
  // -1: the sensor leaves, the code stays; the keys step the ladder again.
  const uint16_t kept = m.code(0);
  CHECK(c.ambient(-1, 0, "test light") && !c.ambientState().automatic() && c.autoFault() && c.ambientState().requested() && m.code(0) == kept &&
        m.logged("automatic brightness: off (test light left it)"));
  Steps ladder;
  ladder.build(128, false);
  c.step(1, "test keys");
  CHECK(m.code(0) == ladder.up(kept));
  CHECK(c.automatic(true, "fault retry") && !c.autoActive());
  c.ambient(900, 5000, "late fault sample");
  CHECK(!c.autoActive() && c.autoFault());
  CHECK(!m.inBurst);
  checkWhitelist(m, 128);
}

static void testAmbientReadbackFault()
{
  Model m;
  Controller c(m);
  Config cfg = config();
  cfg.autoAllowed = true;
  cfg.errorLimit = 3;
  c.configure(cfg);
  CHECK(c.survey() && c.start());
  const uint16_t old = c.ic(1).code;
  const uint32_t good = c.ic(1).codeReadbacks;
  m.deaf[1] = true;
  c.ambient(800, 3000, "fault test");
  CHECK(c.ic(0).code == c.code() && c.ic(0).codeReadbacks > 0);
  CHECK(c.ic(1).code == old && c.ic(1).codeReadbacks == good && c.ic(1).errors > 0);
  CHECK(!c.autoActive() && c.controlling());
  m.deaf[1] = false;
  c.retry();
  CHECK(c.autoActive() && c.ic(1).code == c.code() && c.ic(1).codeReadbacks == good + 1);
  m.deaf[1] = true;
  c.ambient(500, 400, "persistent fault");
  c.retry();
  c.retry();
  CHECK(!c.controlling() && c.autoFault() && !c.autoActive());
  CHECK(!c.automatic(true, "cannot restore control"));
  checkWhitelist(m, 128);
}

struct NativePropertyRecord {
  std::map<std::string, std::vector<uint32_t>> ranges;
  std::map<std::string, uint32_t> numbers, scalars;
  std::map<std::string, std::vector<int32_t>> blobs;
  std::string name;
  bool range(const char *key, uint32_t min, uint32_t max, uint32_t value)
  {
    ranges[key] = {min, max, value};
    return true;
  }
  bool number(const char *key, uint32_t value) { numbers[key] = value; return true; }
  bool scalar(const char *key, uint32_t value) { scalars[key] = value; return true; }
  bool data(const char *key, const void *bytes, unsigned size)
  {
    CHECK(size % sizeof(int32_t) == 0);
    const auto *values = static_cast<const int32_t *>(bytes);
    blobs[key] = {values, values + size / sizeof(int32_t)};
    return true;
  }
  bool commit(const char *key) { name = key; return true; }
};

static void testNativeBrightness()
{
  for (const uint16_t floor : {32, 128, 921, 1024}) {
    const NativeBrightness adapter(floor);
    uint16_t code = 0;
    uint32_t prior = 0;
    for (uint16_t expected = floor; expected <= kFullScale; ++expected) {
      const uint32_t value = adapter.milliNits(expected);
      CHECK(value > prior);
      CHECK(adapter.decode((uint64_t(value) * 65536 + 500) / 1000, code) && code == expected);
      prior = value;
    }
    CHECK(prior == 500000);
    CHECK(!adapter.decode((adapter.minNits() << 16) - 1, code));
    CHECK(!adapter.decode((500u << 16) + 1, code));
    CHECK(!adapter.decode(uint32_t(-65536), code));
    CHECK(!adapter.decode((1ull << 32) | (500u << 16), code));
    NativePropertyRecord params, calibration;
    CHECK(adapter.parameters(params, 1400));
    CHECK(params.name == "IODisplayParameters" && params.ranges.size() == 2);
    CHECK((params.ranges.at("brightness") == std::vector<uint32_t>{floor, 2047, 1400}));
    CHECK((params.ranges.at("BrightnessMilliNits") ==
           std::vector<uint32_t>{1000 * adapter.minNits(), 500000, adapter.milliNits(1400)}));
    CHECK(adapter.calibration(calibration) && calibration.name == "backlight-calibration-parameters");
    const auto &samples = calibration.blobs.at("backlight-marketing-table");
    CHECK(samples.size() == 3 && samples[0] > 0 && samples[0] < samples[1] && samples[1] < samples[2]);
    CHECK(calibration.blobs.at("LmaxProduct").size() == 1 && calibration.blobs.at("LmaxProduct")[0] == samples[2]);
    CHECK(calibration.scalars.at("LminProduct") == adapter.minNits() && calibration.scalars.at("LmidProduct") == adapter.midNits());
    CHECK(calibration.numbers.size() == 6 && calibration.numbers.at("lMidPanel") == uint32_t(samples[1]));
  }
  CHECK(nativeWriter(false, true, "com.apple.WindowServer"));
  {   // 2026-11-20: WindowServer's stream: settle, follow, the user's move
    NativeIntent i;
    i.reset(1000);
    CHECK(i.classify(10 << 16, 1100) == NativeIntent::kFollow);    // the power-on ramp, inside the settle time
    CHECK(i.classify(140 << 16, 3000) == NativeIntent::kFollow);
    CHECK(i.classify(141 << 16, 6000) == NativeIntent::kFollow);   // < 2 % of the settled 140
    CHECK(i.classify(100 << 16, 6100) == NativeIntent::kUserMove);
    CHECK(i.classify(99 << 16, 6150) == NativeIntent::kFollow);    // the drag goes on from its new place
    CHECK(i.classify(60 << 16, 6200) == NativeIntent::kUserMove);
    i.reset(7000);
    CHECK(i.classify(140 << 16, 7100) == NativeIntent::kFollow);
    NativeIntent fresh;   // never reset: the first request starts the settle time
    CHECK(fresh.classify(50 << 16, 100000) == NativeIntent::kFollow);
    CHECK(fresh.classify(200 << 16, 101000) == NativeIntent::kFollow);
    CHECK(fresh.classify(100 << 16, 105000) == NativeIntent::kUserMove);
    {   // 2026-11-21 (SLIDER-INIT.md): an announced sync; CoreBrightness's commit of the panel's level is its echo, not a move
      NativeIntent s;
      s.reset(0);
      CHECK(s.classify(600 << 16, 100) == NativeIntent::kFollow);       // CoreBrightness's stale preference at boot (600 nits)
      CHECK(s.classify(600 << 16, 9000) == NativeIntent::kFollow);
      s.expect(26 << 16, 10000);                                        // the daemon gives it the automatic level, 26 nits
      CHECK(s.classify(26 << 16, 10030) == NativeIntent::kFollow);      // the echo: 600 -> 26 is not the user's move
      CHECK(s.syncs() == 1 && s.echoes() == 1 && s.missed() == 0);
      CHECK(s.classify(int64_t(26.3 * 65536), 11000) == NativeIntent::kFollow);   // the anchor moved to the echo
      CHECK(s.classify(80 << 16, 12000) == NativeIntent::kUserMove);   // the user's drag still counts after the window
      s.expect(10 << 16, 20000);                                        // a ramp to the echo: all follows, the window closes there
      CHECK(s.classify(60 << 16, 20010) == NativeIntent::kFollow);
      CHECK(s.classify(30 << 16, 20020) == NativeIntent::kFollow);
      CHECK(s.classify(int64_t(10.2 * 65536), 20030) == NativeIntent::kFollow && s.echoes() == 2);
      CHECK(s.classify(30 << 16, 20040) == NativeIntent::kUserMove);   // closed by the echo: a move again at once
      s.expect(50 << 16, 30000);                                        // no echo within the window
      CHECK(s.classify(50 << 16, 30000 + NativeIntent::kExpectMs) == NativeIntent::kUserMove);   // 30 -> 50 after the window
      CHECK(s.missed() == 1 && s.echoes() == 2 && s.syncs() == 3);
      s.expect(40 << 16, 40000);
      s.expect(41 << 16, 40100);                                        // replaced before its echo: the first one is missed
      CHECK(s.missed() == 2 && s.syncs() == 5);
      CHECK(s.classify(41 << 16, 40150) == NativeIntent::kFollow && s.echoes() == 3);
      NativeIntent cold;   // never reset: a sync before any request still makes its echo a follow
      cold.expect(5 << 16, 50000);
      CHECK(cold.classify(5 << 16, 50010) == NativeIntent::kFollow && cold.echoes() == 1);
      CHECK(std::strcmp(kNativeSync, "piano-backlight-sync") == 0);
    }
    {   // round 2 (SLIDER-INIT.md): the restore and EDR ramps reach the stream like moves; with the daemon's lease they do not end
        // automatic mode, the daemon's report does
      auto run = [](NativeIntent &n, bool managed, const std::vector<double> &nits, uint64_t t0) {
        bool ended = false;
        for (size_t k = 0; k < nits.size(); ++k) {
          const StreamChoice c = streamChoice(n.classify(int64_t(nits[k] * 65536), t0 + 17 * k), true, managed, false);
          ended = ended || c.endsAuto;
          if (c.apply && !c.endsAuto) return true;   // never while automatic owns the panel
        }
        return ended;
      };
      // installed 14:07: the restore 431.949 (first value), then sdr x EDR headroom 1.04185 / 1.0668
      const std::vector<double> installed = {431.949, 450.025, 460.818};
      // slider2: an EDR fade over the SDR level 347.65, 0.99545 per step from 417.18 (desk-login's last key level)
      std::vector<double> fade = {417.18};
      while (fade.back() > 347.7) fade.push_back(fade.back() * 0.99545);
      NativeIntent a, b, c2, d;
      a.reset(0);
      CHECK(run(a, false, installed, 10000));    // no lease: the EDR step ends automatic mode (what the installed boot did)
      b.reset(0);
      CHECK(!run(b, true, installed, 10000));    // the lease: dropped, automatic mode kept
      c2.reset(0);
      CHECK(c2.classify(int64_t(417.18 * 65536), 100) == NativeIntent::kFollow);
      CHECK(run(c2, false, fade, 20000));        // slider2: the fade ended automatic mode
      d.reset(0);
      CHECK(d.classify(int64_t(417.18 * 65536), 100) == NativeIntent::kFollow);
      CHECK(!run(d, true, fade, 20000));
      // 2026-11-22 (AUTO-BRIGHT.md): the daemon's report never ends automatic mode: while automatic it is the curve's trim
      // (Controller::userLevel, testUserLevel), not a level from the stream; in manual mode it is the level
      const StreamChoice r = streamChoice(NativeIntent::kFollow, true, true, true);
      CHECK(!r.apply && !r.endsAuto);
      const StreamChoice rm = streamChoice(NativeIntent::kUserMove, false, true, true);
      CHECK(rm.apply && !rm.endsAuto);
      CHECK(!streamChoice(NativeIntent::kUserMove, true, false, true).endsAuto);
      const StreamChoice m = streamChoice(NativeIntent::kUserMove, false, true, false);
      CHECK(m.apply && !m.endsAuto);
      const StreamChoice f = streamChoice(NativeIntent::kFollow, true, false, false);
      CHECK(!f.apply && !f.endsAuto);
      d.moved(200 << 16);                        // the stream goes on from the reported level
      CHECK(d.classify(201 << 16, 30000) == NativeIntent::kFollow);
      Lease lease;
      CHECK(!lease.active(0));
      lease.set(120, 1000);
      CHECK(lease.active(120999) && !lease.active(121000));
      lease.set(0, 5000);
      CHECK(!lease.active(5000));
      lease.set(100000, 0);
      CHECK(lease.active(kManagedMaxS * 1000 - 1) && !lease.active(kManagedMaxS * 1000));
      CHECK(std::strcmp(kNativeManaged, "piano-backlight-managed") == 0 && std::strcmp(kNativeMoved, "piano-backlight-moved") == 0);
      // round 3 (slider3 boot: CoreBrightness's default 150 x EDR 1.2 ended automatic mode at the login window before the lease):
      // given back when the lease arrives; not after a display power-on, a reported move or an explicit setting cleared it
      Handover h;
      NativeIntent boot;
      boot.reset(0);
      CHECK(boot.classify(int64_t(150.0 * 65536), 100) == NativeIntent::kFollow);
      const StreamChoice e = streamChoice(boot.classify(int64_t(171.45 * 65536), 9000), true, false, false);
      CHECK(e.endsAuto);
      h.streamEndedAuto(false);
      CHECK(h.leaseArrived() && !h.leaseArrived());   // once
      h.streamEndedAuto(true);                         // ended while a lease held (only a reported move can): never given back
      CHECK(!h.leaseArrived());
      h.streamEndedAuto(false);
      h.cleared();                                     // a display power-on, a reported move or piano-backlight-auto
      CHECK(!h.leaseArrived());
    }
    CHECK(std::strcmp(kNativeOverride, "force-brightness-control") == 0);
    auto bitsOf = [](double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; };   // 2026-11-21: the swap record's double
    CHECK(nitsFixedFromDouble(bitsOf(13.3572)) == int32_t(13.3572 * 65536));
    CHECK(nitsFixedFromDouble(bitsOf(448.484)) == int32_t(448.484 * 65536));
    CHECK(nitsFixedFromDouble(bitsOf(600.0)) == 600 << 16 && nitsFixedFromDouble(bitsOf(0.5)) == 1 << 15);
    CHECK(nitsFixedFromDouble(bitsOf(0.0)) == 0 && nitsFixedFromDouble(bitsOf(-5.0)) == 0);
    CHECK(nitsFixedFromDouble(bitsOf(1e12)) == INT32_MAX && nitsFixedFromDouble(bitsOf(1e-12)) == 0);
    CHECK(nitsFixedFromDouble(0x7ff8000000000000ull) == 0 && nitsFixedFromDouble(0x7ff0000000000000ull) == 0);
  }
  CHECK(nativeWriter(true, false, nullptr));
  CHECK(!nativeWriter(false, false, "com.apple.WindowServer"));
  CHECK(!nativeWriter(false, true, "com.apple.WindowServer.other"));
  CHECK(!nativeWriter(false, true, nullptr));

  Model m;
  Controller ctl(m);
  Config cfg;
  cfg.autoAllowed = true;
  ctl.configure(cfg);
  CHECK(ctl.survey() && ctl.start());
  const NativeBrightness adapter(cfg.floor);
  ctl.ambient(450, 200, "test");
  CHECK(ctl.autoActive());
  uint16_t applied = 0;
  CHECK(adapter.applied(ctl, applied));
  const uint16_t previous = applied;
  m.deaf[1] = true;
  ctl.manualCode(1600, kNativeRequest);
  CHECK(!ctl.ambientState().requested());
  CHECK(ctl.ic(0).code == 1600 && ctl.ic(1).code == previous);
  CHECK(!adapter.applied(ctl, applied) && applied == previous);
  m.deaf[1] = false;
  ctl.retry();
  CHECK(adapter.applied(ctl, applied) && applied == 1600 && !ctl.autoActive());
  ctl.sleep(true);
  ctl.manualCode(1500, kNativeRequest);
  CHECK(!adapter.applied(ctl, applied) && applied == 1600);
  ctl.sleep(false);
  CHECK(adapter.applied(ctl, applied) && applied == 1500);
  ctl.automatic(true, "test");
  ctl.ambient(550, 500, "test");
  CHECK(ctl.autoActive() && adapter.applied(ctl, applied) && applied != 1500);
}

static void testAndroidBrightness()
{
  CHECK(sizeof pa_sdr_points / sizeof pa_sdr_points[0] == 613);
  uint32_t crc = ~0u;
  for (const auto &pair : pa_sdr_points) {
    CHECK(pa_sdr_code_at_dbv(pair[0]) == pair[1]);
    for (uint16_t word : pair)
      for (unsigned i = 0; i < 16; ++i) {
        const uint32_t carry = (crc ^ word) & 1;
        crc = (crc >> 1) ^ (carry ? 0xedb88320u : 0);
        word >>= 1;
      }
  }
  CHECK(~crc == PA_SDR_MAP_CRC);
  for (uint16_t dbv = PA_SDR_MIN_DBV; dbv <= PA_SDR_MAX_DBV; ++dbv) {
    const uint16_t code = pa_sdr_code_at_dbv(dbv);
    CHECK(code >= PA_SDR_MIN_CODE && code <= PA_SDR_MAX_CODE);
    CHECK(code >= pa_sdr_code_at_dbv(dbv - 1));
    CHECK(pa_sdr_code(pa_sdr_fixed_for_code(code)) == code);
  }
  CHECK(pa_sdr_millinits(pa_sdr_fixed_for_code(347)) == 4492);  // DBV 15..16 midpoint
  CHECK(pa_sdr_fixed_for_code(1952) == 600u << 16);
  for (uint16_t floor = PA_SDR_MIN_CODE; floor <= 1024; ++floor) {
    const NativeBrightness adapter(floor, PA_SDR_MAX_CODE, true);
    const uint32_t min = adapter.milliNits(floor);
    uint16_t code = 0;
    CHECK(adapter.decode(uint32_t((uint64_t(min) << 16) / 1000), code) && code == floor);
  }
  for (uint16_t floor : {128, 921}) {
    const uint16_t lo = floor < PA_SDR_MIN_CODE ? uint16_t(PA_SDR_MIN_CODE) : floor;
    const NativeBrightness ref(lo, PA_SDR_MAX_CODE, true), control(lo, PA_SDR_MAX_CODE, false);
    CHECK(ref.minFixed() == control.minFixed());
    CHECK(ref.milliNits(lo) > (floor == 128 ? 2000u : 25000u));
    CHECK(ref.milliNits(lo) < (floor == 128 ? 2400u : 27000u));
    for (const auto &adapter : {ref, control}) {
      uint16_t code = 0;
      CHECK(!adapter.decode(adapter.minFixed() - 1, code));
      CHECK(adapter.decode(adapter.minFixed(), code) && code == lo);
      CHECK(adapter.decode(600u << 16, code) && code == PA_SDR_MAX_CODE);
      CHECK(!adapter.decode((600u << 16) + 1, code));
      NativePropertyRecord params, calibration;
      CHECK(adapter.parameters(params, 1500) && adapter.calibration(calibration));
      CHECK((params.ranges.at("brightness") == std::vector<uint32_t>{lo, PA_SDR_MAX_CODE, 1500}));
      CHECK((params.ranges.at("BrightnessMilliNits") ==
             std::vector<uint32_t>{adapter.milliNits(lo), 600000, adapter.milliNits(1500)}));
      const auto &samples = calibration.blobs.at("backlight-marketing-table");
      CHECK(uint32_t(samples[0]) == adapter.minFixed() && samples[2] == 600 << 16);
      CHECK((calibration.blobs.at("LmaxProduct") == std::vector<int32_t>{600 << 16}));
      CHECK(calibration.numbers.at("lMinPanel") == adapter.minFixed());
      for (uint16_t i = lo + 1; i <= PA_SDR_MAX_CODE; ++i)
        CHECK(adapter.milliNits(i) >= adapter.milliNits(i - 1));
    }
    uint16_t curved = 0, linear = 0;
    CHECK(ref.decode(100u << 16, curved) && control.decode(100u << 16, linear));
    CHECK(curved > 1300 && curved < 1400 && linear < curved);
  }
  for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
    Model model;
    for (unsigned i = 0; i < 2; ++i) {
      model.reg[i][kRegCfg1] &= ~(kCfg1LinearMap | kCfg1PwmEnable);
      model.setFound(i, 2047, true);
    }
    if (mismatch == 1) model.reg[1][kRegCfg1] |= kCfg1LinearMap;
    if (mismatch == 2) model.reg[0][kRegCfg1] |= kCfg1PwmEnable;
    if (mismatch == 3) model.reg[1][kRegEn] ^= 1;
    if (mismatch == 4) model.reg[1][kRegIfs] ^= 0x08;
    Controller ctl(model);
    Config cfg = config(0);
    cfg.android = cfg.autoAllowed = true;
    ctl.configure(cfg);
    CHECK(ctl.survey() && ctl.start());
    CHECK(ctl.floor() == (mismatch ? 128 : PA_SDR_MIN_CODE));
    CHECK(ctl.ceiling() == (mismatch ? 2047 : PA_SDR_MAX_CODE));
    CHECK(ctl.code() == ctl.ceiling());
    ctl.ambient(1000, 12000, "test");
    ctl.step(10, "test");
    CHECK(ctl.code() == ctl.ceiling());
    ctl.percent(100, "test");
    ctl.manualCode(2047, "test");
    CHECK(ctl.code() == ctl.ceiling());
    for (const auto &write : model.writes()) CHECK(writable(write.reg));
  }
}

static Model qualifiedPwmPair();

// 2026-11-22 (AUTO-BRIGHT.md): the owner's hands-on on the installed v13.1y3 (14:44-14:48). Automatic mode at 27 lux (651 permille,
// code 1333 = 91.7 nits); at 14:47:32 Low Power Mode's eco factor took CoreBrightness from 91.575 to 64.103 nits, which the daemon
// reported as a move, and automatic mode ended until the next display power-on, so covering the sensor did nothing. Now each reported
// level keeps automatic mode and becomes the curve's trim: the panel shows it at once, and a darker or brighter room still moves it.
static void testUserLevel()
{
  using pianoauto::Ambient;
  Model m = qualifiedPwmPair();
  Controller c(m);
  Config cf = config(0);
  cf.floor = PA_SDR_MIN_CODE;
  cf.android = cf.autoAllowed = true;
  c.configure(cf);
  CHECK(c.survey() && c.start() && c.ceiling() == PA_SDR_MAX_CODE && c.exponential() && c.floor() == PA_SDR_MIN_CODE);
  c.display(true, 1, 1);
  CHECK(c.ambient(651, 27, "PianoSensors") && c.autoActive() && m.code(0) == 1333 && m.code(1) == 1333);
  const NativeBrightness adapter(c.floor(), c.ceiling(), true);
  auto codeOfNits = [&](double nits) {
    uint16_t code = 0;
    CHECK(adapter.decode(uint32_t(nits * 65536), code));
    return code;
  };
  auto expected = [&](uint32_t permille) {
    return Ambient::trimmed(Ambient::codeForPermille(permille, PA_SDR_MIN_CODE), c.ambientState().trim(), PA_SDR_MIN_CODE, true,
                            PA_SDR_MAX_CODE);
  };
  // the 14:47:32 report (Low Power Mode): automatic mode stays, the panel at the reported level, the trim below the curve
  const uint16_t eco = codeOfNits(64.103);
  CHECK(eco < 1333 && eco > 1100);
  CHECK(c.userLevel(eco, "piano-brightness-sync"));
  CHECK(c.autoActive() && c.ambientState().requested() && m.code(0) == eco && m.code(1) == eco);
  CHECK(c.ambientState().trim() == int(eco) - 1333 && m.logged("automatic mode stays"));
  // covering the sensor (a darker room's target): the panel goes darker with the trim kept; uncovered: back to the user's level
  CHECK(c.ambient(450, 3, "PianoSensors") && c.autoActive() && m.code(0) == expected(450) && m.code(0) < eco);
  CHECK(c.ambient(651, 27, "PianoSensors") && m.code(0) == eco);
  // Low Power Mode off: CoreBrightness back at 91.575 nits, reported again: the trim back near zero
  CHECK(c.userLevel(codeOfNits(91.575), "piano-brightness-sync") && c.autoActive());
  CHECK(c.ambientState().trim() >= -2 && c.ambientState().trim() <= 2);
  // the slider to full in the dim room: full now, full in daylight, full again when the room is dim again
  CHECK(c.userLevel(PA_SDR_MAX_CODE, "piano-brightness-sync") && m.code(0) == PA_SDR_MAX_CODE && c.ambientState().trim() == 1952 - 1333);
  CHECK(c.ambient(1000, 11165, "PianoSensors") && m.code(0) == PA_SDR_MAX_CODE);
  CHECK(c.ambient(651, 27, "PianoSensors") && m.code(0) == PA_SDR_MAX_CODE);
  CHECK(c.ambient(200, 1, "PianoSensors") && m.code(0) == expected(200) && m.code(0) < PA_SDR_MAX_CODE);
  // a darker preference: the slider at 20 nits; the display off/on keeps it (the first fresh target gets the trim)
  const uint16_t dim = codeOfNits(20.0);
  CHECK(c.userLevel(dim, "piano-brightness-sync") && m.code(0) == dim && c.autoActive());
  const int trim = c.ambientState().trim();
  c.display(false, 1, 0);
  CHECK(!m.lit(0) && c.ambientState().requested() && !c.ambientState().valid());
  c.display(true, 1, 1);
  CHECK(c.ambient(651, 27, "fresh wake sample") && c.autoActive() && c.ambientState().trim() == trim && m.code(0) == expected(651));
  // a level before the session's first target (right after a power-on): shown at once, its trim taken at the next target
  c.display(false, 1, 0);
  c.display(true, 1, 1);
  CHECK(c.userLevel(900, "piano-brightness-sync") && m.code(0) == 900 && c.ambientState().pendingLevel() == 900);
  CHECK(m.logged("its trim at the next target"));
  CHECK(c.ambient(651, 27, "fresh wake sample") && c.autoActive() && m.code(0) == 900 && c.ambientState().trim() == 900 - 1333);
  CHECK(c.ambientState().pendingLevel() == 0);
  // the user's word: piano-backlight-auto=0 makes a reported level the manual level (the trim kept for later); =1 resumes with it
  CHECK(c.automatic(false, "setProperties") && !c.ambientState().requested());
  CHECK(!c.userLevel(1500, "piano-brightness-sync") && m.code(0) == 1500 && c.ambientState().trim() == 900 - 1333);
  CHECK(c.ambient(651, 27, "PianoSensors") && m.code(0) == 1500);                           // manual: targets only recorded
  CHECK(c.automatic(true, "setProperties") && c.ambient(651, 27, "fresh auto sample") && c.autoActive() && m.code(0) == 900);
  // a latched sensor fault: no trim (the level is the manual one)
  c.invalidateAmbient(true);
  CHECK(!c.userLevel(1200, "piano-brightness-sync") && m.code(0) == 1200 && c.ambientState().trim() == 900 - 1333);
  CHECK(!m.inBurst);
  checkWhitelist(m, PA_SDR_MIN_CODE);
  // the linear mapping: the trim is a ratio
  Model l;
  l.setFound(0, 812, true);
  l.setFound(1, 812, true);
  Controller lc(l);
  Config lf = config();
  lf.autoAllowed = true;
  lc.configure(lf);
  CHECK(lc.survey() && lc.start() && !lc.exponential());
  lc.display(true, 1, 1);
  CHECK(lc.ambient(500, 400, "test light") && l.code(0) == 1024);
  CHECK(lc.userLevel(512, "piano-brightness-sync") && l.code(0) == 512 && lc.ambientState().trim() == -512 && lc.autoActive());
  CHECK(lc.ambient(1000, 10000, "test light") && l.code(0) == 1024);                         // half of full scale
  CHECK(lc.ambient(250, 50, "test light") && l.code(0) == 256);
  checkWhitelist(l, 128);
}

static void setPwm(Model &m, unsigned ic, uint16_t duty)
{
  m.reg[ic][kRegPwmLsb] = uint8_t(duty);
  m.reg[ic][kRegPwmMsb] = uint8_t(0xf8 | (duty >> 8));   // reserved high bits are outside PWM2DIG
}

static Model qualifiedPwmPair()
{
  Model m;
  for (unsigned i = 0; i < kIcs; ++i) {
    m.reg[i][kRegCfg1] = 0x53;
    m.reg[i][kRegIfs] = 0xd9;
    m.setFound(i, 2047, false);
    setPwm(m, i, 2047);
  }
  return m;
}

static void testPwmContract()
{
  for (uint16_t floor : {128, 921}) {
    Model m = qualifiedPwmPair();
    Controller ctl(m);
    Config cfg = config(0);
    cfg.floor = floor;
    cfg.android = cfg.autoAllowed = true;
    ctl.configure(cfg);
    CHECK(ctl.survey() && m.writes().empty());
    m.trace.clear();
    CHECK(ctl.start() && ctl.floor() == (floor == 128 ? 178 : 921) && ctl.ceiling() == 1952);
    CHECK(m.code(0) == 1952 && m.code(1) == 1952 && !ctl.pending());
    CHECK(m.reg[0][kRegCfg1] == 0x53 && m.reg[1][kRegCfg1] == 0x53);
    CHECK(m.logged("ic=primary cfg1=53 pwm_raw=07ff duty_permille=1000 stable=1 valid=1 context=boot"));
    CHECK(m.logged("ic=secondary cfg1=53 pwm_raw=07ff duty_permille=1000 stable=1 valid=1 context=boot"));
    CHECK(m.logged("contract=exp-pwm-full supported=1 reason=ok context=boot"));
    for (unsigned i = 0; i < kIcs; ++i) {
      const std::vector<Model::Access> expected = {
        {i, true, kRegBrtLsb, 0}, {i, true, kRegBrtMsb, 244}, {i, true, kRegEn, 0x7f},
      };
      std::vector<Model::Access> writes;
      for (const auto &w : m.writes())
        if (w.ic == i) writes.push_back(w);
      CHECK(writes == expected);
      CHECK(ctl.ic(i).codeReadbacks == 1 && ctl.ic(i).enableReadbacks == 1);
      unsigned pairs = 0;
      for (size_t r = 0; r + 1 < m.trace.size(); ++r) {
        const auto &a = m.trace[r];
        if (a.ic != i || a.write || a.reg != kRegPwmLsb) continue;
        const auto &b = m.trace[r + 1];
        CHECK(b.ic == i && !b.write && b.reg == kRegPwmMsb);
        ++pairs;
      }
      CHECK(pairs >= 2);
    }
    checkWhitelist(m, ctl.floor());

    // The same bounded-schema gate and snapshot used by serve() and publishState().
    const NativeBrightness schema(ctl.floor(), ctl.ceiling());
    const uint32_t revision = schema.initialRevision();
    NativePropertyRecord snapshot;
    CHECK(publishAmbientSnapshot(snapshot, ctl, revision, true));
    CHECK(snapshot.name == pianoauto::kPropertyAmbientSnapshot);
    CHECK((snapshot.numbers == std::map<std::string, uint32_t>{
      {"curve", 1}, {"revision", 1}, {"floor", ctl.floor()}, {"target", uint32_t(-1)}, {"lux", 0}, {"valid", 0}, {"fault", 0},
    }));
    NativeBrightness reference(ctl.floor(), ctl.ceiling(), true);
    uint16_t code = 0;
    CHECK(revision == 1 && schema.maxFixed() == 600u << 16);
    const pianoauto::AmbientVersion sample{1, snapshot.numbers["revision"], snapshot.numbers["curve"]};
    CHECK(sample.newer(revision, 1, 0));
    CHECK(!sample.newer(revision + 1, 1, 0) && !sample.newer(revision, 0, 0) && !sample.newer(revision, 1, 1));
    CHECK(ctl.ambient(1000, 12000, "fresh reference ALS") && ctl.autoActive() && ctl.code() == 1952);
    CHECK(publishAmbientSnapshot(snapshot, ctl, revision, true));
    CHECK(snapshot.numbers["target"] == 1000 && snapshot.numbers["lux"] == 12000 && snapshot.numbers["valid"] == 1);
    CHECK(reference.decode(100u << 16, code) && code == pa_sdr_code(100u << 16));
    ctl.manualCode(code, kNativeRequest);
    CHECK(!ctl.ambientState().requested() && !ctl.autoActive());
    CHECK(m.logged("contract=exp-pwm-full supported=1 reason=ok context=apply"));
    CHECK(ctl.automatic(true, "test") && !ctl.autoActive());
    CHECK(ctl.ambient(800, 3000, "fresh reference ALS") && ctl.autoActive());
    ctl.sleep(true);
    CHECK(!ctl.autoActive() && !ctl.ambientState().valid());
    setPwm(m, 1, 2046);
    ctl.sleep(false);
    CHECK(m.logged("contract=exp-pwm-full supported=0 reason=pwm-not-full context=wake"));
    CHECK(!ctl.pwmQualified());
    CHECK(ctl.floor() == (floor == 128 ? 178 : 921) && ctl.ceiling() == 1952);
    m.readReplies[{0, kRegPwmMsb}] = {-1};
    ctl.percent(75, "test");
    CHECK(m.logged("contract=exp-pwm-full supported=0 reason=pwm-read context=apply"));
    CHECK(!ctl.pwmQualified());
    setPwm(m, 1, 2047);
    ctl.percent(76, "test");
    CHECK(m.lines.back().find("contract=exp-pwm-full supported=1 reason=ok context=apply") != std::string::npos);
    CHECK(ctl.pwmQualified());
    m.reg[1][kRegCfg1] = 0x57;
    ctl.percent(76, "same code");
    CHECK(!ctl.pwmQualified() && m.logged("supported=0 reason=mode context=apply"));
    CHECK(m.reg[1][kRegCfg1] == 0x57 && ctl.ceiling() == 1952);
    m.reg[1][kRegCfg1] = 0x53;
    ctl.display(true, 1, 1);
    ctl.display(false, 1, 0);
    ctl.sleep(true);
    ctl.sleep(false);
    CHECK(!m.logged("supported=1 reason=ok context=wake"));
    ctl.display(true, 1, 1);
    CHECK(ctl.pwmQualified() && m.logged("supported=1 reason=ok context=wake"));
    CHECK(!m.inBurst);
    for (const auto &line : m.lines)
      if (line.find("PianoBacklightPWMV1") == 0)
        CHECK(line.size() + sizeof("PianoBacklight: \n") < 200);
  }
}

static void testPwmRefusal()
{
  // Each refusal must leave the same register writes, applied codes and published schema as Android mode disabled.
  auto check = [](Model m, uint16_t floor, const char *reason, const char *receipt = nullptr) {
    Model legacy = m;
    Controller actual(m), expected(legacy);
    Config cfg = config(0, false, true);
    cfg.floor = floor;
    cfg.autoAllowed = true;
    expected.configure(cfg);
    cfg.android = true;
    actual.configure(cfg);
    for (Controller *ctl : {&actual, &expected}) {
      CHECK(ctl->survey());
      CHECK(ctl->start() && ctl->floor() == floor && ctl->ceiling() == 2047);
      ctl->step(-2, "test");
      ctl->ambient(450, 400, "test");
      ctl->step(1, "test");
      ctl->lid(0);
      ctl->percent(35, "test");
      ctl->lid(1);
      ctl->sleep(true);
      ctl->sleep(false);
    }
    const NativeBrightness fallback(actual.floor(), actual.ceiling()), old(expected.floor(), expected.ceiling());
    NativePropertyRecord a, b, ca, cb, snapshot;
    CHECK(fallback.initialRevision() == 0 && fallback.maxFixed() == 500u << 16);
    CHECK(!publishAmbientSnapshot(snapshot, actual, fallback.initialRevision(), true) && snapshot.numbers.empty());
    CHECK(fallback.parameters(a, actual.code()) && old.parameters(b, expected.code()));
    CHECK(fallback.calibration(ca) && old.calibration(cb));
    CHECK(a.name == b.name && a.ranges == b.ranges && ca.name == cb.name);
    CHECK(ca.numbers == cb.numbers && ca.blobs == cb.blobs && ca.scalars == cb.scalars);
    CHECK(m.writes() == legacy.writes() && std::memcmp(m.reg, legacy.reg, sizeof m.reg) == 0);
    CHECK(actual.errors() == expected.errors() && !m.inBurst);
    CHECK(m.logged(reason));
    if (receipt) CHECK(m.logged(receipt));
  };
  for (uint16_t floor : {128, 921}) {
    Model m = qualifiedPwmPair();
    setPwm(m, 0, 2046);
    check(m, floor, "supported=0 reason=pwm-not-full context=boot", "pwm_raw=07fe duty_permille=999 stable=1 valid=1");
    setPwm(m, 1, 2046);
    check(m, floor, "supported=0 reason=pwm-not-full context=boot");
    setPwm(m, 0, 0);
    setPwm(m, 1, 0);
    check(m, floor, "supported=0 reason=pwm-not-full context=boot", "pwm_raw=0000 duty_permille=0 stable=1 valid=1");
    m = qualifiedPwmPair();
    m.readReplies[{0, kRegPwmLsb}] = {0xff, 0xfe, 0xff};   // survey full, fresh pair changes
    check(m, floor, "supported=0 reason=pwm-unstable context=boot", "stable=0 valid=1 context=boot");
    m = qualifiedPwmPair();
    m.readReplies[{1, kRegPwmMsb}] = {0xff, 0xff, -1};   // survey and first sample succeed
    check(m, floor, "supported=0 reason=pwm-read context=boot", "stable=0 valid=0 context=boot");
    m = qualifiedPwmPair();
    m.readReplies[{1, kRegCfg1}] = {0x53, 0x57};   // fresh check supersedes the survey
    check(m, floor, "supported=0 reason=mode context=boot");
    for (uint8_t wrongMode : {0x52, 0x57, 0x5b, 0x73}) {
      m = qualifiedPwmPair();
      m.reg[1][kRegCfg1] = wrongMode;
      check(m, floor, "supported=0 reason=mode context=boot");
    }
    for (uint8_t reg : {kRegEn, kRegIfs, kRegOpt2}) {
      m = qualifiedPwmPair();
      m.reg[1][reg] ^= 1;
      check(m, floor, "supported=0 reason=current context=boot");
    }
    m = qualifiedPwmPair();
    m.reg[0][kRegIfs] = m.reg[1][kRegIfs] = 0xf9;
    check(m, floor, "supported=0 reason=current context=boot");
  }
  Model m = qualifiedPwmPair();
  for (unsigned i = 0; i < kIcs; ++i) {
    m.reg[i][kRegCfg1] = 0x52;
    setPwm(m, i, 0);
  }
  Controller ctl(m);
  Config cfg = config(0);
  cfg.android = true;
  ctl.configure(cfg);
  CHECK(ctl.survey() && ctl.start() && ctl.ceiling() == 1952);
  CHECK(m.logged("contract=exp-pwm-off supported=1 reason=ok context=boot"));

  Model unavailable = qualifiedPwmPair();
  unavailable.readReplies[{1, kRegPwmMsb}] = {-1, -1, -1};
  Controller fallback(unavailable);
  fallback.configure(cfg);
  CHECK(fallback.survey() && fallback.start() && fallback.ceiling() == 2047 && fallback.floor() == 128);
  CHECK(unavailable.logged("supported=0 reason=pwm-read context=boot"));
  CHECK(unavailable.code(0) == 2047 && unavailable.code(1) == 2047 && !fallback.pending());
  checkWhitelist(unavailable, fallback.floor());
}

int main()
{
  testI2c();
  testSteps();
  testSurvey();
  testBootLevel();
  testDisplayAndSteps();
  testLid();
  testSleep();
  testFailures();
  testStack();
  testKeys();
  testAmbient();
  testAmbientReadbackFault();
  testNativeBrightness();
  testAndroidBrightness();
  testUserLevel();
  testPwmContract();
  testPwmRefusal();
  if (failures) {
    std::printf("backlight_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("backlight_host: all passed (%u I2C waits)\n", gDelays);
  return 0;
}
