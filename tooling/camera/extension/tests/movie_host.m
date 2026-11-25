// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The movie probe's file check (PianoCameraCapture --movie) on movies written here with VideoToolbox's H.264: a continuous 10 s
// recording passes; a short one, an HEVC one and a cut timeline (two media segments, what verify-1's rear recording had) fail.
// No camera is opened.
#define main pianoCameraCaptureMain
#include "../PianoCameraCapture.m"
#undef main
#include <assert.h>

static void writeMovie(NSString *file, double seconds, AVVideoCodecType codec)
{
  [NSFileManager.defaultManager removeItemAtPath:file error:NULL];
  NSError *error = nil;
  AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:file] fileType:AVFileTypeQuickTimeMovie error:&error];
  AVAssetWriterInput *input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
      outputSettings:@{AVVideoCodecKey: codec, AVVideoWidthKey: @640, AVVideoHeightKey: @480}];
  input.expectsMediaDataInRealTime = NO;
  AVAssetWriterInputPixelBufferAdaptor *adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
      sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                                    (id)kCVPixelBufferWidthKey: @640, (id)kCVPixelBufferHeightKey: @480}];
  assert(writer && [writer canAddInput:input]);
  [writer addInput:input];
  assert([writer startWriting]);
  [writer startSessionAtSourceTime:kCMTimeZero];
  const int frames = (int)(seconds * 30);
  for (int i = 0; i < frames; ++i) {
    while (!input.readyForMoreMediaData) usleep(1000);
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferPoolCreatePixelBuffer(NULL, adaptor.pixelBufferPool, &pb));
    CVPixelBufferLockBaseAddress(pb, 0);
    for (unsigned plane = 0; plane < 2; ++plane)
      memset(CVPixelBufferGetBaseAddressOfPlane(pb, plane), plane ? 128 : 16 + (i * 7) % 200,
             CVPixelBufferGetBytesPerRowOfPlane(pb, plane) * CVPixelBufferGetHeightOfPlane(pb, plane));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    assert([adaptor appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 30)]);
    CVPixelBufferRelease(pb);
  }
  [input markAsFinished];
  __block BOOL done = NO;
  [writer finishWritingWithCompletionHandler:^{ done = YES; }];
  while (!done) usleep(10000);
  assert(writer.status == AVAssetWriterStatusCompleted);
}

// Two pieces of one movie back to back with a hole between them: a timeline in two media segments.
static void cut(NSString *source, NSString *file)
{
  [NSFileManager.defaultManager removeItemAtPath:file error:NULL];
  AVMutableMovie *movie = [AVMutableMovie movieWithURL:[NSURL fileURLWithPath:source] options:nil];
  AVMutableMovieTrack *track = (AVMutableMovieTrack *)[movie tracksWithMediaType:AVMediaTypeVideo].firstObject;
  assert(track);
  // drop seconds 4..6: the track keeps 0..4 and 6..10 as two separate edits
  [track removeTimeRange:CMTimeRangeMake(CMTimeMake(4, 1), CMTimeMake(2, 1))];
  [track insertEmptyTimeRange:CMTimeRangeMake(CMTimeMake(4, 1), CMTimeMake(2, 1))];
  NSError *error = nil;
  assert([movie writeMovieHeaderToURL:[NSURL fileURLWithPath:file] fileType:AVFileTypeQuickTimeMovie
                              options:AVMovieWritingAddMovieHeaderToDestination error:&error]);
}

int main(void)
{
  @autoreleasepool {
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *good = [dir stringByAppendingPathComponent:@"good.mov"], *shortMovie = [dir stringByAppendingPathComponent:@"short.mov"];
    NSString *hevc = [dir stringByAppendingPathComponent:@"hevc.mov"], *split = [dir stringByAppendingPathComponent:@"split.mov"];
    writeMovie(good, 10, AVVideoCodecTypeH264);
    writeMovie(shortMovie, 3, AVVideoCodecTypeH264);
    writeMovie(hevc, 10, AVVideoCodecTypeHEVC);
    cut(good, split);
    assert(movieFile(good, 10, "good", 0, YES));
    assert(!movieFile(good, 10, "good-not-complete", -11805, NO));
    assert(!movieFile(shortMovie, 10, "short", 0, YES));
    assert(!movieFile(hevc, 10, "hevc", 0, YES));
    assert(!movieFile(split, 10, "split", 0, YES));
    assert(!movieFile([dir stringByAppendingPathComponent:@"missing.mov"], 10, "missing", 0, YES));
    [NSFileManager.defaultManager removeItemAtPath:dir error:NULL];
    puts("camera movie probe file check: continuous H.264 PASS; short/HEVC/cut/incomplete/missing refused PASS");
  }
  return 0;
}
