// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "FrameEvidence.h"
#import <CommonCrypto/CommonDigest.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <dlfcn.h>

BOOL WriteJSON(id object, NSString *path)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingPrettyPrinted |
                    NSJSONWritingSortedKeys error:nil];
    return [data writeToFile:path atomically:YES];
}

NSString *SHA256(NSData *bytes)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest);
    char result[2 * sizeof digest + 1];
    for (size_t i = 0; i < sizeof digest; i++) snprintf(result + i * 2, 3, "%02x", digest[i]);
    return @(result);
}

BOOL WritePNG(NSData *bgra, NSString *path)
{
    if (bgra.length != RGBRow * FrameHeight) return NO;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)bgra);
    CGImageRef image = CGImageCreate(FrameWidth, FrameHeight, 8, 32, RGBRow, space,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, provider, NULL, false,
        kCGRenderingIntentDefault);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
    BOOL ok = NO;
    if (dest && image) {
        CGImageDestinationAddImage(dest, image, NULL);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    if (image) CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    return ok;
}

NSDictionary *ComparePixels(NSData *a, NSData *b, BOOL mirrorA)
{
    if (a.length != RGBRow * FrameHeight || a.length != b.length) return @{@"available": @NO};
    const uint8_t *p = (const uint8_t *)a.bytes, *q = (const uint8_t *)b.bytes;
    unsigned exact = 0, over20 = 0, over40 = 0, maxError = 0;
    uint64_t sum = 0;
    for (unsigned y = 0; y < FrameHeight; y++) for (unsigned x = 0; x < FrameWidth; x++) {
        unsigned ax = mirrorA ? FrameWidth - 1 - x : x;
        unsigned error = 0;
        for (unsigned channel = 0; channel < 3; channel++) {
            unsigned delta = abs(p[y * RGBRow + ax * 4 + channel] - q[y * RGBRow + x * 4 + channel]);
            sum += delta;
            error = std::max(error, delta);
        }
        maxError = std::max(maxError, error);
        exact += error == 0;
        over20 += error > 20;
        over40 += error > 40;
    }
    return @{@"available": @YES, @"pixels": @(FrameWidth * FrameHeight), @"exactRGB": @(exact),
             @"over20RGB": @(over20), @"over40RGB": @(over40), @"maxRGB": @(maxError),
             @"meanAbsoluteRGB": @((double)sum / (FrameWidth * FrameHeight * 3))};
}

// The apps2 provider writes these BGRA bars, a four-row sweep and a 24-bit
// binary frame counter. Use the counter to predict both bar phase and sweep.
static constexpr uint8_t Bars[8][3] = {{255,255,255}, {0,255,255}, {255,255,0}, {0,255,0},
                                      {255,0,255}, {0,0,255}, {255,0,0}, {16,16,16}};
static const uint8_t White[3] = {255,255,255};
static const uint8_t Black[3] = {0,0,0};
static constexpr unsigned Cell = (FrameWidth - 32) / 24;

static const uint8_t *Expected(unsigned x, unsigned y, unsigned counter)
{
    unsigned sweep = counter * 6 % FrameHeight;
    if (y >= 16 && y < 48 && x >= 16 && x < 16 + 24 * Cell) {
        return ((counter >> (23 - (x - 16) / Cell)) & 1) ? White : Black;
    }
    if (y >= sweep && y < sweep + 4) return White;
    return Bars[(x + counter * 8) % FrameWidth / (FrameWidth / 8)];
}

NSDictionary *CheckPattern(NSData *bgra, unsigned counter, BOOL mirrored)
{
    if (bgra.length != RGBRow * FrameHeight) return @{@"available": @NO, @"passed": @NO};
    const uint8_t *p = (const uint8_t *)bgra.bytes;
    unsigned bars = 0, badBars = 0, sweepPoints = 0, badSweep = 0, bits = 0, badBits = 0;
    unsigned sweep = counter * 6 % FrameHeight;
    for (unsigned y = 1; y < FrameHeight; y += 2) for (unsigned x = 5; x < FrameWidth; x += 8) {
        // Exclude the chroma interpolation edges of bars, marks and the sweep.
        if (y >= 14 && y < 50) continue;
        const BOOL sweepInterior = y > sweep && y < sweep + 3;
        if (y + 2 >= sweep && y < sweep + 6 && !sweepInterior) continue;
        unsigned withinBar = (x + counter * 8) % (FrameWidth / 8);
        if (withinBar < 4 || withinBar > 75) continue;
        unsigned px = mirrored ? FrameWidth - 1 - x : x;
        const uint8_t *actual = p + y * RGBRow + 4 * px;
        const uint8_t *expected = Expected(x, y, counter);
        BOOL mismatch = NO;
        for (unsigned k = 0; k < 3; k++) mismatch |= abs(actual[k] - expected[k]) > 40;
        if (sweepInterior) { ++sweepPoints; badSweep += mismatch; }
        else { ++bars; badBars += mismatch; }
    }
    for (unsigned bit = 0; bit < 24; bit++) {
        unsigned x = 16 + bit * Cell + Cell / 2;
        const uint8_t *expected = Expected(x, 31, counter);
        unsigned px = mirrored ? FrameWidth - 1 - x : x;
        const uint8_t *actual = p + 31 * RGBRow + 4 * px;
        ++bits;
        badBits += abs(actual[0] - expected[0]) > 40 || abs(actual[1] - expected[1]) > 40 ||
                   abs(actual[2] - expected[2]) > 40;
    }
    BOOL passed = bars > 15000 && badBars * 1000 <= bars * 5 && badBits == 0 &&
                  (sweepPoints == 0 || badSweep * 10 <= sweepPoints);
    return @{@"available": @YES, @"passed": @(passed), @"counter": @(counter),
             @"barPoints": @(bars), @"barMismatches": @(badBars), @"counterPoints": @(bits),
             @"counterMismatches": @(badBits), @"sweepPoints": @(sweepPoints), @"sweepMismatches": @(badSweep)};
}

static NSDictionary *PatternPlanes(const uint8_t *ys, size_t yRow, const uint8_t *uv, size_t uvRow,
                                    unsigned counter, double kr, double kb)
{
    unsigned points = 0, badY = 0, badUV = 0;
    unsigned sweep = counter * 6 % FrameHeight;
    for (unsigned y = 60; y < FrameHeight; y += 4) for (unsigned x = 9; x < FrameWidth; x += 16) {
        unsigned position = (x + counter * 8) % 80;
        if (position < 4 || position > 75 || (y + 2 >= sweep && y < sweep + 6)) continue;
        const uint8_t *bgr = Expected(x, y, counter);
        double luma = kr * bgr[2] + (1 - kr - kb) * bgr[1] + kb * bgr[0];
        double expectedY = 16 + luma * 219 / 255;
        double expectedU = 128 + (bgr[0] - luma) * 224 / (510 * (1 - kb));
        double expectedV = 128 + (bgr[2] - luma) * 224 / (510 * (1 - kr));
        const uint8_t *chroma = uv + (y / 2) * uvRow + (x & ~1u);
        ++points;
        badY += fabs(ys[y * yRow + x] - expectedY) > 18;
        badUV += fabs(chroma[0] - expectedU) > 24 || fabs(chroma[1] - expectedV) > 24;
    }
    BOOL passed = points > 3000 && badY * 1000 <= points * 5 && badUV * 1000 <= points * 5;
    return @{@"points": @(points), @"yMismatches": @(badY), @"uvMismatches": @(badUV),
             @"totalMismatches": @(badY + badUV), @"passed": @(passed)};
}

static NSDictionary *Time(CMTime t)
{
    return @{@"value": @(t.value), @"scale": @(t.timescale), @"epoch": @(t.epoch), @"flags": @(t.flags)};
}

static NSString *FourCC(OSType f)
{
    return [NSString stringWithFormat:@"%c%c%c%c (0x%08x)", f >> 24, (f >> 16) & 255,
                                      (f >> 8) & 255, f & 255, f];
}

// Preserve the types of attachments used for interpretation. For non-plist
// CM objects preserve their description without blocking the rest of the record.
static id AttachmentValue(id value)
{
    if (!value) return [NSNull null];
    if ([value isKindOfClass:NSString.class] || [value isKindOfClass:NSNumber.class]) return value;
    if ([value isKindOfClass:NSData.class])
        return @{@"base64": [value base64EncodedStringWithOptions:0]};
    if ([value isKindOfClass:NSDictionary.class]) {
        NSMutableDictionary *result = [NSMutableDictionary dictionary];
        for (id key in value) result[[key description]] = AttachmentValue(value[key]);
        return result;
    }
    if ([value isKindOfClass:NSArray.class]) {
        NSMutableArray *result = [NSMutableArray array];
        for (id item in value) [result addObject:AttachmentValue(item)];
        return result;
    }
    return [value description];
}

@implementation FrameEvidence
- (instancetype)initWithSample:(CMSampleBufferRef)sample prefix:(NSString *)prefix pattern:(BOOL)pattern
{
    if (!(self = [super init])) return nil;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    if (!pixel) return nil;
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel);
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    OSType type = CVPixelBufferGetPixelFormatType(pixel);
    size_t width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    NSMutableDictionary *meta = [@{
        @"fourcc": FourCC(type), @"width": @(width), @"height": @(height),
        @"planeCount": @(CVPixelBufferGetPlaneCount(pixel)),
        @"presentationTime": Time(CMSampleBufferGetPresentationTimeStamp(sample)),
        @"decodeTime": Time(CMSampleBufferGetDecodeTimeStamp(sample)),
        @"duration": Time(CMSampleBufferGetDuration(sample)),
        @"hostMonotonicAtSnapshot": @(NSProcessInfo.processInfo.systemUptime),
        @"sampleAttachments": AttachmentValue((__bridge id)CMSampleBufferGetSampleAttachmentsArray(sample, false)),
        @"samplePropagated": AttachmentValue(CFBridgingRelease(CMCopyDictionaryOfAttachments(NULL, sample, kCMAttachmentMode_ShouldPropagate))),
        @"sampleNonPropagated": AttachmentValue(CFBridgingRelease(CMCopyDictionaryOfAttachments(NULL, sample, kCMAttachmentMode_ShouldNotPropagate))),
        @"formatExtensions": AttachmentValue(format ? (__bridge id)CMFormatDescriptionGetExtensions(format) : nil),
        @"pixelPropagated": AttachmentValue(CFBridgingRelease(CVBufferCopyAttachments(pixel, kCVAttachmentMode_ShouldPropagate))),
        @"pixelNonPropagated": AttachmentValue(CFBridgingRelease(CVBufferCopyAttachments(pixel, kCVAttachmentMode_ShouldNotPropagate))),
    } mutableCopy];
    if (surface) {
        auto getCache = (uint32_t (*)(IOSurfaceRef))dlsym(RTLD_DEFAULT, "IOSurfaceGetCacheMode");
        meta[@"surface"] = @{
            @"id": @(IOSurfaceGetID(surface)), @"bytes": @(IOSurfaceGetAllocSize(surface)),
            @"fourcc": FourCC(IOSurfaceGetPixelFormat(surface)),
            @"allocationWidth": @(IOSurfaceGetWidth(surface)), @"allocationHeight": @(IOSurfaceGetHeight(surface)),
            @"cacheMode": getCache ? @(getCache(surface)) : (id)@"getter absent",
            @"cacheModeProperty": AttachmentValue(CFBridgingRelease(IOSurfaceCopyValue(surface, kIOSurfaceCacheMode))),
        };
    }
    auto fail = ^FrameEvidence *(NSString *reason) {
        meta[@"error"] = reason;
        WriteJSON(meta, [prefix stringByAppendingString:@"-frame.json"]);
        fprintf(stderr, "SNAPSHOT_FAIL %s %s\n", prefix.lastPathComponent.UTF8String, reason.UTF8String);
        return nil;
    };

    if (!surface || type != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        IOSurfaceGetPixelFormat(surface) != type || width != FrameWidth || height != FrameHeight ||
        CVPixelBufferGetPlaneCount(pixel) != 2 || IOSurfaceGetPlaneCount(surface) != 2)
        return fail(@"expected IOSurface-backed 640x480 420v with two planes");

    const size_t size = IOSurfaceGetAllocSize(surface);
    if (!size || size > 16 * 1024 * 1024) return fail(@"invalid IOSurface allocation size");
    uint32_t before = IOSurfaceGetSeed(surface), lockSeed = 0, unlockSeed = 0;
    kern_return_t locked = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &lockSeed);
    if (locked != KERN_SUCCESS) return fail([NSString stringWithFormat:@"IOSurfaceLock=%d", locked]);
    const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
    NSMutableArray *planes = [NSMutableArray arrayWithCapacity:2];
    NSMutableArray<NSData *> *bytes = [NSMutableArray arrayWithCapacity:2];
    NSString *layoutError = nil;
    for (unsigned plane = 0; plane < 2; plane++) {
        const uint8_t *planeBase = (const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, plane);
        size_t w = IOSurfaceGetWidthOfPlane(surface, plane), h = IOSurfaceGetHeightOfPlane(surface, plane);
        size_t row = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
        size_t offset = (uintptr_t)planeBase - (uintptr_t)base;
        [planes addObject:@{@"plane": @(plane), @"width": @(w), @"height": @(h), @"rowBytes": @(row),
                            @"offset": @(offset), @"cvWidth": @(CVPixelBufferGetWidthOfPlane(pixel, plane)),
                            @"cvHeight": @(CVPixelBufferGetHeightOfPlane(pixel, plane)),
                            @"cvRowBytes": @(CVPixelBufferGetBytesPerRowOfPlane(pixel, plane))}];
        if (!base || offset >= size || w != (width >> plane) || h != (height >> plane) ||
            row < width || row > (size - offset) / h ||
            w != CVPixelBufferGetWidthOfPlane(pixel, plane) || h != CVPixelBufferGetHeightOfPlane(pixel, plane) ||
            row != CVPixelBufferGetBytesPerRowOfPlane(pixel, plane)) {
            layoutError = @"invalid plane geometry or span";
            break;
        }
        [bytes addObject:[NSData dataWithBytes:planeBase length:row * h]];
    }
    if (!layoutError) _allocation = [NSData dataWithBytes:base length:size];
    kern_return_t unlocked = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &unlockSeed);
    _seed = IOSurfaceGetSeed(surface);
    meta[@"planes"] = planes;
    meta[@"seeds"] = @{@"before": @(before), @"lock": @(lockSeed), @"unlock": @(unlockSeed), @"after": @(_seed),
                      @"lockStatus": @(locked), @"unlockStatus": @(unlocked)};
    if (layoutError) return fail(layoutError);
    if (![bytes[0] writeToFile:[prefix stringByAppendingString:@"-y.bin"] atomically:YES] ||
        ![bytes[1] writeToFile:[prefix stringByAppendingString:@"-uv.bin"] atomically:YES]) return fail(@"write planes");
    if (unlocked != KERN_SUCCESS || lockSeed != unlockSeed || _seed != unlockSeed)
        return fail(@"source seed changed during read-only snapshot");

    id matrix = CFBridgingRelease(CVBufferCopyAttachment(pixel, kCVImageBufferYCbCrMatrixKey, NULL));
    if (!matrix && format) matrix = (__bridge id)CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_YCbCrMatrix);
    double kr = 0.299, kb = 0.114;
    NSString *matrixName = @"BT.601 (SD fallback, inferred)";
    if ([matrix isEqual:(__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_709_2]) {
        kr = 0.2126; kb = 0.0722; matrixName = @"BT.709";
    } else if ([matrix isEqual:(__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_2020]) {
        kr = 0.2627; kb = 0.0593; matrixName = @"BT.2020";
    } else if ([matrix isEqual:(__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_601_4]) {
        matrixName = @"BT.601";
    } else if (matrix) {
        matrixName = [NSString stringWithFormat:@"BT.601 fallback for %@", matrix];
    }
    size_t yRow = [planes[0][@"rowBytes"] unsignedLongValue], uvRow = [planes[1][@"rowBytes"] unsignedLongValue];
    const uint8_t *ys = (const uint8_t *)bytes[0].bytes, *uv = (const uint8_t *)bytes[1].bytes;
    NSDictionary *planeCheck = nil;
    if (pattern) {
        for (unsigned bit = 0; bit < 24; bit++) {
            unsigned x = 16 + bit * Cell + Cell / 2;
            _patternFrame = (_patternFrame << 1) | (ys[31 * yRow + x] > 126 ? 1u : 0u);
        }
        planeCheck = PatternPlanes(ys, yRow, uv, uvRow, _patternFrame, kr, kb);
        if (!matrix) {
            // An absent matrix attachment leaves SD conversion coefficients
            // ambiguous. Let the known bars distinguish 601 from 709.
            NSDictionary *candidate = PatternPlanes(ys, yRow, uv, uvRow, _patternFrame, 0.2126, 0.0722);
            meta[@"unlabelledMatrixPatternChecks"] = @{@"601": planeCheck, @"709": candidate};
            if ([candidate[@"totalMismatches"] unsignedIntValue] < [planeCheck[@"totalMismatches"] unsignedIntValue]) {
                kr = 0.2126; kb = 0.0722;
                matrixName = @"BT.709 (inferred from pattern planes)";
                planeCheck = candidate;
            }
        }
    }
    meta[@"cpuMatrix"] = matrixName;
    NSMutableData *bgra = [NSMutableData dataWithLength:RGBRow * FrameHeight];
    uint8_t *dest = (uint8_t *)bgra.mutableBytes;
    auto byte = [](double x) { return (uint8_t)std::clamp(lround(x), 0L, 255L); };
    for (unsigned y = 0; y < FrameHeight; y++) for (unsigned x = 0; x < FrameWidth; x++) {
        double l = (ys[y * yRow + x] - 16) * 255.0 / 219.0;
        double cb = (uv[(y / 2) * uvRow + (x & ~1u)] - 128) * 255.0 / 224.0;
        double cr = (uv[(y / 2) * uvRow + (x & ~1u) + 1] - 128) * 255.0 / 224.0;
        double b = l + 2 * (1 - kb) * cb, r = l + 2 * (1 - kr) * cr;
        uint8_t *p = dest + y * RGBRow + x * 4;
        p[0] = byte(b); p[1] = byte((l - kr * r - kb * b) / (1 - kr - kb)); p[2] = byte(r); p[3] = 255;
    }
    _cpu = [bgra copy];
    if (pattern) {
        NSDictionary *rgb = CheckPattern(_cpu, _patternFrame, NO);
        _patternGood = [rgb[@"passed"] boolValue] && [planeCheck[@"passed"] boolValue];
        meta[@"pattern"] = @{@"rgb": rgb, @"planes": planeCheck, @"passed": @(_patternGood)};
    }
    if (!WritePNG(_cpu, [prefix stringByAppendingString:@"-cpu.png"])) return fail(@"write CPU PNG");
    if (!WriteJSON(meta, [prefix stringByAppendingString:@"-frame.json"])) return fail(@"write frame metadata");
    printf("SNAPSHOT_OK %s 420v surface=%u seed=%u\n", prefix.lastPathComponent.UTF8String,
           IOSurfaceGetID(surface), _seed);
    return self;
}
@end
