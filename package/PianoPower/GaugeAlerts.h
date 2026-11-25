// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Read-only gauge alerts. ISC level 3 asserts DTPT; an unrecognized ISC
// byte is unknown, never an alarm level or permission to raise current.
#ifndef PIANO_POWER_GAUGE_ALERTS_H
#define PIANO_POWER_GAUGE_ALERTS_H
#include <stdint.h>
#include "Gauge.h"

namespace pianopower {
struct GaugeAlerts {
  uint8_t isc[2] = {}, soa[2] = {};
  // Same per-field bits as PianoCharge.SafetyValid (bit 0 is CP control).
  uint32_t valid = 0, alarms = 0;
  uint8_t firstIsc[2] = {}, firstSoa[2] = {};
  uint32_t retried = 0, retryValid = 0, rereads = 0;

  void beginSample() { retried = retryValid = 0; }

  // Observe all-ones again before the caller votes or queues any writes.
  // This is a new transaction under the same pin owner, never a cached clear.
  // A persistent value or failed reread keeps the first read's safety meaning.
  uint32_t readPack(I2cBus &bus, unsigned pack)
  {
    const uint32_t ib = 2u << (2 * pack), sb = 4u << (2 * pack);
    uint32_t reads = 0;
    if (readField(bus, 0x72, isc[pack], firstIsc[pack], ib, 2)) reads |= ib;
    if (readField(bus, 0x70, soa[pack], firstSoa[pack], sb, 1)) reads |= sb;
    return reads;
  }

  void sampled(uint32_t readValid)
  {
    valid = readValid & 0x1e;
    for (unsigned i = 0; i < 2; ++i) {
      const uint32_t ib = 2u << (2 * i), sb = 4u << (2 * i);
      if (isc[i] > 3) valid &= ~ib;
      // Only a recognized, successful read can replace an earlier vote.
      // A missing other field cannot hide a confirmed alarm on this one.
      if (valid & ib) alarms = isc[i] == 3 ? alarms | ib : alarms & ~ib;
      if (valid & sb) alarms = soa[i] ? alarms | sb : alarms & ~sb;
    }
  }
  bool complete() const { return valid == 0x1e; }
  bool active() const { return alarms != 0; }

 private:
  bool readField(I2cBus &bus, uint8_t reg, uint8_t &raw, uint8_t &first, uint32_t bit, unsigned maxRereads)
  {
    if (bus.readRegisterByte(kGaugeAddress, reg, &raw) != I2cResult::ok) return false;
    first = raw;
    if (raw == 0xff) {
      retried |= bit;
      for (unsigned attempt = 0; attempt < maxRereads && raw == 0xff; ++attempt) {
        ++rereads;
        uint8_t next = raw;
        if (bus.readRegisterByte(kGaugeAddress, reg, &next) != I2cResult::ok) break;
        raw = next;
        retryValid |= bit;
      }
    }
    return true;
  }
};
}  // namespace pianopower
#endif
