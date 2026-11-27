// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery for macOS (piano-power-ps=1): an IOPMPowerSource whose values come from the two fuel gauges and the Type-C state that
// PianoPower reads at level 1 (BatteryMap.h). No DSP is needed and nothing is written to any hardware here.
//
// How the system finds it (xnu source, iokit/Kernel): IOPMrootDomain looks up services matching "IOPMPowerSource", and powerd
// (which feeds the menu bar's battery item) watches for that class too; updateStatus() copies the values into the registry and sends
// kIOPMMessageBatteryStatusHasChanged to its clients. The class is named AppleSmartBattery, the name a Mac's battery service has, so
// anything that looks the battery up by that class name or registry name finds this one; the vma2 kernel collection has no class of
// that name (it boots virtual machines, which have no battery), so there is no clash.
//
// Low charge: AtWarnLevel and AtCriticalLevel are never set here. The kernel's own low-battery sleep is started by
// kIOPMPowerEmergency from Apple's battery manager, which this is not; and every system sleep request is refused while the ADT's
// /defaults sleep-disabled is 1 (IOPMrootDomain::checkSystemSleepAllowed; configs/k27-adt.toml sets it), so a low percentage gives
// macOS's warnings but no sleep attempt on this tablet. docs/drivers/charging.md §5d.
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include "PianoBattery.h"
#include "BatteryHealthData.h"

using namespace pianopower;

class AppleSmartBattery : public IOPMPowerSource {
  OSDeclareDefaultStructors(AppleSmartBattery)

  void number(const char *key, int64_t value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
    if (symbol && n) setPSProperty(symbol, n);
    OSSafeReleaseNULL(n);
    OSSafeReleaseNULL(symbol);
  }
  void flag(const char *key, bool value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
    OSSafeReleaseNULL(symbol);
  }
  void text(const char *key, const char *value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSString *s = OSString::withCString(value);
    if (symbol && s) setPSProperty(symbol, s);
    OSSafeReleaseNULL(s);
    OSSafeReleaseNULL(symbol);
  }
  void clear(const char *key)
  {
    if (!properties->getObject(key)) return;
    properties->removeObject(key);
    removeProperty(key);
    settingsChangedSinceUpdate = true;
  }

 public:
  void apply(const BatteryValues &v)
  {
    flag("PianoChargeLimitEnabled", v.healthEnabled);
    flag("PianoChargeHold", v.healthHold);
    flag("PianoChargePending", v.healthPending);
    flag("PianoChargeApplied", v.healthApplied);
    number("PianoChargeStop", v.healthStop); number("PianoChargeResume", v.healthResume);
    number("PianoChargeReason", v.healthReason); number("PianoChargeRevision", v.healthRevision);
    flag("PianoAdapterContractKnown", v.adapterContractKnown);
    text("PianoAdapterSource", v.adapterContractKnown ? "UCSI RDO" : "Rp/unknown");
    setBatteryInstalled(v.installed);
    setExternalConnected(v.external);
    setExternalChargeCapable(v.chargeCapable);
    setIsCharging(v.charging);
    setAtWarnLevel(false);
    setAtCriticalLevel(false);
    setCurrentCapacity(v.percent);
    setMaxCapacity(v.maxCapacity);
    setVoltage(v.voltageMv);
    setAmperage(v.amperageMa);
    if (!v.gaugeFields || v.cycles != kCycleCountUnknown) setCycleCount(v.cycles);
    else clear(kIOPMPSCycleCountKey);
    setTimeRemaining(v.systemTimeRemaining());
    flag("FullyCharged", v.full);
    number("InstantAmperage", v.instantMa);
    number("AppleRawCurrentCapacity", v.rawMah);
    number("AppleRawMaxCapacity", v.fullMah);
    if (!v.gaugeFields || v.designMah) {
      number(kIOPMPSDesignCapacityKey, v.designMah);
      number("NominalChargeCapacity", v.fullMah);
    } else {
      clear(kIOPMPSDesignCapacityKey);
      clear("NominalChargeCapacity");
    }
    if (v.gaugeFields) {
      // Legacy source identity. The 27.3 pack path also needs Serial inside BatteryData (ps-health below).
      text(kIOPMPSSerialKey, kLogicalBatterySerial);
      if (v.designMah) number(kIOPMPSBatteryHealthKey, v.lowCapacity ? kIOPMFairValue : kIOPMGoodValue);
      else clear(kIOPMPSBatteryHealthKey);
      if (v.gaugeStatusValid) flag("PianoGaugeFullyDischarged", v.gaugeFullyDischarged);
      else clear("PianoGaugeFullyDischarged");
    }
    number("AvgTimeToEmpty", v.timeToEmpty);
    number("AvgTimeToFull", v.timeToFull);
    // What a Mac's AppleSmartBattery publishes next to these (this Mac's own node is the reference): the update stamp, the
    // built-in mark and the raw duplicates. powerd tracks the battery through these keys (2026-11-15 CHGUI: it showed 12 % all
    // boot while the registry tracked 95 -> 90 %, so the shape it expects matters as much as the values).
    if (v.updateSec) number("UpdateTime", v.updateSec);
    flag("built-in", true);
    flag("AppleRawExternalConnected", v.external);
    if (v.temperatureValid) number("Temperature", v.temperatureCenti);
    text("DeviceName", "bq27z561 x2");
    text("Manufacturer", "Texas Instruments");
    OSDictionary *adapter = OSDictionary::withCapacity(6);
    if (adapter) {
      if (v.adapter) {
        OSNumber *family = OSNumber::withNumber(v.adapterFamily, 32), *w = OSNumber::withNumber(v.adapterWatts, 32),
                 *mv = OSNumber::withNumber(v.adapterMv, 32), *ma = OSNumber::withNumber(v.adapterMa, 32),
                 *id = OSNumber::withNumber((unsigned long long)0, 32);
        if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
        if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
        if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
        if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
        if (id) {adapter->setObject(kIOPMPSAdapterDetailsIDKey, id); id->release();}
        adapter->setObject("IsWireless", kOSBooleanFalse);   // the key a Mac's dict carries; no constant in this xnu's IOPM.h
      }
      const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
      if (key) setPSProperty(key, adapter);
      OSSafeReleaseNULL(key);
      const OSSymbol *rawKey = OSSymbol::withCString("AppleRawAdapterDetails");
      if (rawKey) setPSProperty(rawKey, adapter);
      OSSafeReleaseNULL(rawKey);
      adapter->release();
    }
  }
};

OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource)

// powerd 27.3 matches this class and ID=0; a BatteryData property on the power
// source alone cannot satisfy that lookup. This is the same logical A+B pack.
class AppleSmartBatteryPack : public IOService {
  OSDeclareDefaultStructors(AppleSmartBatteryPack)
};
OSDefineMetaClassAndStructors(AppleSmartBatteryPack, IOService)

namespace {
AppleSmartBattery *gBattery;
AppleSmartBatteryPack *gHealthPack;

bool healthNumber(OSDictionary *dict, const char *key, uint64_t value, unsigned bits)
{
  OSNumber *n = OSNumber::withNumber(value, bits);
  const bool ok = n && dict->setObject(key, n);
  OSSafeReleaseNULL(n);
  return ok;
}

void updateHealthPack(AppleSmartBattery *battery, const BatteryValues &v)
{
  OSDictionary *props = batteryHealthProperties<OSDictionary, OSString>(v, healthNumber);
  if (!props) return; // keep the last complete sample across a transient invalid health report
  AppleSmartBatteryPack *pack = gHealthPack;
  if (!pack) {
    pack = OSTypeAlloc(AppleSmartBatteryPack);
    if (!pack || !pack->init() || !pack->attach(battery)) {
      IOLog("PianoPower: battery: FAILED to create the health pack\n");
      OSSafeReleaseNULL(pack);
      props->release();
      return;
    }
  }
  bool ok = true;
  if (!gHealthPack) {
    ok = pack->setProperty("ID", props->getObject("ID")) &&
         pack->setProperty("FeatureFlags", props->getObject("FeatureFlags"));
  }
  ok = ok && pack->setProperty("BatteryData", props->getObject("BatteryData"));
  props->release();
  if (!ok) {
    IOLog("PianoPower: battery: FAILED to publish the health pack properties\n");
    if (!gHealthPack) { pack->detach(battery); pack->release(); }
    return;
  }
  if (!gHealthPack) {
    pack->registerService(); // ready before the source is registered or its update notification is sent
    gHealthPack = pack;
    IOLog("PianoPower: battery: ps-health: AppleSmartBatteryPack ID=0 published (nominal metric)\n");
  }
}
}

bool pianoBatteryUpdate(IOService *owner, const BatteryValues &v)
{
  if (!v.known) return gBattery != nullptr;   // no reading this cycle: what macOS sees stays as it was
  if (!gBattery) {
    if (!owner) return false;
    AppleSmartBattery *b = OSTypeAlloc(AppleSmartBattery);
    if (!b || !b->init()) {
      IOLog("PianoPower: battery: FAILED to create the power source\n");
      OSSafeReleaseNULL(b);
      return false;
    }
    // The values are in the registry before anyone can find the service.
    b->apply(v);
    b->updateStatus();
    if (!b->attach(owner)) {
      IOLog("PianoPower: battery: FAILED to attach the power source\n");
      b->release();
      return false;
    }
    updateHealthPack(b, v);
    b->registerService();
    gBattery = b;   // the attachment keeps it; this reference is the driver's
    IOLog("PianoPower: battery: AppleSmartBattery (IOPMPowerSource) published: %u %%, %s%s\n", v.percent,
          v.external ? (v.charging ? "on power, charging" : v.full ? "on power, full" : "on power, not charging") : "on battery",
          v.clamped ? " (shown at the floor)" : "");
    return true;
  }
  updateHealthPack(gBattery, v);
  gBattery->apply(v);
  gBattery->updateStatus();
  return true;
}
