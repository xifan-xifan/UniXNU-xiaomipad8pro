// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BACKLIGHT_TESTS_KTZ8866_CHIP_H
#define PIANO_BACKLIGHT_TESTS_KTZ8866_CHIP_H

#include "BitI2c.h"
#include "Ktz8866.h"
#include <cstring>
#include <utility>
#include <vector>

namespace backlighttest {

using namespace pianobacklight;

// The datasheet's reset values (Table 5), with the ID's reserved bits 0b001.
inline void resetRegs(uint8_t *r)
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
  int bits = 0, stretch = 0, holding = 0, holdAfterWrite = 0;
  unsigned releaseSdaAfterClocks = 0;
  int nackWriteReg = -1;
  uint8_t shift = 0, byte = 0;
  bool reading = false, masterAck = false, firstByte = false;
  std::vector<std::pair<uint8_t, uint8_t>> writes;   // (register, value)
  unsigned starts = 0, stops = 0;
  LineIo lines(void (*delayUs)(void *, unsigned))
  {
    return {
      this,
      [](void *ctx, bool release) { auto &c = from(ctx); c.mSda = release; c.update(); },
      [](void *ctx, bool release) { auto &c = from(ctx); c.mScl = release; c.update(); },
      [](void *ctx) { return from(ctx).sda(); },
      [](void *ctx) { return from(ctx).scl(); },
      delayUs,
    };
  }
  bool sda() const { return mSda && !sdaLow && !releaseSdaAfterClocks; }
  bool scl() const { return mScl && !sclLow; }
  void load() { byte = pointer < kRegCount ? reg[pointer] : 0; ++pointer; sdaLow = !(byte & 0x80); }
  void rising()
  {
    if (releaseSdaAfterClocks) --releaseSdaAfterClocks;
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
        if (!firstByte && pointer == nackWriteReg) {
          nackWriteReg = -1;
          st = ignore;
          break;
        }
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
    case ackWr:
      sdaLow = false;
      st = wr; bits = 0; shift = 0;
      if (holdAfterWrite && !writes.empty()) { sclLow = true; holding = holdAfterWrite; }
      break;
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

 private:
  static Chip &from(void *ctx) { return *static_cast<Chip *>(ctx); }
};
}  // namespace backlighttest

#endif
