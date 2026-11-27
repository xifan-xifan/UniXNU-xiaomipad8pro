// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "FrameEvidence.h"
#include <cassert>

int main(int argc, char **argv)
{
    assert(argc == 2);
    @autoreleasepool {
        const NSUInteger size = RGBRow * FrameHeight;
        NSData *cut = [NSMutableData dataWithLength:size - 1];
        NSMutableData *a = [NSMutableData dataWithLength:size];
        NSMutableData *b = [NSMutableData dataWithLength:size];
        assert(![ComparePixels(a, cut, NO)[@"available"] boolValue]);
        assert(![CheckPattern(cut, 0, NO)[@"available"] boolValue]);
        assert(![CheckPattern(a, 0, NO)[@"passed"] boolValue]);

        static_cast<uint8_t *>(a.mutableBytes)[0] = 41;
        static_cast<uint8_t *>(b.mutableBytes)[(FrameWidth - 1) * 4] = 41;
        NSDictionary *diff = ComparePixels(a, b, NO);
        assert([diff[@"exactRGB"] unsignedIntValue] == FrameWidth * FrameHeight - 2);
        assert([diff[@"over40RGB"] unsignedIntValue] == 2);
        assert([ComparePixels(a, b, YES)[@"exactRGB"] unsignedIntValue] == FrameWidth * FrameHeight);

        NSString *path = @(argv[1]);
        assert(!WritePNG(cut, path) && ![NSFileManager.defaultManager fileExistsAtPath:path]);
        assert(WritePNG(a, path) && [NSFileManager.defaultManager fileExistsAtPath:path]);
    }
}
