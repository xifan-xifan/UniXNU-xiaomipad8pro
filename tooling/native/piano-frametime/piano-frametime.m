// SPDX-License-Identifier: MIT
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#import "display_evidence.h"
#include "frametime_stats.h"
#include "panel_match.h"
#import "scene.h"

typedef struct {
    unsigned fps, seconds, load;
    bool mainDisplay;
} Options;

typedef struct {
    double tick, linkTime, target, submit, gpuStart, gpuEnd, completed, presented;
    MTLCommandBufferStatus gpuStatus;
    bool presentCallback;
} Frame;

typedef struct {
    float time, aspect;
    uint32_t iterations;
} Scene;

static const double warmupSeconds = 3, drainSeconds = 2;

static id number(double value)
{
    return isfinite(value) && value > 0 ? @(value) : NSNull.null;
}

static BOOL gui_session(void)
{
    NSDictionary *info = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return getuid() && getuid() == geteuid() &&
        [info[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] &&
        [info[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue] &&
        [info[(__bridge NSString *)kCGSessionUserIDKey] unsignedIntValue] == getuid();
}

static CGDirectDisplayID screen_id(NSScreen *screen)
{
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

typedef struct { size_t width, height; } PixelSize;

static PixelSize display_pixels(CGDirectDisplayID display)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode)
        return (PixelSize){0, 0};
    // Use backing pixels, as active_mode does, including on a HiDPI mode.
    const PixelSize pixels = {CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode)};
    CGDisplayModeRelease(mode);
    return pixels;
}

static NSScreen *select_screen(bool mainDisplay)
{
    NSDictionary *driver = mainDisplay ? @{} : ft_gop_properties();
    const bool verified = [driver[@"LegacyManufacturerID"] unsignedIntValue] == FT_PANEL_VENDOR &&
        [driver[@"ProductID"] unsignedIntValue] == FT_PANEL_PRODUCT;
    NSScreen *selected = nil;
    unsigned best = 0, matches = 0;
    for (NSScreen *screen in NSScreen.screens) {
        const CGDirectDisplayID display = screen_id(screen);
        if (mainDisplay) {
            if (display == CGMainDisplayID())
                return screen;
            continue;
        }
        const PixelSize pixels = display_pixels(display);
        const unsigned rank = ft_panel_match(CGDisplayVendorNumber(display), CGDisplayModelNumber(display),
            (unsigned)pixels.width, (unsigned)pixels.height, CGDisplayIsBuiltin(display),
            verified, [driver[@"NativeFormatHorizontalPixels"] unsignedIntValue],
            [driver[@"NativeFormatVerticalPixels"] unsignedIntValue], [driver[@"builtin"] boolValue],
            [driver[@"panel_product_name"] boolValue] && [screen.localizedName isEqual:@"Xiaomi Pad 8 Pro"]);
        if (rank > best) {
            best = rank;
            matches = 0;
            selected = screen;
        }
        if (rank && rank == best)
            ++matches;
    }
    return matches == 1 ? selected : nil;
}

static NSDictionary *display_identity(CGDirectDisplayID display)
{
    const PixelSize pixels = display_pixels(display);
    return @{@"vendor": @(CGDisplayVendorNumber(display)), @"product": @(CGDisplayModelNumber(display)),
             @"builtin": @(CGDisplayIsBuiltin(display)), @"main": @(display == CGMainDisplayID()),
             @"pixel_width": @(pixels.width), @"pixel_height": @(pixels.height)};
}

static NSDictionary *display_inventory(void)
{
    NSMutableArray *displays = [NSMutableArray array];
    for (NSScreen *screen in NSScreen.screens)
        [displays addObject:display_identity(screen_id(screen))];
    return @{@"displays": displays, @"native_gop": ft_gop_properties(),
             @"panel_match_available": @(select_screen(false) != nil)};
}

static NSDictionary *active_mode(CGDirectDisplayID display)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode)
        return @{};
    NSDictionary *result = @{
        @"mode_id": @(CGDisplayModeGetIODisplayModeID(mode)),
        @"reported_hz": number(CGDisplayModeGetRefreshRate(mode)),
        @"pixel_width": @(CGDisplayModeGetPixelWidth(mode)),
        @"pixel_height": @(CGDisplayModeGetPixelHeight(mode))
    };
    CGDisplayModeRelease(mode);
    return result;
}

static NSDictionary *stats_json(const FtStats *stats, double start, double end)
{
    NSMutableArray *histogram = [NSMutableArray arrayWithCapacity:FT_HISTOGRAM_BINS];
    for (size_t i = 0; i < FT_HISTOGRAM_BINS; ++i) {
        id upper = i < FT_HISTOGRAM_BINS - 1 ? @(ft_histogram_upper_ms[i]) : NSNull.null;
        [histogram addObject:@{@"upper_ms": upper, @"count": @(stats->histogram[i])}];
    }
    return @{
        @"unique": @(stats->unique), @"missing": @(stats->missing),
        @"outside_window": @(stats->outside), @"duplicates": @(stats->duplicates),
        @"intervals": @(stats->intervals), @"span_s": @(stats->span_s),
        @"first_s": number(stats->first_s), @"last_s": number(stats->last_s),
        @"leading_gap_ms": stats->unique ? @((stats->first_s - start) * 1000) : (id)NSNull.null,
        @"trailing_gap_ms": stats->unique ? @((end - stats->last_s) * 1000) : (id)NSNull.null,
        @"cadence_hz": number(stats->cadence_hz), @"window_hz": @(stats->window_hz),
        @"p50_ms": number(stats->p50_ms), @"p95_ms": number(stats->p95_ms),
        @"p99_ms": number(stats->p99_ms), @"max_ms": number(stats->max_ms),
        @"missed_vsync": @(stats->missed_vsync),
        @"non_single_period_intervals": @(stats->non_single_period),
        @"off_grid_intervals": @(stats->off_grid), @"histogram": histogram
    };
}

@interface Probe : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithOptions:(Options)options;
@end

@implementation Probe {
    Options _options;
    NSWindow *_window;
    CADisplayLink *_link;
    CAMetalLayer *_layer;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLDepthStencilState> _depthState;
    id<MTLTexture> _depth;
    id _activity;
    CGDirectDisplayID _display;
    NSDictionary *_modeBefore, *_gopBefore, *_snapshotBefore;
    double _animationStart, _start, _end, _stopped, _startUnix, _stoppedUnix;
    Frame *_frames;
    size_t _count, _capacity;
    bool _overflow, _visibilityLost, _modeChanged;
}

- (instancetype)initWithOptions:(Options)options
{
    self = [super init];
    if (self)
        _options = options;
    return self;
}

- (void)failSetup:(const char *)reason error:(NSError *)error
{
    fprintf(stderr, "piano-frametime: %s\n", reason);
    if (error)
        fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    exit(1);
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    NSScreen *screen = select_screen(_options.mainDisplay);
    if (!screen) {
        for (NSScreen *candidate in NSScreen.screens) {
            const CGDirectDisplayID display = screen_id(candidate);
            const PixelSize pixels = display_pixels(display);
            fprintf(stderr, "piano-frametime: candidate vendor=0x%x product=0x%x builtin=%u pixels=%zux%zu\n",
                    CGDisplayVendorNumber(display), CGDisplayModelNumber(display), CGDisplayIsBuiltin(display),
                    pixels.width, pixels.height);
        }
        [self failSetup:"requested display is unavailable in the GUI session" error:nil];
    }
    _display = screen_id(screen);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    _queue = [device newCommandQueue];
    if (!_queue)
        [self failSetup:"Metal device or command queue unavailable" error:nil];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:sceneSource options:nil error:&error];
    if (!library)
        [self failSetup:"could not compile the 3D scene" error:error];
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@"cube_vertex"];
    pipeline.fragmentFunction = [library newFunctionWithName:@"cube_fragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    _pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!_pipeline)
        [self failSetup:"could not create the Metal pipeline" error:error];
    MTLDepthStencilDescriptor *depthState = [MTLDepthStencilDescriptor new];
    depthState.depthWriteEnabled = YES;
    depthState.depthCompareFunction = MTLCompareFunctionLess;
    _depthState = [device newDepthStencilStateWithDescriptor:depthState];

    const NSRect area = screen.visibleFrame;
    const NSRect rect = NSInsetRect(area, area.size.width / 8, area.size.height / 8);
    _window = [[NSWindow alloc] initWithContentRect:rect styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered defer:NO];
    _window.delegate = self;
    _window.title = @"Piano Frame Time";
    NSView *view = _window.contentView;
    _layer = [CAMetalLayer layer];
    _layer.device = device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer.maximumDrawableCount = 3;
    _layer.displaySyncEnabled = YES;
    _layer.allowsNextDrawableTimeout = YES;
    _layer.opaque = YES;
    _layer.contentsScale = screen.backingScaleFactor;
    const NSSize pixels = [view convertSizeToBacking:view.bounds.size];
    _layer.drawableSize = CGSizeMake(pixels.width, pixels.height);
    MTLTextureDescriptor *depth = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:(NSUInteger)pixels.width height:(NSUInteger)pixels.height mipmapped:NO];
    depth.storageMode = MTLStorageModePrivate;
    depth.usage = MTLTextureUsageRenderTarget;
    _depth = [device newTextureWithDescriptor:depth];
    if (!_depth || !_depthState)
        [self failSetup:"could not allocate the depth buffer or state" error:nil];
    view.layer = _layer;
    view.wantsLayer = YES;

    _capacity = (size_t)(_options.seconds + 1) * 240;
    _frames = calloc(_capacity, sizeof(*_frames));
    if (!_frames)
        [self failSetup:"could not allocate the frame records" error:nil];
    _link = [view displayLinkWithTarget:self selector:@selector(tick:)];
    if (!_link)
        [self failSetup:"display link unavailable for the window" error:nil];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(_options.fps, _options.fps, _options.fps);
    _activity = [NSProcessInfo.processInfo beginActivityWithOptions:
                  NSActivityUserInitiated | NSActivityIdleDisplaySleepDisabled reason:@"Piano frame time measurement"];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activate];
    _animationStart = CACurrentMediaTime();
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    [self scheduleAfter:warmupSeconds action:@selector(beginMeasurement:)];
}

- (void)scheduleAfter:(double)delay action:(SEL)action
{
    NSTimer *timer = [NSTimer timerWithTimeInterval:delay target:self selector:action userInfo:nil repeats:NO];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
}

- (void)beginMeasurement:(NSTimer *)timer
{
    (void)timer;
    _modeBefore = active_mode(_display);
    _gopBefore = _options.mainDisplay ? @{} : ft_gop_properties();
    _snapshotBefore = _options.mainDisplay ? @{@"status": @"not_requested"} : ft_read_snapshot();
    _visibilityLost = !(_window.occlusionState & NSWindowOcclusionStateVisible);
    _modeChanged = screen_id(_window.screen) != _display;
    _start = CACurrentMediaTime();
    _startUnix = NSDate.date.timeIntervalSince1970;
    _end = _start + _options.seconds;
    [self scheduleAfter:_options.seconds action:@selector(stopMeasurement:)];
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification
{
    (void)notification;
    if (_start && !_stopped && !(_window.occlusionState & NSWindowOcclusionStateVisible))
        _visibilityLost = true;
}

- (void)applicationDidChangeScreenParameters:(NSNotification *)notification
{
    (void)notification;
    if (_start && !_stopped &&
        (screen_id(_window.screen) != _display || ![_modeBefore isEqual:active_mode(_display)]))
        _modeChanged = true;
}

- (void)tick:(CADisplayLink *)link
{
    @autoreleasepool {
        const double now = CACurrentMediaTime();
        const bool measuring = _start > 0;
        size_t index = 0;
        if (measuring) {
            if (now >= _end) {
                [self stopMeasurement:nil];
                return;
            }
            if (_count == _capacity) {
                _overflow = true;
                [self stopMeasurement:nil];
                return;
            }
            index = _count++;
            _frames[index] = (Frame){.tick = now, .linkTime = link.timestamp, .target = link.targetTimestamp};
        }

        id<CAMetalDrawable> drawable = [_layer nextDrawable];
        if (!drawable || (measuring && CACurrentMediaTime() >= _end))
            return;
        if (drawable.texture.width != _depth.width || drawable.texture.height != _depth.height) {
            if (measuring)
                _modeChanged = true;
            return;
        }
        id<MTLCommandBuffer> command = [_queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.018, 0.027, 0.05, 1);
        pass.depthAttachment.texture = _depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder)
            return;
        const Scene scene = {
            .time = (float)(now - _animationStart),
            .aspect = (float)drawable.texture.width / drawable.texture.height,
            .iterations = _options.load
        };
        [encoder setRenderPipelineState:_pipeline];
        [encoder setDepthStencilState:_depthState];
        [encoder setVertexBytes:&scene length:sizeof(scene) atIndex:0];
        [encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36 instanceCount:12];
        [encoder endEncoding];
        if (measuring) {
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
                const double presented = shown.presentedTime;
                dispatch_async(dispatch_get_main_queue(), ^{
                    self->_frames[index].presented = presented;
                    self->_frames[index].presentCallback = true;
                });
            }];
            [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
                const double completed = CACurrentMediaTime();
                const double gpuStart = finished.GPUStartTime, gpuEnd = finished.GPUEndTime;
                const MTLCommandBufferStatus status = finished.status;
                dispatch_async(dispatch_get_main_queue(), ^{
                    Frame *frame = &self->_frames[index];
                    frame->gpuStart = gpuStart;
                    frame->gpuEnd = gpuEnd;
                    frame->completed = completed;
                    frame->gpuStatus = status;
                });
            }];
        }
        [command presentDrawable:drawable];
        if (measuring)
            _frames[index].submit = CACurrentMediaTime();
        [command commit];
    }
}

- (void)stopMeasurement:(NSTimer *)timer
{
    if (_stopped)
        return;
    const double now = CACurrentMediaTime();
    if (timer && now < _end) {
        [self scheduleAfter:_end - now action:@selector(stopMeasurement:)];
        return;
    }
    _stopped = now;
    _stoppedUnix = NSDate.date.timeIntervalSince1970;
    [_link invalidate];
    _link = nil;
    [self scheduleAfter:drainSeconds action:@selector(finish:)];
}

- (void)finish:(NSTimer *)timer
{
    (void)timer;
    NSDictionary *snapshotAfter = _options.mainDisplay ? @{@"status": @"not_requested"} : ft_read_snapshot();
    NSDictionary *gopAfter = _options.mainDisplay ? @{} : ft_gop_properties();
    NSDictionary *modeAfter = active_mode(_display);
    _modeChanged |= ![_modeBefore isEqual:modeAfter] || screen_id(_window.screen) != _display;
    _visibilityLost |= !(_window.occlusionState & NSWindowOcclusionStateVisible);
    [_window orderOut:nil];
    [NSProcessInfo.processInfo endActivity:_activity];

    const double end = fmin(_end, _stopped);
    double *presents = malloc(_capacity * sizeof(*presents));
    double *targets = malloc(_capacity * sizeof(*targets));
    if (!presents || !targets)
        [self failSetup:"could not allocate the timestamp analysis" error:nil];
    NSMutableArray *frames = [NSMutableArray arrayWithCapacity:_count];
    size_t submitted = 0, completed = 0, gpuErrors = 0, missingGpuEnd = 0;
    size_t missingPresentCallback = 0, zeroPresentedTime = 0;
    for (size_t i = 0; i < _count; ++i) {
        const Frame f = _frames[i];
        targets[i] = f.target;
        if (f.submit > 0) {
            presents[submitted++] = f.presented;
            completed += f.gpuStatus == MTLCommandBufferStatusCompleted;
            gpuErrors += f.gpuStatus == MTLCommandBufferStatusError;
            missingGpuEnd += f.gpuEnd <= 0;
            missingPresentCallback += !f.presentCallback;
            zeroPresentedTime += f.presentCallback && !(f.presented > 0);
        }
        // Omit device identity and user/session data. Field names are recorded once below.
        [frames addObject:@[number(f.tick), number(f.linkTime), number(f.target), number(f.submit),
                            number(f.gpuStart), number(f.gpuEnd), number(f.completed), number(f.presented),
                            @(f.presentCallback), f.completed > 0 ? @(f.gpuStatus) : (id)NSNull.null]];
    }

    FtStats presented, target;
    if (!ft_analyze(presents, submitted, _start, end, _options.fps, &presented) ||
        !ft_analyze(targets, _count, _start, end, _options.fps, &target))
        [self failSetup:"could not analyze the measurement window" error:nil];
    free(presents);
    free(targets);

    id modeHz = _modeBefore[@"reported_hz"];
    const double modeRate = [modeHz isKindOfClass:NSNumber.class] ? [modeHz doubleValue] : 0;
    const bool matchingMode = fabs(modeRate - _options.fps) <= _options.fps * 0.01;
    const bool valid = ft_capture_valid(&presented, _stopped >= _end && !_overflow,
        !_visibilityLost && !_modeChanged && _modeBefore.count, submitted, completed, gpuErrors);
    NSDictionary *report = @{
        @"schema_version": @1, @"capture_valid": @(valid),
        @"display": display_identity(_display),
        @"config": @{@"preferred_fps": @(_options.fps), @"seconds": @(_options.seconds), @"load": @(_options.load),
                     @"main_display": @(_options.mainDisplay), @"warmup_s": @(warmupSeconds), @"drain_s": @(drainSeconds)},
        @"window": @{@"start_s": @(_start), @"end_s": @(end), @"duration_s": @(end - _start),
                     @"stopped_s": @(_stopped), @"start_unix_s": @(_startUnix), @"stopped_unix_s": @(_stoppedUnix),
                     @"visibility_lost": @(_visibilityLost), @"sample_limit_hit": @(_overflow)},
        @"mode": @{@"before": _modeBefore, @"after": modeAfter, @"changed": @(_modeChanged),
                   @"matches_preferred_fps": @(matchingMode),
                   @"presented_percent_of_reported_hz": modeRate ? number(100 * presented.cadence_hz / modeRate) : NSNull.null},
        @"drawable": @{@"width": @(_depth.width), @"height": @(_depth.height)},
        @"counts": @{@"display_link_callbacks": @(_count), @"submitted": @(submitted),
                     @"ticks_without_submit": @(_count - submitted), @"gpu_completed": @(completed),
                     @"gpu_errors": @(gpuErrors), @"missing_gpu_end_time": @(missingGpuEnd),
                     @"missing_present_callback": @(missingPresentCallback), @"zero_presented_time": @(zeroPresentedTime)},
        @"presented": stats_json(&presented, _start, end),
        @"display_link_target": stats_json(&target, _start, end),
        @"kernel": @{@"scanout": ft_scanout_report(_snapshotBefore, snapshotAfter),
                     @"native_gop_before": _gopBefore, @"native_gop_after": gopAfter},
        @"frame_columns": @[@"display_link_callback_s", @"display_link_timestamp_s", @"target_timestamp_s", @"cpu_submit_s",
                            @"gpu_start_s", @"gpu_end_s", @"gpu_completion_callback_s", @"presented_s",
                            @"present_callback_seen", @"gpu_command_status"],
        @"frames": frames
    };
    const BOOL wrote = ft_write_json(report);
    fprintf(stderr, "piano-frametime: %s, %.3f s, preferred %u fps, CoreGraphics %.3f Hz (%s)\n",
            valid && wrote ? "capture complete" : "capture invalid", end - _start, _options.fps, modeRate,
            matchingMode ? "matches" : modeRate ? "differs" : "unknown");
    if (presented.intervals) {
        fprintf(stderr, "presented %.3f Hz (%.3f Hz over full window), %zu distinct, %llu missed vsyncs, "
                "p50/p95/p99/max %.3f/%.3f/%.3f/%.3f ms, %zu off-grid intervals\n",
                presented.cadence_hz, presented.window_hz, presented.unique,
                (unsigned long long)presented.missed_vsync, presented.p50_ms, presented.p95_ms,
                presented.p99_ms, presented.max_ms, presented.off_grid);
        fputs("presented interval histogram (upper ms=count):", stderr);
        for (size_t i = 0; i < FT_HISTOGRAM_BINS - 1; ++i)
            fprintf(stderr, " %.0f=%zu", ft_histogram_upper_ms[i], presented.histogram[i]);
        fprintf(stderr, " >100=%zu\n", presented.histogram[FT_HISTOGRAM_BINS - 1]);
    } else {
        fputs("presented cadence unavailable\n", stderr);
    }
    exit(valid && wrote ? 0 : 1);
}
@end

static void usage(FILE *stream)
{
    fputs("usage: piano-frametime [--fps 60|90|120|144] [--seconds 60..600] [--load 0..512] [--main-display]\n"
          "       piano-frametime --kernel-report BEFORE AFTER [KERNEL_LOG]\n"
          "       piano-frametime --list-displays\n"
          "       piano-frametime --help\n", stream);
}

static bool read_uint(const char *text, unsigned low, unsigned high, unsigned *value)
{
    if (!*text || strspn(text, "0123456789") != strlen(text))
        return false;
    errno = 0;
    const unsigned long parsed = strtoul(text, NULL, 10);
    if (errno || parsed < low || parsed > high)
        return false;
    *value = (unsigned)parsed;
    return true;
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            usage(stdout);
            return 0;
        }
        if ((argc == 4 || argc == 5) && !strcmp(argv[1], "--kernel-report"))
            return ft_kernel_report(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
        if (argc == 2 && !strcmp(argv[1], "--list-displays")) {
            if (!gui_session()) {
                fputs("piano-frametime: launch as the logged-in user in the console GUI session\n", stderr);
                return 1;
            }
            return ft_write_json(display_inventory()) ? 0 : 1;
        }
        Options options = {.fps = 60, .seconds = 65};
        for (int i = 1; i < argc; ++i) {
            const char *arg = argv[i];
            if (!strcmp(arg, "--main-display")) {
                options.mainDisplay = true;
                continue;
            }
            if (i + 1 < argc) {
                if ((!strcmp(arg, "--fps") && read_uint(argv[i + 1], 60, 144, &options.fps) &&
                     (options.fps == 60 || options.fps == 90 || options.fps == 120 || options.fps == 144)) ||
                    (!strcmp(arg, "--seconds") && read_uint(argv[i + 1], 60, 600, &options.seconds)) ||
                    (!strcmp(arg, "--load") && read_uint(argv[i + 1], 0, 512, &options.load))) {
                    ++i;
                    continue;
                }
            }
            usage(stderr);
            return 2;
        }
        if (!gui_session()) {
            fputs("piano-frametime: launch as the logged-in user in the console GUI session\n", stderr);
            return 1;
        }
        NSApplication *app = NSApplication.sharedApplication;
        __attribute__((objc_precise_lifetime)) Probe *probe = [[Probe alloc] initWithOptions:options];
        app.delegate = probe;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 1;
}
