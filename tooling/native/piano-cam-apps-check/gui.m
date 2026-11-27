// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <IOKit/IOKitLib.h>
#include "../../camera/extension/Devices.h"
#include <sys/stat.h>
#include <unistd.h>

static NSString *const FaceTime = @"com.apple.FaceTime";
static NSString *const QuickTime = @"com.apple.QuickTimePlayerX";

NSDictionary *movieStats(NSString *path);

static int emit(id value)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:NULL];
    if (!data || fwrite(data.bytes, 1, data.length, stdout) != data.length || putchar('\n') == EOF) return 2;
    return 0;
}

static BOOL unlocked(void)
{
    const uid_t uid = geteuid();
    struct stat console;
    if (uid < 501 || stat("/dev/console", &console) || console.st_uid != uid) return NO;
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    if (![session[(__bridge NSString *)kCGSessionUserIDKey] isEqual:@(uid)] ||
        ![session[(__bridge NSString *)kCGSessionOnConsoleKey] isEqual:@YES] ||
        ![session[(__bridge NSString *)kCGSessionLoginDoneKey] isEqual:@YES] ||
        (session[@"CGSSessionScreenIsLocked"] && ![session[@"CGSSessionScreenIsLocked"] isEqual:@NO])) return NO;

    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    if (!root) return NO;
    NSArray *users = CFBridgingRelease(IORegistryEntryCreateCFProperty(root, CFSTR("IOConsoleUsers"), kCFAllocatorDefault, 0));
    IOObjectRelease(root);
    if (![users isKindOfClass:NSArray.class]) return NO;
    NSUInteger matches = 0;
    for (NSDictionary *user in users) {
        if (![user isKindOfClass:NSDictionary.class]) return NO;
        if (![user[(__bridge NSString *)kCGSessionOnConsoleKey] isEqual:@YES]) continue;
        if (![user[(__bridge NSString *)kCGSessionUserIDKey] isEqual:@(uid)] ||
            (user[@"CGSSessionScreenIsLocked"] && ![user[@"CGSSessionScreenIsLocked"] isEqual:@NO])) return NO;
        ++matches;
    }
    return matches == 1;
}

static int idleCameras(void)
{
    const CMIOObjectPropertyAddress devices = {
        kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain
    };
    const CMIOObjectPropertyAddress uidProperty = {
        kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain
    };
    const CMIOObjectPropertyAddress runningProperty = {
        kCMIODevicePropertyDeviceIsRunningSomewhere, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain
    };
    UInt32 bytes = 0, used = 0;
    if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &devices, 0, NULL, &bytes) ||
        !bytes || bytes % sizeof(CMIOObjectID) || bytes > 4096) return 2;
    NSMutableData *data = [NSMutableData dataWithLength:bytes];
    if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &devices, 0, NULL, bytes, &used, data.mutableBytes) ||
        used != bytes) return 2;
    const CMIOObjectID *ids = data.bytes;
    unsigned real = 0;
    for (unsigned i = 0; i < used / sizeof *ids; ++i) {
        CFStringRef uid = NULL;
        if (CMIOObjectGetPropertyData(ids[i], &uidProperty, 0, NULL, sizeof uid, &bytes, &uid) || !uid) return 2;
        NSString *identifier = [CFBridgingRelease(uid) uppercaseString];
        if ([identifier isEqual:PIANO_FRONT_ID]) real |= 1;
        else if ([identifier isEqual:PIANO_REAR_ID]) real |= 2;
        else if (![identifier isEqual:PIANO_PATTERN_ID]) continue;
        UInt32 running = 0;
        if (CMIOObjectGetPropertyData(ids[i], &runningProperty, 0, NULL, sizeof running, &bytes, &running) ||
            bytes != sizeof running || running) return 2;
    }
    return real == 3 ? 0 : 2;
}

static NSArray<NSRunningApplication *> *running(NSString *bundle)
{
    return [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle];
}

static BOOL waitUntil(double timeout, BOOL (^condition)(void))
{
    double deadline = NSProcessInfo.processInfo.systemUptime + timeout;
    do {
        if (condition()) return YES;
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    } while (NSProcessInfo.processInfo.systemUptime < deadline);
    return NO;
}

static int launch(NSString *bundle)
{
    if (running(bundle).count || !unlocked()) return 2;
    NSURL *url = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:bundle];
    if (!url || ![url.path hasPrefix:@"/System/Applications/"]) return 2;
    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.promptsUserIfNeeded = NO;
    configuration.addsToRecentItems = NO;
    configuration.arguments = @[@"-ApplePersistenceIgnoreState", @"YES"];
    __block BOOL done = NO, opened = NO;
    // The completion handler can run off the main thread.
    [NSWorkspace.sharedWorkspace openApplicationAtURL:url configuration:configuration
                                   completionHandler:^(NSRunningApplication *app, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            opened = app != nil && error == nil;
            done = YES;
        });
    }];
    return waitUntil(8, ^{ return done; }) && opened ? 0 : 1;
}

static id ax(AXUIElementRef element, CFStringRef attr)
{
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, attr, &value) != kAXErrorSuccess) return nil;
    return CFBridgingRelease(value);
}

static BOOL standardWindow(AXUIElementRef window)
{
    return [ax(window, kAXSubroleAttribute) isEqual:(__bridge NSString *)kAXStandardWindowSubrole] &&
           [ax(window, kAXMinimizedAttribute) isEqual:@NO];
}

static BOOL visibleWindow(AXUIElementRef app)
{
    NSArray *windows = ax(app, kAXWindowsAttribute);
    for (id item in windows) {
        if (standardWindow((__bridge AXUIElementRef)item)) return YES;
    }
    return NO;
}

static CGDirectDisplayID internalDisplay(void)
{
    CGDirectDisplayID displays[32], internal = 0;
    uint32_t count = 0;
    if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess || count == 32) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!CGDisplayIsBuiltin(displays[i])) continue;
        if (internal) return 0;
        internal = displays[i];
    }
    return internal;
}

static BOOL axWindowBounds(AXUIElementRef window, CGRect *bounds)
{
    id position = ax(window, kAXPositionAttribute), size = ax(window, kAXSizeAttribute);
    return position && size &&
           CFGetTypeID((__bridge CFTypeRef)position) == AXValueGetTypeID() &&
           CFGetTypeID((__bridge CFTypeRef)size) == AXValueGetTypeID() &&
           AXValueGetValue((__bridge AXValueRef)position, kAXValueTypeCGPoint, &bounds->origin) &&
           AXValueGetValue((__bridge AXValueRef)size, kAXValueTypeCGSize, &bounds->size);
}

static NSNumber *onScreenWindow(pid_t pid, CGRect bounds)
{
    NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    // Quartz and AX share top-left global coordinates. Allow point rounding at window edges.
    const CGFloat edge = 1;
    CGRect interior = CGRectInset(bounds, edge, edge);
    for (NSDictionary *window in windows) {
        if ([window[(__bridge NSString *)kCGWindowAlpha] doubleValue] <= 0) continue;
        CGRect rect;
        id geometry = window[(__bridge NSString *)kCGWindowBounds];
        if (!geometry || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)geometry, &rect)) return nil;
        if ([window[(__bridge NSString *)kCGWindowOwnerPID] isEqual:@(pid)] &&
            [window[(__bridge NSString *)kCGWindowLayer] isEqual:@0] &&
            CGRectContainsRect(CGRectInset(rect, -edge, -edge), bounds) &&
            CGRectContainsRect(CGRectInset(bounds, -edge, -edge), rect))
            return window[(__bridge NSString *)kCGWindowNumber];
        if (CGRectIntersectsRect(rect, interior)) return nil;
    }
    return nil;
}

static NSArray *rectPoints(CGRect rect)
{
    return @[@(rect.origin.x), @(rect.origin.y), @(rect.size.width), @(rect.size.height)];
}

static int faceTimeGeometry(NSRunningApplication *app, AXUIElementRef element)
{
    if (app.hidden || !app.active) return 2;
    id main = ax(element, kAXMainWindowAttribute);
    AXUIElementRef window = (__bridge AXUIElementRef)main;
    CGRect bounds;
    if (!window || !standardWindow(window) || !axWindowBounds(window, &bounds) ||
        bounds.size.width <= 2 || bounds.size.height <= 2) return 2;
    CGDirectDisplayID display = internalDisplay();
    if (!display || CGDisplayRotation(display) != 0 || CGDisplayIsInMirrorSet(display)) return 2;
    CGRect screen = CGDisplayBounds(display);
    if (!CGRectContainsRect(screen, bounds)) return 2;
    NSNumber *number = onScreenWindow(app.processIdentifier, bounds);
    if (!number) return 2;
    return emit(@{@"window": rectPoints(bounds), @"screen": rectPoints(screen),
                  @"window_number": number, @"display": @(display)});
}

static AXUIElementRef findNewMovieItem(AXUIElementRef element, unsigned depth)
{
    if (!depth) return NULL;
    NSString *key = ax(element, kAXMenuItemCmdCharAttribute);
    if ([ax(element, kAXRoleAttribute) isEqual:(__bridge NSString *)kAXMenuItemRole] &&
        key && [key caseInsensitiveCompare:@"n"] == NSOrderedSame &&
        [ax(element, kAXMenuItemCmdModifiersAttribute) isEqual:@(kAXMenuItemModifierOption)])
        return (AXUIElementRef)CFRetain(element);
    NSArray *children = ax(element, kAXChildrenAttribute);
    for (id child in children) {
        AXUIElementRef item = findNewMovieItem((__bridge AXUIElementRef)child, depth - 1);
        if (item) return item;
    }
    return NULL;
}

typedef NSAppleEventDescriptor Event;

static Event *sendEvent(Event *target, AEEventClass suite, AEEventID command, Event *object,
                        AEKeyword option, Event *value, BOOL *permitted)
{
    *permitted = AEDeterminePermissionToAutomateTarget(target.aeDesc, suite, command, false) == noErr;
    if (!*permitted) return nil;
    Event *event = [Event appleEventWithEventClass:suite eventID:command targetDescriptor:target
                                         returnID:kAutoGenerateReturnID transactionID:kAnyTransactionID];
    if (object) [event setParamDescriptor:object forKeyword:keyDirectObject];
    if (value) [event setParamDescriptor:value forKeyword:option];
    Event *reply = [event sendEventWithOptions:kAEWaitReply timeout:5 error:NULL];
    return [[reply paramDescriptorForKeyword:keyErrorNumber] int32Value] == 0 ? reply : nil;
}

static Event *objectSpecifier(OSType type, OSType form, Event *key, Event *container)
{
    Event *record = [Event recordDescriptor];
    [record setDescriptor:[Event descriptorWithTypeCode:type] forKeyword:keyAEDesiredClass];
    [record setDescriptor:[Event descriptorWithEnumCode:form] forKeyword:keyAEKeyForm];
    [record setDescriptor:key forKeyword:keyAEKeyData];
    [record setDescriptor:container forKeyword:keyAEContainer];
    return [record coerceToDescriptorType:typeObjectSpecifier];
}

static Event *property(Event *target, Event *document, OSType code, BOOL *permitted)
{
    Event *item = objectSpecifier(cProperty, formPropertyID, [Event descriptorWithTypeCode:code], document);
    return [sendEvent(target, kAECoreSuite, kAEGetData, item, 0, nil, permitted) paramDescriptorForKeyword:keyDirectObject];
}

static int quit(NSString *bundle, BOOL discard)
{
    NSArray<NSRunningApplication *> *apps = running(bundle);
    if (!apps.count) return 0;
    if (apps.count != 1) return 2;
    NSRunningApplication *app = apps.firstObject;
    if (![app.bundleURL.path hasPrefix:@"/System/Applications/"]) return 2;
    if (discard) {
        BOOL permitted;
        Event *target = [Event descriptorWithProcessIdentifier:app.processIdentifier];
        Event *reply = sendEvent(target, kCoreEventClass, kAEQuitApplication, nil, 'savo',
                                 [Event descriptorWithEnumCode:kAENo], &permitted);
        if (!reply && !app.terminated) [app terminate];
    } else {
        [app terminate];
    }
    return waitUntil(8, ^BOOL{ return running(bundle).count == 0; }) ? 0 : 2;
}

static BOOL dismissOpenDialog(AXUIElementRef app)
{
    // A clean QuickTime launch may show its initial Open dialog.
    // Touch only a QuickTime-owned dialog with an exposed Cancel button.
    NSArray *windows = ax(app, kAXWindowsAttribute);
    for (id item in windows) {
        AXUIElementRef window = (__bridge AXUIElementRef)item;
        NSString *role = ax(window, kAXSubroleAttribute);
        if (![role isEqual:(__bridge NSString *)kAXDialogSubrole] &&
            ![role isEqual:(__bridge NSString *)kAXSystemDialogSubrole]) continue;
        id cancel = ax(window, kAXCancelButtonAttribute);
        if (cancel && (!unlocked() || AXUIElementPerformAction((__bridge AXUIElementRef)cancel, kAXPressAction))) return NO;
    }
    return YES;
}

static int newMovie(AXUIElementRef app, Event *target)
{
    const AEEventClass suites[] = {kAECoreSuite, kAECoreSuite, kAECoreSuite, kCoreEventClass, kCoreEventClass,
                                   'MVWR', 'MVWR', 'MVWR'};
    const AEEventID commands[] = {kAEGetData, kAESave, kAESetData, kAEOpenDocuments, kAEQuitApplication, 'star', 'stop', 'play'};
    for (size_t i = 0; i < sizeof commands / sizeof *commands; ++i)
        if (AEDeterminePermissionToAutomateTarget(target.aeDesc, suites[i], commands[i], false) != noErr) return 2;

    // A clean launch can still restore documents after a previous session.
    // Refuse to record or use the discard-on-quit path in that case.
    BOOL permitted;
    Event *documents = objectSpecifier(cDocument, formAbsolutePosition, [Event descriptorWithEnumCode:kAEAll],
                                       [Event nullDescriptor]);
    Event *list = [sendEvent(target, kAECoreSuite, kAEGetData, documents, 0, nil, &permitted)
                   paramDescriptorForKeyword:keyDirectObject];
    if (!permitted || list.descriptorType != typeAEList || list.numberOfItems) return 2;

    if (!dismissOpenDialog(app)) return 2;
    id menu = ax(app, kAXMenuBarAttribute);
    AXUIElementRef item = menu ? findNewMovieItem((__bridge AXUIElementRef)menu, 6) : NULL;
    if (!item) return 3;
    BOOL pressed = [ax(item, kAXEnabledAttribute) isEqual:@YES] && unlocked() &&
                   AXUIElementPerformAction(item, kAXPressAction) == kAXErrorSuccess;
    CFRelease(item);
    return pressed ? 0 : 3;
}

static int quickCommand(NSString *op, NSString *path, AXUIElementRef app, pid_t pid)
{
    Event *target = [Event descriptorWithProcessIdentifier:pid];
    if ([op isEqual:@"qt-new"]) return newMovie(app, target);
    BOOL permitted = NO;
    Event *doc = objectSpecifier(cDocument, formAbsolutePosition, [Event descriptorWithInt32:1], [Event nullDescriptor]);
    NSURL *url = path ? [NSURL fileURLWithPath:path] : nil;
    Event *reply = nil;
    if ([op isEqual:@"qt-open"]) {
        if (!dismissOpenDialog(app)) return 2;
        Event *files = [Event listDescriptor];
        [files insertDescriptor:[Event descriptorWithFileURL:url] atIndex:1];
        reply = sendEvent(target, kCoreEventClass, kAEOpenDocuments, files, 0, nil, &permitted);
    } else if ([op isEqual:@"qt-save"]) {
        reply = sendEvent(target, kAECoreSuite, kAESave, doc, keyAEFile, [Event descriptorWithFileURL:url], &permitted);
    } else if ([op isEqual:@"qt-start"] || [op isEqual:@"qt-stop"]) {
        reply = sendEvent(target, 'MVWR', [op isEqual:@"qt-start"] ? 'star' : 'stop', doc, 0, nil, &permitted);
    } else if ([op isEqual:@"qt-finalized"]) {
        Event *duration = property(target, doc, 'durn', &permitted);
        return !permitted ? 2 : (duration.doubleValue > 0 ? 0 : 3);
    } else if ([op isEqual:@"qt-play"] || [op isEqual:@"qt-time"]) {
        NSURL *actual = property(target, doc, 'file', &permitted).fileURLValue;
        if (!permitted) return 2;
        if (!actual || ![actual.URLByResolvingSymlinksInPath.path isEqual:url.URLByResolvingSymlinksInPath.path]) return 3;
        if ([op isEqual:@"qt-time"]) {
            Event *time = property(target, doc, 'time', &permitted);
            if (!permitted || !time || !isfinite(time.doubleValue)) return 2;
            Event *playing = property(target, doc, 'play', &permitted);
            if (!permitted || !playing) return 2;
            return emit(@{@"time": @(time.doubleValue), @"playing": @(playing.booleanValue)});
        }
        // Mute this document before playing it.
        Event *mute = objectSpecifier(cProperty, formPropertyID, [Event descriptorWithTypeCode:'mute'], doc);
        if (!sendEvent(target, kAECoreSuite, kAESetData, mute, keyAEData, [Event descriptorWithBoolean:YES], &permitted))
            return permitted ? 1 : 2;
        Event *time = objectSpecifier(cProperty, formPropertyID, [Event descriptorWithTypeCode:'time'], doc);
        if (!sendEvent(target, kAECoreSuite, kAESetData, time, keyAEData, [Event descriptorWithDouble:0], &permitted))
            return permitted ? 1 : 2;
        reply = sendEvent(target, 'MVWR', 'play', doc, 0, nil, &permitted);
    }
    return !permitted ? 2 : (reply ? 0 : 1);
}

static int gui(NSString *op, NSString *path)
{
    if ([op isEqual:@"quit-ft"]) return quit(FaceTime, NO);
    if ([op isEqual:@"quit-qt"]) return quit(QuickTime, NO);
    if ([op isEqual:@"quit-qt-discard"]) return quit(QuickTime, YES);
    if (!unlocked()) return 2;
    if ([op isEqual:@"unlocked"]) return 0;
    if (!AXIsProcessTrusted()) return 2;
    if ([op isEqual:@"prerequisites"]) {
        NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
        if (v.majorVersion != 27 || v.minorVersion != 3 || running(FaceTime).count || running(QuickTime).count) return 2;
        io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoCamera"));
        if (!service) return 2;
        IOObjectRelease(service);
        return 0;
    }
    if ([op isEqual:@"idle"]) return idleCameras();
    if ([op isEqual:@"launch-ft"]) return launch(FaceTime);
    if ([op isEqual:@"launch-qt"]) return launch(QuickTime);
    BOOL geometry = [op isEqual:@"geometry-ft"];
    BOOL faceTime = geometry || [op isEqual:@"window-ft"];
    NSArray<NSRunningApplication *> *apps = running(faceTime ? FaceTime : QuickTime);
    if (apps.count != 1) return 1;
    NSRunningApplication *app = apps.firstObject;
    if (![app.bundleURL.path hasPrefix:@"/System/Applications/"]) return 2;
    if (!app.finishedLaunching) return 3;
    AXUIElementRef element = AXUIElementCreateApplication(app.processIdentifier);
    AXUIElementSetMessagingTimeout(element, 0.5);
    int result;
    if (geometry)
        result = faceTimeGeometry(app, element);
    else if (faceTime || [op isEqual:@"window-qt"])
        result = !app.hidden && visibleWindow(element) ? 0 : 3;
    else
        result = quickCommand(op, path, element, app.processIdentifier);
    CFRelease(element);
    return result;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc < 2 || argc > 3) return 2;
        NSString *op = @(argv[1]), *path = argc == 3 ? @(argv[2]) : nil;
        if ([op isEqual:@"inspect"]) return path ? emit(movieStats(path)) : 2;
        if ([op isEqual:@"qt-save"] || [op isEqual:@"qt-open"] || [op isEqual:@"qt-time"] || [op isEqual:@"qt-play"]) {
            if (![path hasPrefix:@"/private/var/tmp/cam-apps/run-"] || ![path.lastPathComponent isEqual:@"recording.mov"])
                return 2;
        } else if (path) {
            return 2;
        }
        return gui(op, path);
    }
}
