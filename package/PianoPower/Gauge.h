// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as its two fuel gauges report it. piano has two cell packs in parallel, each with a TI bq27z561 at I2C address 0x55 on a
// bus of the application processor (live DT): pack A on i2c@a94000 (qup1_se5, pins gpio52/gpio53, shared with the SC8541 charge pump at
// 0x6a), pack B on i2c@88c000 (qup2_se3, pins gpio12/gpio13). Ubuntu reads them directly (bq27xxx); they are not behind the DSP.
//
// Reading them needs no QUP serial engine (its clocks, firmware and DFS tables are not ours to set up): the two pins of each bus are
// taken as plain GPIOs and the bus is driven in software, open drain (a line is either driven low or released to the board's pull-up;
// never driven high). Only reads: each transaction writes the one-byte command pointer and reads two bytes back, which changes nothing
// in the gauge. Pack values follow Ubuntu's piano_battery (Project-SunUEFI docs/devel/linux/kernel.md): currents and charges summed,
// voltage averaged, state of charge weighted by full charge capacity.
//
// bq27z561 standard commands (TI datasheet, two bytes little endian, Smart Battery style): 0x06 Temperature (0.1 K), 0x08 Voltage (mV),
// 0x0a BatteryStatus, 0x0c Current (mA, signed, + charging), 0x10 RemainingCapacity (mAh), 0x12 FullChargeCapacity (mAh), 0x14
// AverageCurrent (mA), 0x16 AverageTimeToEmpty and 0x18 AverageTimeToFull (minutes), 0x2a CycleCount, 0x2c RelativeStateOfCharge
// (%), 0x3c DesignCapacity (mAh). BatteryStatus bit 6 DSG (discharging), bit 5 FC (fully charged), bit 4 FD (fully discharged), bit 7 INIT.
#ifndef PIANO_POWER_GAUGE_H
#define PIANO_POWER_GAUGE_H
#include <stdint.h>

namespace pianopower {

// ---------------------------------------------------------------------------------------------------------- the bus in software
// Line control: release (true) lets the pull-up take the line high, false drives it low.
struct I2cIo {
  void *ctx;
  void (*sda)(void *ctx, bool release);
  void (*scl)(void *ctx, bool release);
  bool (*readSda)(void *ctx);
  bool (*readScl)(void *ctx);
  void (*delayUs)(void *ctx, unsigned us);
};

constexpr unsigned kHalfBitUs = 5;        // about 80 kHz with the register accesses
constexpr unsigned kStretchUs = 50000;    // a gauge may hold SCL low while it prepares data
constexpr unsigned kBusFreeUs = 100;      // between transactions (the bq27 family wants >= 66 us)

enum class I2cResult : uint8_t { ok, nackAddress, nackData, busBusy, stretchTimeout, refused };

inline const char *i2cName(I2cResult r)
{
  switch (r) {
  case I2cResult::ok: return "ok";
  case I2cResult::nackAddress: return "no ack (address)";
  case I2cResult::nackData: return "no ack (command)";
  case I2cResult::busBusy: return "bus held low";
  case I2cResult::stretchTimeout: return "clock stretch timeout";
  case I2cResult::refused: return "write refused (not the SC8541)";
  }
  return "?";
}

class I2cBus {
 public:
  explicit I2cBus(const I2cIo &io) : io_(io) {}

  // UniXNU-piano (ChargePump.h): a function run before every transaction on this bus, so the SC8541's watchdog kick (same bus as
  // pack A's gauge) goes ahead of any gauge read when it is due. The hook may itself use the bus: it is not re-entered.
  void setHook(void (*fn)(void *ctx, I2cBus &bus), void *ctx) { hook_ = fn; hookCtx_ = ctx; }

  // Frees a bus a device holds (SDA low): clocks until SDA is released, at most 9 times, then a stop.
  bool recover()
  {
    io_.sda(io_.ctx, true);
    for (unsigned i = 0; i < 9 && !io_.readSda(io_.ctx); ++i) {
      io_.scl(io_.ctx, false);
      io_.delayUs(io_.ctx, kHalfBitUs);
      if (!sclHigh()) return false;
      io_.delayUs(io_.ctx, kHalfBitUs);
    }
    stop();
    return io_.readSda(io_.ctx) && io_.readScl(io_.ctx);
  }

  // A register read: START, address+W, command, repeated START, address+R, two bytes (ACK, NACK), STOP.
  I2cResult readWord(uint8_t address, uint8_t command, uint16_t *value)
  {
    runHook();
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2cResult::busBusy;
    const I2cResult r = transfer(address, command, value);
    stop();
    io_.delayUs(io_.ctx, kBusFreeUs);
    return r;
  }

  // Read-only standard byte register (SC8541 CTRL2; bq27z561 SOA/ISC).
  // The command pointer is the sole write, followed by one NACKed data byte.
  I2cResult readRegisterByte(uint8_t address, uint8_t command, uint8_t *value)
  {
    runHook();
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2cResult::busBusy;
    uint16_t word = 0;
    const I2cResult r = transfer(address, command, &word, true);
    stop();
    io_.delayUs(io_.ctx, kBusFreeUs);
    if (r == I2cResult::ok) *value = uint8_t(word);
    return r;
  }

  // UniXNU-piano (ChargePump.h, SC8541 only): one register write, START, address+W, register, value, STOP. The caller (the CP
  // driver) checks the register against its allowlist first and reads it back afterwards.
  I2cResult writeRegisterByte(uint8_t address, uint8_t reg, uint8_t value)
  {
    if (address != 0x6a) return I2cResult::refused;   // the gauges (0x55) are never written
    runHook();
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2cResult::busBusy;
    I2cResult r = I2cResult::ok;
    if (!start()) r = I2cResult::busBusy;
    else if (!writeByte(uint8_t(address << 1), &r)) r = r != I2cResult::ok ? r : I2cResult::nackAddress;
    else if (!writeByte(reg, &r)) r = r != I2cResult::ok ? r : I2cResult::nackData;
    else if (!writeByte(value, &r)) r = r != I2cResult::ok ? r : I2cResult::nackData;
    stop();
    io_.delayUs(io_.ctx, kBusFreeUs);
    return r;
  }

  // UniXNU-piano (ChargePump.h): a sequential read of n bytes from reg (auto-increment), ACK on every byte but the last.
  I2cResult readBlock(uint8_t address, uint8_t reg, uint8_t *buf, unsigned n)
  {
    runHook();
    if (!n) return I2cResult::ok;
    if ((!io_.readSda(io_.ctx) || !io_.readScl(io_.ctx)) && !recover()) return I2cResult::busBusy;
    I2cResult r = I2cResult::ok;
    if (!start()) r = I2cResult::busBusy;
    else if (!writeByte(uint8_t(address << 1), &r)) r = r != I2cResult::ok ? r : I2cResult::nackAddress;
    else if (!writeByte(reg, &r)) r = r != I2cResult::ok ? r : I2cResult::nackData;
    else if (!start()) r = I2cResult::busBusy;
    else if (!writeByte(uint8_t(address << 1 | 1), &r)) r = r != I2cResult::ok ? r : I2cResult::nackAddress;
    else
      for (unsigned i = 0; i < n; ++i)
        if (!readByte(&buf[i], i + 1 < n, &r)) break;
    stop();
    io_.delayUs(io_.ctx, kBusFreeUs);
    return r;
  }

 private:
  const I2cIo io_;
  void (*hook_)(void *ctx, I2cBus &bus) = nullptr;
  void *hookCtx_ = nullptr;
  bool inHook_ = false;
  void runHook()
  {
    if (!hook_ || inHook_) return;
    inHook_ = true;
    hook_(hookCtx_, *this);
    inHook_ = false;
  }

  I2cResult transfer(uint8_t address, uint8_t command, uint16_t *value, bool byteOnly = false)
  {
    I2cResult r = I2cResult::ok;
    uint8_t lo = 0, hi = 0;
    if (!start()) return I2cResult::busBusy;
    if (!writeByte(uint8_t(address << 1), &r)) return r != I2cResult::ok ? r : I2cResult::nackAddress;
    if (!writeByte(command, &r)) return r != I2cResult::ok ? r : I2cResult::nackData;
    if (!start()) return I2cResult::busBusy;   // repeated START
    if (!writeByte(uint8_t(address << 1 | 1), &r)) return r != I2cResult::ok ? r : I2cResult::nackAddress;
    if (!readByte(&lo, !byteOnly, &r)) return r;
    if (!byteOnly && !readByte(&hi, false, &r)) return r;
    *value = uint16_t(lo | hi << 8);
    return I2cResult::ok;
  }

  bool sclHigh()
  {
    io_.scl(io_.ctx, true);
    for (unsigned waited = 0; !io_.readScl(io_.ctx); waited += 10) {
      if (waited >= kStretchUs) return false;
      io_.delayUs(io_.ctx, 10);
    }
    return true;
  }
  // START (or repeated START): SDA falls while SCL is high. Expects SCL low or high on entry; leaves SCL low.
  bool start()
  {
    io_.sda(io_.ctx, true);
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!sclHigh()) return false;
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!io_.readSda(io_.ctx)) return false;   // someone else holds SDA
    io_.sda(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    io_.scl(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    return true;
  }
  void stop()
  {
    io_.sda(io_.ctx, false);
    io_.delayUs(io_.ctx, kHalfBitUs);
    sclHigh();
    io_.delayUs(io_.ctx, kHalfBitUs);
    io_.sda(io_.ctx, true);
    io_.delayUs(io_.ctx, kHalfBitUs);
  }
  // One bit with SCL low on entry and exit; returns the SDA level sampled while SCL was high.
  bool bit(bool release, I2cResult *r)
  {
    io_.sda(io_.ctx, release);
    io_.delayUs(io_.ctx, kHalfBitUs);
    if (!sclHigh()) {
      *r = I2cResult::stretchTimeout;
      return false;
    }
    io_.delayUs(io_.ctx, kHalfBitUs);
    const bool level = io_.readSda(io_.ctx);
    io_.scl(io_.ctx, false);
    return level;
  }
  bool writeByte(uint8_t byte, I2cResult *r)
  {
    for (int i = 7; i >= 0; --i) {
      bit((byte >> i) & 1, r);
      if (*r != I2cResult::ok) return false;
    }
    const bool nack = bit(true, r);   // the device pulls SDA low to acknowledge
    return *r == I2cResult::ok && !nack;
  }
  bool readByte(uint8_t *byte, bool ack, I2cResult *r)
  {
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = uint8_t(v << 1 | (bit(true, r) ? 1 : 0));
      if (*r != I2cResult::ok) return false;
    }
    bit(!ack, r);
    *byte = v;
    return *r == I2cResult::ok;
  }
};

// ------------------------------------------------------------------------------------------------------------------ bq27z561
constexpr uint8_t kGaugeAddress = 0x55;
enum GaugeCommand : uint8_t {
  kGaugeTemperature = 0x06, kGaugeVoltage = 0x08, kGaugeStatus = 0x0a, kGaugeCurrent = 0x0c, kGaugeRemaining = 0x10,
  kGaugeFull = 0x12, kGaugeAverageCurrent = 0x14, kGaugeTimeToEmpty = 0x16, kGaugeTimeToFull = 0x18,
  kGaugeCycles = 0x2a, kGaugeSoc = 0x2c, kGaugeDesign = 0x3c,
};
constexpr uint16_t kGaugeDsg = 1u << 6, kGaugeFc = 1u << 5, kGaugeFd = 1u << 4, kGaugeInit = 1u << 7;
constexpr uint16_t kGaugeTimeUnknown = 0xffff;

struct GaugeReading {
  bool ok = false;
  I2cResult error = I2cResult::ok;
  uint8_t failedCommand = 0, firstRetryCommand = 0;
  int voltageMv = 0, currentMa = 0, averageMa = 0, socPct = 0, remainingMah = 0, fullMah = 0, cycles = 0;
  int designMah = 0;      // 0 when the gauge did not answer it (read on its own: it never invalidates the reading)
  int tempDeciC = 0;      // tenths of a degree Celsius
  unsigned wordRereads = 0;
  uint16_t status = 0;
  uint16_t wordRetryMask = 0, wordRetryValid = 0;
  uint16_t firstRetryValue = 0;
  uint16_t timeToEmpty = kGaugeTimeUnknown, timeToFull = kGaugeTimeUnknown;  // minutes
};

inline int16_t asSigned(uint16_t v) { return int16_t(v); }

inline bool gaugeWordCredible(uint8_t command, uint16_t value)
{
  switch (command) {
    case kGaugeVoltage: return value >= 2500 && value <= 5000;
    case kGaugeCurrent: case kGaugeAverageCurrent: {
      // Leave headroom above the 7 A per-pack overcurrent trip while rejecting gross signed-word corruption.
      constexpr int kMaxCurrentMa = 16000;
      const int currentMa = asSigned(value);
      return currentMa >= -kMaxCurrentMa && currentMa <= kMaxCurrentMa;
    }
    case kGaugeSoc: return value <= 100;
    case kGaugeTemperature: return value > 2332 && value < 3632;
    case kGaugeRemaining: case kGaugeFull: case kGaugeStatus: case kGaugeCycles: return value != 0xffff;
    default: return true;
  }
}

inline GaugeReading readGaugeOnce(I2cBus &bus)
{
  static const uint8_t kOrder[] = {kGaugeVoltage, kGaugeCurrent, kGaugeAverageCurrent, kGaugeSoc, kGaugeRemaining, kGaugeFull,
                                   kGaugeTemperature, kGaugeStatus, kGaugeCycles};
  GaugeReading g;
  uint16_t v[sizeof kOrder] = {};
  for (unsigned i = 0; i < sizeof kOrder; ++i) {
    g.error = bus.readWord(kGaugeAddress, kOrder[i], &v[i]);
    if (g.error != I2cResult::ok) {
      g.failedCommand = kOrder[i];
      return g;
    }
    if (!gaugeWordCredible(kOrder[i], v[i])) {
      // An ACK is not a plausibility check. Reobserve this command before a
      // whole new snapshot exposes all the other words to another bad read.
      // readWord supplies a fresh command pointer, STOP and bus-free interval.
      if (!g.wordRetryMask) {g.firstRetryCommand = kOrder[i]; g.firstRetryValue = v[i];}
      const uint16_t bit = uint16_t(1u << i);
      g.wordRetryMask |= bit;
      g.wordRereads++;
      uint16_t next = 0;
      const I2cResult error = bus.readWord(kGaugeAddress, kOrder[i], &next);
      if (error == I2cResult::ok) {
        v[i] = next;
        if (gaugeWordCredible(kOrder[i], next)) g.wordRetryValid |= bit;
      } else {
        g.error = error;
        g.failedCommand = kOrder[i];
        return g;
      }
    }
  }
  g.voltageMv = v[0];
  g.currentMa = asSigned(v[1]);
  g.averageMa = asSigned(v[2]);
  g.socPct = v[3];
  g.remainingMah = v[4];
  g.fullMah = v[5];
  g.tempDeciC = int(v[6]) - 2732;
  g.status = v[7];
  g.cycles = v[8];
  uint16_t design = 0;
  if (bus.readWord(kGaugeAddress, kGaugeDesign, &design) == I2cResult::ok && design && design != 0xffff) g.designMah = design;
  // A reading no battery can have means the transaction went wrong somewhere (a stuck line reads as 0xffff).
  g.ok = gaugeWordCredible(kGaugeVoltage, v[0]) && gaugeWordCredible(kGaugeCurrent, v[1]) &&
         gaugeWordCredible(kGaugeAverageCurrent, v[2]) && gaugeWordCredible(kGaugeSoc, v[3]) &&
         gaugeWordCredible(kGaugeTemperature, v[6]);
  if (!g.ok) g.failedCommand = 0xff;
  return g;
}

inline bool gaugeWordSentinel(const GaugeReading &g)
{
  return g.remainingMah == 0xffff || g.fullMah == 0xffff || g.status == 0xffff || g.cycles == 0xffff;
}

inline GaugeReading readGaugeCore(I2cBus &bus)
{
  const GaugeReading first = readGaugeOnce(bus);
  if (first.ok && !gaugeWordSentinel(first)) return first;

  // Do not feed one failed observation to every policy decision until the next
  // report. Each attempt starts over with the command pointers and a new sample.
  const GaugeReading next = readGaugeOnce(bus);
  // Capacity/flags are independent of the voltage, SOC and temperature gate.
  // Keep a credible core reading if its reread failed; publication still has
  // its own capacity guard. A failed core reading is never made valid here.
  GaugeReading result = next.ok || !first.ok ? next : first;
  result.wordRetryMask = first.wordRetryMask | next.wordRetryMask;
  result.wordRetryValid = first.wordRetryValid | next.wordRetryValid;
  result.wordRereads = first.wordRereads + next.wordRereads;
  if (first.wordRetryMask) {
    result.firstRetryCommand = first.firstRetryCommand;
    result.firstRetryValue = first.firstRetryValue;
  }
  return result;
}

inline GaugeReading readGauge(I2cBus &bus, bool readTimes = false)
{
  GaugeReading g = readGaugeCore(bus);
  // Ubuntu's bq27z561 time registers already report minutes. A failed read or
  // 0xffff leaves that time unknown without invalidating the core sample.
  if (readTimes && g.ok) {
    bus.readWord(kGaugeAddress, kGaugeTimeToEmpty, &g.timeToEmpty);
    bus.readWord(kGaugeAddress, kGaugeTimeToFull, &g.timeToFull);
  }
  return g;
}

struct PackReading {
  bool ok = false;     // at least one pack read
  unsigned packs = 0;  // how many
  int voltageMv = 0, currentMa = 0, socPct = 0, remainingMah = 0, fullMah = 0, tempDeciC = 0;
  bool charging = false, discharging = false, full = false;
};

inline PackReading combinePacks(const GaugeReading *g, unsigned n)
{
  PackReading p;
  long voltage = 0, temperature = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (!g[i].ok) continue;
    p.packs++;
    voltage += g[i].voltageMv;
    if (p.packs == 1 || g[i].tempDeciC > temperature) temperature = g[i].tempDeciC;  // the warmer pack
    p.currentMa += g[i].currentMa;
    p.remainingMah += g[i].remainingMah;
    p.fullMah += g[i].fullMah;
    p.full = (p.packs == 1 || p.full) && gaugeWordCredible(kGaugeStatus, g[i].status) && (g[i].status & kGaugeFc);
  }
  if (!p.packs) return p;
  p.ok = true;
  p.voltageMv = int(voltage / long(p.packs));
  p.tempDeciC = int(temperature);
  if (p.fullMah > 0) {
    p.socPct = int((long(p.remainingMah) * 100 + p.fullMah / 2) / p.fullMah);
  } else {
    long soc = 0;
    for (unsigned i = 0; i < n; ++i) if (g[i].ok) soc += g[i].socPct;
    p.socPct = int(soc / long(p.packs));
  }
  if (p.socPct > 100) p.socPct = 100;
  p.charging = p.currentMa > 20;
  p.discharging = p.currentMa < -20;
  return p;
}

}  // namespace pianopower
#endif
