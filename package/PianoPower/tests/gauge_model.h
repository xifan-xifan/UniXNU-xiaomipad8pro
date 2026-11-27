// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "Gauge.h"
#include <map>
#include <vector>

namespace pianopower::test {

struct Gauge {
  uint8_t address = kGaugeAddress;
  bool present = true, stuckLow = false;
  std::map<uint8_t, uint16_t> regs;
  std::map<uint8_t, std::vector<uint16_t>> replies;
  std::map<uint8_t, unsigned> replyAt;
  unsigned nackReadNumber = 0;
  bool nackOnce = false;
  unsigned stretchReadNumber = 0;
  bool mSda = true, mScl = true, sSda = true, lastSda = true, lastScl = true;
  enum State { idle, address_, command, ignore, transmit } state = idle;
  int bits = 0;
  uint8_t shift = 0, reg = 0, txByte = 0;
  int txBits = 0, txIndex = 0;
  bool ackPhase = false, ackIsMine = false, nacked = false, reading = false;
  unsigned starts = 0, stops = 0, reads = 0;

  bool sda() const { return mSda && sSda && !stuckLow; }
  bool scl() const { return mScl; }
  void driveBit(bool one) { sSda = one; }
  void startTx()
  {
    if (!txIndex && replies.count(reg) && replyAt[reg] < replies[reg].size())
      regs[reg] = replies[reg][replyAt[reg]++];
    const uint16_t v = regs.count(reg) ? regs[reg] : 0;
    txByte = uint8_t(txIndex == 0 ? v : v >> 8);
    txIndex++;
    txBits = 1;
    driveBit((txByte >> 7) & 1);
  }
  void rising(bool d)
  {
    if (ackPhase) {
      if (!ackIsMine) nacked = d;
      return;
    }
    if (state == address_ || state == command || state == ignore) {
      shift = uint8_t(shift << 1 | (d ? 1 : 0));
      bits++;
    }
  }
  void falling()
  {
    if (ackPhase) {
      ackPhase = false;
      if (ackIsMine) {
        sSda = true;
        if (state == transmit) startTx();
      } else if (nacked) {
        sSda = true;
        state = idle;
      } else {
        startTx();
      }
      return;
    }
    if (state == transmit) {
      if (txBits < 8) {
        driveBit((txByte >> (7 - txBits)) & 1);
        txBits++;
      } else {
        sSda = true;
        ackPhase = true;
        ackIsMine = false;
        nacked = false;
      }
      return;
    }
    if ((state == address_ || state == command || state == ignore) && bits == 8) {
      bits = 0;
      if (state == address_) {
        const bool nackRead = (shift & 1) && reads + 1 == nackReadNumber;
        if (nackRead && nackOnce) nackReadNumber = 0;
        if (present && (shift >> 1) == address && !nackRead) {
          reading = shift & 1;
          state = reading ? transmit : command;
          if (reading) {txIndex = 0; reads++;}
          sSda = false;
          ackPhase = true;
          ackIsMine = true;
        } else {
          state = idle;
        }
      } else if (state == command) {
        reg = shift;
        state = ignore;
        sSda = false;
        ackPhase = true;
        ackIsMine = true;
      } else {
        sSda = false;
        ackPhase = true;
        ackIsMine = true;
      }
      shift = 0;
    }
  }
  void update()
  {
    const bool d = sda(), c = scl();
    if (c && lastScl && lastSda != d) {
      if (!d) {starts++; state = address_; bits = 0; shift = 0; ackPhase = false; sSda = true;}
      else {stops++; state = idle; sSda = true;}
    }
    if (!lastScl && c) rising(d);
    if (lastScl && !c) falling();
    lastSda = sda();
    lastScl = c;
  }
  static void setSda(void *ctx, bool release) { Gauge *g = static_cast<Gauge *>(ctx); g->mSda = release; g->update(); }
  static void setScl(void *ctx, bool release) { Gauge *g = static_cast<Gauge *>(ctx); g->mScl = release; g->update(); }
  static bool getSda(void *ctx) { return static_cast<Gauge *>(ctx)->sda(); }
  static bool getScl(void *ctx) {
    Gauge *g = static_cast<Gauge *>(ctx);
    return g->scl() && !(g->stretchReadNumber && g->reads >= g->stretchReadNumber);
  }
  static void delay(void *, unsigned) {}
  I2cIo io() { return I2cIo{this, setSda, setScl, getSda, getScl, delay}; }
};

inline void fillGauge(Gauge &g, int mv, int ma, int soc, int rem, int full, int deciC, uint16_t status)
{
  g.regs[kGaugeVoltage] = uint16_t(mv);
  g.regs[kGaugeCurrent] = uint16_t(int16_t(ma));
  g.regs[kGaugeAverageCurrent] = uint16_t(int16_t(ma));
  g.regs[kGaugeSoc] = uint16_t(soc);
  g.regs[kGaugeRemaining] = uint16_t(rem);
  g.regs[kGaugeFull] = uint16_t(full);
  g.regs[kGaugeTemperature] = uint16_t(deciC + 2732);
  g.regs[kGaugeStatus] = status;
  g.regs[kGaugeCycles] = 172;
}

} // namespace pianopower::test
