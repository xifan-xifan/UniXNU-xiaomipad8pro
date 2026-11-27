// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int stubSysctl(const char *, void *, size_t *, void *, size_t);
static uid_t stubUid(void);
static CFTypeRef stubProperty(io_registry_entry_t, CFStringRef, CFAllocatorRef, IOOptionBits);
static kern_return_t stubTell(io_registry_entry_t, CFTypeRef);
#define sysctlbyname stubSysctl
#define geteuid stubUid
#define IORegistryEntryCreateCFProperty stubProperty
#define IORegistryEntrySetCFProperties stubTell
#define main brightnessSyncMain
#include "../piano-brightness-sync.m"
#undef main

static enum { full, legacy, denied, truncated, unterminated, embeddedNul, empty, oversized } bootReply;
static unsigned sysctlCalls, leaseWrites;
static int leaseValue;
static id requestedFlag;
static id panelParams;

static uid_t stubUid(void) { return 501; }

static int stubSysctl(const char *name, void *old, size_t *size, void *new, size_t newSize)
{
  assert(new == NULL && newSize == 0 && *size == PIANO_BOOT_ARGS_MAX);
  const bool fallback = sysctlCalls++ == 1;
  assert(!strcmp(name, fallback ? "kern.bootargs" : "kern.piano_bootargs"));
  static const char flags[] = "piano-backlight-native=1\tpiano-backlight-auto=1";
  if (!fallback && bootReply == legacy) {
    errno = ENOENT;
    return -1;
  }
  if (bootReply == denied || bootReply == truncated) {
    memcpy(old, flags, sizeof flags);
    errno = bootReply == denied ? EPERM : ENOMEM;
    return -1;
  }
  if (bootReply == empty || bootReply == oversized) {
    *size = bootReply == empty ? 0 : PIANO_BOOT_ARGS_MAX + 1;
    return 0;
  }
  if (bootReply == full) {
    memset(old, ' ', *size);
    memcpy((char *)old + *size - sizeof flags, flags, sizeof flags);
    return 0;
  }
  memcpy(old, flags, sizeof flags);
  *size = sizeof flags;
  if (bootReply == unterminated) ((char *)old)[*size - 1] = ' ';
  if (bootReply == embeddedNul) {
    ((char *)old)[*size] = '\0';
    ++*size;
  }
  return 0;
}

static CFTypeRef stubProperty(io_registry_entry_t service, CFStringRef key, CFAllocatorRef alloc, IOOptionBits options)
{
  assert(service == 1 && alloc == kCFAllocatorDefault && options == 0);
  if (CFEqual(key, CFSTR("IODisplayParameters"))) return CFBridgingRetain(panelParams);
  if (CFEqual(key, CFSTR("piano-backlight-native-implementation"))) return CFRetain(CFSTR("PianoBacklightNativeV1"));
  if (CFEqual(key, CFSTR("piano-backlight-auto-requested"))) return CFBridgingRetain(requestedFlag);
  return CFRetain(kCFBooleanTrue);
}

static kern_return_t stubTell(io_registry_entry_t service, CFTypeRef properties)
{
  assert(service == 1);
  NSDictionary *values = (__bridge NSDictionary *)properties;
  assert(values.count == 1 && values[@"piano-backlight-managed"]);
  ++leaseWrites;
  leaseValue = [values[@"piano-backlight-managed"] intValue];
  return KERN_SUCCESS;
}

@interface TestClient : NSObject <PianoDisplayServicesClient>
@property id reply;
@end

@implementation TestClient
- (id)copyPropertyForKey:(NSString *)key
{
  assert([key isEqualToString:@"DisplayNitsKey"]);
  return self.reply;
}
- (BOOL)setProperty:(id)value withKey:(NSString *)key
{
  (void)value;
  (void)key;
  assert(false);
  return NO;
}
@end

static void testBootArgs(void)
{
  for (bootReply = full; bootReply <= oversized; ++bootReply) {
    sysctlCalls = 0;
    assert(kextReady(IO_OBJECT_NULL) == (bootReply <= legacy));
    assert(sysctlCalls == (bootReply == legacy ? 2u : 1u));
  }
  assert(bootArgEnabled("piano-backlight-auto=0\tpiano-backlight-auto=1 ", "piano-backlight-auto"));
  assert(!bootArgEnabled("piano-backlight-auto=1 piano-backlight-auto=0", "piano-backlight-auto"));
  assert(!bootArgEnabled("piano-backlight-auto-extra=1 piano-backlight-auto=", "piano-backlight-auto"));
}

static void testInputs(void)
{
  assert(coreMilliNits(@(26.1595)) == 26160 && coreMilliNits(@(-0.0)) == 0);
  assert(coreMilliNits(@(PBS_MAX_MILLINITS / 1000.0)) == PBS_MAX_MILLINITS);
  assert(pbs_fixed(PBS_MAX_MILLINITS) <= INT32_MAX && pbs_fixed(PBS_MAX_MILLINITS + 1) > INT32_MAX);
  for (id value in @[@YES, @"12", [NSNull null], @(-0.1), @(NAN), @(INFINITY), @(DBL_MAX), @(PBS_MAX_MILLINITS + 1)]) {
    assert(coreMilliNits(value) == -1);
    assert(positiveMilliNits(value) == 0);
  }
  assert(coreMilliNits(@32768.0) == -1);
  assert(coreMilliNits(nil) == -1 && panelValue(nil) == 0 && panelValue(@[]) == 0);
  assert(panelValue(@{@"BrightnessMilliNits": @7000}) == 0);
  assert(panelValue(@{@"BrightnessMilliNits": @{@"min": @2000, @"max": @600000, @"value": @7000.5}}) == 0);
  assert(panelValue(@{@"BrightnessMilliNits": @{@"min": @2000, @"max": @600000, @"value": @700000}}) == 0);

  TestClient *client = [TestClient new];
  client.reply = @26.16;
  gClient = client;
  gBacklight = 1;
  pbs_init(&gState);
  requestedFlag = @YES;
  panelParams = @{@"BrightnessMilliNits": @{@"min": @2000, @"max": @600000, @"value": @(-1)}};
  evaluate();
  assert(leaseWrites == 0 && gState.level == 0);
  panelParams = @{@"BrightnessMilliNits": @{@"min": @2000, @"max": @600000, @"value": @26160}};
  assert(panelMilliNits(gBacklight) == 26160);
  requestedFlag = @1;
  sysctlCalls = 0;
  bootReply = full;
  evaluate();
  assert(leaseWrites == 0 && gState.level == 0 && sysctlCalls == 1);
  requestedFlag = @YES;
  evaluate();
  assert(leaseWrites == 1 && leaseValue == kLeaseS && gLeaseHeld);
}

static void testSignal(int sig)
{
  struct sigaction previous, after;
  dispatch_source_t signalSource = watchSignal(sig, &previous);
  assert(signalSource);
  const double now = CFAbsoluteTimeGetCurrent();
  CFRunLoopTimerRef send = CFRunLoopTimerCreateWithHandler(NULL, now + 0.05, 0, 0, 0, ^(CFRunLoopTimerRef timer) {
    (void)timer;
    assert(kill(getpid(), sig) == 0);
  });
  CFRunLoopTimerRef deadline = CFRunLoopTimerCreateWithHandler(NULL, now + 2, 0, 0, 0, ^(CFRunLoopTimerRef timer) {
    (void)timer;
    assert(false && "signal did not stop the run loop");
  });
  CFRunLoopAddTimer(CFRunLoopGetMain(), send, kCFRunLoopDefaultMode);
  CFRunLoopAddTimer(CFRunLoopGetMain(), deadline, kCFRunLoopDefaultMode);
  gExitStatus = 69;
  CFRunLoopRun();
  assert(gExitStatus == 0);
  releaseLease();
  assert(!gLeaseHeld && leaseValue == 0);
  unwatchSignal(signalSource, sig, &previous);
  assert(sigaction(sig, NULL, &after) == 0 && after.sa_handler == previous.sa_handler);
  CFRunLoopTimerInvalidate(send);
  CFRunLoopTimerInvalidate(deadline);
  CFRelease(send);
  CFRelease(deadline);
}

int main(void)
{
  @autoreleasepool {
    const char *args[] = {"piano-brightness-sync", "--run"};
    assert(brightnessSyncMain(2, args) == 77 && !gPort && leaseWrites == 0);
    testBootArgs();
    testInputs();
    testSignal(SIGTERM);
    assert(leaseWrites == 2);
    testSignal(SIGINT);
    assert(leaseWrites == 2);   // no lease to revoke
    puts("sync daemon: boot arguments, IOKit and CoreBrightness validation, privileges and signal cleanup passed");
  }
}
