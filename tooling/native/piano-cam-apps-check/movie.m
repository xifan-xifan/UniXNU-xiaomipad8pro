// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <AVFoundation/AVFoundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

static uint32_t atomWord(const uint8_t *bytes)
{
    uint32_t word;
    memcpy(&word, bytes, sizeof word);
    return CFSwapInt32BigToHost(word);
}

static BOOL quickTimeAtoms(NSData *file)
{
    const uint8_t *bytes = file.bytes;
    BOOL movie = NO, media = NO, brand = NO;
    for (uint64_t pos = 0; pos < file.length;) {
        uint64_t remaining = file.length - pos;
        if (remaining < 8) return NO;
        uint64_t size = atomWord(bytes + pos);
        uint32_t type = atomWord(bytes + pos + 4);
        uint64_t header = 8;
        if (size == 1) {
            if (remaining < 16) return NO;
            size = ((uint64_t)atomWord(bytes + pos + 8) << 32) | atomWord(bytes + pos + 12);
            header = 16;
        } else if (size == 0) {
            size = remaining;
        }
        if (size < header || size > remaining) return NO;
        if (type == 'ftyp') {
            if (brand || size < header + 8 || atomWord(bytes + pos + header) != 'qt  ') return NO;
            brand = YES;
        }
        if (type == 'moov' && size > header) movie = YES;
        if (type == 'mdat' && size > header) media = YES;
        pos += size;
    }
    return brand && movie && media;
}

static BOOL readLuma(CVPixelBufferRef pixel, uint8_t grid[1024])
{
    if (!pixel || CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly)) return NO;
    const size_t width = CVPixelBufferGetWidthOfPlane(pixel, 0), height = CVPixelBufferGetHeightOfPlane(pixel, 0);
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
    const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
    BOOL valid = base && width && height && stride >= width;
    if (valid) {
        for (size_t y = 0; y < 32; ++y)
            for (size_t x = 0; x < 32; ++x)
                grid[y * 32 + x] = base[((2 * y + 1) * height / 64) * stride + (2 * x + 1) * width / 64];
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    return valid;
}

static NSString *frameHash(const uint8_t grid[1024])
{
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(grid, 1024, digest);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return @(hex);
}

// The command runs off the applications' event loops. Synchronous loading/reading
// keeps this one-shot file probe compatible with the repository's macOS SDK target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
NSDictionary *movieStats(NSString *path)
{
    NSDictionary *invalid = @{@"valid": @NO};
    NSURL *url = [NSURL fileURLWithPath:path];
    NSData *file = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:NULL];
    if (!file.length) return invalid;
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
    NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    if (tracks.count != 1) return invalid;
    AVAssetTrack *track = tracks[0];
    BOOL h264 = track.formatDescriptions.count > 0;
    for (id description in track.formatDescriptions)
        h264 &= CMFormatDescriptionGetMediaSubType((__bridge CMFormatDescriptionRef)description) == kCMVideoCodecType_H264;
    double duration = CMTimeGetSeconds(asset.duration);
    double videoDuration = CMTimeGetSeconds(track.timeRange.duration);
    if (!isfinite(duration) || duration <= 0 || !isfinite(videoDuration) || videoDuration <= 0) return invalid;
    BOOL container = quickTimeAtoms(file);
    if (!container || !h264) return invalid;
    NSDictionary *metadata = @{@"bytes": @(file.length), @"duration": @(duration), @"video_span": @(videoDuration),
                                @"codec": @"avc1", @"container": @"quicktime"};

    NSDictionary *settings = @{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:
                                   @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:NULL];
    AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output]) return metadata;
    [reader addOutput:output];
    if (![reader startReading]) return metadata;
    NSUInteger frames = 0, nonblack = 0;
    double first = 0, last = 0;
    NSMutableArray *hashes = [NSMutableArray arrayWithCapacity:3];
    CMSampleBufferRef sample;
    while ((sample = [output copyNextSampleBuffer])) {
        uint8_t grid[1024];
        double pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        BOOL valid = readLuma(CMSampleBufferGetImageBuffer(sample), grid);
        CFRelease(sample);
        if (!valid || !isfinite(pts) || (frames && pts <= last)) {
            [reader cancelReading];
            return metadata;
        }
        if (!frames) first = pts;
        last = pts;
        ++frames;
        unsigned bright = 0;
        for (size_t i = 0; i < sizeof grid; ++i) bright += grid[i] >= 32;
        if (bright >= 52) {
            ++nonblack;
            if (hashes.count < 3) [hashes addObject:frameHash(grid)];
        }
    }
    if (reader.status != AVAssetReaderStatusCompleted) return metadata;
    NSMutableDictionary *result = [metadata mutableCopy];
    [result addEntriesFromDictionary:@{@"valid": @YES, @"decoded": @(frames), @"video_span": @(last - first),
                                      @"nonblack": @(nonblack), @"frame_hashes": hashes}];
    return result;
}
#pragma clang diagnostic pop
