// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-brightness-sync: CoreBrightness and PianoBacklight's automatic mode agree on the panel's level, so the Control Center /
// menu-bar brightness slider shows the real brightness and the user's moves trim the automatic curve (2026-11-21, SLIDER-INIT.md;
// docs/drivers/backlight.md).
//
// Why: corebrightnessd takes its level from its BrightnessLevelNits preference at start (-[CBDisplayModuleSKL initialiseSDR]) and
// changes it only by its own commits; nothing on macOS 27.3 reads a built-in panel's level back. WindowServer's swap brightness,
// which PianoBacklight sees, is that level times the EDR headroom, so CoreBrightness's restore and EDR ramps looked like slider moves
// there and ended automatic mode (slider2; the installed boot of 14:06).
//
// What: the daemon holds PianoBacklight's lease (root's "piano-backlight-managed", renewed while corebrightnessd answers): the stream
// then leaves the curve alone. It watches CoreBrightness's own level (DisplayServicesClient DisplayNitsKey: EDR leaves it alone)
//   - a change it did not cause while automatic mode owns the panel is the user's move: "piano-backlight-moved" (16.16) adjusts
//     the curve's trim;
//   - once the automatic level has settled 1 s and CoreBrightness is more than 3 % away, it gives CoreBrightness the panel's level:
//     announced first ("piano-backlight-sync", 16.16), then set as DisplayNitsKey (getUserBrightnessForNits, updateUserBrightness:
//     commit:YES: the slider is notified and the preference stored, so the next restore lands on the panel's level), then read back.
// CoreBrightness's restore at its start is the baseline, never a move. Woken by PianoBacklight's property-change messages (it publishes
// each new request, "piano-backlight-native-requested"), plus one check every 30 s; no polling loop.
//
//   piano-brightness-sync --run     the LaunchDaemon (root); exits 0 when PianoBacklight is not native with automatic brightness
//   piano-brightness-sync --check   read only: the panel's level and flags, CoreBrightness's level, the decision
// Status: /private/var/db/piano-userland/brightness-sync/status (syncs, taken, moves, failed, the last values).
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <math.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../../package/include/PianoBootArgs.h"
#include "SyncPolicy.h"

@protocol PianoDisplayServicesClient
- (id)copyPropertyForKey:(NSString *)key;
- (BOOL)setProperty:(id)value withKey:(NSString *)key;
@end

enum { kLeaseS = 120, kRenewMs = 40000, kKextWaitMs = 300000, kAutoWaitMs = 15000 };

static os_log_t gLog;
static io_service_t gBacklight;
static io_object_t gInterest;
static IONotificationPortRef gPort;
static CFRunLoopTimerRef gTimer;
static id<PianoDisplayServicesClient> gClient;
static struct pbs_state gState;
static unsigned gSyncs, gTaken, gFailed, gMoves;
static uint32_t gLastSynced;
static uint64_t gStartMs, gMatchedMs, gLeaseMs;
static int gExitStatus;
static bool gLeaseHeld;
static NSString *const kStatusDir = @"/private/var/db/piano-userland/brightness-sync";

static uint64_t nowMs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

static int bootArgEnabled(const char *args, const char *name)
{
  size_t length;
  const char *value = pianoBootArgValue(args, name, &length);
  return value && length == 1 && value[0] == '1';
}

static int hasType(io_service_t s, CFStringRef key, CFTypeID type)
{
  CFTypeRef v = s ? IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0) : NULL;
  const int valid = v && CFGetTypeID(v) == type;
  if (v) CFRelease(v);
  return valid;
}

// The kext's own properties report readiness: native when it publishes its native implementation, automatic-capable when it publishes
// piano-backlight-auto-requested (from its first publication, shortly after it registers). The boot arguments still count.
static int kextReady(io_service_t s)
{
  if (hasType(s, CFSTR("piano-backlight-native-implementation"), CFStringGetTypeID()) &&
      hasType(s, CFSTR("piano-backlight-auto-requested"), CFBooleanGetTypeID())) return 1;
  char args[PIANO_BOOT_ARGS_MAX];
  pianoBootArgs(args);
  return bootArgEnabled(args, "piano-backlight-native") && bootArgEnabled(args, "piano-backlight-auto");
}

static id<PianoDisplayServicesClient> client(void)
{
  if (gClient) return gClient;
  NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/PrivateFrameworks/CoreBrightness.framework"];
  Class cls = [bundle load] ? NSClassFromString(@"DisplayServicesClient") : Nil;
  id candidate = cls ? [[cls alloc] init] : nil;
  if ([candidate respondsToSelector:@selector(copyPropertyForKey:)] && [candidate respondsToSelector:@selector(setProperty:withKey:)])
    gClient = candidate;
  return gClient;
}

// CoreBrightness's level in millinits, -1 when it does not answer (the client is made again next time).
static bool numberValue(id value, double *number)
{
  if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) != CFNumberGetTypeID()) return false;
  *number = [value doubleValue];
  return true;
}

static int64_t coreMilliNits(id value)
{
  double nits;
  if (!numberValue(value, &nits) || !(nits >= 0 && nits <= PBS_MAX_MILLINITS / 1000.0)) return -1;
  return llround(nits * 1000.0);
}

static int64_t coreBrightness(void)
{
  const int64_t result = coreMilliNits([client() copyPropertyForKey:@"DisplayNitsKey"]);
  if (result < 0) gClient = nil;
  return result;
}

static uint32_t positiveMilliNits(id value)
{
  double level;
  if (!numberValue(value, &level) || !(level >= 1 && level <= PBS_MAX_MILLINITS)) return 0;
  const uint32_t milli = (uint32_t)level;
  return level == milli ? milli : 0;
}

static uint32_t panelValue(id params)
{
  NSDictionary *nits = [params isKindOfClass:NSDictionary.class] ? params[@"BrightnessMilliNits"] : nil;
  if (![nits isKindOfClass:NSDictionary.class]) return 0;
  const uint32_t low = positiveMilliNits(nits[@"min"]), high = positiveMilliNits(nits[@"max"]);
  const uint32_t level = positiveMilliNits(nits[@"value"]);
  return low && level >= low && level <= high ? level : 0;
}

static uint32_t panelMilliNits(io_service_t s)
{
  return panelValue(CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("IODisplayParameters"), kCFAllocatorDefault, 0)));
}

// -1 is a missing or malformed reply.
static int flag(io_service_t s, CFStringRef key)
{
  CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
  const int on = v && CFGetTypeID(v) == CFBooleanGetTypeID() ? CFBooleanGetValue((CFBooleanRef)v) : -1;
  if (v) CFRelease(v);
  return on;
}

static kern_return_t tell(NSString *key, int64_t value)
{
  NSDictionary *d = @{key: @(value)};
  return IORegistryEntrySetCFProperties(gBacklight, (__bridge CFDictionaryRef)d);
}

static void writeStatus(uint32_t panel, int64_t cb, const char *result)
{
  mkdir("/private/var/db/piano-userland", 0755);
  mkdir(kStatusDir.fileSystemRepresentation, 0755);
  NSString *text = [NSString stringWithFormat:@"syncs=%u taken=%u moves=%u failed=%u last_panel_millinits=%u last_corebrightness_millinits=%lld "
                                               @"last_result=%s policy=sync2\n", gSyncs, gTaken, gMoves, gFailed, panel, cb, result];
  [text writeToFile:[kStatusDir stringByAppendingPathComponent:@"status"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

static void arm(double seconds)
{
  const CFAbsoluteTime want = CFAbsoluteTimeGetCurrent() + seconds;
  if (gTimer && CFRunLoopTimerGetNextFireDate(gTimer) > want) CFRunLoopTimerSetNextFireDate(gTimer, want);
}

static void stopDaemon(int status)
{
  gExitStatus = status;
  CFRunLoopStop(CFRunLoopGetMain());
}

static void evaluate(void)
{
  if (!gBacklight) {
    if (nowMs() - gStartMs > kKextWaitMs) {
      os_log(gLog, "inert: no PianoBacklight within %u s", kKextWaitMs / 1000);
      stopDaemon(0);
    }
    return;
  }
  if (!kextReady(gBacklight)) {   // its first publication comes shortly after it registers
    if (nowMs() - gMatchedMs > kAutoWaitMs) {
      os_log(gLog, "inert: PianoBacklight is not native with automatic brightness");
      stopDaemon(0);
      return;
    }
    arm(1.0);
    return;
  }
  const uint32_t panel = panelMilliNits(gBacklight);
  const int owned = flag(gBacklight, CFSTR("piano-backlight-auto-requested"));
  const int active = flag(gBacklight, CFSTR("piano-backlight-auto-active")), lit = flag(gBacklight, CFSTR("piano-backlight-lit"));
  const int64_t cb = coreBrightness();
  if (!panel || owned < 0 || active < 0 || lit < 0) {
    pbs_decide(&gState, nowMs(), 0, 0, 0, 0, cb);
    return;
  }
  if (cb >= 0 && (!gLeaseMs || nowMs() - gLeaseMs >= kRenewMs)) {   // the lease only while CoreBrightness can be watched
    const kern_return_t r = tell(@"piano-backlight-managed", kLeaseS);
    if (!gLeaseMs || r != KERN_SUCCESS) os_log(gLog, "lease %u s: %#x", kLeaseS, r);
    gLeaseMs = r == KERN_SUCCESS ? nowMs() : 0;
    if (r == KERN_SUCCESS) gLeaseHeld = true;
  }
  if (gLastSynced && cb >= 0 && pbs_matches((uint32_t)cb, gLastSynced)) {   // the last sync was taken
    ++gTaken;
    if (gTaken <= 8 || gTaken % 25 == 0) os_log(gLog, "CoreBrightness took %.3f nits (%u syncs, %u taken)", gLastSynced / 1000.0, gSyncs, gTaken);
    writeStatus(panel, cb, "taken");
    gLastSynced = 0;
  }
  const enum pbs_action action = pbs_decide(&gState, nowMs(), panel, owned, active, lit, cb);
  if (action == PBS_MOVED) {
    ++gMoves;
    const kern_return_t told = tell(@"piano-backlight-moved", pbs_fixed((uint32_t)cb));
    os_log(gLog, "move %u: the user set CoreBrightness to %.3f nits while automatic mode owned the panel (%.3f): reported %#x", gMoves,
           cb / 1000.0, panel / 1000.0, told);
    if (told != KERN_SUCCESS) ++gFailed;
    writeStatus(panel, cb, told == KERN_SUCCESS ? "moved" : "move-refused");
    arm(1.0);
  } else if (action == PBS_SYNC) {
    ++gSyncs;
    const kern_return_t told = tell(@"piano-backlight-sync", pbs_fixed(panel));
    const BOOL set = told == KERN_SUCCESS && [client() setProperty:@((float)(panel / 1000.0)) withKey:@"DisplayNitsKey"];
    const int64_t after = set ? coreBrightness() : -1;
    pbs_synced(&gState, set, after);
    if (set) gLastSynced = panel; else ++gFailed;
    if (gSyncs <= 8 || gSyncs % 25 == 0 || !set)
      os_log(gLog, "sync %u: CoreBrightness %.3f -> the panel's %.3f nits (automatic): announce %#x, set %d, now %.3f", gSyncs, cb / 1000.0,
             panel / 1000.0, told, set, after / 1000.0);
    writeStatus(panel, after >= 0 ? after : cb, set ? "set" : told != KERN_SUCCESS ? "announce-refused" : "set-refused");
    arm(1.5);
  } else if (action == PBS_WAIT) {
    arm(1.0);
  }
}

static void interest(void *refcon, io_service_t service, natural_t type, void *argument)
{
  (void)refcon;
  (void)argument;
  if (service != gBacklight) return;
  if (type == kIOMessageServicePropertyChange) arm(0.1);   // a new request (maybe the user's) or a new automatic level
  else if (type == kIOMessageServiceIsTerminated) {
    gLeaseHeld = false;
    stopDaemon(69);
  }
}

static void matched(void *refcon, io_iterator_t it)
{
  (void)refcon;
  for (io_service_t s; (s = IOIteratorNext(it));) {
    if (gBacklight) {
      IOObjectRelease(s);
      continue;
    }
    gBacklight = s;
    gMatchedMs = nowMs();
    @autoreleasepool {
      const kern_return_t r = IOServiceAddInterestNotification(gPort, s, kIOGeneralInterest, interest, NULL, &gInterest);
      os_log(gLog, "PianoBacklight found (interest %#x): CoreBrightness and automatic mode are kept in agreement", r);
      writeStatus(panelMilliNits(s), -1, "started");
    }
    arm(1.0);
  }
}

static int check(void)
{
  CFMutableDictionaryRef matching = IOServiceMatching("PianoBacklight");
  io_service_t s = matching ? IOServiceGetMatchingService(kIOMainPortDefault, matching) : IO_OBJECT_NULL;
  if (!s) {
    puts("backlight=missing");
    return 69;
  }
  const uint32_t panel = panelMilliNits(s);
  const int owned = flag(s, CFSTR("piano-backlight-auto-requested"));
  const int active = flag(s, CFSTR("piano-backlight-auto-active")), lit = flag(s, CFSTR("piano-backlight-lit"));
  const int64_t cb = coreBrightness();
  printf("boot_args=%s\npanel_millinits=%u\nautomatic=%s\nrequested=%s\nmanaged=%s\nlit=%s\ncorebrightness_millinits=%lld\nin_step=%s\n",
         kextReady(s) ? "yes" : "no", panel, active > 0 ? "yes" : "no", owned > 0 ? "yes" : "no",
         flag(s, CFSTR("piano-backlight-managed")) > 0 ? "yes" : "no", lit > 0 ? "yes" : "no", cb,
         cb >= 0 && panel && pbs_matches((uint32_t)cb, panel) ? "yes" : "no");
  struct pbs_state probe;
  pbs_init(&probe);
  probe.level = panel;
  const enum pbs_action a = pbs_decide(&probe, PBS_SETTLE_MS, panel, owned > 0, active > 0, lit > 0, cb);
  printf("decision=%s\n", a == PBS_SYNC ? "sync" : a == PBS_WAIT ? "wait" : a == PBS_MOVED ? "moved" : "idle");
  IOObjectRelease(s);
  return 0;
}

static dispatch_source_t watchSignal(int sig, struct sigaction *previous)
{
  const struct sigaction ignore = {.sa_handler = SIG_IGN};
  if (sigaction(sig, &ignore, previous) != 0) return NULL;
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, dispatch_get_main_queue());
  if (!source) {
    sigaction(sig, previous, NULL);
    return NULL;
  }
  dispatch_source_set_event_handler(source, ^{ stopDaemon(0); });
  dispatch_activate(source);
  return source;
}

static void unwatchSignal(dispatch_source_t source, int sig, const struct sigaction *previous)
{
  if (!source) return;
  dispatch_source_cancel(source);
  sigaction(sig, previous, NULL);
}

static void releaseLease(void)
{
  if (!gLeaseHeld) return;
  const kern_return_t r = tell(@"piano-backlight-managed", 0);
  if (r != KERN_SUCCESS) os_log_error(gLog, "lease release failed: %#x", r);
  gLeaseHeld = false;
}

static int runDaemon(void)
{
  pbs_init(&gState);
  gStartMs = nowMs();
  gExitStatus = 69;
  gPort = IONotificationPortCreate(kIOMainPortDefault);
  CFRunLoopSourceRef notifications = gPort ? IONotificationPortGetRunLoopSource(gPort) : NULL;
  CFMutableDictionaryRef matching = notifications ? IOServiceMatching("PianoBacklight") : NULL;
  io_iterator_t it = IO_OBJECT_NULL;
  if (matching && IOServiceAddMatchingNotification(gPort, kIOFirstMatchNotification, matching, matched, NULL, &it) == KERN_SUCCESS) {
    gTimer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 30, 30, 0, 0,
                                            ^(CFRunLoopTimerRef timer) {
                                              @autoreleasepool {
                                                CFRunLoopTimerSetNextFireDate(timer, CFAbsoluteTimeGetCurrent() + 30);
                                                evaluate();
                                              }
                                            });
    struct sigaction previousTerm, previousInt;
    dispatch_source_t term = gTimer ? watchSignal(SIGTERM, &previousTerm) : NULL;
    dispatch_source_t interrupt = term ? watchSignal(SIGINT, &previousInt) : NULL;
    if (interrupt) {
      CFRunLoopAddSource(CFRunLoopGetCurrent(), notifications, kCFRunLoopDefaultMode);
      CFRunLoopAddTimer(CFRunLoopGetCurrent(), gTimer, kCFRunLoopDefaultMode);
      matched(NULL, it);   // arms the matching notification and takes an already published PianoBacklight
      gExitStatus = 0;
      CFRunLoopRun();
    } else {
      os_log_error(gLog, "could not set up the daemon's timer or signal handlers");
    }
    releaseLease();
    unwatchSignal(interrupt, SIGINT, &previousInt);
    unwatchSignal(term, SIGTERM, &previousTerm);
  } else {
    os_log_error(gLog, "could not register for PianoBacklight notifications");
  }
  if (gTimer) {
    CFRunLoopTimerInvalidate(gTimer);
    CFRelease(gTimer);
    gTimer = NULL;
  }
  if (gInterest) IOObjectRelease(gInterest);
  if (gBacklight) IOObjectRelease(gBacklight);
  if (it) IOObjectRelease(it);
  if (gPort) IONotificationPortDestroy(gPort);
  gInterest = gBacklight = IO_OBJECT_NULL;
  gPort = NULL;
  gClient = nil;
  return gExitStatus;
}

int main(int argc, const char **argv)
{
  @autoreleasepool {
    gLog = os_log_create("com.unixnu.piano", "brightness-sync");
    if (argc == 2 && !strcmp(argv[1], "--check")) return check();
    if (argc != 2 || strcmp(argv[1], "--run")) {
      fprintf(stderr, "usage: piano-brightness-sync --run | --check\n");
      return 64;
    }
    if (geteuid()) {
      fprintf(stderr, "piano-brightness-sync --run: root only (piano-backlight-managed, -moved, -sync)\n");
      return 77;
    }
    return runDaemon();
  }
}
