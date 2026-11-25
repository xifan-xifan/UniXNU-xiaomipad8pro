// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "Replay.h"
#import "../extension/Devices.h"
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <sys/sysctl.h>
#include <unistd.h>

static NSString *const SeedPlugin = @"/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal";

@interface CaptureSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
    @public
    CMSampleBufferRef first, thirtySecond;
    unsigned frames, dropped;
}
@property(nonatomic, strong) dispatch_semaphore_t ready;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *surfaceIDs;
@end

@implementation CaptureSink
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output; (void)connection;
    if (frames >= 32) return;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    IOSurfaceRef surface = pixel ? CVPixelBufferGetIOSurface(pixel) : NULL;
    [_surfaceIDs addObject:@(surface ? IOSurfaceGetID(surface) : 0)];
    ++frames;
    if (frames == 1) first = (CMSampleBufferRef)CFRetain(sample);
    if (frames == 32) {
        thirtySecond = (CMSampleBufferRef)CFRetain(sample);
        dispatch_semaphore_signal(_ready);
    }
}
- (void)captureOutput:(AVCaptureOutput *)output didDropSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output; (void)sample; (void)connection;
    if (frames < 32) ++dropped;
}
- (void)dealloc
{
    if (first) CFRelease(first);
    if (thirtySecond) CFRelease(thirtySecond);
}
@end

static CaptureSink *Capture(AVCaptureDevice *device, NSString *label, NSString *dir)
{
    if (!device || device.formats.count <= 1) return nil;
    AVCaptureDeviceFormat *format = device.formats[1];
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    const OSType subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
    if (dimensions.width != FrameWidth || dimensions.height != FrameHeight ||
        subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) return nil;
    BOOL supports30 = NO;
    for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges)
        supports30 |= range.minFrameRate <= 30 && range.maxFrameRate >= 30;
    if (!supports30) return nil;

    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    AVCaptureVideoDataOutput *output = [AVCaptureVideoDataOutput new];
    CaptureSink *sink = [CaptureSink new];
    sink.ready = dispatch_semaphore_create(0);
    sink.surfaceIDs = [NSMutableArray arrayWithCapacity:32];
    dispatch_queue_t frames = dispatch_queue_create("com.unixnu.PianoCamera.DiagClient.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:sink queue:frames];
    output.alwaysDiscardsLateVideoFrames = YES;
    AVCaptureSession *session = [AVCaptureSession new];

    if (!input || ![session canAddInput:input] || ![session canAddOutput:output]) {
        printf("CAPTURE_SETUP_FAIL %s inputError=%ld\n", label.UTF8String, error.code);
        return nil;
    }
    [session beginConfiguration];
    [session addInput:input];
    [session addOutput:output];
    if (![device lockForConfiguration:&error]) {
        [session commitConfiguration];
        printf("CAPTURE_SETUP_FAIL %s lockError=%ld\n", label.UTF8String, error.code);
        return nil;
    }
    @try {
        device.activeFormat = format;
        device.activeVideoMinFrameDuration = CMTimeMake(1, 30);
        device.activeVideoMaxFrameDuration = CMTimeMake(1, 30);
    } @finally {
        [device unlockForConfiguration];
    }
    output.videoSettings = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(FrameWidth),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(FrameHeight),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES
    };
    [session commitConfiguration];
    if (![device.activeFormat isEqual:format]) {
        printf("CAPTURE_SETUP_FAIL %s active index changed\n", label.UTF8String);
        return nil;
    }
    printf("CAPTURE_BEGIN %s index=1 640x480 native=420v requested=420v MetalCompatibility=YES\n", label.UTF8String);
    [session startRunning];
    BOOL reached32 = dispatch_semaphore_wait(sink.ready, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) == 0;
    BOOL formatStayed = [device.activeFormat isEqual:format];
    CMTime minDuration = device.activeVideoMinFrameDuration, maxDuration = device.activeVideoMaxFrameDuration;
    [session stopRunning];
    [output setSampleBufferDelegate:nil queue:NULL];
    dispatch_sync(frames, ^{});
    // Count only nonzero IDs so missing IOSurfaces cannot serve as pool reuse evidence.
    NSCountedSet *seen = [NSCountedSet setWithArray:sink.surfaceIDs];
    NSUInteger repeats = sink.surfaceIDs.count - seen.count;
    NSUInteger zeroes = [seen countForObject:@0];
    if (zeroes) repeats -= zeroes - 1;
    printf("CAPTURE_END %s frames=%u dropped=%u repeatedSurfaceIDs=%lu frame32=%d index1=%d\n",
        label.UTF8String, sink->frames, sink->dropped, repeats, reached32, formatStayed);
    NSDictionary *receipt = @{@"surfaceIDs": sink.surfaceIDs, @"repeatedSurfaceIDs": @(repeats),
        @"frames": @(sink->frames), @"dropped": @(sink->dropped), @"activeIndex1": @(formatStayed),
        @"requestedOutput": @"420v 640x480 MetalCompatibility=YES", @"nativeOutput": @"420v",
        @"activeMinFrameDuration": @{@"value": @(minDuration.value), @"scale": @(minDuration.timescale)},
        @"activeMaxFrameDuration": @{@"value": @(maxDuration.value), @"scale": @(maxDuration.timescale)}};
    WriteJSON(receipt, [dir stringByAppendingPathComponent:[label stringByAppendingString:@"-capture.json"]]);
    return sink;
}

@interface DiagDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, copy) NSString *directory;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic) BOOL decided;
@end

@implementation DiagDelegate
- (void)finish:(NSString *)result
{
    printf("DIAG_COMPLETE %s\n", result.UTF8String);
    [NSApp terminate:nil];
}

- (void)capture
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL complete = YES;
        @autoreleasepool {
            AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeExternal] mediaType:AVMediaTypeVideo
                position:AVCaptureDevicePositionUnspecified];
            NSDictionary *devices = [NSDictionary dictionaryWithObjects:discovery.devices
                forKeys:[discovery.devices valueForKey:@"uniqueID"]];
            Replay *replay = [[Replay alloc] initWithDevice:MTLCreateSystemDefaultDevice() pluginPath:SeedPlugin];
            if (!replay) complete = NO;
            // Collect all six snapshots before submitting GPU work. A render
            // fault must not discard an otherwise independent sensor capture.
            NSMutableArray<BOOL (^)(void)> *pendingReplays = [NSMutableArray array];
            NSArray *labels = @[@"pattern", @"front", @"rear"];
            NSArray *uids = @[PIANO_PATTERN_ID, PIANO_FRONT_ID, PIANO_REAR_ID];
            for (unsigned i = 0; i < labels.count; ++i) @autoreleasepool {
                NSString *label = labels[i];
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.status.stringValue = [NSString stringWithFormat:@"Collecting %@ frames…", label];
                });
                AVCaptureDevice *device = devices[uids[i]];
                if (!device) {
                    printf("CAPTURE_DEVICE_MISSING %s\n", label.UTF8String);
                    complete = NO;
                    continue;
                }
                @try {
                    CaptureSink *sink = Capture(device, label, self.directory);
                    if (!sink) { complete = NO; continue; }
                    CMSampleBufferRef samples[] = {sink->first, sink->thirtySecond};
                    for (unsigned j = 0; j < 2; j++) {
                        if (!samples[j]) { complete = NO; continue; }
                        NSString *prefix = [self.directory stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"%@-%u", label, j == 0 ? 1 : 32]];
                        FrameEvidence *saved = [[FrameEvidence alloc] initWithSample:samples[j] prefix:prefix pattern:i == 0];
                        if (!saved) { complete = NO; continue; }
                        BOOL pattern = i == 0, firstFrame = j == 0;
                        [pendingReplays addObject:^BOOL {
                            CMSampleBufferRef retained = firstFrame ? sink->first : sink->thirtySecond;
                            return [replay runSample:retained evidence:saved prefix:prefix pattern:pattern];
                        }];
                    }
                } @catch (NSException *e) {
                    printf("CAPTURE_EXCEPTION %s %s %s\n", label.UTF8String, e.name.UTF8String, e.reason.UTF8String);
                    complete = NO;
                }
            }
            for (BOOL (^run)(void) in pendingReplays) @autoreleasepool {
                if (!run()) complete = NO;
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{ [self finish:complete ? @"COLLECTED" : @"PARTIAL"]; });
    });
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    char boot[128] = {};
    size_t size = sizeof boot;
    if (sysctlbyname("kern.bootsessionuuid", boot, &size, NULL, 0)) strcpy(boot, "unavailable");
    printf("DIAG_START boot=%s uid=%u\n", boot, getuid());
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 450, 110)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"DiagClient";
    _status = [NSTextField labelWithString:@"Waiting for Camera permission…"];
    _status.frame = NSMakeRect(22, 42, 400, 22);
    [_window.contentView addSubview:_status];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activate];
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    printf("TCC_INITIAL %ld\n", status);
    if (status == AVAuthorizationStatusAuthorized) {
        [self capture];
    } else if (status == AVAuthorizationStatusNotDetermined) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (self.decided) return;
            self.decided = YES;
            [self finish:@"TCC_TIMEOUT"];
        });
        printf("TCC_REQUEST\n");
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL allowed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self.decided) return;
                self.decided = YES;
                printf("TCC_ANSWER granted=%d\n", allowed);
                if (allowed) [self capture];
                else [self finish:@"TCC_DENIED"];
            });
        }];
    } else {
        [self finish:@"TCC_DENIED"];
    }
}
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *dir = [[NSString alloc] initWithUTF8String:argv[1]];
        if (!freopen([dir stringByAppendingPathComponent:@"diag.log"].fileSystemRepresentation, "w", stdout) ||
            dup2(fileno(stdout), fileno(stderr)) < 0) return 1;
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) DiagDelegate *delegate = [DiagDelegate new];
        delegate.directory = dir;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
