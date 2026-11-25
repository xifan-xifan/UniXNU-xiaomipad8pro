// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

enum { FrameWidth = 640, FrameHeight = 480, RGBRow = FrameWidth * 4 };

@interface FrameEvidence : NSObject
@property(nonatomic, readonly) NSData *allocation;
@property(nonatomic, readonly) NSData *cpu;
@property(nonatomic, readonly) uint32_t seed;
@property(nonatomic, readonly) unsigned patternFrame;
@property(nonatomic, readonly) BOOL patternGood;
- (instancetype)initWithSample:(CMSampleBufferRef)sample prefix:(NSString *)prefix pattern:(BOOL)pattern;
@end

BOOL WritePNG(NSData *bgra, NSString *path);
NSString *SHA256(NSData *bytes);
NSDictionary *ComparePixels(NSData *a, NSData *b, BOOL mirrorA);
NSDictionary *CheckPattern(NSData *bgra, unsigned counter, BOOL mirrored);
BOOL WriteJSON(id object, NSString *path);
