// SPDX-License-Identifier: BSD-2-Clause-Patent
// Unattended decode and playback probe for the native VideoToolbox bridge.
//
//   piano-play-probe [-f clip] [-t seconds]
//
// Part A is a single frame through AVAssetReader: the codec, the decoded size, whether the frame
// landed in an IOSurface, the pixel buffer's creation attributes and a SHA-256 of the bytes, so a
// decoded frame is identified by content rather than by a counter. Part B plays the same clip in a
// real on-screen AVPlayer window and prints the played position every two seconds, so a run either
// reaches the end within its wall-clock budget or names where it stopped.
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CommonCrypto/CommonDigest.h>
#import <IOSurface/IOSurfaceRef.h>

static void printFrame(CVPixelBufferRef pb, CMTime pts, const char *tag) {
  OSType codec = 0;
  if (pb) codec = CVPixelBufferGetPixelFormatType(pb);
  const void *base = CVPixelBufferGetBaseAddress(pb);
  size_t used = CVPixelBufferGetDataSize(pb);
  size_t planes = CVPixelBufferGetPlaneCount(pb);
  IOSurfaceRef surface = CVPixelBufferGetIOSurface(pb);
  CFDictionaryRef attrs = pb ? CVPixelBufferCopyCreationAttributes(pb) : NULL;
  char hex[65]; hex[0] = 0;
  // An IOSurface-backed buffer has no base address until it is locked, so lock it before hashing;
  // without this the frame0 hash used to print empty and proved nothing about the decoded pixels.
  bool locked = pb && CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
  base = CVPixelBufferGetBaseAddress(pb);
  if (base && used) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(base, (CC_LONG)used, digest);
    for (int i = 0; i < 16; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  }
  if (locked) { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); }
  char fourcc[5] = {(char)((codec >> 24) & 0xff), (char)((codec >> 16) & 0xff), (char)((codec >> 8) & 0xff), (char)(codec & 0xff), 0};
  printf("probe: %s pts=%lld/%d %zux%zu fourcc=%s planes=%zu iosurface=%s bytes=%zu sha256=%s\n",
         tag, (long long)CMTimeGetSeconds(pts), 1,
         pb ? CVPixelBufferGetWidth(pb) : 0, pb ? CVPixelBufferGetHeight(pb) : 0,
         fourcc, planes, surface ? "yes" : "no", used, hex);
  if (attrs) { printf("probe: %s attributes=%s\n", tag, [[(NSDictionary *)CFBridgingRelease(attrs) description] UTF8String]); }
  fflush(stdout);
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    NSString *path = @"/tmp/clip.mp4";
    double budget = 45.0;
    for (int i = 1; i + 1 < argc; ++i) {
      if (!strcmp(argv[i], "-f")) path = [NSString stringWithUTF8String:argv[++i]];
      else if (!strcmp(argv[i], "-t")) budget = atof(argv[++i]);
    }
    printf("probe: start clip=%s budget=%.0f s\n", [path UTF8String], budget); fflush(stdout);
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    AVAssetTrack *video = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
    if (!video) { printf("probe: FAIL no video track\n"); return 2; }
    CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)[video formatDescriptions].firstObject;
    printf("probe: track %zux%zu codec=%u trackTimeRange=%lld s\n",
           (size_t)CMVideoFormatDescriptionGetDimensions(fd).width,
           (size_t)CMVideoFormatDescriptionGetDimensions(fd).height,
           (unsigned)CMFormatDescriptionGetMediaSubType(fd),
           (long long)CMTimeGetSeconds(video.timeRange.duration));

    // Part A: one frame, decoded, identified by content.
    NSError *err = nil;
    AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
    AVAssetTrack *vtrack = video;
    AVAssetReaderTrackOutput *out =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vtrack
                                                   outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                                                     (id)kCVPixelBufferMetalCompatibilityKey: @YES}];
    [reader addOutput:out];
    if ([reader startReading]) {
      CMSampleBufferRef sb = [out copyNextSampleBuffer];
      if (sb) { printFrame(CMSampleBufferGetImageBuffer(sb), CMSampleBufferGetPresentationTimeStamp(sb), "frame0"); CFRelease(sb); }
      else printf("probe: FAIL frame0 no sample (%s)\n", [[[reader error] localizedDescription] UTF8String]);
      [reader cancelReading];
    } else printf("probe: FAIL reader start (%s)\n", [[[reader error] localizedDescription] UTF8String]);

    // Part B: continuous playback in an on-screen window.
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 800)
                                                   styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    [window setLevel:NSMainMenuWindowLevel + 1];
    AVPlayer *player = [AVPlayer playerWithPlayerItem:[AVPlayerItem playerItemWithAsset:asset]];
    AVPlayerLayer *layer = [AVPlayerLayer playerLayerWithPlayer:player];
    [layer setFrame:window.contentView.bounds];
    [layer setVideoGravity:AVLayerVideoGravityResizeAspect];
    [window.contentView.layer addSublayer:layer];
    [window makeKeyAndOrderFront:nil];
    __block double last = -1.0, highest = 0.0; __block int stalls = 0;
    printf("probe: window shown, playing\n"); fflush(stdout);
    __auto_type token = [player addPeriodicTimeObserverForInterval:CMTimeMake(2, 1) queue:dispatch_get_main_queue()
        usingBlock:^(CMTime t) {
          double s = CMTimeGetSeconds(t);
          if (s > highest) highest = s;
          printf("probe: play %.2f s\n", s); fflush(stdout);
        }];
    [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemPlaybackStalledNotification
        object:player.currentItem queue:nil usingBlock:^(NSNotification *n) { stalls++; printf("probe: stalled\n"); fflush(stdout); }];
    [player play];
    NSDate *begin = [NSDate date];
    while ([[NSDate date] timeIntervalSinceDate:begin] < budget && highest + 0.5 < CMTimeGetSeconds(asset.duration)) {
      NSEvent *e;
      while ((e = [window nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05] inMode:NSDefaultRunLoopMode dequeue:YES]))
        [window sendEvent:e];
    }
    double wall = [[NSDate date] timeIntervalSinceDate:begin];
    printf("probe: end played=%.2f s of %.2f s wall=%.2f s stalls=%d rate=%.2f\n",
           highest, CMTimeGetSeconds(asset.duration), wall, stalls,
           CMTimeGetSeconds(asset.duration) > 0 ? highest / wall : 0.0);
    fflush(stdout);
    [player pause];
    return highest + 0.5 >= CMTimeGetSeconds(asset.duration) ? 0 : 1;
  }
}