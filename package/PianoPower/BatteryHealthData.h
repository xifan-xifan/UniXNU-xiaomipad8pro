// SPDX-License-Identifier: BSD-2-Clause-Patent
// macOS 27.3 powerd's AppleSmartBatteryPack input, not its IOPS health output.
// Binary contract and the nominal metric selector: docs/drivers/power-bh2-powerd-analysis.md.
#ifndef PIANO_POWER_BATTERY_HEALTH_DATA_H
#define PIANO_POWER_BATTERY_HEALTH_DATA_H
#include "BatteryMap.h"

namespace pianopower {

constexpr const char *kLogicalBatterySerial = "Piano-bq27z561-A+B";
constexpr uint64_t kPackNominalHealthMetric = 1ull << 10; // FeatureFlags: select measured nominal/design health on Mac

inline bool healthPackReady(const BatteryValues &v)
{
  // No partial pack, substituted design=full, sentinel, or bad capacity may seed powerd's history.
  return v.packHealth && v.known && v.gaugeFields && v.cycles < kCycleCountUnknown &&
         v.designMah > 0 && v.designMah <= 20000 && v.fullMah >= 2000 &&
         v.fullMah <= 20000 && uint64_t(v.fullMah) * 100 <= uint64_t(v.designMah) * 150 &&
         v.rawMah > 0 && v.rawMah <= v.fullMah + v.fullMah / 20;
}

// The kernel uses OS collections; host tests use real CF collections and serialize
// this exact schema before decoding it as powerd and SPPowerReporter do.
template <class Dictionary, class String, class NumberWriter>
Dictionary *batteryHealthProperties(const BatteryValues &v, NumberWriter number)
{
  if (!healthPackReady(v)) return nullptr;
  Dictionary *props = Dictionary::withCapacity(3), *data = Dictionary::withCapacity(11);
  String *serial = String::withCString(kLogicalBatterySerial);
  bool ok = props && data && serial;
  if (ok) {
    ok = data->setObject("Serial", serial);
    ok = number(data, "RemainingCapacity", v.rawMah, 32) && ok;
    ok = number(data, "MaxCapacity", v.fullMah, 32) && ok; // mAh here; source MaxCapacity stays 100
    ok = number(data, "AppleRawMaxCapacity", v.fullMah, 32) && ok; // required again by the history update path
    ok = number(data, "DesignCapacity", v.designMah, 32) && ok;
    ok = number(data, "NominalChargeCapacity", v.fullMah, 32) && ok;
    ok = number(data, "CycleCount", v.cycles, 32) && ok;
    ok = number(data, "Voltage", v.voltageMv, 32) && ok;
    ok = number(data, "AverageCurrent", uint64_t(int64_t(v.amperageMa)), 32) && ok;
    ok = number(data, "FullyCharged", v.full ? 1 : 0, 32) && ok;
    if (v.temperatureValid) ok = number(data, "Temperature", uint64_t(int64_t(v.temperatureCenti)), 32) && ok;
    ok = number(props, "ID", 0, 32) && ok; // one logical aggregate, matching the existing source
    ok = number(props, "FeatureFlags", kPackNominalHealthMetric, 64) && ok;
    ok = props->setObject("BatteryData", data) && ok;
  }
  if (serial) serial->release();
  if (data) data->release();
  if (!ok && props) { props->release(); props = nullptr; }
  // PF status, auth, dates, Qmax and precomputed UI health are not gauge measurements we have.
  return props;
}

} // namespace pianopower
#endif
