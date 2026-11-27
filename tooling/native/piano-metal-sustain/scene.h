// SPDX-License-Identifier: MIT
#import <Metal/Metal.h>

@interface SustainScene : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device width:(NSUInteger)width height:(NSUInteger)height
                         draws:(NSUInteger)draws error:(NSError **)error;
- (BOOL)encode:(id<MTLCommandBuffer>)command color:(id<MTLTexture>)color time:(double)time;
@end
