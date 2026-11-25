// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Retain actual provider samples across timer stop, then validate CPU planes.
#define main pianoCameraProviderMain
#include "../PianoCameraExtension.m"
#undef main
#import "../../diagnostic/FrameEvidence.h"
#include <assert.h>

@interface CompatFrameProbe : PianoStreamSource {
@public
  CMSampleBufferRef first, later;
  unsigned frames;
}
@property(nonatomic, strong) dispatch_semaphore_t ready;
@end
@implementation CompatFrameProbe
- (void)publish:(CVPixelBufferRef)pb ticks:(uint64_t)ticks sequence:(uint64_t)sequence drops:(uint64_t)drops
{
  [super publish:pb ticks:ticks sequence:sequence drops:drops];
  ++frames;
  if (frames != 1 && frames != 32) return;
  CMFormatDescriptionRef fd = NULL;
  CMSampleBufferRef sample = NULL;
  CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMClockMakeHostTimeFromSystemUnits(ticks), kCMTimeInvalid};
  assert(!CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fd));
  assert(!CMSampleBufferCreateReadyWithImageBuffer(NULL, pb, fd, &timing, &sample));
  CFRelease(fd);
  if (frames == 1) first = sample;
  else { later = sample; dispatch_semaphore_signal(self.ready); }
}
- (void)dealloc { if (first) CFRelease(first); if (later) CFRelease(later); }
@end

int main(int argc, const char **argv)
{
  @autoreleasepool {
    assert(argc == 2);
    gLog = os_log_create("com.unixnu.PianoCamera", "compat-frame-host");
    CompatFrameProbe *source = [[CompatFrameProbe alloc] initWithWidth:1280 height:720 sensor:0];
    source.ready = dispatch_semaphore_create(0);
    CMIOExtensionStreamProperties *properties = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
    properties.activeFormatIndex = @1;
    properties.frameDuration = timeDictionary(CMTimeMake(1, 30));
    properties.maxFrameDuration = timeDictionary(CMTimeMake(1, 30));
    assert([source setStreamProperties:properties error:NULL]);
    assert([source startStreamAndReturnError:NULL]);
    assert(dispatch_semaphore_wait(source.ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    assert([source stopStreamAndReturnError:NULL]);
    FrameEvidence *a = [[FrameEvidence alloc] initWithSample:source->first prefix:[@(argv[1]) stringByAppendingPathComponent:@"pattern-1"] pattern:YES];
    FrameEvidence *b = [[FrameEvidence alloc] initWithSample:source->later prefix:[@(argv[1]) stringByAppendingPathComponent:@"pattern-32"] pattern:YES];
    assert(a && b && a.patternGood && b.patternGood);
    assert(a.patternFrame == 0 && b.patternFrame == 31);
    assert(![a.allocation isEqual:b.allocation]);
    puts("provider retained frames 1/32 CPU 420v planes/bars/counter/sweep PASS");
  }
  return 0;
}
