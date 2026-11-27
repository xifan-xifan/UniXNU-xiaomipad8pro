// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only m120c-120 evidence, 2026-11-26. No inferred per-pack remaining charge.
// console.out:11922–11929 and bf-ioreg.txt; ps-fields:11923, transient design:10339.
// Only health inputs are replayed; the settled aggregate comes from the saved registry.
#ifndef PIANO_M120C_120_FIXTURE_H
#define PIANO_M120C_120_FIXTURE_H
#include "BatteryMap.h"
inline pianopower::BatteryValues m120c120(bool optIn, bool invalidDesign = false)
{
  using namespace pianopower;
  BatteryValues v;
  v.known = v.installed = v.external = v.chargeCapable = true;
  v.percent = 54; v.maxCapacity = 100; v.rawMah = 4750; v.fullMah = 8781;
  v.voltageMv = 3914; v.amperageMa = 40; v.instantMa = 25;
  v.temperatureValid = true; v.temperatureCenti = 4040;
  v.healthEnabled = v.healthApplied = true; v.healthStop = 80; v.healthResume = 75;
  GaugeReading packs[2];
  for (unsigned i = 0; i < 2; ++i) {
    packs[i].ok = true; packs[i].designMah = invalidDesign && i == 1 ? 0 : 4600;
    packs[i].fullMah = i ? 4450 : 4331; packs[i].cycles = i ? 122 : 121;
    packs[i].status = 0x40; packs[i].timeToEmpty = packs[i].timeToFull = kTimeUnknown;
  }
  mapGaugeBatteryFields(v, packs);
  v.packHealth = optIn;
  return v;
}
#endif
