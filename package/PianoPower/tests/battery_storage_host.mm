// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independently written contract replay, not an implementation of Apple powerd.
// docs/drivers/power-bh2-powerd-analysis.md gives the call sites and limits.
// Real Foundation JSON, temporary-file backing, injected IOKit failures; no host NVRAM access.
#import <Foundation/Foundation.h>
#include <IOKit/IOReturn.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static constexpr IOReturn kJsonError = IOReturn(0xe0000001u);
static bool dictionary(id value) { return [value isKindOfClass:[NSDictionary class]]; }
static NSData *json(id value) {
  if (![NSJSONSerialization isValidJSONObject:value]) return nil;
  return [NSJSONSerialization dataWithJSONObject:value options:0 error:nullptr];
}
static NSString *packKey(uint32_t pack) { return [NSString stringWithFormat:@"%u", pack]; }

// This is an injected provider, not a model of all IODTNVRAM policy. The size
// limit is a test parameter; there is no newly inferred powerd JSON-size limit.
struct Provider {
  bool optionsPresent = true, controller = true;
  IOReturn propertyError = kIOReturnSuccess;
  IOReturn syncError = kIOReturnSuccess;
  size_t limit = 8192;
  unsigned writes = 0, syncs = 0;
  id property = nil;
  NSString *path;
  explicit Provider(NSString *file) : path(file) {}
  IOReturn set(NSData *bytes) {
    ++writes;
    if (propertyError) return propertyError;
    if (bytes.length > limit) return kIOReturnNoSpace;
    property = [bytes copy];
    return kIOReturnSuccess;
  }
  IOReturn sync() {
    ++syncs;
    if (syncError) return syncError;
    if (!controller) return kIOReturnNotReady;
    return [(NSData *)property writeToFile:path options:NSDataWritingAtomic error:nullptr]
               ? kIOReturnSuccess : kIOReturnIOError;
  }
  void reboot() { property = [NSData dataWithContentsOfFile:path]; }
};

struct Storage {
  Provider &provider;
  NSDictionary *cache = nil;
  explicit Storage(Provider &p) : provider(p) {}
  // 0x100001de4, 0x100002034: missing value is an empty cold start,
  // missing options is NotFound, wrong property/root types are BadArgument.
  IOReturn root(NSDictionary **out) {
    *out = nil;
    if (cache) { *out = [cache copy]; return kIOReturnSuccess; }
    if (!provider.optionsPresent) return kIOReturnNotFound;
    id value = provider.property;
    if (!value) return kIOReturnSuccess;
    if (![value isKindOfClass:[NSData class]]) return kIOReturnBadArgument;
    id decoded = [NSJSONSerialization JSONObjectWithData:value options:0 error:nullptr];
    if (!decoded) return kJsonError;
    if (!dictionary(decoded)) return kIOReturnBadArgument;
    cache = [decoded copy]; *out = [cache copy];
    return kIOReturnSuccess;
  }
  // 0x100001eb4..0x100001f4c: deliberately loses the property's raw error;
  // cache changes only after success; sync's return is deliberately ignored.
  IOReturn writeRoot(NSDictionary *value) {
    NSData *bytes = json(value);
    if (!bytes) return kJsonError;
    if (!provider.optionsPresent) return kIOReturnNotFound;
    if (provider.set(bytes)) return kIOReturnIOError;
    (void)provider.sync();
    cache = [value copy];
    return kIOReturnSuccess;
  }
  // 0x100002934: legacy flat dictionaries are accepted for migration.
  IOReturn read(uint32_t pack, NSDictionary **out) {
    if (!out) return kIOReturnBadArgument;
    *out = nil;
    NSDictionary *value = nil;
    IOReturn status = root(&value);
    if (status) return status;
    if (!value.count) return kIOReturnSuccess;
    id packs = value[@"BatteryPacks"];
    if (!packs) { *out = value; return kIOReturnSuccess; }
    if (!dictionary(packs)) return kJsonError;
    id record = packs[packKey(pack)];
    if (record && !dictionary(record)) return kJsonError;
    if ([record count]) *out = record;
    return kIOReturnSuccess;
  }
  // 0x100002b74: preserve other packs; replace this pack's whole dictionary.
  IOReturn write(uint32_t pack, NSDictionary *record) {
    if (!record) return kIOReturnBadArgument;
    NSDictionary *old = nil;
    IOReturn status = root(&old);
    if (status) return status;
    NSMutableDictionary *packs = dictionary(old[@"BatteryPacks"])
                                   ? [old[@"BatteryPacks"] mutableCopy] : [NSMutableDictionary dictionary];
    packs[packKey(pack)] = record;
    return writeRoot(@{@"BatteryPacks": packs});
  }
  // 0x10001638c: public tester API merges entries only into an existing record.
  IOReturn entries(uint32_t pack, id values) {
    NSDictionary *record = nil;
    IOReturn status = read(pack, &record);
    if (status) return status;
    if (!record) return kIOReturnNotReady;
    if (!dictionary(values)) return kIOReturnBadArgument;
    NSMutableDictionary *changed = [record mutableCopy]; [changed addEntriesFromDictionary:values];
    return write(pack, changed);
  }
  // 0x100015db4: validate ALL keys before removing any, then rewrite JSON.
  IOReturn erase(uint32_t pack, NSArray *keys) {
    NSDictionary *record = nil;
    IOReturn status = read(pack, &record);
    if (status) return status;
    if (!record) return kIOReturnNotReady;
    for (id key in keys) {
      if (![key isKindOfClass:[NSString class]]) return kIOReturnBadArgument;
      if (!record[key]) return kIOReturnNotFound;
    }
    NSMutableDictionary *changed = [record mutableCopy];
    for (NSString *key in keys) [changed removeObjectForKey:key];
    return write(pack, changed);
  }
};

static NSDictionary *healthy() {
  // Synthetic identity and timestamp; capacities/cycles from the saved fixture.
  return @{@"Battery Serial": @"test-logical-pack", @"Maximum Capacity Percent": @96,
           @"CycleCount": @122, @"Battery Service Flags": @4, @"Battery Service State": @0, @"ts": @1};
}

static void operations(NSString *file) {
  Provider p(file); Storage s(p); NSDictionary *value = nil;
  CHECK(s.read(0, &value) == 0 && !value);
  CHECK(s.read(0, nullptr) == kIOReturnBadArgument);
  CHECK(s.write(0, nil) == kIOReturnBadArgument);
  CHECK(s.entries(0, @{@"ts": @2}) == kIOReturnNotReady);
  CHECK(s.erase(0, @[@"ts"]) == kIOReturnNotReady);
  CHECK(s.write(0, healthy()) == 0);
  CHECK(s.read(0, &value) == 0 && [value isEqual:healthy()]);
  CHECK(s.entries(0, @{@"ts": @2}) == 0);
  CHECK(s.read(0, &value) == 0 && [value[@"ts"] intValue] == 2 && value[@"Battery Serial"]);
  CHECK(s.entries(0, @[@"bad-type"]) == kIOReturnBadArgument);
  const unsigned before = p.writes;
  CHECK(s.erase(0, @[@"ts", @"missing"]) == kIOReturnNotFound && p.writes == before);
  CHECK(s.erase(0, @[@1]) == kIOReturnBadArgument && p.writes == before);
  CHECK(s.read(0, &value) == 0 && value[@"ts"]);
  CHECK(s.erase(0, @[@"ts"]) == 0);
  CHECK(s.read(0, &value) == 0 && !value[@"ts"] && value[@"Battery Serial"]);
  CHECK(s.erase(0, @[]) == 0); // internal worker supports an empty list; XPC rejects it earlier
  CHECK(s.write(1, @{@"other": @7}) == 0);
  CHECK(s.write(UINT32_MAX, @{@"edge": @8}) == 0);
  CHECK(s.read(UINT32_MAX, &value) == 0 && [value[@"edge"] intValue] == 8);
  CHECK(s.read(1, &value) == 0 && [value[@"other"] intValue] == 7);
  CHECK(s.read(2, &value) == 0 && !value);
  // Replacement, rather than entries, must not leave stale per-pack keys.
  CHECK(s.write(0, @{@"only": @3}) == 0);
  CHECK(s.read(0, &value) == 0 && value.count == 1 && value[@"only"]);
  // A new daemon instance reads JSON bytes, and a new provider reads a real file.
  p.reboot(); Storage reopened(p);
  CHECK(reopened.read(1, &value) == 0 && [value[@"other"] intValue] == 7);
  Provider newProvider(file); newProvider.reboot(); Storage boot(newProvider);
  CHECK(boot.read(0, &value) == 0 && value.count == 1 && value[@"only"]);
  CHECK(boot.erase(0, @[@"only"]) == 0);
  CHECK(boot.read(0, &value) == 0 && !value);
}

static void errors(NSString *file) {
  NSDictionary *value = nil;
  for (IOReturn raw : {kIOReturnNotPermitted, kIOReturnNotPrivileged, kIOReturnNoSpace,
                       kIOReturnNotReady, kIOReturnUnsupported, kIOReturnNoMemory}) {
    Provider p(file); p.propertyError = raw; Storage s(p);
    CHECK(s.write(0, healthy()) == kIOReturnIOError);
    CHECK(!s.cache && !p.property && p.syncs == 0);
  }
  Provider p(file); p.optionsPresent = false; Storage absent(p);
  CHECK(absent.write(0, healthy()) == kIOReturnNotFound && p.writes == 0);
  CHECK(absent.read(0, &value) == kIOReturnNotFound);
  p.optionsPresent = true; p.property = @"wrong-type";
  CHECK(absent.read(0, &value) == kIOReturnBadArgument);
  p.property = [@"{" dataUsingEncoding:NSUTF8StringEncoding];
  CHECK(absent.read(0, &value) == kJsonError);
  p.property = json(@[]); CHECK(absent.read(0, &value) == kIOReturnBadArgument);
  for (NSDictionary *bad in @[@{@"BatteryPacks": @[]}, @{@"BatteryPacks": @{@"0": @1}}]) {
    p.property = json(bad); Storage invalid(p); CHECK(invalid.read(0, &value) == kJsonError);
  }
  p.property = json(healthy()); Storage legacy(p);
  CHECK(legacy.read(0, &value) == 0 && [value isEqual:healthy()]);
  CHECK(legacy.write(0, healthy()) == 0);
  // A failed write must not update the cached history or the previous bytes.
  NSData *old = p.property; p.propertyError = kIOReturnNoSpace;
  CHECK(legacy.entries(0, @{@"ts": @99}) == kIOReturnIOError);
  CHECK(legacy.read(0, &value) == 0 && [value[@"ts"] intValue] == 1 && [p.property isEqual:old]);
}

static void boundsAndDurability(NSString *directory) {
  NSString *file = [directory stringByAppendingPathComponent:@"bounds.json"];
  NSData *expected = json(@{@"BatteryPacks": @{@"0": healthy()}});
  for (int delta : {-1, 0, 1}) {
    Provider p(file); p.limit = expected.length + delta; Storage s(p);
    CHECK(s.write(0, healthy()) == (delta < 0 ? kIOReturnIOError : kIOReturnSuccess));
    CHECK(p.syncs == (delta < 0 ? 0u : 1u));
  }
  // No controller: powerd STILL reports success. Same-boot reads work, but
  // nothing survives reboot. This cannot explain a failing property write.
  Provider volatileProvider([directory stringByAppendingPathComponent:@"absent.json"]);
  volatileProvider.controller = false; Storage volatileStore(volatileProvider);
  CHECK(volatileStore.write(0, healthy()) == 0);
  NSDictionary *value = nil;
  Storage sameBoot(volatileProvider); CHECK(sameBoot.read(0, &value) == 0 && value);
  volatileProvider.reboot(); Storage nextBoot(volatileProvider);
  CHECK(nextBoot.read(0, &value) == 0 && !value);
  Provider syncFailure([directory stringByAppendingPathComponent:@"sync-error.json"]);
  syncFailure.syncError = kIOReturnIOError; Storage syncStore(syncFailure);
  CHECK(syncStore.write(0, healthy()) == 0 && syncFailure.syncs == 1);
  syncFailure.reboot(); Storage failedBoot(syncFailure);
  CHECK(failedBoot.read(0, &value) == 0 && !value);
  // File write failure also cannot change nvramRawDataWrite's ignored sync return.
  Provider fileFailure([directory stringByAppendingPathComponent:@"missing/health.json"]);
  Storage fileStore(fileFailure); CHECK(fileStore.write(0, healthy()) == 0);
  CHECK(![[NSFileManager defaultManager] fileExistsAtPath:fileFailure.path]);
  // The OTHER backend can return the identical code on synchronize=false.
  const auto prefsStatus = [](bool synchronized) { return synchronized ? kIOReturnSuccess : kIOReturnIOError; };
  CHECK(prefsStatus(false) == IOReturn(0xe00002cau) && prefsStatus(true) == 0);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    CHECK(argc == 2); // wrapper supplies a fresh directory; never a system path
    NSString *directory = [NSString stringWithUTF8String:argv[1]];
    operations([directory stringByAppendingPathComponent:@"health.json"]);
    errors([directory stringByAppendingPathComponent:@"errors.json"]);
    boundsAndDurability(directory);
    std::puts("battery_storage_host: JSON read/write/merge/delete/bounds/reopen/error contracts passed; backend and raw failure remain unqualified");
  }
}
