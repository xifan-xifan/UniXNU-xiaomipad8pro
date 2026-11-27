// SPDX-License-Identifier: MIT
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMediaTiming.h>
#import "scene.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static const unsigned windowSeconds = 10, warmupSeconds = 2, callbackGrace = 2, queueDepth = 3;
static const unsigned timeoutSeconds = 5;

typedef struct { unsigned seconds, width, height, draws; bool onscreen; } Options;

@interface Frame : NSObject {
@public
    size_t sequence;
    double begin, encodeBegin, submit, gpuStart, gpuEnd, completion, present, presentCallback;
    MTLCommandBufferStatus status;
    atomic_bool completed, presentReported;
}
@end

@implementation Frame
- (instancetype)init
{
    if ((self = [super init])) {
        atomic_init(&completed, false);
        atomic_init(&presentReported, false);
    }
    return self;
}
@end

@interface Capture : NSObject {
@public
    atomic_bool commandError;
    atomic_uint_fast64_t lastCompletionMs;
}
@end

@implementation Capture
- (instancetype)init
{
    if ((self = [super init])) {
        atomic_init(&commandError, false);
        atomic_init(&lastCompletionMs, 0);
    }
    return self;
}
@end

@interface WindowSamples : NSObject
@property(nonatomic, readonly) NSMutableArray<Frame *> *frames;
@property(nonatomic) unsigned frequencySamples, minLevel, maxLevel, maxMHz;
@end

@implementation WindowSamples
- (instancetype)init
{
    if ((self = [super init]))
        _frames = [NSMutableArray array];
    return self;
}
@end

static void usage(FILE *out)
{
    fprintf(out, "usage: piano-metal-sustain [--offscreen | --onscreen] [--seconds 1..86400 | --minutes 1..1440]\n"
                 "                           [--width 64..4096] [--height 64..4096] [--draws 1..1024]\n");
}

static bool parse_uint(const char *str, unsigned min, unsigned max, unsigned *value)
{
    char *end;
    errno = 0;
    const unsigned long parsed = strtoul(str, &end, 10);
    if (*str < '0' || *str > '9' || *end || errno || parsed < min || parsed > max)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool parse_options(int argc, const char *argv[], Options *options)
{
    *options = (Options){.seconds = 10, .width = 1920, .height = 1280, .draws = 256};
    bool modeSet = false, durationSet = false;
    for (int i = 1; i < argc; ++i) {
        const char *flag = argv[i];
        if (!strcmp(flag, "--offscreen") || !strcmp(flag, "--onscreen")) {
            if (modeSet)
                return false;
            options->onscreen = !strcmp(flag, "--onscreen");
            modeSet = true;
            continue;
        }
        if (++i == argc)
            return false;
        const char *value = argv[i];
        if (!strcmp(flag, "--seconds") || !strcmp(flag, "--minutes")) {
            const bool minutes = !strcmp(flag, "--minutes");
            if (durationSet || !parse_uint(value, 1, minutes ? 1440 : 86400, &options->seconds))
                return false;
            if (minutes)
                options->seconds *= 60;
            durationSet = true;
        } else if (!strcmp(flag, "--width")) {
            if (!parse_uint(value, 64, 4096, &options->width))
                return false;
        } else if (!strcmp(flag, "--height")) {
            if (!parse_uint(value, 64, 4096, &options->height))
                return false;
        } else if (!strcmp(flag, "--draws")) {
            if (!parse_uint(value, 1, 1024, &options->draws))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static void sample_frequency(WindowSamples *samples, unsigned window, double elapsed, uint64_t *latest)
{
    char state[320] = {0};
    size_t size = sizeof(state) - 1;
    unsigned long long at;
    unsigned level, mhz;
    if (sysctlbyname("machdep.piano_gpu_state", state, &size, NULL, 0) != 0 ||
        sscanf(state, "at_us=%llu lvl=%u mhz=%u", &at, &level, &mhz) != 3 || at <= *latest)
        return;
    *latest = at;
    if (!samples.frequencySamples || level < samples.minLevel)
        samples.minLevel = level;
    if (level > samples.maxLevel)
        samples.maxLevel = level;
    if (mhz > samples.maxMHz)
        samples.maxMHz = mhz;
    samples.frequencySamples++;
    state[strcspn(state, "\r\n")] = '\0';
    fprintf(stderr, "GPU_STATE window=%u elapsed_s=%.6f %s\n", window, elapsed, state);
}

static int compare_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_stats(const char *label, double *ms, size_t count)
{
    fprintf(stderr, " %s_n=%zu", label, count);
    if (!count) {
        fprintf(stderr, " %s_p50_ms=na %s_p95_ms=na %s_p99_ms=na %s_max_ms=na %s_stalls=0",
                label, label, label, label, label);
        return;
    }
    qsort(ms, count, sizeof(*ms), compare_double);
    size_t stalls = 0;
    while (stalls < count && ms[count - stalls - 1] > 50.0)
        ++stalls;
    fprintf(stderr, " %s_p50_ms=%.6f %s_p95_ms=%.6f %s_p99_ms=%.6f %s_max_ms=%.6f %s_stalls=%zu",
            label, ms[(size_t)ceil(0.50 * count) - 1], label, ms[(size_t)ceil(0.95 * count) - 1],
            label, ms[(size_t)ceil(0.99 * count) - 1], label, ms[count - 1], label, stalls);
}

// Each positive interval belongs to its later event. Preserve the last timestamp across windows.
static size_t intervals(double *stamps, size_t count, double *previous, size_t *duplicates)
{
    qsort(stamps, count, sizeof(*stamps), compare_double);
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const double stamp = stamps[i];
        if (stamp <= *previous) {
            ++*duplicates;
            continue;
        }
        if (*previous > 0)
            stamps[used++] = (stamp - *previous) * 1000;
        *previous = stamp;
    }
    return used;
}

static bool timestamp(double t)
{
    return isfinite(t) && t > 0;
}

static void csv_time(double t)
{
    if (timestamp(t))
        printf("%.9f", t);
    putchar(',');
}

typedef struct { double submit, completion, gpuEnd, present; } Previous;

static size_t report_window(WindowSamples *window, unsigned index, double seconds, Previous *previous)
{
    const size_t count = window.frames.count, capacity = count ? count : 1;
    NSMutableData *scratch = [NSMutableData dataWithLength:capacity * 6 * sizeof(double)];
    double *cpu = scratch.mutableBytes, *encode = cpu + capacity, *completion = encode + capacity;
    double *gpu = completion + capacity, *gpuEnd = gpu + capacity, *present = gpuEnd + capacity;
    size_t completed = 0, errors = 0, presentCallbacks = 0, gpuCount = 0, presents = 0;
    size_t cpuDuplicates = 0, completionDuplicates = 0, gpuDuplicates = 0, presentDuplicates = 0;
    size_t i = 0;
    for (Frame *f in window.frames) {
        const bool done = atomic_load_explicit(&f->completed, memory_order_acquire);
        const bool reported = atomic_load_explicit(&f->presentReported, memory_order_acquire);
        cpu[i] = f->submit;
        encode[i++] = (f->submit - f->encodeBegin) * 1000;
        printf("%zu,%u,", f->sequence, index);
        csv_time(f->begin);
        csv_time(f->encodeBegin);
        csv_time(f->submit);
        csv_time(done ? f->gpuStart : 0);
        csv_time(done ? f->gpuEnd : 0);
        csv_time(done ? f->completion : 0);
        csv_time(reported ? f->present : 0);
        csv_time(reported ? f->presentCallback : 0);
        printf("%s\n", !done ? "pending" : f->status == MTLCommandBufferStatusCompleted ? "completed" : "error");
        if (done && f->status == MTLCommandBufferStatusCompleted) {
            completion[completed++] = f->completion;
            if (timestamp(f->gpuStart) && timestamp(f->gpuEnd) && f->gpuEnd > f->gpuStart) {
                gpu[gpuCount] = (f->gpuEnd - f->gpuStart) * 1000;
                gpuEnd[gpuCount++] = f->gpuEnd;
            }
        } else if (done) {
            ++errors;
        }
        if (reported) {
            ++presentCallbacks;
            if (timestamp(f->present))
                present[presents++] = f->present;
        }
    }
    const size_t cpuIntervals = intervals(cpu, count, &previous->submit, &cpuDuplicates);
    const size_t completionIntervals = intervals(completion, completed, &previous->completion, &completionDuplicates);
    const size_t gpuIntervals = intervals(gpuEnd, gpuCount, &previous->gpuEnd, &gpuDuplicates);
    const size_t presentIntervals = intervals(present, presents, &previous->present, &presentDuplicates);
    fprintf(stderr, "WINDOW index=%u start_s=%u seconds=%.3f submitted=%zu completed=%zu errors=%zu pending=%zu "
                    "render_fps=%.6f gpu_timed=%zu presented=%zu present_callbacks=%zu present_duplicates_or_older=%zu "
                    "present_fps=",
            index, index * windowSeconds, seconds, count, completed, errors, count - completed - errors,
            (completed - completionDuplicates) / seconds, gpuCount, presents - presentDuplicates,
            presentCallbacks, presentDuplicates);
    if (presents > presentDuplicates)
        fprintf(stderr, "%.6f", (presents - presentDuplicates) / seconds);
    else
        fprintf(stderr, "na");
    print_stats("cpu_frame", cpu, cpuIntervals);
    print_stats("cpu_encode", encode, count);
    print_stats("render_frame", completion, completionIntervals);
    print_stats("gpu_work", gpu, gpuCount);
    print_stats("gpu_frame", gpuEnd, gpuIntervals);
    print_stats("present_frame", present, presentIntervals);
    fprintf(stderr, " freq_samples=%u", window.frequencySamples);
    if (window.frequencySamples)
        fprintf(stderr, " min_level=%u max_level=%u max_mhz=%u\n", window.minLevel, window.maxLevel, window.maxMHz);
    else
        fprintf(stderr, " min_level=na max_level=na max_mhz=na\n");
    fflush(stdout);
    return count - completed;
}

static bool console_session(void)
{
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return getuid() && getuid() == geteuid() &&
        [session[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] &&
        [session[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue] &&
        [session[(__bridge NSString *)kCGSessionUserIDKey] unsignedIntValue] == getuid();
}

static int failure(const char *reason)
{
    fprintf(stderr, "METAL_SUSTAIN_ERROR %s\n", reason);
    return 1;
}

// Slot and drawable deadlines cover calls that return. Keep a separate deadline for stalled Metal calls.
static dispatch_source_t completion_watchdog(Capture *capture)
{
    dispatch_source_t watchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    atomic_store(&capture->lastCompletionMs, (uint64_t)(CACurrentMediaTime() * 1000));
    dispatch_source_set_timer(watchdog, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(watchdog, ^{
        const uint64_t last = atomic_load(&capture->lastCompletionMs);
        if (CACurrentMediaTime() * 1000 - last > 15000) {
            static const char message[] = "METAL_SUSTAIN_ERROR no command completed for 15 seconds\n";
            (void)write(STDERR_FILENO, message, sizeof message - 1);
            _Exit(1);
        }
    });
    dispatch_activate(watchdog);
    return watchdog;
}

static int benchmark(Options options, id<MTLDevice> device, CAMetalLayer *layer)
{
    NSError *setup = nil;
    SustainScene *scene = [[SustainScene alloc] initWithDevice:device width:options.width height:options.height
                                                       draws:options.draws error:&setup];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!scene || !queue)
        return failure(setup ? setup.localizedDescription.UTF8String : "Metal queue or scene resource unavailable");

    id<MTLTexture> offscreen = nil;
    if (!layer) {
        MTLTextureDescriptor *color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                        width:options.width height:options.height mipmapped:NO];
        color.storageMode = MTLStorageModePrivate;
        color.usage = MTLTextureUsageRenderTarget;
        offscreen = [device newTextureWithDescriptor:color];
        if (!offscreen)
            return failure("offscreen render target unavailable");
    }

    NSActivityOptions activityOptions = NSActivityUserInitiated;
    if (layer)
        activityOptions |= NSActivityIdleDisplaySleepDisabled;
    id activity = [NSProcessInfo.processInfo beginActivityWithOptions:activityOptions reason:@"Metal sustained 3D measurement"];
    dispatch_semaphore_t slots = dispatch_semaphore_create(queueDepth);
    Capture *capture = [Capture new];
    dispatch_source_t watchdog = completion_watchdog(capture);
    NSMutableDictionary<NSNumber *, WindowSamples *> *windows = [NSMutableDictionary dictionary];
    Previous previous = {0};
    uint64_t latestState = 0;
    size_t sequence = 0, unfinished = 0, drawableMisses = 0;
    unsigned nextReport = 0;
    const unsigned numWindows = (options.seconds + windowSeconds - 1) / windowSeconds;
    const double warmup = CACurrentMediaTime(), start = warmup + warmupSeconds, end = start + options.seconds;
    double nextFrequencyRead = start, lastSubmit = warmup;
    int rc = 0;
    fprintf(stderr, "METAL_SUSTAIN_BEGIN mode=%s seconds=%u width=%u height=%u draws=%u device=\"%s\" start=%.9f\n",
            layer ? "onscreen" : "offscreen", options.seconds, options.width, options.height, options.draws + 1,
            device.name.UTF8String, start);
    puts("frame,window,cpu_begin_s,cpu_encode_begin_s,cpu_submit_s,gpu_start_s,gpu_end_s,completion_callback_s,"
         "presented_s,present_callback_s,status");

    while (CACurrentMediaTime() < end && !rc && !atomic_load(&capture->commandError)) {
        @autoreleasepool {
            const double now = CACurrentMediaTime();
            while (nextReport < numWindows && now >= start + (nextReport + 1) * windowSeconds + callbackGrace) {
                NSNumber *key = @(nextReport);
                unfinished += report_window(windows[key], nextReport, windowSeconds, &previous);
                [windows removeObjectForKey:key];
                ++nextReport;
            }
            if (dispatch_semaphore_wait(slots, dispatch_time(DISPATCH_TIME_NOW, timeoutSeconds * NSEC_PER_SEC))) {
                rc = failure("command completion timeout waiting for a queue slot");
                break;
            }
            if (atomic_load(&capture->commandError)) {
                dispatch_semaphore_signal(slots);
                break;
            }
            const double begin = CACurrentMediaTime();
            if (begin >= end) {
                dispatch_semaphore_signal(slots);
                break;
            }
            const double elapsed = begin - start;
            WindowSamples *window = nil;
            if (elapsed >= 0) {
                const unsigned index = (unsigned)(elapsed / windowSeconds);
                NSNumber *key = @(index);
                window = windows[key];
                if (!window)
                    windows[key] = window = [WindowSamples new];
                if (begin >= nextFrequencyRead) {
                    sample_frequency(window, index, elapsed, &latestState);
                    nextFrequencyRead = begin + 0.5;
                }
            }
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (layer && !drawable) {
                dispatch_semaphore_signal(slots);
                ++drawableMisses;
                if (CACurrentMediaTime() - lastSubmit > timeoutSeconds)
                    rc = failure("CAMetalLayer did not provide a drawable within five seconds");
                else
                    [NSThread sleepForTimeInterval:0.01];
                continue;
            }
            Frame *frame = [Frame new];
            frame->sequence = ++sequence;
            frame->begin = begin;
            frame->encodeBegin = CACurrentMediaTime();
            id<MTLTexture> color = layer ? drawable.texture : offscreen;
            if (color.width != options.width || color.height != options.height) {
                dispatch_semaphore_signal(slots);
                rc = failure("render target dimensions differ from the requested drawable size");
                break;
            }
            id<MTLCommandBuffer> command = [queue commandBuffer];
            if (!command || ![scene encode:command color:color time:begin - warmup]) {
                dispatch_semaphore_signal(slots);
                rc = failure("could not encode the 3D render pass");
                break;
            }
            [window.frames addObject:frame];
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
                frame->presentCallback = CACurrentMediaTime();
                frame->present = shown.presentedTime;
                atomic_store_explicit(&frame->presentReported, true, memory_order_release);
            }];
            if (drawable)
                [command presentDrawable:drawable];
            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                frame->completion = CACurrentMediaTime();
                atomic_store(&capture->lastCompletionMs, (uint64_t)(frame->completion * 1000));
                frame->status = completed.status;
                frame->gpuStart = completed.GPUStartTime;
                frame->gpuEnd = completed.GPUEndTime;
                if (frame->status != MTLCommandBufferStatusCompleted) {
                    atomic_store(&capture->commandError, true);
                    fprintf(stderr, "METAL_SUSTAIN_COMMAND_ERROR frame=%zu status=%ld detail=%s\n", frame->sequence,
                            (long)frame->status, completed.error.localizedDescription.UTF8String ?: "unknown");
                }
                atomic_store_explicit(&frame->completed, true, memory_order_release);
                dispatch_semaphore_signal(slots);
            }];
            frame->submit = lastSubmit = CACurrentMediaTime();
            [command commit];
        }
    }

    const double stopped = CACurrentMediaTime();
    const dispatch_time_t drainDeadline = dispatch_time(DISPATCH_TIME_NOW, timeoutSeconds * NSEC_PER_SEC);
    unsigned drained = 0;
    for (; drained < queueDepth; ++drained) {
        if (dispatch_semaphore_wait(slots, drainDeadline)) {
            rc = failure("command completion timeout during drain");
            break;
        }
    }
    for (unsigned i = 0; i < drained; ++i)
        dispatch_semaphore_signal(slots);
    if (layer && !rc && !atomic_load(&capture->commandError)) {
        const double graceEnd = stopped + callbackGrace;
        while (CACurrentMediaTime() < graceEnd) {
            bool waiting = false;
            for (WindowSamples *window in windows.allValues)
                for (Frame *frame in window.frames)
                    waiting |= !atomic_load_explicit(&frame->presentReported, memory_order_acquire);
            if (!waiting)
                break;
            [NSThread sleepForTimeInterval:0.01];
        }
    }
    const double measured = fmax(0, fmin(stopped - start, options.seconds));
    while (nextReport < numWindows && measured > nextReport * windowSeconds) {
        @autoreleasepool {
            NSNumber *key = @(nextReport);
            const double seconds = fmin(windowSeconds, measured - nextReport * windowSeconds);
            unfinished += report_window(windows[key], nextReport, seconds, &previous);
            [windows removeObjectForKey:key];
            ++nextReport;
        }
    }
    dispatch_source_cancel(watchdog);
    [NSProcessInfo.processInfo endActivity:activity];
    if (!rc && (unfinished || atomic_load(&capture->commandError) || !previous.completion || measured < options.seconds))
        rc = 1;
    if (fflush(stdout) == EOF || ferror(stdout))
        rc = failure("could not write frame CSV");
    fprintf(stderr, "METAL_SUSTAIN_END rc=%d elapsed_s=%.6f windows=%u drawable_misses=%zu unfinished=%zu\n",
            rc, measured, nextReport, drawableMisses, unfinished);
    return rc;
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        Options options;
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!parse_options(argc, argv, &options)) {
            usage(stderr);
            return 2;
        }
        setvbuf(stdout, NULL, _IOFBF, 128 * 1024);
        if (options.onscreen && !console_session())
            return failure("onscreen mode requires the logged-in console user's GUI session");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return failure("Metal device unavailable");
        if (!options.onscreen)
            return benchmark(options, device, nil);

        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSScreen *screen = NSScreen.mainScreen;
        if (!screen)
            return failure("main display unavailable");
        const NSRect area = screen.visibleFrame;
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSInsetRect(area, area.size.width / 20, area.size.height / 20)
                                                       styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Piano Metal 3D Sustain";
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.maximumDrawableCount = queueDepth;
        layer.allowsNextDrawableTimeout = YES;
        layer.displaySyncEnabled = YES;
        layer.opaque = YES;
        layer.contentsScale = screen.backingScaleFactor;
        layer.drawableSize = CGSizeMake(options.width, options.height);
        layer.contentsGravity = kCAGravityResizeAspect;
        window.contentView.layer = layer;
        window.contentView.wantsLayer = YES;
        window.contentView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        [window makeKeyAndOrderFront:nil];
        [app activate];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                const int result = benchmark(options, device, layer);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [window orderOut:nil];
                    exit(result);
                });
            }
        });
        [app run];
        return failure("AppKit exited before the benchmark finished");
    }
}
