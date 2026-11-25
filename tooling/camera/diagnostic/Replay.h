// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "FrameEvidence.h"
#import <Metal/Metal.h>

@interface Replay : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device pluginPath:(NSString *)pluginPath;
- (BOOL)runSample:(CMSampleBufferRef)sample evidence:(FrameEvidence *)evidence
          prefix:(NSString *)prefix pattern:(BOOL)pattern;
@end
