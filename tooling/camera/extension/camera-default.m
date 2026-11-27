// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Initialize the two stock camera apps through their own AVFoundation histories.
// No AVCaptureSession, device input, or camera acquisition is created here.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#import "Devices.h"

typedef id (^PianoHistoryReader)(NSString *key);
typedef BOOL (^PianoCameraSetter)(id camera, NSString *domain);

static NSArray<NSString *> *targetDomains(void)
{
  return @[@"com.apple.PhotoBooth", @"com.apple.QuickTimePlayerX"];
}

static NSString *historyKey(NSString *domain)
{
  return [@"camera-history/preferred-camera-history/" stringByAppendingString:
      [domain stringByReplacingOccurrencesOfString:@"." withString:@"-"]];
}

static BOOL methodMatches(Class type, SEL selector, const char *result, unsigned objects)
{
  Method method = class_getClassMethod(type, selector);
  if (!method || method_getNumberOfArguments(method) != objects + 2) return NO;
  char *value = method_copyReturnType(method);
  BOOL valid = value && !strcmp(value, result); free(value);
  for (unsigned index = 2; valid && index < objects + 2; ++index) {
    value = method_copyArgumentType(method, index);
    valid = value && !strcmp(value, "@"); free(value);
  }
  return valid;
}

// 27.3 stores at most three camera-info dictionaries, compared by CreatorID+DeviceUID.
// Unknown layouts are never interpreted as absence or permission to replace a choice.
static BOOL validHistory(id value)
{
  if (![value isKindOfClass:NSArray.class] || [value count] > 3) return NO;
  for (id entry in value) {
    if (![entry isKindOfClass:NSDictionary.class] ||
        ![entry[@"CreatorID"] isKindOfClass:NSString.class] ||
        ![entry[@"DeviceUID"] isKindOfClass:NSString.class] ||
        ![entry[@"OverrideRanking"] isKindOfClass:NSNumber.class]) return NO;
  }
  return YES;
}

static BOOL historyIsFront(id value)
{
  return validHistory(value) && [value count] &&
      [value[0][@"DeviceUID"] caseInsensitiveCompare:PIANO_FRONT_ID] == NSOrderedSame;
}

static void report(NSString *domain, const char *action)
{
  printf("CMIO_CAMERA_DEFAULT target=%s action=%s\n", domain.UTF8String, action);
  fflush(stdout);
}

// --set-front is a deliberate selection. Automatic login policy only changes
// a recognized empty/synthetic history and preserves every real-camera choice.
static BOOL applyTargets(id front, BOOL explicitSelection, PianoHistoryReader read, PianoCameraSetter set)
{
  if (!front) { puts("CMIO_CAMERA_DEFAULT_NO_FRONT_CAMERA"); return NO; }
  BOOL complete = YES;
  for (NSString *domain in targetDomains()) {
    NSString *key = historyKey(domain);
    id before = read(key);
    if (historyIsFront(before)) { report(domain, "front-ready"); continue; }
    if (!explicitSelection) {
      if (!validHistory(before)) {
        // Apple's nil reader conflates missing preferences with an unavailable
        // backend. Defer instead of silently overwriting an unreadable choice.
        report(domain, before ? "deferred-unknown-layout" : "deferred-unavailable-or-absent");
        complete = NO; continue;
      }
      if ([before count] && [before[0][@"DeviceUID"] caseInsensitiveCompare:PIANO_PATTERN_ID] != NSOrderedSame) {
        report(domain, "retained-real-choice"); continue;
      }
    }
    if (!set(front, domain)) { report(domain, "setter-failed"); complete = NO; continue; }
    if (!historyIsFront(read(key))) { report(domain, "readback-failed"); complete = NO; continue; }
    report(domain, "front-selected");
  }
  return complete;
}

// QuickTime Player keeps its recording window's own camera choice beside the AVFoundation history (APPS3-SELECT.md section 8-9);
// that choice is the camera it opens. Photo Booth keeps none.
typedef id (^PianoAppReader)(NSString *domain, NSString *key);

static NSString *appChoiceKey(NSString *domain)
{
  return [domain isEqual:@"com.apple.QuickTimePlayerX"] ?
      @"MGDeviceRecordingDocumentViewControllerVideoDeviceSelectionUniqueID" : nil;
}

// What each app opens: its own saved choice first (QuickTime), else the front history entry. No recorded choice at all means the
// app takes the discovery order, which lists the front first (the provider adds Front, then Rear; with the pattern camera off):
// ready, reported as such. Any recorded choice of another camera, or an unreadable history layout, is not ready.
static BOOL checkTargets(PianoHistoryReader read, PianoAppReader app)
{
  BOOL ready = YES;
  for (NSString *domain in targetDomains()) {
    id history = read(historyKey(domain));
    NSString *key = appChoiceKey(domain);
    id choice = key && app ? app(domain, key) : nil;
    const char *action;
    BOOL front = NO;
    if (choice && ![choice isKindOfClass:NSString.class]) {
      action = "unknown-app-choice";
    } else if ([choice length]) {
      front = [choice caseInsensitiveCompare:PIANO_FRONT_ID] == NSOrderedSame;
      action = front ? "front-ready-app-choice" : "other-camera-app-choice";
    } else if (historyIsFront(history)) {
      front = YES; action = "front-ready";
    } else if (!history || (validHistory(history) && ![history count])) {
      front = YES; action = "front-ready-default-order";
    } else {
      action = validHistory(history) ? "front-not-selected" : "unknown-layout";
    }
    report(domain, action);
    ready &= front;
  }
  return ready;
}

static BOOL sessionAllowed(BOOL automatic, uid_t user, uid_t effectiveUser, uid_t consoleUser, BOOL aqua, BOOL gui)
{
  if (!user || user != effectiveUser) return NO;
  return automatic ? aqua : user == consoleUser && gui;
}

#ifndef PIANO_CAMERA_DEFAULT_HOST_TEST
static BOOL launchctlCommand(NSArray<NSString *> *arguments, NSString **output)
{
  NSTask *task = [NSTask new]; NSPipe *pipe = output ? [NSPipe pipe] : nil;
  task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
  task.arguments = arguments; task.standardOutput = output ? (id)pipe : NSFileHandle.fileHandleWithNullDevice;
  task.standardError = NSFileHandle.fileHandleWithNullDevice;
  if (![task launchAndReturnError:NULL]) return NO;
  NSData *data = output ? [pipe.fileHandleForReading readDataToEndOfFile] : nil; [task waitUntilExit];
  if (output) *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  return !task.terminationStatus;
}

static BOOL commandSession(BOOL automatic)
{
  uid_t user = getuid();
  if (!user || user != geteuid()) return NO;
  if (automatic) {
    NSString *manager = nil;
    BOOL aqua = launchctlCommand(@[@"managername"], &manager) &&
        [[manager stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqual:@"Aqua"];
    return sessionAllowed(YES, user, geteuid(), 0, aqua, NO);
  }
  // Explicit selection / inspection may come from the logged-in user's SSH
  // session. Prove that user owns the console and has a GUI bootstrap domain;
  // no AppleEvents, Accessibility permission, or GUI launch is needed.
  struct stat console;
  if (stat("/dev/console", &console) || console.st_uid != user) return NO;
  BOOL gui = launchctlCommand(@[@"print", [NSString stringWithFormat:@"gui/%u", user]], NULL);
  return sessionAllowed(NO, user, geteuid(), console.st_uid, NO, gui);
}

static AVCaptureDevice *discoverFront(void)
{
  // A registrar/provider can appear after the Aqua login job. This wait is
  // bounded; running the run loop also allows discovery notifications through.
  AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
      discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeBuiltInWideAngleCamera]
      mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
  NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 30;
  do {
    for (AVCaptureDevice *device in discovery.devices)
      if (device.uniqueID.length && [device.uniqueID caseInsensitiveCompare:PIANO_FRONT_ID] == NSOrderedSame &&
          device.connected && !device.suspended) return device;
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
  } while (NSProcessInfo.processInfo.systemUptime < deadline);
  return nil;
}

static NSString *receiptPath(void)
{
  return [NSHomeDirectory() stringByAppendingPathComponent:
      @"Library/Application Support/PianoCamera/camera-default.plist"];
}

static BOOL receiptValid(id value)
{
  return [value isKindOfClass:NSDictionary.class] && [value[@"format"] isEqual:@1] &&
      [value[@"frontUID"] isEqual:PIANO_FRONT_ID] && [value[@"targets"] isEqual:targetDomains()];
}

static BOOL saveReceipt(NSString *path)
{
  NSData *data = [NSPropertyListSerialization dataWithPropertyList:
      @{@"format":@1, @"frontUID":PIANO_FRONT_ID, @"targets":targetDomains()}
      format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
  return data && [data writeToFile:path options:NSDataWritingAtomic error:NULL] &&
      [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:NULL];
}

int main(int argc, const char **argv)
{
  @autoreleasepool {
    if (argc != 2) return 2;
    NSString *mode = @(argv[1]);
    if (![@[@"--apply", @"--set-front", @"--check"] containsObject:mode]) return 2;
    // Guard synchronous private framework IPC too, not just discovery.
    alarm(45);
    if (!commandSession([mode isEqual:@"--apply"])) { puts("CMIO_CAMERA_DEFAULT_NEEDS_LOGGED_IN_USER"); return 1; }
    BOOL check = [mode isEqual:@"--check"], explicitSelection = [mode isEqual:@"--set-front"];
    NSString *path = receiptPath(), *directory = path.stringByDeletingLastPathComponent;
    if (!check) {
      if (![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES
          attributes:@{NSFilePosixPermissions:@0700} error:NULL]) return 1;
      int lock = open([directory stringByAppendingPathComponent:@"camera-default.lock"].fileSystemRepresentation,
          O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (lock < 0 || flock(lock, LOCK_EX)) return 1;
      // The descriptor deliberately remains open until exit, including through readback.
      if (!explicitSelection && [NSFileManager.defaultManager fileExistsAtPath:path]) {
        id receipt = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:NULL];
        if (!receiptValid(receipt)) { puts("CMIO_CAMERA_DEFAULT_BAD_RECEIPT"); return 1; }
        puts("CMIO_CAMERA_DEFAULT_ALREADY_APPLIED"); return 0;
      }
    }
    Class preferences = NSClassFromString(@"AVCaptureProprietaryDefaultsSingleton");
    SEL reader = NSSelectorFromString(@"objectForKey:");
    SEL setter = NSSelectorFromString(@"setUserPreferredCamera:forClientPreferenceDomain:");
    if (!methodMatches(preferences, reader, "@", 1) || !methodMatches(AVCaptureDevice.class, setter, "v", 2)) {
      puts("CMIO_CAMERA_DEFAULT_UNSUPPORTED_ABI"); return 1;
    }
    PianoHistoryReader read = ^id(NSString *key) {
      return ((id (*)(id, SEL, id))objc_msgSend)(preferences, reader, key);
    };
    PianoCameraSetter set = ^BOOL(id camera, NSString *domain) {
      ((void (*)(id, SEL, id, id))objc_msgSend)(AVCaptureDevice.class, setter, camera, domain);
      return YES;
    };
    @try {
      if (check) {
        PianoAppReader app = ^id(NSString *domain, NSString *key) {
          return CFBridgingRelease(CFPreferencesCopyAppValue((__bridge CFStringRef)key, (__bridge CFStringRef)domain));
        };
        BOOL ready = checkTargets(read, app); puts(ready ? "CMIO_CAMERA_FRONT_READY" : "CMIO_CAMERA_FRONT_NOT_READY");
        return ready ? 0 : 1;
      }
      AVCaptureDevice *front = discoverFront();
      BOOL complete = applyTargets(front, explicitSelection, read, set);
      if (!complete) { puts("CMIO_CAMERA_DEFAULT_DEFERRED"); return explicitSelection ? 1 : 0; }
      if (!saveReceipt(path)) { puts("CMIO_CAMERA_DEFAULT_RECEIPT_FAILED"); return 1; }
      puts("CMIO_CAMERA_DEFAULT_APPLIED"); return 0;
    } @catch (NSException *exception) {
      // Never log a framework exception reason: it may contain foreign device UIDs.
      (void)exception; puts("CMIO_CAMERA_DEFAULT_FRAMEWORK_EXCEPTION"); return 1;
    }
  }
}
#endif
