// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: the SC8541 driver's implementation (ChargePump.h declares it). Header-only like Gauge.h, so the kext and the host
// tests (tests/cp_driver_host.cpp) compile the same code. Sequences: private/host-checks/20261123-charging/CP-SPEC.md section 2.4.
#ifndef PIANO_POWER_CHARGE_PUMP_IMPL_H
#define PIANO_POWER_CHARGE_PUMP_IMPL_H
#include "ChargePump.h"

namespace pianopower {

inline bool cpWriteAllowed(uint8_t reg, uint8_t mask, uint8_t value, int savedWd)
{
  const uint8_t v = uint8_t(value & mask);
  if (v != value) return false;                                   // no bits outside the mask
  for (const CpAllowedWrite &w : kCpAllowed)
    if (w.reg == reg && w.mask == mask && v >= w.lo && v <= w.hi) return true;
  // the watchdog field back to what it read before the driver took it (only that value, only CTRL3's field)
  return reg == PIANO_CP_REG_CTRL3 && mask == PIANO_CP_WD_MASK && savedWd >= 0 && v == uint8_t(savedWd);
}

inline void Sc8541::fail(uint32_t fault)
{
  faults_ |= fault;
  lost_ = true;            // no further writes this boot except the stop below
  switching_ = false;
}

inline void Sc8541::busError()
{
  faults_ |= PIANO_CP_FAULT_BUS;
  if (++busErrors_ >= PIANO_CP_MAX_BUS_ERRORS) {
    lost_ = true;           // the hardware watchdog ends any switching: no more kicks reach it
    switching_ = false;
  }
}

inline bool Sc8541::readReg(I2cBus &bus, uint8_t reg, uint8_t *value)
{
  if (bus.readRegisterByte(PIANO_CP_ADDRESS, reg, value) != I2cResult::ok) {
    busError();
    return false;
  }
  busErrors_ = 0;
  return true;
}

inline bool Sc8541::writeChecked(I2cBus &bus, uint8_t reg, uint8_t mask, uint8_t value)
{
  if (!arm.armed()) {faults_ |= PIANO_CP_FAULT_UNARMED; return false;}
  if (lost_ || !present_) return false;
  if (!cpWriteAllowed(reg, mask, value, wdSaved_)) {fail(PIANO_CP_FAULT_REFUSED); return false;}
  uint8_t old = 0, back = 0;
  if (!readReg(bus, reg, &old)) return false;
  // sim (fork B): the bits outside mask come from the register; two agreeing reads first, so a read corrupted on the wire is never
  // written back into another field (a flipped bit 3 of CTRL2 is EN_BYPASS)
  if (mask != 0xff) {
    uint8_t again = 0;
    if (!readReg(bus, reg, &again)) return false;
    if (again != old) {fail(PIANO_CP_FAULT_READBACK); return false;}
  }
  const uint8_t next = uint8_t((old & ~mask) | value);
  if (bus.writeRegisterByte(PIANO_CP_ADDRESS, reg, next) != I2cResult::ok) {busError(); return false;}
  if (!readReg(bus, reg, &back)) return false;
  if ((back & mask) != value) {fail(PIANO_CP_FAULT_READBACK); return false;}
  return true;
}

inline bool Sc8541::probe(I2cBus &bus)
{
  uint8_t id = 0;
  if (!readReg(bus, PIANO_CP_REG_DEVICE, &id)) return false;
  present_ = id == PIANO_CP_DEVICE_ID;
  if (!present_) faults_ |= PIANO_CP_FAULT_ID;
  return present_;
}

inline bool Sc8541::readCtrl2(I2cBus &bus, uint8_t *ctrl2)
{
  return readReg(bus, PIANO_CP_REG_CTRL2, ctrl2);
}

inline bool Sc8541::readStatus(I2cBus &bus, uint8_t stat[PIANO_CP_STAT_BYTES])
{
  if (bus.readBlock(PIANO_CP_ADDRESS, PIANO_CP_REG_STAT_FIRST, stat, PIANO_CP_STAT_BYTES) != I2cResult::ok) {
    busError();
    return false;
  }
  busErrors_ = 0;
  return true;
}

inline bool Sc8541::readAdc(I2cBus &bus, piano_cp_adc *adc)
{
  // the stock scales (CP-SPEC.md 2.3): LSB in uV / uA / 0.1 degC per raw unit, as numerator / denominator
  static const struct { uint32_t num, den; } kScale[PIANO_CP_ADC_CHANNELS] = {
    {2500, 1000}, {3750, 1000}, {5000, 1000}, {5000, 1000}, {1250, 1000}, {1250, 1000}, {3125, 1000}, {1, 1}, {1, 1}, {5, 1},
  };
  adc->valid = 0;
  uint8_t ctrl = 0;
  if (!readReg(bus, PIANO_CP_REG_ADC_CTRL1, &ctrl)) return false;
  if (!(ctrl & PIANO_CP_ADC_EN)) return false;                 // not converting: no values (not a fault)
  uint8_t raw[2 * PIANO_CP_ADC_CHANNELS];
  if (bus.readBlock(PIANO_CP_ADDRESS, PIANO_CP_REG_ADC_BASE, raw, sizeof raw) != I2cResult::ok) {busError(); return false;}
  busErrors_ = 0;
  int32_t v[PIANO_CP_ADC_CHANNELS];
  for (unsigned c = 0; c < PIANO_CP_ADC_CHANNELS; ++c) {
    const uint32_t r = uint32_t(raw[2 * c]) << 8 | raw[2 * c + 1];
    v[c] = int32_t(uint64_t(r) * kScale[c].num / kScale[c].den);
  }
  adc->ibus_ma = v[PIANO_CP_ADC_IBUS]; adc->vbus_mv = v[PIANO_CP_ADC_VBUS];
  adc->vac1_mv = v[PIANO_CP_ADC_VAC1]; adc->vac2_mv = v[PIANO_CP_ADC_VAC2];
  adc->vout_mv = v[PIANO_CP_ADC_VOUT]; adc->vbat_mv = v[PIANO_CP_ADC_VBAT]; adc->ibat_ma = v[PIANO_CP_ADC_IBAT];
  adc->tsbus_raw = v[PIANO_CP_ADC_TSBUS]; adc->tsbat_raw = v[PIANO_CP_ADC_TSBAT]; adc->tdie_dc = v[PIANO_CP_ADC_TDIE];
  adc->valid = (1u << PIANO_CP_ADC_CHANNELS) - 1;
  return true;
}

inline bool Sc8541::kick(I2cBus &bus)
{
  const uint64_t now = clock_.nowMs(clock_.ctx);
  if (switching_ && lastKickMs_ && now > lastKickMs_ + PIANO_CP_KICK_LATE_MS) {
    // the worker stalled: the hardware watchdog may already have stopped it; stop it ourselves and say so
    faults_ |= PIANO_CP_FAULT_KICK_LATE;
    stop(bus, 0x4b49434bu /* 'KICK' */);
    return false;
  }
  if (!writeChecked(bus, PIANO_CP_REG_CTRL3, PIANO_CP_WD_MASK, PIANO_CP_WD_ACTIVE)) {
    if (switching_ || lost_) stop(bus, 0x57444f47u /* 'WDOG' */);
    return false;
  }
  lastKickMs_ = now;
  return true;
}

inline bool Sc8541::setBusOcp(I2cBus &bus, uint32_t ma)
{
  if (ma < PIANO_CP_BUSOCP_MIN_MA) ma = PIANO_CP_BUSOCP_MIN_MA;
  if (ma > PIANO_CP_BUSOCP_MAX_MA) ma = PIANO_CP_BUSOCP_MAX_MA;
  ma = PIANO_CP_BUSOCP_MIN_MA + (ma - PIANO_CP_BUSOCP_MIN_MA) / 250u * 250u;   // the register's step, rounded down
  if (!writeChecked(bus, PIANO_CP_REG_BUSOCP, 0x9f, PIANO_CP_ENC_BUSOCP(ma))) {
    if (switching_ || lost_) stop(bus, 0x4f435053u /* 'OCPS' */);
    return false;
  }
  busocpMa_ = ma;
  return true;
}

inline bool Sc8541::start(I2cBus &bus, uint32_t busocpMa)
{
  if (!arm.armed()) {faults_ |= PIANO_CP_FAULT_UNARMED; return false;}
  if (lost_ || !present_ || switching_ || guardAtMs_) return false;   // a stop's guard must finish first
  // VAC OVP: never written; it must read the stock field
  uint8_t usbovp = 0;
  if (!readReg(bus, PIANO_CP_REG_USBOVP, &usbovp)) return false;
  if ((usbovp & PIANO_CP_USBOVP_MASK) != PIANO_CP_USBOVP_STOCK) {faults_ |= PIANO_CP_FAULT_VACOVP; return false;}
  // the watchdog first (its saved field kept for the release)
  if (!wdOwned_) {
    uint8_t ctrl3 = 0;
    if (!readReg(bus, PIANO_CP_REG_CTRL3, &ctrl3)) return false;
    wdSaved_ = ctrl3 & PIANO_CP_WD_MASK;
    wdOwned_ = true;
  }
  if (!writeChecked(bus, PIANO_CP_REG_CTRL3, PIANO_CP_WD_MASK, PIANO_CP_WD_ACTIVE)) return false;
  lastKickMs_ = clock_.nowMs(clock_.ctx);
  // the protections and the ADC, every one read back
  uint32_t ocp = busocpMa ? busocpMa : PIANO_CP_BUSOCP_START_MA;
  if (ocp < PIANO_CP_BUSOCP_MIN_MA) ocp = PIANO_CP_BUSOCP_MIN_MA;
  if (ocp > PIANO_CP_BUSOCP_MAX_MA) ocp = PIANO_CP_BUSOCP_MAX_MA;
  ocp = PIANO_CP_BUSOCP_MIN_MA + (ocp - PIANO_CP_BUSOCP_MIN_MA) / 250u * 250u;
  const struct { uint8_t reg, mask, value; } seq[] = {
    {PIANO_CP_REG_BATOVP, 0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_MV)},
    {PIANO_CP_REG_BATOVP_ALM, 0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_ALM_MV)},
    {PIANO_CP_REG_BUSOVP, 0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_MV)},
    {PIANO_CP_REG_BUSOVP_ALM, 0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_ALM_MV)},
    {PIANO_CP_REG_BUSOCP, 0x9f, PIANO_CP_ENC_BUSOCP(ocp)},
    {PIANO_CP_REG_BATOCP, 0xff, PIANO_CP_ENC_BATOCP(PIANO_CP_BATOCP_MA)},
    {PIANO_CP_REG_TEMP_CTRL, PIANO_CP_TSHUT_DISABLE, 0},
    {PIANO_CP_REG_CTRL2, PIANO_CP_EN_BYPASS, 0},
    {PIANO_CP_REG_ADC_CTRL2, 0xff, 0},
    {PIANO_CP_REG_ADC_CTRL1, 0xc3, PIANO_CP_ADC_EN},
  };
  for (const auto &w : seq)
    if (!writeChecked(bus, w.reg, w.mask, w.value)) {stop(bus, 0x41524d00u /* 'ARM' */); return false;}
  busocpMa_ = ocp;
  // CHG_EN: read-modify-write; it reads back set only some time later, so the session checks it at its next poll
  ctrl2ShadowValid_ = false;
  {uint8_t c = 0, d = 0;   // sim (fork B): CTRL2 as verified before CHG_EN (two agreeing reads), the stop's fallback
   if (readReg(bus, PIANO_CP_REG_CTRL2, &c) && readReg(bus, PIANO_CP_REG_CTRL2, &d) && c == d) {ctrl2Shadow_ = uint8_t(c & ~(PIANO_CP_CHG_EN | PIANO_CP_EN_BYPASS)); ctrl2ShadowValid_ = true;}}
  if (!cpWriteAllowed(PIANO_CP_REG_CTRL2, PIANO_CP_CHG_EN, PIANO_CP_CHG_EN, wdSaved_)) {fail(PIANO_CP_FAULT_REFUSED); return false;}
  // sim (fork B): CHG_EN set on the CTRL2 value just verified by two agreeing reads (EN_BYPASS clear), never on a single read
  if (!ctrl2ShadowValid_) {stop(bus, 0x43544c32u); return false;}
  if (bus.writeRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, uint8_t(ctrl2Shadow_ | PIANO_CP_CHG_EN)) != I2cResult::ok) {
    busError();
    stop(bus, 0x43484745u /* 'CHGE' */);
    return false;
  }
  busErrors_ = 0;
  switching_ = true;
  return true;
}

inline void Sc8541::stop(I2cBus &bus, uint32_t reason)
{
  stopReason_ = reason;
  const bool wasOn = switching_;
  switching_ = false;
  if (!present_) return;
  // CHG_EN = 0 whatever the register reads (the bits beside it as read; if the read fails, try once more, then the watchdog)
  // sim (fork B): the bits beside CHG_EN from two agreeing reads, else from the value verified at the start (a read corrupted on
  // the wire must not reach another CTRL2 field); EN_BYPASS is always cleared by a stop
  uint8_t ctrl2 = 0, again = 0;
  const bool a = bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, &ctrl2) == I2cResult::ok;
  const bool b = a && bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, &again) == I2cResult::ok;
  const bool agree = a && b && ((ctrl2 ^ again) & uint8_t(~PIANO_CP_CHG_EN)) == 0;
  const uint8_t base = agree ? ctrl2 : ctrl2Shadow_;
  const bool read = agree || ctrl2ShadowValid_;
  if (read && bus.writeRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, uint8_t(base & ~(PIANO_CP_CHG_EN | PIANO_CP_EN_BYPASS))) == I2cResult::ok) {
    busErrors_ = 0;
  } else {
    busError();
  }
  // the latched status once per falling CHG_EN (the evidence)
  if (wasOn) statValid_ = readStatus(bus, stat_);
  guardAtMs_ = clock_.nowMs(clock_.ctx) + PIANO_CP_GUARD_MS;
}

inline bool Sc8541::guard(I2cBus &bus)
{
  if (!guardAtMs_) return true;
  uint8_t ctrl2 = 0;
  if (!readReg(bus, PIANO_CP_REG_CTRL2, &ctrl2)) {guardAtMs_ = clock_.nowMs(clock_.ctx) + PIANO_CP_GUARD_MS; return false;}
  if (ctrl2 & PIANO_CP_CHG_EN) {
    faults_ |= PIANO_CP_FAULT_STOP_STUCK;
    stop(bus, stopReason_);           // again, as long as it takes
    return false;
  }
  guardAtMs_ = 0;
  // CHG_EN reads clear: the watchdog back to its own field (only when the driver still may write)
  if (wdOwned_ && wdSaved_ >= 0 && !lost_ && arm.armed() &&
      writeChecked(bus, PIANO_CP_REG_CTRL3, PIANO_CP_WD_MASK, uint8_t(wdSaved_)))
    wdOwned_ = false;
  return true;
}

inline uint64_t Sc8541::nextDeadline() const
{
  uint64_t d = switching_ ? lastKickMs_ + PIANO_CP_KICK_MS : 0;
  if (guardAtMs_ && (!d || guardAtMs_ < d)) d = guardAtMs_;
  return d;
}

inline void Sc8541::beforeBusTransaction(I2cBus &bus)
{
  if (inHook_) return;
  inHook_ = true;
  const uint64_t now = clock_.nowMs(clock_.ctx);
  if (kickDue(now)) kick(bus);
  if (guardDue(now)) guard(bus);
  inHook_ = false;
}

inline void Sc8541::fill(piano_cp_in *in) const
{
  in->armed = arm.armed();
  in->present = present_;
  in->lost = lost_;
  in->switching = switching_;
  in->busocp_ma = busocpMa_;
  in->last_kick_ms = lastKickMs_;
  in->stat_valid = statValid_;
  for (unsigned i = 0; i < PIANO_CP_STAT_BYTES; ++i) in->stat[i] = stat_[i];
}

}  // namespace pianopower
#endif  // PIANO_POWER_CHARGE_PUMP_IMPL_H
