// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Policy and selector-ABI checks only; never call a real preferred-camera writer.
#define PIANO_CAMERA_DEFAULT_HOST_TEST 1
#include "../camera-default.m"
#include <assert.h>

@interface WrongCameraABI : NSObject
+ (int)setUserPreferredCamera:(id)camera forClientPreferenceDomain:(id)domain;
@end
@implementation WrongCameraABI
+ (int)setUserPreferredCamera:(id)camera forClientPreferenceDomain:(id)domain { return 0; }
@end

static NSArray *history(NSString *uid)
{
  return @[@{@"CreatorID":@"fixture-provider", @"DeviceUID":uid, @"OverrideRanking":@2}];
}

static NSMutableDictionary *both(id value)
{
  NSMutableDictionary *map = [NSMutableDictionary new];
  for (NSString *domain in targetDomains()) if (value) map[historyKey(domain)] = value;
  return map;
}

int main(void)
{
  @autoreleasepool {
    NSObject *front = [NSObject new];
    __block NSMutableDictionary *map = both(nil);
    __block unsigned writes = 0, reads = 0;
    PianoHistoryReader read = ^id(NSString *key) { ++reads; return map[key]; };
    __block NSMutableDictionary *apps = [NSMutableDictionary new];
    PianoAppReader app = ^id(NSString *domain, NSString *key) { return apps[[domain stringByAppendingFormat:@"/%@", key]]; };
    NSString *qtChoice = [@"com.apple.QuickTimePlayerX/" stringByAppendingString:appChoiceKey(@"com.apple.QuickTimePlayerX")];
    PianoCameraSetter set = ^BOOL(id camera, NSString *domain) {
      assert(camera == front && [targetDomains() containsObject:domain]); ++writes;
      map[historyKey(domain)] = history(PIANO_FRONT_ID); return YES;
    };
    assert([historyKey(@"com.apple.PhotoBooth") isEqual:@"camera-history/preferred-camera-history/com-apple-PhotoBooth"]);
    assert([historyKey(@"com.apple.QuickTimePlayerX") isEqual:@"camera-history/preferred-camera-history/com-apple-QuickTimePlayerX"]);

    // Automatic login work requires Aqua; explicit SSH actions require the
    // same non-root console UID and a proven GUI domain, not an Aqua manager.
    assert(sessionAllowed(YES, 501, 501, 0, YES, NO));
    assert(!sessionAllowed(YES, 501, 501, 501, NO, YES));
    assert(sessionAllowed(NO, 501, 501, 501, NO, YES));
    assert(!sessionAllowed(NO, 501, 501, 0, NO, YES));
    assert(!sessionAllowed(NO, 501, 501, 502, YES, YES));
    assert(!sessionAllowed(NO, 501, 501, 501, NO, NO));
    assert(!sessionAllowed(NO, 0, 0, 0, YES, YES));
    assert(!sessionAllowed(NO, 501, 0, 501, YES, YES));

    // No discovered front means no preference read or mutation.
    assert(!applyTargets(nil, YES, read, set) && reads == 0 && writes == 0);
    // nil is never inferred to be a genuinely absent history by automatic mode.
    assert(!applyTargets(front, NO, read, set) && writes == 0);
    map = both(@[]); assert(applyTargets(front, NO, read, set) && writes == 2);
    assert(checkTargets(read, app));
    unsigned before = writes;
    assert(applyTargets(front, NO, read, set) && writes == before);
    map = both(history(PIANO_FRONT_ID.lowercaseString));
    assert(applyTargets(front, NO, read, set) && writes == before && checkTargets(read, app));

    // A current synthetic choice is replaceable. Older history may be retained by AVFoundation.
    map = both(history(PIANO_PATTERN_ID.lowercaseString));
    assert(applyTargets(front, NO, read, set) && writes == before + 2); before = writes;
    // A deliberate rear choice, including older history entries, survives automatic login policy.
    NSArray *rear = @[history(PIANO_REAR_ID)[0], history(PIANO_PATTERN_ID)[0]];
    map = both(rear); NSDictionary *unchanged = [map copy];
    assert(applyTargets(front, NO, read, set) && writes == before && [map isEqual:unchanged]);
    assert(!checkTargets(read, app));
    map = both(history(@"fixture-disconnected-real-camera")); unchanged = [map copy];
    assert(applyTargets(front, NO, read, set) && writes == before && [map isEqual:unchanged]);
    // Only the explicit operation intentionally changes a real selected camera.
    assert(applyTargets(front, YES, read, set) && writes == before + 2); before = writes;

    for (id invalid in @[@YES, @"unknown", @{}, @[@{}],
        @[@{@"CreatorID":@"fixture", @"DeviceUID":PIANO_PATTERN_ID}],
        @[history(PIANO_PATTERN_ID)[0], history(PIANO_PATTERN_ID)[0],
          history(PIANO_PATTERN_ID)[0], history(PIANO_PATTERN_ID)[0]]]) {
      map = both(invalid); unchanged = [map copy];
      assert(!applyTargets(front, NO, read, set) && writes == before && [map isEqual:unchanged]);
    }
    // A failed/no-op framework call cannot count as a verified default.
    map = both(@[]);
    assert(!applyTargets(front, YES, read, ^BOOL(id camera, NSString *domain) { return NO; }));
    assert(!applyTargets(front, YES, read, ^BOOL(id camera, NSString *domain) { return YES; }));
    // Independent targets: retain rear in one app and seed the other's synthetic default.
    map = both(history(PIANO_REAR_ID));
    map[historyKey(targetDomains()[1])] = history(PIANO_PATTERN_ID);
    assert(applyTargets(front, NO, read, set) && writes == before + 1);
    assert([map[historyKey(targetDomains()[0])] isEqual:history(PIANO_REAR_ID)]);
    assert(historyIsFront(map[historyKey(targetDomains()[1])]));

    // --check reports what each app opens (verify-1 f1: QuickTime's own saved choice was the front, --check said not selected)
    assert([appChoiceKey(@"com.apple.QuickTimePlayerX") isEqual:@"MGDeviceRecordingDocumentViewControllerVideoDeviceSelectionUniqueID"]);
    assert(!appChoiceKey(@"com.apple.PhotoBooth"));
    map = both(nil); assert(checkTargets(read, app) && checkTargets(read, nil));   // no choice recorded: the discovery order, front first
    map = both(@[]); assert(checkTargets(read, app));
    map = both(history(PIANO_REAR_ID)); assert(!checkTargets(read, app));
    apps[qtChoice] = PIANO_FRONT_ID; assert(!checkTargets(read, app));             // Photo Booth still holds the rear
    map[historyKey(@"com.apple.PhotoBooth")] = history(PIANO_FRONT_ID); assert(checkTargets(read, app));
    apps[qtChoice] = PIANO_FRONT_ID.lowercaseString; assert(checkTargets(read, app));
    map = both(history(PIANO_FRONT_ID)); apps[qtChoice] = PIANO_REAR_ID; assert(!checkTargets(read, app));   // the app's choice wins
    apps[qtChoice] = @""; assert(checkTargets(read, app));
    apps[qtChoice] = @YES; assert(!checkTargets(read, app));
    [apps removeAllObjects];
    map = both(@YES); assert(!checkTargets(read, app));
    map = both(history(PIANO_PATTERN_ID)); assert(!checkTargets(read, app));

    SEL selection = NSSelectorFromString(@"setUserPreferredCamera:forClientPreferenceDomain:");
    assert(!methodMatches(Nil, selection, "v", 2));
    assert(!methodMatches(WrongCameraABI.class, selection, "v", 2));
    assert(!methodMatches(AVCaptureDevice.class, selection, "v", 1));
    // Genuine host framework metadata, inspected without invoking either private method.
    assert(methodMatches(AVCaptureDevice.class, selection, "v", 2));
    assert(methodMatches(NSClassFromString(@"AVCaptureProprietaryDefaultsSingleton"),
        NSSelectorFromString(@"objectForKey:"), "@", 1));
    puts("camera-default session/no-camera/typed-history/preservation/explicit-selection/readback/app-choice-check/ABI PASS");
  }
}
