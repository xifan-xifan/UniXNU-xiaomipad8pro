// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CommonCrypto/CommonDigest.h>
#import "Devices.h"

@interface FrameSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) NSUInteger frames, invalid, dropped;
@property(nonatomic) size_t expectedWidth, expectedHeight;
@property(nonatomic) BOOL moved, scene, saved;
@property(nonatomic) uint64_t firstFingerprint;
@property(nonatomic) CMTime lastPTS;
@property(nonatomic, copy) NSString *file;
@end

@implementation FrameSink
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!pixel || CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_32BGRA || !CMTIME_IS_NUMERIC(pts) ||
        (_frames && CMTimeCompare(pts, _lastPTS) <= 0) || CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly)) {
        ++_invalid;
        return;
    }
    size_t width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel);
    const uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    if (!base || width != _expectedWidth || height != _expectedHeight || stride < width * 4) {
        CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        ++_invalid;
        return;
    }
    unsigned lo = 255, hi = 0;
    uint64_t fingerprint = 14695981039346656037ULL;
    for (size_t y = 0; y < height; y += 8) {
        for (size_t x = 0; x < width; x += 8) {
            const uint8_t *p = base + y * stride + x * 4;
            unsigned level = (p[0] + 2 * p[1] + p[2]) / 4;
            lo = MIN(lo, level);
            hi = MAX(hi, level);
            fingerprint = (fingerprint ^ level) * 1099511628211ULL;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    _lastPTS = pts;
    if (++_frames == 1) {
        _firstFingerprint = fingerprint;
        printf("first-frame %zux%zu stride=%zu FourCC=BGRA pts=%.9f sequence=%lu luminance=%u..%u\n",
               width, height, stride, CMTimeGetSeconds(pts), (unsigned long)_frames, lo, hi);
    }
    _moved |= fingerprint != _firstFingerprint;
    BOOL scene = hi >= 16 && hi - lo >= 8;
    _scene |= scene;
    if (_frames == 1 && _file) {
        CIImage *image = [CIImage imageWithCVPixelBuffer:pixel];
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        NSError *error = nil;
        _saved = [[CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @YES}]
            writePNGRepresentationOfImage:image toURL:[NSURL fileURLWithPath:_file]
            format:kCIFormatRGBA8 colorSpace:srgb options:@{} error:&error];
        CGColorSpaceRelease(srgb);
        if (!_saved) printf("PNG error domain=%s code=%ld\n", error.domain.UTF8String, (long)error.code);
        else {
            NSData *data = [NSData dataWithContentsOfFile:_file];
            if (!data.length) { _saved = NO; return; }
            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
            printf("saved sequence=%lu sha256=", (unsigned long)_frames);
            for (unsigned i = 0; i < sizeof digest; ++i) printf("%02x", digest[i]);
            puts("");
        }
    }
}
- (void)captureOutput:(AVCaptureOutput *)output didDropSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection *)connection
{
    ++_dropped;
}
@end

// Discovery-only diagnostics use this compiled AVFoundation client: JavaScript's
// framework import need not expose the Tundra class aliases on a native boot.
static void inventory(void)
{
    // QuickTime still queries the legacy type; keep that diagnostic comparison.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
                       AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    for (AVCaptureDeviceType type in types) {
        NSArray *devices = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[type]
                            mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
        printf("discovery type=%s count=%lu\n", type.UTF8String, (unsigned long)devices.count);
        for (AVCaptureDevice *device in devices) {
            if (![device.uniqueID.uppercaseString hasPrefix:@"6A1F2A8E-4C2B-4E8F-9B57-50494E4F000"]) continue;
            printf("device name=%s type=%s model=%s connected=%d suspended=%d inUse=%d formats=%lu\n",
                   device.localizedName.UTF8String, device.deviceType.UTF8String, device.modelID.UTF8String,
                   device.connected, device.suspended, device.inUseByAnotherApplication, (unsigned long)device.formats.count);
            for (AVCaptureDeviceFormat *format in device.formats) {
                CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
                for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges)
                    printf("inventory format %s %dx%d FourCC=%08x fps=%.2f..%.2f\n", device.localizedName.UTF8String,
                           size.width, size.height, CMFormatDescriptionGetMediaSubType(format.formatDescription),
                           range.minFrameRate, range.maxFrameRate);
            }
        }
    }
    puts("CAMERA_INVENTORY_COMPLETE");
}

static BOOL capture(AVCaptureDevice *device, int seconds, NSString *file)
{
    if (!device) return NO;
    AVCaptureDeviceFormat *native = nil;
    for (AVCaptureDeviceFormat *format in device.formats) {
        CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        FourCharCode type = CMFormatDescriptionGetMediaSubType(format.formatDescription);
        for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges) {
            printf("format %s %dx%d FourCC=%08x fps=%.2f..%.2f\n", device.localizedName.UTF8String,
                   size.width, size.height, type, range.minFrameRate, range.maxFrameRate);
            if (!native && type == kCVPixelFormatType_32BGRA && range.minFrameRate <= 15 && range.maxFrameRate >= 15) native = format;
        }
    }
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    AVCaptureSession *session = [AVCaptureSession new];
    AVCaptureVideoDataOutput *output = [AVCaptureVideoDataOutput new];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
    FrameSink *sink = [FrameSink new];
    sink.file = file;
    dispatch_queue_t queue = dispatch_queue_create("com.unixnu.PianoCameraCapture.frames", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:sink queue:queue];
    if (!native || !input || ![session canAddInput:input] || ![session canAddOutput:output]) {
        printf("capture configuration failed for %s (%ld)\n", device.localizedName.UTF8String, (long)error.code);
        return NO;
    }
    [session beginConfiguration];
    [session addInput:input];
    [session addOutput:output];
    // The macOS SDK selects the device format through activeFormat.
    if (![device lockForConfiguration:&error]) {
        [session commitConfiguration];
        printf("format lock failed code=%ld\n", (long)error.code);
        return NO;
    }
    device.activeFormat = native;
    device.activeVideoMinFrameDuration = CMTimeMake(1, 15);
    device.activeVideoMaxFrameDuration = CMTimeMake(1, 15);
    [device unlockForConfiguration];
    [session commitConfiguration];
    CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
    printf("capture begin %s %dx%d duration=%.9f\n", device.localizedName.UTF8String, size.width, size.height,
           CMTimeGetSeconds(device.activeVideoMinFrameDuration));
    sink.expectedWidth = size.width;
    sink.expectedHeight = size.height;
    [session startRunning];
    if (session.isRunning) {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:10];
        __block BOOL received = NO;
        while (!received && [until timeIntervalSinceNow] > 0) {
            dispatch_sync(queue, ^{ received = sink.frames > 0; });
            if (!received) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        if (received) [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }
    [session stopRunning];
    [output setSampleBufferDelegate:nil queue:NULL];
    __block BOOL ok;
    dispatch_sync(queue, ^{
        ok = !sink.invalid && (file ? sink.frames >= 60 && sink.saved && sink.scene : sink.frames >= 12 && sink.moved);
        printf("capture end %s valid=%lu invalid=%lu dropped=%lu fps=%.2f moved=%d scene=%d png=%d result=%s\n",
               device.localizedName.UTF8String, (unsigned long)sink.frames, (unsigned long)sink.invalid,
               (unsigned long)sink.dropped, (double)sink.frames / seconds, sink.moved, sink.scene, sink.saved, ok ? "PASS" : "FAIL");
    });
    return ok;
}

// --movie: what QuickTime's New Movie Recording at "High" does, without its window: AVCaptureMovieFileOutput with H.264 from the
// 640x480 420v format (index 1, the one the stock apps use), then the written file read back. verify-1 (2026-11-21): the software
// H.264 encoder needs ~15 inputs before its first output; with a 4-buffer delivery pool nothing was written.
@interface MovieSink : NSObject <AVCaptureFileOutputRecordingDelegate>
@property(atomic) BOOL started, finished, complete;
@property(atomic) long code;
@end

@implementation MovieSink
- (void)captureOutput:(AVCaptureFileOutput *)output didStartRecordingToOutputFileAtURL:(NSURL *)url
      fromConnections:(NSArray<AVCaptureConnection *> *)connections
{
    self.started = YES;
}
- (void)captureOutput:(AVCaptureFileOutput *)output didFinishRecordingToOutputFileAtURL:(NSURL *)url
      fromConnections:(NSArray<AVCaptureConnection *> *)connections error:(NSError *)error
{
    self.code = error ? (long)error.code : 0;
    self.complete = !error || [error.userInfo[AVErrorRecordingSuccessfullyFinishedKey] boolValue];
    self.finished = YES;
}
@end

static void spin(NSTimeInterval seconds, BOOL (^done)(void))
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ((!done || !done()) && [until timeIntervalSinceNow] > 0)
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}

// The written movie: duration, codec, the video track's edit segments (QuickTime's cut timeline showed as 15 segments) and the
// number of compressed samples, read with AVFoundation.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static BOOL movieFile(NSString *file, int seconds, const char *label, long code, BOOL complete)
{
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:file error:NULL];
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:file] options:nil];
    AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    double duration = CMTimeGetSeconds(asset.duration), media = 0;
    NSUInteger segments = 0, empty = 0, frames = 0;
    FourCharCode codec = 0;
    if (track) {
        CMFormatDescriptionRef description = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
        if (description) codec = CMFormatDescriptionGetMediaSubType(description);
        for (AVAssetTrackSegment *segment in track.segments) {
            ++segments;
            if (segment.empty) ++empty; else media += CMTimeGetSeconds(segment.timeMapping.target.duration);
        }
        NSError *error = nil;
        AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
        AVAssetReaderTrackOutput *samples = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
        if (reader && [reader canAddOutput:samples]) {
            [reader addOutput:samples];
            if ([reader startReading]) {
                CMSampleBufferRef sample;
                while ((sample = [samples copyNextSampleBuffer])) {
                    frames += (NSUInteger)MAX(0, CMSampleBufferGetNumSamples(sample));
                    CFRelease(sample);
                }
            }
        }
    }
    const char c[5] = {(char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8), (char)codec, 0};
    const BOOL ok = complete && track && codec == kCMVideoCodecType_H264 && duration >= seconds - 1 && segments - empty == 1 &&
                    media >= 0.9 * duration && frames >= (NSUInteger)seconds * 10;
    printf("MOVIE %s %s file=%s bytes=%llu duration=%.2f codec=%s segments=%lu empty=%lu media=%.2f frames=%lu fps=%.1f error=%ld\n",
           label, ok ? "PASS" : "FAIL", file.lastPathComponent.UTF8String, [attributes[NSFileSize] unsignedLongLongValue], duration,
           codec ? c : "none", (unsigned long)segments, (unsigned long)empty, media, (unsigned long)frames,
           duration > 0 ? frames / duration : 0, code);
    return ok;
}
#pragma clang diagnostic pop

static BOOL movie(AVCaptureDevice *device, int seconds, NSString *file, const char *label)
{
    if (!device) { printf("MOVIE %s FAIL no device\n", label); return NO; }
    AVCaptureDeviceFormat *compat = nil;
    for (AVCaptureDeviceFormat *format in device.formats) {
        CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        if (size.width == 640 && size.height == 480 &&
            CMFormatDescriptionGetMediaSubType(format.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
            compat = format;
    }
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    AVCaptureSession *session = [AVCaptureSession new];
    AVCaptureMovieFileOutput *output = [AVCaptureMovieFileOutput new];
    if (!compat || !input || ![session canAddInput:input] || ![session canAddOutput:output]) {
        printf("MOVIE %s FAIL configuration format=%d input=%d code=%ld\n", label, compat != nil, input != nil, (long)error.code);
        return NO;
    }
    [session beginConfiguration];
    if ([session canSetSessionPreset:AVCaptureSessionPresetHigh]) session.sessionPreset = AVCaptureSessionPresetHigh;
    [session addInput:input];
    [session addOutput:output];
    if (![device lockForConfiguration:&error]) {
        [session commitConfiguration];
        printf("MOVIE %s FAIL format lock code=%ld\n", label, (long)error.code);
        return NO;
    }
    device.activeFormat = compat;
    device.activeVideoMinFrameDuration = CMTimeMake(1, 30);
    device.activeVideoMaxFrameDuration = CMTimeMake(1, 30);
    [device unlockForConfiguration];
    [session commitConfiguration];
    AVCaptureConnection *connection = [output connectionWithMediaType:AVMediaTypeVideo];
    if (!connection) {
        printf("MOVIE %s FAIL no video connection on the movie output\n", label);
        return NO;
    }
    @try {
        [output setOutputSettings:@{AVVideoCodecKey: AVVideoCodecTypeH264} forConnection:connection];
    } @catch (NSException *exception) {
        printf("MOVIE %s FAIL H.264 output settings refused\n", label);
        return NO;
    }
    [session startRunning];
    spin(5, ^BOOL { return session.isRunning; });
    [NSFileManager.defaultManager removeItemAtPath:file error:NULL];
    MovieSink *sink = [MovieSink new];
    printf("movie begin %s 640x480 420v 30 fps H.264 %d s running=%d\n", label, seconds, session.isRunning);
    [output startRecordingToOutputFileURL:[NSURL fileURLWithPath:file] recordingDelegate:sink];
    spin(10, ^BOOL { return sink.started || sink.finished; });
    const BOOL started = sink.started;
    if (started) spin(seconds, ^BOOL { return sink.finished; });
    const double recorded = CMTimeGetSeconds(output.recordedDuration);
    [output stopRecording];
    spin(30, ^BOOL { return sink.finished; });
    [session stopRunning];
    printf("movie end %s started=%d finished=%d recorded=%.2f code=%ld\n", label, started, sink.finished, recorded, sink.code);
    if (!sink.finished || ![NSFileManager.defaultManager fileExistsAtPath:file]) {
        printf("MOVIE %s FAIL no movie written\n", label);
        return NO;
    }
    return movieFile(file, seconds, label, sink.code, sink.complete);
}

static int movies(NSString *directory, int seconds)
{
    NSString *log = [directory stringByAppendingPathComponent:@"movie.txt"];
    if (!freopen(log.fileSystemRepresentation, "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    printf("movie client started uid=%u camera authorization=%ld\n", geteuid(), (long)status);
    // A probe never asks: without a standing Camera grant it fails instead of putting a prompt on the owner's screen.
    if (status != AVAuthorizationStatusAuthorized) { puts("MOVIE CLIENT FAIL authorization"); return 1; }
    NSArray *devices = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:
                        @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal]
                        mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
    AVCaptureDevice *front = nil, *rear = nil;
    for (AVCaptureDevice *device in devices) {
        if ([device.uniqueID caseInsensitiveCompare:PIANO_FRONT_ID] == NSOrderedSame) front = device;
        if ([device.uniqueID caseInsensitiveCompare:PIANO_REAR_ID] == NSOrderedSame) rear = device;
    }
    BOOL a = movie(front, seconds, [directory stringByAppendingPathComponent:@"front.mov"], "front");
    BOOL b = movie(rear, seconds, [directory stringByAppendingPathComponent:@"rear.mov"], "rear");
    printf("MOVIE CLIENT %s front=%d rear=%d\n", a && b ? "PASS" : "FAIL", a, b);
    return a && b ? 0 : 1;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (geteuid() == 0) return 2;
        if (argc == 4 && !strcmp(argv[1], "--movie")) {
            int seconds = atoi(argv[3]);
            if (seconds < 2 || seconds > 60) return 2;
            return movies([NSString stringWithUTF8String:argv[2]], seconds);
        }
        if (argc != 2) return 2;
        if (!strcmp(argv[1], "--inventory")) { inventory(); return 0; }
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        NSString *log = [directory stringByAppendingPathComponent:@"capture.txt"];
        if (!freopen(log.fileSystemRepresentation, "w", stdout)) return 2;
        setvbuf(stdout, NULL, _IOLBF, 0);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        printf("capture GUI client started uid=%u\n", geteuid());
        AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        printf("Camera authorization initial=%ld\n", (long)status);
        if (status == AVAuthorizationStatusNotDetermined) {
            __block BOOL answered = NO, granted = NO;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL value) {
                dispatch_async(dispatch_get_main_queue(), ^{ granted = value; answered = YES; });
            }];
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:100];
            while (!answered && [until timeIntervalSinceNow] > 0)
                [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            printf("Camera authorization answered=%d granted=%d\n", answered, granted);
            if (!granted) { puts("CLIENT FAIL authorization"); return 1; }
        } else if (status != AVAuthorizationStatusAuthorized) {
            puts("CLIENT FAIL authorization");
            return 1;
        }
        NSArray *types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal];
        NSArray *devices = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                          mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
        NSDictionary *names = @{PIANO_PATTERN_ID: PIANO_PATTERN_NAME, PIANO_FRONT_ID: PIANO_FRONT_NAME,
                                PIANO_REAR_ID: PIANO_REAR_NAME};
        NSMutableDictionary *piano = [NSMutableDictionary new];
        for (AVCaptureDevice *device in devices) {
            NSString *idKey = device.uniqueID.uppercaseString;
            if (![device.localizedName isEqualToString:names[idKey]]) continue;
            piano[idKey] = device;
            printf("discovered %s %s\n", device.localizedName.UTF8String, idKey.UTF8String);
        }
        // The synthetic control is optional when the kernel disables patterns.
        const BOOL hasPattern = piano[PIANO_PATTERN_ID] != nil;
        BOOL pattern = !hasPattern || capture(piano[PIANO_PATTERN_ID], 2, nil);
        BOOL front = capture(piano[PIANO_FRONT_ID], 5, [directory stringByAppendingPathComponent:@"front.png"]);
        BOOL rear = capture(piano[PIANO_REAR_ID], 5, [directory stringByAppendingPathComponent:@"rear.png"]);
        printf("CLIENT %s pattern=%s front=%d rear=%d\n", pattern && front && rear ? "PASS" : "FAIL",
               hasPattern ? (pattern ? "PASS" : "FAIL") : "DISABLED", front, rear);
        return pattern && front && rear ? 0 : 1;
    }
}
