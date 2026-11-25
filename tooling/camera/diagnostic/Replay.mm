// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "Replay.h"
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <limits.h>

struct NBTextureDescriptor {
    uint64_t type, format, width, height, depth, levels, samples, arrayLength, usage, options,
             rotation, framebufferOnly, drawable;
};

@interface NBReplayBuffer : NSObject
@property(nonatomic) void *handle;
@end

@interface NBReplayTexture : NSObject
@property(nonatomic) void *handle;
@property(nonatomic) NBTextureDescriptor descriptor;
@property(nonatomic, strong) id<MTLBuffer> buffer;
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) NSUInteger offset, rowBytes;
@end

using CreateSurface = void *(*)(void *, const NBTextureDescriptor *, uint64_t, uint64_t, uint32_t,
                                uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using DescribeTexture = void (*)(void *, NBTextureDescriptor *);
typedef id (*SurfaceIMP)(id, SEL, MTLTextureDescriptor *, IOSurfaceRef, NSUInteger) NS_RETURNS_RETAINED;
static SurfaceIMP OriginalSurface;
static CreateSurface Create;
static DescribeTexture Describe;
static Class TextureClass;
static NSObject *ImportLock;
static IOSurfaceID SourceID;
static id<MTLBuffer> CopiedAllocation;
static NSMutableSet<NSString *> *SourceDescriptors;
static NSMutableSet<NSNumber *> *SourcePlanes;
static NSUInteger SourceFailures, Other420v;

static id TraceSurface(id dev, SEL sel, MTLTextureDescriptor *desc, IOSurfaceRef surface,
                       NSUInteger plane) NS_RETURNS_RETAINED;
static id TraceSurface(id dev, SEL sel, MTLTextureDescriptor *desc, IOSurfaceRef surface, NSUInteger plane)
{
    @synchronized (ImportLock) {
        if (SourceID && IOSurfaceGetID(surface) != SourceID &&
            IOSurfaceGetPixelFormat(surface) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ++Other420v;
        if (!SourceID || IOSurfaceGetID(surface) != SourceID)
            return OriginalSurface(dev, sel, desc, surface, plane);
        if (plane >= IOSurfaceGetPlaneCount(surface)) {
            ++SourceFailures;
            return nil;
        }
        uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
        size_t offset = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, plane) - base;
        size_t row = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
        size_t chroma = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, 1) - base;
        size_t chromaRow = IOSurfaceGetBytesPerRowOfPlane(surface, 1);
        NBTextureDescriptor d = {desc.textureType, desc.pixelFormat, desc.width, desc.height,
            desc.depth, desc.mipmapLevelCount, desc.sampleCount, desc.arrayLength, desc.usage,
            desc.resourceOptions, 0, 0, 0};
        NSString *signature = [NSString stringWithFormat:
            @"plane=%lu format=%lu %lux%lu depth=%lu type=%lu levels=%lu samples=%lu array=%lu "
             "usage=%lu options=%lu offset=%zu row=%zu uvOffset=%zu uvRow=%zu",
            plane, desc.pixelFormat, desc.width, desc.height, desc.depth, desc.textureType, desc.mipmapLevelCount,
            desc.sampleCount, desc.arrayLength, desc.usage, desc.resourceOptions, offset, row,
            chroma, chromaRow];
        [SourceDescriptors addObject:signature];
        [SourcePlanes addObject:@(plane)];
        printf("SOURCE_ARM_%s %s surface=%u\n", CopiedAllocation ? "B" : "A", signature.UTF8String, SourceID);
        if (!CopiedAllocation) {
            id tex = OriginalSurface(dev, sel, desc, surface, plane);
            if (!tex) ++SourceFailures;
            return tex;
        }
        void *handle = Create([(NBReplayBuffer *)CopiedAllocation handle], &d, offset, row,
            IOSurfaceGetPixelFormat(surface), plane, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
            chroma, chromaRow);
        if (!handle) {
            ++SourceFailures;
            return nil;
        }
        NBReplayTexture *tex = [TextureClass new];
        Describe(handle, &d);
        tex.handle = handle;
        tex.descriptor = d;
        tex.buffer = CopiedAllocation;
        tex.offset = offset;
        tex.rowBytes = row;
        tex.surface = (IOSurfaceRef)CFRetain(surface);
        return tex;
    }
}

typedef id<MTLLibrary> (*LibraryDataIMP)(id, SEL, dispatch_data_t, NSError **) NS_RETURNS_RETAINED;
typedef id<MTLLibrary> (*LibrarySourceIMP)(id, SEL, NSString *, MTLCompileOptions *, NSError **) NS_RETURNS_RETAINED;
typedef id<MTLLibrary> (*LibraryStitchedIMP)(id, SEL, MTLStitchedLibraryDescriptor *, NSError **) NS_RETURNS_RETAINED;
static LibraryDataIMP OriginalData;
static LibrarySourceIMP OriginalSource;
static LibraryStitchedIMP OriginalStitched;

static id<MTLLibrary> TraceData(id dev, SEL sel, dispatch_data_t bytes, NSError **error) NS_RETURNS_RETAINED;
static id<MTLLibrary> TraceData(id dev, SEL sel, dispatch_data_t bytes, NSError **error)
{
    const void *data = NULL;
    size_t count = 0;
    dispatch_data_t mapped = bytes ? dispatch_data_create_map(bytes, &data, &count) : nil;
    NSString *sha = mapped ? SHA256([NSData dataWithBytes:data length:count]) : @"absent";
    printf("LIBRARY_DATA_BEGIN bytes=%zu sha256=%s\n", count, sha.UTF8String);
    id<MTLLibrary> library = OriginalData(dev, sel, bytes, error);
    printf("LIBRARY_DATA_END ok=%d functions=%s\n", library != nil,
           [library.functionNames componentsJoinedByString:@","].UTF8String ?: "-");
    return library;
}

static id<MTLLibrary> TraceSource(id dev, SEL sel, NSString *source,
                                 MTLCompileOptions *options, NSError **error) NS_RETURNS_RETAINED;
static id<MTLLibrary> TraceSource(id dev, SEL sel, NSString *source, MTLCompileOptions *options, NSError **error)
{
    NSData *bytes = [source dataUsingEncoding:NSUTF8StringEncoding];
    printf("LIBRARY_SOURCE_BEGIN bytes=%lu sha256=%s language=%lu\n",
           bytes.length, SHA256(bytes).UTF8String, options.languageVersion);
    id<MTLLibrary> library = OriginalSource(dev, sel, source, options, error);
    printf("LIBRARY_SOURCE_END ok=%d functions=%s\n", library != nil,
           [library.functionNames componentsJoinedByString:@","].UTF8String ?: "-");
    return library;
}

static id<MTLLibrary> TraceStitched(id dev, SEL sel, MTLStitchedLibraryDescriptor *desc,
                                   NSError **error) NS_RETURNS_RETAINED;
static id<MTLLibrary> TraceStitched(id dev, SEL sel, MTLStitchedLibraryDescriptor *desc, NSError **error)
{
    for (MTLFunctionStitchingGraph *graph in desc.functionGraphs)
        printf("LIBRARY_STITCH name=%s nodes=%lu\n", graph.functionName.UTF8String, graph.nodes.count);
    id<MTLLibrary> library = OriginalStitched(dev, sel, desc, error);
    printf("LIBRARY_STITCH_END ok=%d functions=%s\n", library != nil,
           [library.functionNames componentsJoinedByString:@","].UTF8String ?: "-");
    return library;
}

typedef id (*ComputeIMP)(id, SEL, MTLComputePipelineDescriptor *, MTLPipelineOption, id *, NSError **) NS_RETURNS_RETAINED;
typedef id (*RenderIMP)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption, id *, NSError **) NS_RETURNS_RETAINED;
static ComputeIMP OriginalCompute;
static RenderIMP OriginalRender;
static id TraceCompute(id dev, SEL sel, MTLComputePipelineDescriptor *desc, MTLPipelineOption opt,
                       id *reflection, NSError **error) NS_RETURNS_RETAINED;
static id TraceCompute(id dev, SEL sel, MTLComputePipelineDescriptor *desc, MTLPipelineOption opt,
                       id *reflection, NSError **error)
{
    printf("COMPUTE_PIPELINE label=%s function=%s options=%lu\n", desc.label.UTF8String ?: "-",
           desc.computeFunction.name.UTF8String ?: "-", opt);
    return OriginalCompute(dev, sel, desc, opt, reflection, error);
}
static id TraceRender(id dev, SEL sel, MTLRenderPipelineDescriptor *desc, MTLPipelineOption opt,
                      id *reflection, NSError **error) NS_RETURNS_RETAINED;
static id TraceRender(id dev, SEL sel, MTLRenderPipelineDescriptor *desc, MTLPipelineOption opt,
                      id *reflection, NSError **error)
{
    printf("RENDER_PIPELINE label=%s vertex=%s fragment=%s color0=%lu options=%lu\n",
           desc.label.UTF8String ?: "-", desc.vertexFunction.name.UTF8String ?: "-",
           desc.fragmentFunction.name.UTF8String ?: "-", desc.colorAttachments[0].pixelFormat, opt);
    return OriginalRender(dev, sel, desc, opt, reflection, error);
}

using DispatchIMP = void (*)(id, SEL, MTLSize, MTLSize);
static DispatchIMP OriginalGroups, OriginalThreads;
static void TraceDispatch(id encoder, SEL sel, MTLSize grid, MTLSize group)
{
    const BOOL threads = sel == @selector(dispatchThreads:threadsPerThreadgroup:);
    printf("DISPATCH %s grid=%lux%lux%lu group=%lux%lux%lu\n", threads ? "threads" : "groups",
           grid.width, grid.height, grid.depth, group.width, group.height, group.depth);
    (threads ? OriginalThreads : OriginalGroups)(encoder, sel, grid, group);
}

using DrawIMP = void (*)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger, NSUInteger, NSUInteger);
static DrawIMP OriginalDraw;
static void TraceDraw(id encoder, SEL sel, MTLPrimitiveType type, NSUInteger start, NSUInteger count,
                      NSUInteger instances, NSUInteger base)
{
    printf("DRAW type=%lu vertexStart=%lu vertices=%lu instances=%lu baseInstance=%lu\n",
           type, start, count, instances, base);
    OriginalDraw(encoder, sel, type, start, count, instances, base);
}

template<class T> static void Observe(Class cls, SEL sel, T hook, T &original)
{
    Method m = cls ? class_getInstanceMethod(cls, sel) : NULL;
    if (m) original = (T)method_setImplementation(m, (IMP)hook);
    printf("TRACE_HOOK %s %s\n", sel_getName(sel), m ? "installed" : "absent");
}

@implementation Replay {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    BOOL _unusable;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device pluginPath:(NSString *)path
{
    if (!(self = [super init])) return nil;
    if (!device || OriginalSurface) return nil;
    Class cls = object_getClass(device);
    Method method = class_getInstanceMethod(cls, @selector(newTextureWithDescriptor:iosurface:plane:));
    Dl_info image = {};
    char implemented[PATH_MAX], expected[PATH_MAX];
    if (!method || !dladdr((void *)method_getImplementation(method), &image) ||
        !realpath(image.dli_fname, implemented) || !realpath(path.fileSystemRepresentation, expected) ||
        strcmp(implemented, expected)) {
        printf("REPLAY_GATE device=%s provider=unexpected\n", class_getName(cls));
        return nil;
    }
    void *bundle = dlopen(implemented, RTLD_NOLOAD | RTLD_LAZY);
    if (!bundle) return nil;
    Create = (CreateSurface)dlsym(bundle, "nmb_texture_new_on_surface");
    Describe = (DescribeTexture)dlsym(bundle, "nmb_texture_describe");
    TextureClass = NSClassFromString(@"NBTexture");
    if (!Create || !Describe || !TextureClass) return nil;
    _device = device;
    _queue = [device newCommandQueue];
    if (!_queue) return nil;
    ImportLock = [NSObject new];
    Observe(cls, @selector(newTextureWithDescriptor:iosurface:plane:), TraceSurface, OriginalSurface);
    Observe(cls, @selector(newLibraryWithData:error:), TraceData, OriginalData);
    Observe(cls, @selector(newLibraryWithSource:options:error:), TraceSource, OriginalSource);
    Observe(cls, @selector(newLibraryWithStitchedDescriptor:error:), TraceStitched, OriginalStitched);
    Observe(cls, @selector(newComputePipelineStateWithDescriptor:options:reflection:error:), TraceCompute, OriginalCompute);
    Observe(cls, @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:), TraceRender, OriginalRender);
    Class compute = NSClassFromString(@"NBComputeEncoder"), render = NSClassFromString(@"NBRenderEncoder");
    Observe(compute, @selector(dispatchThreadgroups:threadsPerThreadgroup:), TraceDispatch, OriginalGroups);
    Observe(compute, @selector(dispatchThreads:threadsPerThreadgroup:), TraceDispatch, OriginalThreads);
    Observe(render, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:), TraceDraw, OriginalDraw);
    printf("REPLAY_GATE device=%s provider=exact-seed-plugin\n", class_getName(cls));
    return self;
}

static BOOL Clear(id<MTLCommandBuffer> cb, id<MTLTexture> tex, MTLClearColor color)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = tex;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = color;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc endEncoding];
    return enc != nil;
}

static NSData *PNGRowOrder(NSData *raw)
{
    // With this CI render API the Metal staging rows run from the CI bottom.
    if (raw.length != RGBRow * FrameHeight) return nil;
    NSMutableData *image = [NSMutableData dataWithLength:raw.length];
    const uint8_t *src = (const uint8_t *)raw.bytes;
    uint8_t *dest = (uint8_t *)image.mutableBytes;
    for (unsigned y = 0; y < FrameHeight; y++)
        memcpy(dest + y * RGBRow, src + (FrameHeight - 1 - y) * RGBRow, RGBRow);
    return image;
}

static BOOL Stage(id<MTLCommandBuffer> cb, id<MTLTexture> tex, id<MTLBuffer> buffer)
{
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(FrameWidth, FrameHeight, 1) toBuffer:buffer
        destinationOffset:0 destinationBytesPerRow:RGBRow destinationBytesPerImage:RGBRow * FrameHeight];
    [blit endEncoding];
    return blit != nil;
}

- (NSData *)render:(CVPixelBufferRef)pixel allocation:(NSData *)allocation
             prefix:(NSString *)prefix details:(NSMutableDictionary *)details
{
    if (_unusable) return nil;
    id<MTLBuffer> copied = nil;
    if (allocation) {
        copied = [_device newBufferWithLength:allocation.length options:MTLResourceStorageModeShared];
        if (!copied.contents || ![(id)copied respondsToSelector:@selector(handle)]) return nil;
        memcpy(copied.contents, allocation.bytes, allocation.length);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:FrameWidth height:FrameHeight mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [_device newTextureWithDescriptor:td];
    const size_t size = RGBRow * FrameHeight;
    id<MTLBuffer> control = [_device newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [_device newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    if (!texture || !control || !output || !cb) return nil;

    // A fresh destination and separate fresh GPU-written staging buffers in
    // every arm. The colored clear verifies the same blit/readback route.
    cb.label = prefix.lastPathComponent;
    if (!Clear(cb, texture, MTLClearColorMake(17 / 255., 101 / 255., 217 / 255., 1)) ||
        !Stage(cb, texture, control) || !Clear(cb, texture, MTLClearColorMake(0, 0, 0, 1))) return nil;
    @synchronized (ImportLock) {
        SourceID = IOSurfaceGetID(CVPixelBufferGetIOSurface(pixel));
        CopiedAllocation = copied;
        SourceDescriptors = [NSMutableSet set];
        SourcePlanes = [NSMutableSet set];
        SourceFailures = Other420v = 0;
    }
    BOOL submitted = NO, complete = NO;
    printf("REPLAY_BEGIN %s\n", cb.label.UTF8String);
    @try {
        // Create an independent context so Core Image cannot cache and reuse
        // source textures from the other arm.
        CIContext *ci = [CIContext contextWithMTLCommandQueue:_queue options:@{
            kCIContextCacheIntermediates: @NO, kCIContextName: @"PBRendererMetalContext",
            kCIContextHighQualityDownsample: @YES, kCIContextUseSoftwareRenderer: @NO}];
        if (!ci) return nil;
        CGRect bounds = CGRectMake(0, 0, FrameWidth, FrameHeight);
        CIImage *src = [CIImage imageWithCVPixelBuffer:pixel];
        src = [src imageByApplyingTransform:CGAffineTransformMake(-1, 0, 0, 1, FrameWidth, 0)];
        src = [src imageByCroppingToRect:bounds];
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        @try {
            [ci render:src toMTLTexture:texture commandBuffer:cb bounds:bounds colorSpace:color];
        } @finally {
            CGColorSpaceRelease(color);
        }
        if (!Stage(cb, texture, output)) return nil;
        dispatch_semaphore_t fence = dispatch_semaphore_create(0);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            (void)finished;
            dispatch_semaphore_signal(fence);
        }];
        [cb commit];
        submitted = YES;
        complete = dispatch_semaphore_wait(fence, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) == 0;
    } @catch (NSException *e) {
        fprintf(stderr, "REPLAY_EXCEPTION %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
        details[@"exception"] = e.name;
    } @finally {
        @synchronized (ImportLock) {
            details[@"descriptors"] = [[SourceDescriptors allObjects] sortedArrayUsingSelector:@selector(compare:)];
            details[@"planes"] = [[SourcePlanes allObjects] sortedArrayUsingSelector:@selector(compare:)];
            details[@"importFailures"] = @(SourceFailures);
            details[@"other420vSurfaces"] = @(Other420v);
            SourceID = 0;
            CopiedAllocation = nil;
        }
    }
    if (submitted && !complete) _unusable = YES;
    details[@"commandStatus"] = @(cb.status);
    details[@"fenceCompleted"] = @(complete);
    if (cb.error) details[@"commandError"] = @{@"domain": cb.error.domain, @"code": @(cb.error.code)};
    if (!complete || cb.status != MTLCommandBufferStatusCompleted || !control.contents || !output.contents) return nil;
    size_t controlWrong = 0;
    const uint8_t *check = (const uint8_t *)control.contents;
    for (size_t i = 0; i < size; i += 4)
        controlWrong += check[i] != 217 || check[i + 1] != 101 || check[i + 2] != 17 || check[i + 3] != 255;
    details[@"controlPixels"] = @(size / 4);
    details[@"controlMismatch"] = @(controlWrong);
    NSData *data = [NSData dataWithBytes:output.contents length:size];
    BOOL raw = [data writeToFile:[prefix stringByAppendingString:@".bgra"] atomically:YES];
    NSData *pixels = PNGRowOrder(data);
    BOOL png = WritePNG(pixels, [prefix stringByAppendingString:@".png"]);
    if (controlWrong)
        [[NSData dataWithBytes:control.contents length:size]
            writeToFile:[prefix stringByAppendingString:@"-control.bgra"] atomically:YES];
    details[@"artifactsWritten"] = @(raw && png);
    printf("REPLAY_END %s fence=1 controlMismatch=%zu raw=%d png=%d\n", cb.label.UTF8String, controlWrong, raw, png);
    return pixels;
}

static BOOL ReadbackOK(NSDictionary *arm)
{
    return [arm[@"artifactsWritten"] boolValue] && [arm[@"fenceCompleted"] boolValue] &&
           [arm[@"controlMismatch"] isEqual:@0];
}

- (BOOL)runSample:(CMSampleBufferRef)sample evidence:(FrameEvidence *)frame
          prefix:(NSString *)prefix pattern:(BOOL)pattern
{
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel);
    NSMutableDictionary *a = [NSMutableDictionary dictionary], *b = [NSMutableDictionary dictionary];
    NSData *ai = [self render:pixel allocation:nil prefix:[prefix stringByAppendingString:@"-A"] details:a];
    uint32_t seedAfterA = IOSurfaceGetSeed(surface);
    NSData *bi = [self render:pixel allocation:frame.allocation prefix:[prefix stringByAppendingString:@"-B"] details:b];
    uint32_t seedAfterB = IOSurfaceGetSeed(surface);
    // Two plane views, or the plugin's combined 420v view (format 500).
    NSArray *planes = a[@"planes"];
    BOOL sawSource = [planes isEqual:@[@0, @1]] ||
        ([planes isEqual:@[@0]] && [a[@"descriptors"] count] == 1 &&
         [a[@"descriptors"][0] containsString:@"plane=0 format=500 "]);
    BOOL comparable = ReadbackOK(a) && ReadbackOK(b) && sawSource &&
        [planes isEqual:b[@"planes"]] && [a[@"descriptors"] isEqual:b[@"descriptors"]] &&
        [a[@"importFailures"] isEqual:@0] && [a[@"other420vSurfaces"] isEqual:@0] &&
        [b[@"importFailures"] isEqual:@0] && [b[@"other420vSurfaces"] isEqual:@0] &&
        seedAfterA == frame.seed && seedAfterB == frame.seed;
    NSMutableDictionary *result = [@{@"A": a, @"B": b, @"comparable": @(comparable),
        @"rawMetalRows": @"bottom first; PNG and CPU comparisons top first", @"snapshotSeed": @(frame.seed), @"seedAfterA": @(seedAfterA), @"seedAfterB": @(seedAfterB),
        @"A_vs_B": ComparePixels(ai, bi, NO), @"CPU_mirrored_vs_A": ComparePixels(frame.cpu, ai, YES),
        @"CPU_mirrored_vs_B": ComparePixels(frame.cpu, bi, YES)} mutableCopy];
    if (pattern) {
        result[@"CPU_pattern"] = @(frame.patternGood);
        if (frame.patternGood) {
            result[@"A_pattern"] = CheckPattern(ai, frame.patternFrame, YES);
            result[@"B_pattern"] = CheckPattern(bi, frame.patternFrame, YES);
        }
    }
    BOOL written = WriteJSON(result, [prefix stringByAppendingString:@"-replay.json"]);
    printf("PAIR %s comparable=%d metadata=%d\n", prefix.lastPathComponent.UTF8String, comparable, written);
    return comparable && written;
}
@end
