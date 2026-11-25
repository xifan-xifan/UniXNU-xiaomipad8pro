// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: host tests of the SC8541 driver (ChargePump.h / ChargePumpImpl.h) against a line-level I2C model of the shared bus
// i2c@a94000: the SC8541 (0x6a) and pack A's bq27z561 (0x55) on the same two open-drain lines, a fake clock advanced by the bus's own
// delays, and fault injection (NAK, stuck SDA, clock stretch, a register that ignores writes, CHG_EN latency and a stop that does not
// take, the SC8541's own watchdog).
#include "ChargePumpImpl.h"
#include "Gauge.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianopower;

static int gFail = 0, gPass = 0;
#define CHECK(c, ...) do { if (c) ++gPass; else { ++gFail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ------------------------------------------------------------------------------------------------ the model of the shared bus
struct Clock { uint64_t us = 0; uint64_t ms() const { return us / 1000; } };

struct Dev {
  uint8_t address;
  // line state the device drives
  bool sdaLow = false, sclLow = false;
  // protocol state
  enum St { idle, addr, reg, data, tx, ignore } st = idle;
  uint8_t shift = 0, bits = 0, ptr = 0, txByte = 0, txBits = 0;
  bool ackPhase = false, ackMine = false, masterNack = false;
  // fault injection
  unsigned nackAddrCount = 0;      // the next n address bytes for us are not acknowledged
  unsigned stuckClocks = 0;        // SDA held low until this many SCL falls
  bool stuckForever = false;
  uint64_t stretchUntilUs = 0;     // SCL held low until then
  virtual ~Dev() {}
  virtual uint8_t readReg(uint8_t r) = 0;
  virtual void writeReg(uint8_t r, uint8_t v) = 0;
  unsigned writes = 0, reads = 0;
};

struct Sc8541Sim : Dev {
  Clock *clk;
  uint8_t regs[256] = {};
  int stuckReg = -1;               // writes to it are ignored (read-back mismatch)
  unsigned chgEnLatencyMs = 50;    // CHG_EN reads set this long after it is written
  uint64_t chgEnSetAtMs = 0;
  bool chgEn = false;              // the stage's real state
  unsigned stopIgnoredCount = 0;   // the next n CHG_EN clears do not take
  unsigned wdPeriodMs = 5000;      // the hardware watchdog (the model's choice; the real period is open, CP-SPEC.md 3)
  uint64_t lastWdWriteMs = 0;
  bool wdFired = false;
  std::vector<uint8_t> writeLog;   // register of every write, in order
  explicit Sc8541Sim(Clock *c) : clk(c)
  {
    address = 0x6a;
    regs[PIANO_CP_REG_DEVICE] = PIANO_CP_DEVICE_ID;
    regs[PIANO_CP_REG_USBOVP] = 0x60 | 0x05;    // the stock VAC OVP field, other bits arbitrary
    regs[PIANO_CP_REG_CTRL3] = 0x04;            // a watchdog field the driver must restore later
    regs[PIANO_CP_REG_CTRL2] = 0x01;            // a bit beside CHG_EN that must survive
  }
  void tick()
  {
    const uint64_t now = clk->ms();
    if (chgEn && (regs[PIANO_CP_REG_CTRL3] & PIANO_CP_WD_MASK) == PIANO_CP_WD_ACTIVE && now > lastWdWriteMs + wdPeriodMs) {
      chgEn = false;               // the watchdog ends switching in hardware
      wdFired = true;
      regs[0x18] |= 0x01;
    }
  }
  uint8_t readReg(uint8_t r) override
  {
    tick();
    reads++;
    if (r == PIANO_CP_REG_CTRL2) {
      const bool shown = chgEn && clk->ms() >= chgEnSetAtMs + chgEnLatencyMs;
      return uint8_t((regs[r] & ~PIANO_CP_CHG_EN) | (shown ? PIANO_CP_CHG_EN : 0));
    }
    return regs[r];
  }
  void writeReg(uint8_t r, uint8_t v) override
  {
    tick();
    writes++;
    writeLog.push_back(r);
    if (int(r) == stuckReg) return;
    if (r == PIANO_CP_REG_CTRL3) lastWdWriteMs = clk->ms();
    if (r == PIANO_CP_REG_CTRL2) {
      const bool on = v & PIANO_CP_CHG_EN;
      if (on && !chgEn) {chgEn = true; chgEnSetAtMs = clk->ms(); lastWdWriteMs = clk->ms() > lastWdWriteMs ? lastWdWriteMs : clk->ms();}
      if (!on && chgEn) {
        if (stopIgnoredCount) --stopIgnoredCount;
        else chgEn = false;
      }
      regs[r] = uint8_t(v & ~PIANO_CP_CHG_EN);
      return;
    }
    regs[r] = v;
  }
};

struct GaugeSim : Dev {
  Clock *clk;
  unsigned stretchUsPerRead = 0;   // each word read holds SCL this long (a gauge preparing data)
  explicit GaugeSim(Clock *c) : clk(c) { address = 0x55; }
  uint8_t readReg(uint8_t r) override { reads++; return uint8_t(r * 3 + 1); }
  void writeReg(uint8_t, uint8_t) override { writes++; }
};

struct Bus {
  Clock clk;
  bool mSda = true, mScl = true;
  bool lastSda = true, lastScl = true;
  std::vector<Dev *> devs;
  unsigned starts = 0, transactions = 0;
  bool sda() const { bool v = mSda; for (Dev *d : devs) if (d->sdaLow || d->stuckForever || d->stuckClocks) v = false; return v; }
  bool scl() const { bool v = mScl; for (Dev *d : devs) if (d->sclLow || clk.us < d->stretchUntilUs) v = false; return v; }
  void update()
  {
    const bool d = sda(), c = scl();
    if (c && lastScl && lastSda != d) {
      if (!d) { starts++; for (Dev *x : devs) {x->st = Dev::addr; x->bits = 0; x->shift = 0; x->ackPhase = false; x->sdaLow = false;} }
      else for (Dev *x : devs) {x->st = Dev::idle; x->sdaLow = false; x->ackPhase = false;}
    }
    if (!lastScl && c) for (Dev *x : devs) rising(*x, d);
    if (lastScl && !c) for (Dev *x : devs) falling(*x);
    lastSda = sda();
    lastScl = scl();
  }
  void rising(Dev &x, bool d)
  {
    if (x.ackPhase) { if (!x.ackMine) x.masterNack = d; return; }
    if (x.st == Dev::addr || x.st == Dev::reg || x.st == Dev::data) { x.shift = uint8_t(x.shift << 1 | (d ? 1 : 0)); x.bits++; }
  }
  void load(Dev &x)
  {
    x.txByte = x.readReg(x.ptr++);
    x.txBits = 1;
    x.sdaLow = !((x.txByte >> 7) & 1);
  }
  void falling(Dev &x)
  {
    if (x.stuckClocks) { x.stuckClocks--; }
    if (x.ackPhase) {
      x.ackPhase = false;
      if (x.ackMine) {
        x.sdaLow = false;
        if (x.st == Dev::tx) load(x);
      } else if (x.masterNack) {
        x.sdaLow = false;
        x.st = Dev::ignore;
      } else {
        load(x);
      }
      return;
    }
    if (x.st == Dev::tx) {
      if (x.txBits < 8) { x.sdaLow = !((x.txByte >> (7 - x.txBits)) & 1); x.txBits++; }
      else { x.sdaLow = false; x.ackPhase = true; x.ackMine = false; x.masterNack = false; }
      return;
    }
    if ((x.st == Dev::addr || x.st == Dev::reg || x.st == Dev::data) && x.bits == 8) {
      x.bits = 0;
      if (x.st == Dev::addr) {
        if ((x.shift >> 1) == x.address) {
          if (x.nackAddrCount) { x.nackAddrCount--; x.st = Dev::ignore; x.shift = 0; return; }
          transactions++;
          x.st = (x.shift & 1) ? Dev::tx : Dev::reg;
          x.sdaLow = true; x.ackPhase = true; x.ackMine = true;
        } else {
          x.st = Dev::ignore;
        }
      } else if (x.st == Dev::reg) {
        x.ptr = x.shift; x.st = Dev::data; x.sdaLow = true; x.ackPhase = true; x.ackMine = true;
      } else {
        x.writeReg(x.ptr++, x.shift); x.sdaLow = true; x.ackPhase = true; x.ackMine = true;
      }
      x.shift = 0;
    }
  }
  static void setSda(void *c, bool rel) { Bus *b = static_cast<Bus *>(c); b->mSda = rel; b->update(); }
  static void setScl(void *c, bool rel) { Bus *b = static_cast<Bus *>(c); b->mScl = rel; b->update(); }
  static bool getSda(void *c) { return static_cast<Bus *>(c)->sda(); }
  static bool getScl(void *c) { return static_cast<Bus *>(c)->scl(); }
  static void delay(void *c, unsigned us) { Bus *b = static_cast<Bus *>(c); b->clk.us += us; b->update(); }
  I2cIo io() { return I2cIo{this, setSda, setScl, getSda, getScl, delay}; }
};

static uint64_t nowMs(void *ctx) { return static_cast<Bus *>(ctx)->clk.ms(); }

struct Rig {
  Bus bus;
  Sc8541Sim cp{&bus.clk};
  GaugeSim gauge{&bus.clk};
  I2cBus i2c{bus.io()};
  Sc8541 drv{Sc8541::Clock{&bus, nowMs}};
  Rig() { bus.devs = {&cp, &gauge}; }
  void armAll() { drv.arm.bootArg = true; drv.arm.sysctl = true; }
  void advanceMs(unsigned ms) { bus.clk.us += uint64_t(ms) * 1000; cp.tick(); }
  void hook() { i2c.setHook([](void *ctx, I2cBus &b) { static_cast<Sc8541 *>(ctx)->beforeBusTransaction(b); }, &drv); }
};

// ------------------------------------------------------------------------------------------------------------------ the tests
static void testAllowlist()
{
  unsigned allowed = 0;
  for (unsigned reg = 0; reg <= 0x41; ++reg)
    for (unsigned m = 0; m < 6; ++m) {
      const uint8_t masks[6] = {0xff, 0x9f, 0x80, 0x10, 0x08, 0x1c};
      for (unsigned v = 0; v < 256; ++v) {
        const bool ok = cpWriteAllowed(uint8_t(reg), masks[m], uint8_t(v), -1);
        if (!ok) continue;
        ++allowed;
        bool inTable = false;
        for (const CpAllowedWrite &w : kCpAllowed)
          if (w.reg == reg && w.mask == masks[m] && (v & masks[m]) == v && v >= w.lo && v <= w.hi) inTable = true;
        CHECK(inTable, "write reg %#x mask %#x value %#x allowed but not in the table", reg, masks[m], v);
      }
    }
  // exactly: 9 fixed rows + BUSOCP 25 values + CHG_EN 2 values = 36
  CHECK(allowed == 36, "allowed writes %u, expected 36", allowed);
  CHECK(!cpWriteAllowed(PIANO_CP_REG_REVERSE, 0xff, 0, -1), "REVERSE refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_USBOVP, 0xe0, 0x60, -1), "USBOVP refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_PROT_CTRL, 0x80, 0x80, -1), "PROT_CTRL refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_BUSOVP, 0xff, 0x78, -1), "BUSOVP 13000 refused (pick 11000)");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_BUSOCP, 0x9f, 25, -1), "BUSOCP 7250 refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_BUSOCP, 0x9f, 0x80, -1), "BUSOCP disable refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_BATOVP, 0xff, 0x61, -1), "BATOVP 4810 refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_BATOVP, 0xff, 0xe0, -1), "BATOVP disable refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_TEMP_CTRL, 0x80, 0x80, -1), "TSHUT disable refused");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_CTRL3, 0x1c, 0x04, -1), "CTRL3 other value refused without a saved field");
  CHECK(cpWriteAllowed(PIANO_CP_REG_CTRL3, 0x1c, 0x04, 0x04), "CTRL3 back to the saved field allowed");
  CHECK(!cpWriteAllowed(PIANO_CP_REG_CTRL3, 0x1c, 0x08, 0x04), "CTRL3 another value refused even with a saved field");
  CHECK(!cpWriteAllowed(0x40, 0xff, 0, -1) && !cpWriteAllowed(0x41, 0xff, 0, -1), "gate / manual reverse refused");
}

static void testGaugeNeverWritten()
{
  Rig r;
  CHECK(r.i2c.writeRegisterByte(0x55, 0x00, 0x01) == I2cResult::refused, "a write to the gauge address is refused");
  CHECK(r.bus.starts == 0 && r.gauge.writes == 0, "no bus activity for a refused write");
}

static void testProbeAndUnarmed()
{
  Rig r;
  CHECK(r.drv.probe(r.i2c) && r.drv.present(), "probe reads id 0x41");
  CHECK(r.cp.writes == 0, "probe writes nothing");
  for (int mode = 0; mode < 3; ++mode) {
    Rig q;
    q.drv.arm.bootArg = mode == 1;
    q.drv.arm.sysctl = mode == 2;
    q.drv.probe(q.i2c);
    CHECK(!q.drv.start(q.i2c, 3500), "start refused unarmed (mode %d)", mode);
    CHECK(q.cp.writes == 0, "unarmed: no SC8541 write at all (mode %d, %u writes)", mode, q.cp.writes);
    CHECK(q.drv.takeFaults() & PIANO_CP_FAULT_UNARMED, "unarmed fault reported");
    CHECK(!q.drv.setBusOcp(q.i2c, 4000) && !q.drv.kick(q.i2c) && q.cp.writes == 0,
          "unarmed: setBusOcp and kick write nothing (mode %d, %u writes)", mode, q.cp.writes);
    q.advanceMs(PIANO_CP_GUARD_MS + 1);
    q.drv.guard(q.i2c);
    CHECK(q.cp.writes == 0, "unarmed: the guard writes nothing (mode %d)", mode);
  }
  {
    // disarmed while switching: no further write but the stop
    Rig d;
    d.armAll();
    d.drv.probe(d.i2c);
    d.drv.start(d.i2c, 3500);
    d.drv.arm.sysctl = false;
    const size_t w = d.cp.writeLog.size();
    CHECK(!d.drv.setBusOcp(d.i2c, 4000), "disarmed: setBusOcp refused");
    bool other = false;
    for (size_t i = w; i < d.cp.writeLog.size(); ++i) other |= d.cp.writeLog[i] != PIANO_CP_REG_CTRL2;
    CHECK(!other, "disarmed: the only write is the CTRL2 stop");
    CHECK(!d.cp.chgEn && !d.drv.switching(), "disarmed while switching: the refused write stops the stage");
    d.advanceMs(PIANO_CP_KICK_MS);
    const size_t w2 = d.cp.writeLog.size();
    d.drv.kick(d.i2c);
    CHECK(d.cp.writeLog.size() == w2, "disarmed and stopped: no watchdog write");
  }
  Rig bad;
  bad.cp.regs[PIANO_CP_REG_DEVICE] = 0x42;
  CHECK(!bad.drv.probe(bad.i2c) && (bad.drv.takeFaults() & PIANO_CP_FAULT_ID), "a wrong id is not the SC8541");
  bad.armAll();
  CHECK(!bad.drv.start(bad.i2c, 3500) && bad.cp.writes == 0, "not present: no write");
}

static void testStart()
{
  Rig r;
  r.armAll();
  r.drv.probe(r.i2c);
  CHECK(r.drv.start(r.i2c, 3500), "start");
  CHECK(r.drv.switching(), "switching after start");
  const uint8_t *g = r.cp.regs;
  CHECK(g[PIANO_CP_REG_BATOVP] == 0x60 && g[PIANO_CP_REG_BATOVP_ALM] == 0x4c, "BATOVP 4800 / 4600");
  CHECK(g[PIANO_CP_REG_BUSOVP] == 80 && g[PIANO_CP_REG_BUSOVP_ALM] == 70, "BUSOVP 11000 / 10500");
  CHECK((g[PIANO_CP_REG_BUSOCP] & 0x9f) == 10, "BUSOCP 3500 = 10");
  CHECK(g[PIANO_CP_REG_BATOCP] == 0x50, "BATOCP 8000");
  CHECK(!(g[PIANO_CP_REG_TEMP_CTRL] & 0x80), "TSHUT on");
  CHECK((g[PIANO_CP_REG_ADC_CTRL1] & 0xc3) == 0x80 && g[PIANO_CP_REG_ADC_CTRL2] == 0, "ADC on, all channels");
  CHECK((g[PIANO_CP_REG_CTRL3] & 0x1c) == 0x10, "watchdog active");
  CHECK(r.cp.chgEn && (g[PIANO_CP_REG_CTRL2] & 0x01), "CHG_EN set, the bit beside it kept");
  bool usbovp = false, chgBeforeWd = false, seenWd = false;
  for (uint8_t reg : r.cp.writeLog) {
    if (reg == PIANO_CP_REG_USBOVP) usbovp = true;
    if (reg == PIANO_CP_REG_CTRL3) seenWd = true;
    if (reg == PIANO_CP_REG_CTRL2 && !seenWd) chgBeforeWd = true;
  }
  CHECK(!usbovp, "VAC OVP never written");
  CHECK(!chgBeforeWd, "the watchdog is taken before any CTRL2 write");
  uint8_t c2 = 0;
  r.drv.readCtrl2(r.i2c, &c2);
  CHECK(!(c2 & PIANO_CP_CHG_EN), "CHG_EN reads clear within its latency");
  r.advanceMs(60);
  r.drv.readCtrl2(r.i2c, &c2);
  CHECK(c2 & PIANO_CP_CHG_EN, "CHG_EN reads set after its latency");
  CHECK(r.drv.takeFaults() == 0, "no faults on a clean start");
  // stop, guard, watchdog released to the saved field
  r.drv.stop(r.i2c, 7);
  CHECK(!r.cp.chgEn && !r.drv.switching(), "stopped");
  r.advanceMs(PIANO_CP_GUARD_MS + 1);
  CHECK(r.drv.guard(r.i2c), "guard: clear");
  CHECK((r.cp.regs[PIANO_CP_REG_CTRL3] & 0x1c) == 0x04, "watchdog field back to the saved 0x04");
}

static void testVacOvp()
{
  Rig r;
  r.armAll();
  r.cp.regs[PIANO_CP_REG_USBOVP] = 0xe0;
  r.drv.probe(r.i2c);
  CHECK(!r.drv.start(r.i2c, 3500), "start refused on a non-stock VAC OVP");
  CHECK(r.cp.writes == 0, "no write at all (%u)", r.cp.writes);
  CHECK(r.drv.takeFaults() & PIANO_CP_FAULT_VACOVP, "VAC OVP fault");
}

static void testReadback()
{
  Rig r;
  r.armAll();
  r.cp.stuckReg = PIANO_CP_REG_BUSOVP;
  r.drv.probe(r.i2c);
  CHECK(!r.drv.start(r.i2c, 3500), "start fails on a read-back mismatch");
  CHECK(r.drv.lost(), "read-back mismatch: CP lost");
  CHECK(!r.cp.chgEn, "CHG_EN never set");
  CHECK(r.drv.takeFaults() & PIANO_CP_FAULT_READBACK, "read-back fault");
  const unsigned w = r.cp.writes;
  CHECK(!r.drv.start(r.i2c, 3500) && r.cp.writes == w, "lost: no further writes");
  CHECK(!r.drv.setBusOcp(r.i2c, 4000) || r.cp.writes == w, "lost: no BUSOCP write");
}

static void testNak()
{
  Rig r;
  r.armAll();
  r.drv.probe(r.i2c);
  r.cp.nackAddrCount = 1;
  CHECK(!r.drv.start(r.i2c, 3500), "start fails on one NAK");
  CHECK(!r.drv.lost(), "one NAK: not lost");
  CHECK(r.drv.takeFaults() & PIANO_CP_FAULT_BUS, "bus fault");
  r.advanceMs(PIANO_CP_GUARD_MS + 1);
  r.drv.guard(r.i2c);
  CHECK(r.drv.start(r.i2c, 3500), "start works after the NAK");
  Rig q;
  q.armAll();
  q.drv.probe(q.i2c);
  q.cp.nackAddrCount = 100;
  q.drv.start(q.i2c, 3500);
  for (int i = 0; i < 4; ++i) { uint8_t v; q.drv.readCtrl2(q.i2c, &v); }
  CHECK(q.drv.lost(), "repeated NAKs: lost");
  q.cp.nackAddrCount = 0;
  const unsigned w = q.cp.writes;
  CHECK(!q.drv.start(q.i2c, 3500) && q.cp.writes == w, "lost after NAKs: no writes");
}

static void testStuckBus()
{
  Rig r;
  r.cp.stuckClocks = 5;              // SDA held for 5 clocks: the 9-clock recovery frees it
  CHECK(r.drv.probe(r.i2c), "probe after a recovered stuck bus");
  Rig q;
  q.cp.stuckForever = true;
  uint8_t v;
  for (int i = 0; i < 3; ++i) q.drv.readCtrl2(q.i2c, &v);
  CHECK(q.drv.lost() && (q.drv.takeFaults() & PIANO_CP_FAULT_BUS), "a bus that never frees: lost");
}

static void testStopStuck()
{
  Rig r;
  r.armAll();
  r.drv.probe(r.i2c);
  r.drv.start(r.i2c, 3500);
  r.advanceMs(100);
  r.cp.stopIgnoredCount = 1;
  r.drv.stop(r.i2c, 9);
  CHECK(r.cp.chgEn, "the first stop did not take (model)");
  r.advanceMs(PIANO_CP_GUARD_MS + 1);
  CHECK(!r.drv.guard(r.i2c), "guard sees it still set");
  CHECK(!r.cp.chgEn, "guard stopped it again");
  CHECK(r.drv.takeFaults() & PIANO_CP_FAULT_STOP_STUCK, "stop-stuck fault");
  r.advanceMs(PIANO_CP_GUARD_MS + 1);
  CHECK(r.drv.guard(r.i2c), "second guard: clear");
}

static void testBusOcp()
{
  Rig r;
  r.armAll();
  r.drv.probe(r.i2c);
  r.drv.start(r.i2c, 3500);
  r.drv.setBusOcp(r.i2c, 7500); CHECK(r.drv.busOcpMa() == 7000 && (r.cp.regs[PIANO_CP_REG_BUSOCP] & 0x1f) == 24, "7500 -> 7000");
  r.drv.setBusOcp(r.i2c, 900);  CHECK(r.drv.busOcpMa() == 1000 && (r.cp.regs[PIANO_CP_REG_BUSOCP] & 0x1f) == 0, "900 -> 1000");
  r.drv.setBusOcp(r.i2c, 3600); CHECK(r.drv.busOcpMa() == 3500, "3600 -> 3500 (rounded down)");
  CHECK(!(r.cp.regs[PIANO_CP_REG_BUSOCP] & 0x80), "BUSOCP stays enabled");
}

static void testAdc()
{
  Rig r;
  r.armAll();
  r.drv.probe(r.i2c);
  r.drv.start(r.i2c, 3500);
  auto put = [&](unsigned ch, uint16_t raw) { r.cp.regs[PIANO_CP_REG_ADC_BASE + 2 * ch] = uint8_t(raw >> 8); r.cp.regs[PIANO_CP_REG_ADC_BASE + 2 * ch + 1] = uint8_t(raw); };
  put(PIANO_CP_ADC_IBUS, 1600);   // 4000 mA
  put(PIANO_CP_ADC_VBUS, 2400);   // 9000 mV
  put(PIANO_CP_ADC_VBAT, 3200);   // 4000 mV
  put(PIANO_CP_ADC_IBAT, 2560);   // 8000 mA
  put(PIANO_CP_ADC_TDIE, 900);    // 450 = 45.0 C
  piano_cp_adc a{};
  CHECK(r.drv.readAdc(r.i2c, &a), "ADC read");
  CHECK(a.ibus_ma == 4000 && a.vbus_mv == 9000 && a.vbat_mv == 4000 && a.ibat_ma == 8000 && a.tdie_dc == 4500,
        "ADC scales: ibus %d vbus %d vbat %d ibat %d tdie %d", a.ibus_ma, a.vbus_mv, a.vbat_mv, a.ibat_ma, a.tdie_dc);
  Rig off;
  off.drv.probe(off.i2c);
  piano_cp_adc b{};
  CHECK(!off.drv.readAdc(off.i2c, &b) && b.valid == 0 && off.cp.writes == 0, "ADC off: no values, no write");
}

// The worker under gauge load. runLoad: 60 s of a worker that reads pack A's gauge (12 words) every 2 s, each word held stretchMs by
// clock stretching, and services the CP's deadlines between gauge batches; returns the longest gap between two watchdog writes.
static uint64_t runLoad(Rig &r, unsigned stretchMs)
{
  uint64_t lastWd = r.cp.lastWdWriteMs, maxGap = 0;
  auto see = [&]() {
    if (r.cp.lastWdWriteMs != lastWd) {
      const uint64_t g = r.cp.lastWdWriteMs - lastWd;
      if (g > maxGap) maxGap = g;
      lastWd = r.cp.lastWdWriteMs;
    }
  };
  const uint64_t end = r.bus.clk.ms() + 60000;
  uint64_t nextGauge = r.bus.clk.ms();
  while (r.bus.clk.ms() < end) {
    if (r.bus.clk.ms() >= nextGauge) {
      for (int w = 0; w < 12; ++w) {
        r.gauge.stretchUntilUs = r.bus.clk.us + uint64_t(stretchMs) * 1000;
        uint16_t v;
        r.i2c.readWord(0x55, uint8_t(0x06 + 2 * w), &v);
        see();
      }
      nextGauge = r.bus.clk.ms() + 2000;
    }
    if (r.drv.kickDue(r.bus.clk.ms())) r.drv.kick(r.i2c);
    see();
    r.advanceMs(10);
  }
  return maxGap;
}

static void testWatchdogUnderLoad()
{
  {
    // the test's own sensitivity: without the bus hook, 12 x 40 ms of gauge stretching delays the kick past its period
    Rig n;
    n.armAll();
    n.drv.probe(n.i2c);
    n.drv.start(n.i2c, 3500);
    const uint64_t gap = runLoad(n, 40);
    CHECK(gap > PIANO_CP_KICK_MS + 150, "without the hook the kick is late under load (%llu ms): the test can see it",
          (unsigned long long)gap);
  }
  Rig r;
  r.armAll();
  r.hook();
  r.drv.probe(r.i2c);
  r.drv.start(r.i2c, 3500);
  const uint64_t gap = runLoad(r, 40);
  CHECK(gap <= PIANO_CP_KICK_MS + 100, "with the hook (one 40 ms word at most): kick gap under 12 x 40 ms gauge load %llu ms", (unsigned long long)gap);
  CHECK(!r.cp.wdFired && r.cp.chgEn, "the hardware watchdog never fired under load");
  CHECK(r.drv.takeFaults() == 0, "no faults under load");
  // a worker stall of 6 s: the model's hardware watchdog (5 s) ends switching; the next kick reports it late and stops
  r.advanceMs(6000);
  CHECK(r.cp.wdFired && !r.cp.chgEn, "a host stall: the hardware watchdog stopped switching");
  CHECK(!r.drv.kick(r.i2c), "the late kick is refused");
  CHECK(r.drv.takeFaults() & PIANO_CP_FAULT_KICK_LATE, "kick-late fault");
  CHECK(!r.drv.switching(), "driver: not switching after the stall");
}

int main()
{
  testAllowlist();
  testGaugeNeverWritten();
  testProbeAndUnarmed();
  testStart();
  testVacOvp();
  testReadback();
  testNak();
  testStuckBus();
  testStopStuck();
  testBusOcp();
  testAdc();
  testWatchdogUnderLoad();
  std::printf("cp_driver_host: %d passed, %d failed\n", gPass, gFail);
  return gFail ? 1 : 0;
}
