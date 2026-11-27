// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cold-init consumer replay, independently derived from 27.3 powerd/SPPowerReporter.
// Addresses: docs/drivers/power-bh2-powerd-analysis.md. Does not execute powerd
// or model its low-capacity policy, persisted history, authentication or NVRAM backend.
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "BatteryHealthData.h"
#include "fixtures/m120c_120.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static CFStringRef key(const char *s) { return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8); }
static bool put(CFMutableDictionaryRef d, const char *s, CFTypeRef value) {
  CFStringRef k = key(s); CFDictionarySetValue(d, k, value); CFRelease(k); return true;
}
static CFTypeRef get(CFDictionaryRef d, const char *s) {
  if (!d) return nullptr;
  CFStringRef k = key(s); CFTypeRef v = CFDictionaryGetValue(d, k); CFRelease(k); return v;
}
static void erase(CFMutableDictionaryRef d, const char *s) {
  CFStringRef k = key(s); CFDictionaryRemoveValue(d, k); CFRelease(k);
}
static int64_t integer(CFDictionaryRef d, const char *s, int64_t fallback = -1) {
  CFTypeRef v = get(d, s);
  if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return fallback;
  int64_t result = fallback; CHECK(CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &result));
  return result;
}
struct String {
  CFStringRef cf;
  static String *withCString(const char *s) { return new String{key(s)}; }
  void release() { CFRelease(cf); delete this; }
};
struct Dictionary {
  CFMutableDictionaryRef cf = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  static Dictionary *withCapacity(unsigned) { return new Dictionary; }
  bool setObject(const char *s, String *v) { return put(cf, s, v->cf); }
  bool setObject(const char *s, Dictionary *v) { return put(cf, s, v->cf); }
  void release() { CFRelease(cf); delete this; }
};
static unsigned writes, failWrite;
static bool number(Dictionary *d, const char *s, uint64_t raw, unsigned bits) {
  if (++writes == failWrite) return false;
  int32_t n32 = int32_t(raw); int64_t n64; std::memcpy(&n64, &raw, sizeof n64);
  CFNumberRef n = CFNumberCreate(nullptr, bits == 32 ? kCFNumberSInt32Type : kCFNumberSInt64Type,
                                bits == 32 ? static_cast<void *>(&n32) : static_cast<void *>(&n64));
  bool ok = put(d->cf, s, n); CFRelease(n); return ok;
}
static CFMutableDictionaryRef serialized(const pianopower::BatteryValues &v) {
  Dictionary *d = pianopower::batteryHealthProperties<Dictionary, String>(v, number);
  if (!d) return nullptr;
  CFDataRef bytes = CFPropertyListCreateData(nullptr, d->cf, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
  CHECK(bytes);
  auto result = (CFMutableDictionaryRef)CFPropertyListCreateWithData(nullptr, bytes,
                                                 kCFPropertyListMutableContainers, nullptr, nullptr);
  CHECK(result); CFRelease(bytes); d->release(); return result;
}
static CFMutableDictionaryRef data(CFMutableDictionaryRef props) {
  auto d = get(props, "BatteryData");
  return d && CFGetTypeID(d) == CFDictionaryGetTypeID() ? (CFMutableDictionaryRef)d : nullptr;
}
struct Health { int service = -1, maximum = -1; };
static int ceilPercent(int64_t n, int64_t design) { return int((100 * n + design - 1) / design); }
// 0x1000169a0, 0x100016e40: one logical pack by default, match service CLASS + ID.
// 0x1000170c0, 0x10001eaa0: nested Serial and capacities are init prerequisites.
static Health coldInit(const char *serviceClass, CFMutableDictionaryRef props) {
  if (std::strcmp(serviceClass, "AppleSmartBatteryPack") || integer(props, "ID") != 0) return {};
  auto d = data(props); if (!d) return {};
  CFTypeRef serial = get(d, "Serial");
  if (!serial || CFGetTypeID(serial) != CFStringGetTypeID() || !CFStringGetLength((CFStringRef)serial)) return {};
  const auto full = integer(d, "MaxCapacity"), design = integer(d, "DesignCapacity");
  const auto nominal = integer(d, "NominalChargeCapacity"), cycles = integer(d, "CycleCount");
  if (full <= 0 || design <= 0 || cycles < 0) return {};
  // 0x100019104–0x100019194: the immediately following history update separately
  // validates raw full and design. MaxCapacity alone passes init but fails update.
  const auto rawFull = integer(d, "AppleRawMaxCapacity");
  if (rawFull <= 0 || ceilPercent(rawFull, design) > 150) return {5, -1};
  // Mac FeatureFlags bit 10 -> nominal metric, otherwise the legacy 200 mAh reserve.
  const bool nominalMetric = (integer(props, "FeatureFlags", 0) & 1024) != 0;
  if (nominalMetric && nominal <= 0) return {};
  const int maximum = nominalMetric ? (cycles < 21 ? 104 : ceilPercent(nominal, design))
                                    : ceilPercent(full, design) + ceilPercent(200, design);
  if (maximum <= 0 || maximum > 150) return {};
  // The cold healthy fixture has no prior sticky flags, failed auth or storage error.
  // 0x10001e89c/0x100024090: PF => state 8; do not suppress a real failure.
  CHECK(maximum >= 80); // this replay models healthy cold init; low-capacity policy needs history
  return {integer(d, "PermanentFailureStatus", 0) ? 8 : 0, maximum};
}
static const char *condition(Health h) { return h.service == 0 ? "Normal" : "Service Recommended"; }
static int maximumShown(Health h) { return h.service == 8 || h.maximum < 0 ? -1 : h.maximum > 100 ? 100 : h.maximum; }

int main() {
  using namespace pianopower;
  BatteryValues v = m120c120(false);
  CHECK(!serialized(v)); // existing ps-fields=1 with new gate omitted/0
  v.packHealth = true; v.gaugeFields = false; CHECK(!serialized(v));
  v = m120c120(true);
  CHECK(v.cycles == 122 && v.designMah == 9200 && v.fullMah == 8781 && !v.lowCapacity);
  CHECK(v.percent == 54 && v.maxCapacity == 100 && v.rawMah == 4750);
  CHECK(v.external && !v.charging && !v.full && v.systemTimeRemaining() == -1);
  CHECK(v.healthStop == 80 && v.healthResume == 75 && v.healthEnabled && v.healthApplied);
  auto props = serialized(v); CHECK(props);
  auto d = data(props); CHECK(d);
  CHECK(integer(props, "ID") == 0 && integer(props, "FeatureFlags") == 1024);
  CHECK(integer(d, "RemainingCapacity") == 4750 && integer(d, "MaxCapacity") == 8781);
  CHECK(integer(d, "AppleRawMaxCapacity") == 8781);
  CHECK(integer(d, "DesignCapacity") == 9200 && integer(d, "NominalChargeCapacity") == 8781);
  CHECK(integer(d, "CycleCount") == 122 && integer(d, "Temperature") == 4040);
  CHECK(!get(props, "BatteryHealth") && !get(d, "BatteryHealthCondition") && !get(d, "MaximumCapacityPercent"));
  CHECK(!get(d, "PermanentFailureStatus") && !get(d, "ErrorCondition"));
  const auto h = coldInit("AppleSmartBatteryPack", props);
  CHECK(std::strcmp(condition(h), "Normal") == 0 && maximumShown(h) == 96);

  // Reproduce the old shape: healthy top-level fields and Serial alone produce the two UI failures.
  Dictionary old;
  number(&old, "BatteryHealth", 3, 32); number(&old, "DesignCapacity", 9200, 32);
  number(&old, "AppleRawMaxCapacity", 8781, 32); number(&old, "CycleCount", 122, 32);
  String *s = String::withCString(kLogicalBatterySerial); old.setObject("Serial", s); s->release();
  auto before = coldInit("AppleSmartBattery", old.cf);
  CHECK(std::strcmp(condition(before), "Service Recommended") == 0 && maximumShown(before) == -1);
  CHECK(coldInit("AppleSmartBattery", props).maximum == -1); // dictionary without matching class is insufficient

  // Counterexamples distinguish class/ID/schema, metric selector, and fault handling.
  for (const char *missing : {"Serial", "DesignCapacity", "NominalChargeCapacity", "CycleCount"}) {
    auto copy = serialized(v); erase(data(copy), missing);
    CHECK(coldInit("AppleSmartBatteryPack", copy).maximum == -1); CFRelease(copy);
  }
  auto copy = serialized(v); erase(copy, "BatteryData");
  CHECK(coldInit("AppleSmartBatteryPack", copy).maximum == -1); CFRelease(copy);
  copy = serialized(v); erase(data(copy), "AppleRawMaxCapacity");
  CHECK(coldInit("AppleSmartBatteryPack", copy).service == 5); CFRelease(copy);
  Dictionary view; CFRelease(view.cf); view.cf = props;
  number(&view, "ID", 1, 32); CHECK(coldInit("AppleSmartBatteryPack", props).maximum == -1);
  number(&view, "ID", 0, 32);
  erase(props, "FeatureFlags"); CHECK(coldInit("AppleSmartBatteryPack", props).maximum == 99);
  Dictionary nested; CFRelease(nested.cf); nested.cf = d;
  number(&nested, "FeatureFlags", 1024, 64); CHECK(coldInit("AppleSmartBatteryPack", props).maximum == 99);
  number(&view, "FeatureFlags", 1024, 64); erase(d, "FeatureFlags");
  number(&nested, "PermanentFailureStatus", 1, 64);
  CHECK(std::strcmp(condition(coldInit("AppleSmartBatteryPack", props)), "Service Recommended") == 0);
  CHECK(maximumShown(coldInit("AppleSmartBatteryPack", props)) == -1); erase(d, "PermanentFailureStatus");
  CFRelease(props); CFRelease(old.cf);

  // Saved [120s] transient: B design=0 -> mapGaugeBatteryFields withholds design.
  // No new dictionary replaces the last complete sample; recover on the next good report.
  v = m120c120(true, true); CHECK(v.designMah == 0 && !serialized(v));
  v = m120c120(true); v.cycles = kCycleCountUnknown; CHECK(!serialized(v));
  v = m120c120(true); v.designMah = 0xffff; CHECK(!serialized(v));
  v = m120c120(true); v.fullMah = 0xffff; CHECK(!serialized(v));
  v = m120c120(true); v.known = false; CHECK(!serialized(v));
  v = m120c120(true); v.cycles = 0;
  copy = serialized(v); CHECK(copy && maximumShown(coldInit("AppleSmartBatteryPack", copy)) == 100); CFRelease(copy);
  v.cycles = 20; copy = serialized(v); CHECK(maximumShown(coldInit("AppleSmartBatteryPack", copy)) == 100); CFRelease(copy);
  v.cycles = 21; copy = serialized(v); CHECK(maximumShown(coldInit("AppleSmartBatteryPack", copy)) == 96); CFRelease(copy);
  v = m120c120(true); v.fullMah = 7000;
  copy = serialized(v); CHECK(integer(data(copy), "NominalChargeCapacity") == 7000); CFRelease(copy);
  v = m120c120(true); v.amperageMa = -1231;
  copy = serialized(v); CHECK(integer(data(copy), "AverageCurrent") == -1231); CFRelease(copy);
  // Every numeric allocation failure withholds the complete publication.
  writes = 0; copy = serialized(v); CHECK(copy); CFRelease(copy); unsigned totalWrites = writes;
  for (unsigned i = 1; i <= totalWrites; ++i) { writes = 0; failWrite = i; CHECK(!serialized(v)); }
  failWrite = 0;
  std::puts("battery_health_host: publisher schema and conditional healthy replay passed (storage assumed; UI unqualified)");
}
