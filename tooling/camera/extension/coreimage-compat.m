// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Select Core Image's legacy CIKL metadata path until native Metal has function reflection.
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

static NSString *control(NSArray<NSString *> *arguments, int *status)
{
  NSTask *task = [NSTask new]; NSPipe *pipe = [NSPipe pipe];
  task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
  task.arguments = arguments; task.standardOutput = pipe; task.standardError = [NSFileHandle fileHandleWithNullDevice];
  if (![task launchAndReturnError:NULL]) { *status = -1; return nil; }
  NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile]; [task waitUntilExit];
  *status = task.terminationStatus;
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}
static NSString *environment(NSString *key, BOOL *ok)
{
  int status; NSString *text = control(@[@"print", @"gui/501"], &status); *ok = status == 0 && text != nil;
  if (!*ok) return nil;
  NSString *pattern = [NSString stringWithFormat:@"(?m)^\\s*%@ => (.*)$", [NSRegularExpression escapedPatternForString:key]];
  NSRegularExpression *expression = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL];
  NSTextCheckingResult *match = [expression firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
  return match ? [text substringWithRange:[match rangeAtIndex:1]] : nil;
}

int main(int argc, const char **argv)
{
  @autoreleasepool {
    if (argc < 2 || getuid() != 501) return 2;
    NSString *mode = @(argv[1]);
    NSString *domain = @"com.apple.coreimage";
    NSString *directory = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/PianoCamera"];
#ifdef PIANO_CMIO_POLICY_HOST_TEST
    if (argc != 4) return 2;
    domain = @(argv[2]); directory = @(argv[3]);
#else
    if (argc != 2) return 2;
#endif
    if (![@[@"--apply", @"--check", @"--remove"] containsObject:mode]) return 2;
    // A login agent and an explicit retry must preserve the same original value.
    if (![mode isEqualToString:@"--check"]) {
      if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
               attributes:@{NSFilePosixPermissions:@0700} error:NULL]) return 1;
      NSString *lock = [directory stringByAppendingPathComponent:@"coreimage-compat.lock"];
      int fd = open(lock.fileSystemRepresentation, O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0 || flock(fd, LOCK_EX)) return 1; // Process exit releases this descriptor and lock.
    }
    NSString *path = [directory stringByAppendingPathComponent:@"coreimage-compat.plist"];
    NSString *key = @"CI_USE_MTL_DAG_FOR_CIKL_SRC";
    NSString *envKey = key;
#ifdef PIANO_CMIO_POLICY_HOST_TEST
    envKey = domain;
#endif
    BOOL inspected = NO; NSString *priorEnvironment = environment(envKey, &inspected);
    if (!inspected) return 1;
    CFStringRef app = (__bridge CFStringRef)domain, property = (__bridge CFStringRef)key;
    id old = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
    if ([mode isEqualToString:@"--check"]) {
      BOOL ok = [priorEnvironment isEqualToString:@"0"] && old && CFGetTypeID((__bridge CFTypeRef)old) == CFBooleanGetTypeID() && ![old boolValue];
      puts(ok ? "CMIO_CIKL_LEGACY_READY policy=cmio-apps2" : "CMIO_CIKL_POLICY_NOT_READY");
      return ok ? 0 : 1;
    }
    int controlStatus; NSString *manager = control(@[@"managername"], &controlStatus);
    if (controlStatus || ![[manager stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] isEqualToString:@"Aqua"]) {
      puts("CMIO_CIKL_POLICY_NEEDS_AQUA_AGENT"); return 1;
    }
    NSDictionary *receipt = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:path] error:NULL];
    if ([mode isEqualToString:@"--remove"]) {
      if (!receipt) { puts("CMIO_CIKL_POLICY_UNCHANGED"); return 0; }
      if (![receipt[@"domain"] isEqualToString:domain] || ![receipt[@"key"] isEqualToString:key] ||
          ![receipt[@"existed"] isKindOfClass:[NSNumber class]]) return 1;
      id restored = [receipt[@"existed"] boolValue] ? receipt[@"value"] : nil;
      if ([receipt[@"existed"] boolValue] && !restored) return 1;
      if ([receipt[@"environmentExisted"] boolValue])
        control(@[@"setenv", envKey, receipt[@"environmentValue"]], &controlStatus);
      else control(@[@"unsetenv", envKey], &controlStatus);
      if (controlStatus) return 1;
      CFPreferencesSetValue(property, (__bridge CFPropertyListRef)restored, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
      if (!CFPreferencesSynchronize(app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) return 1;
      id verified = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
      if (restored ? ![restored isEqual:verified] : verified != nil) return 1;
      if (![[NSFileManager defaultManager] removeItemAtPath:path error:NULL]) return 1;
      puts("CMIO_CIKL_POLICY_RESTORED"); return 0;
    }
    if (!receipt) {
      if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
               attributes:@{NSFilePosixPermissions:@0700} error:NULL]) return 1;
      NSMutableDictionary *saved = [@{@"domain":domain, @"key":key, @"existed":@(old != nil)} mutableCopy];
      if (old) saved[@"value"] = old;
      saved[@"environmentExisted"] = @(priorEnvironment != nil);
      if (priorEnvironment) saved[@"environmentValue"] = priorEnvironment;
      NSData *data = [NSPropertyListSerialization dataWithPropertyList:saved format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
      if (!data || ![data writeToFile:path options:NSDataWritingAtomic error:NULL]) return 1;
      if (![[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:NULL]) return 1;
    } else if (![receipt[@"domain"] isEqualToString:domain] || ![receipt[@"key"] isEqualToString:key]) return 1;
    // Sandbox apps may not read the outside suite: GUI launchd supplies the same
    // single backend selection directly, before Core Image caches its first read.
    control(@[@"setenv", envKey, @"0"], &controlStatus);
    if (controlStatus) return 1;
    CFPreferencesSetValue(property, kCFBooleanFalse, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (!CFPreferencesSynchronize(app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) return 1;
    id verified = CFBridgingRelease(CFPreferencesCopyValue(property, app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
    if (!verified || CFGetTypeID((__bridge CFTypeRef)verified) != CFBooleanGetTypeID() || [verified boolValue]) return 1;
    NSString *currentEnvironment = environment(envKey, &inspected);
    if (!inspected || ![currentEnvironment isEqualToString:@"0"]) return 1;
    puts("CMIO_CIKL_LEGACY_READY policy=cmio-apps2");
  }
  return 0;
}
