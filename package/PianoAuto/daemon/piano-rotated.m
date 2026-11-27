// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// piano-rotated: automatic display rotation on native macOS for piano (docs/drivers/sensors.md section 5a). A launchd agent
// (com.unixnu.piano-rotated.plist: the login window's session and each user's, so it acts at the login window too) that
//   reads    PianoSensors' "piano-sensors-edge" (its level 4: "top", "left", "bottom", "right", "none"; a number 0..3 is taken
//            too) whenever its "piano-sensors-seq" moves, polled every 250 ms only while that can matter (PianoSensors at level 4,
//            the cover permits rotation; every 5 s while the display sleeps): PianoSensors notifies on edge changes
//            and is found by class even unregistered; PianoKeyboard's "piano-keyboard-attached" (0/1) on each
//            kIOMessageServicePropertyChange it sends;
//   decides  AutoRotate.c over SensorsPolicy's ps_rotation_step, the functions the host tests run;
//   rotates  WindowServer's own rotation of the panel's display, the one System Settings > Displays > Rotation sets. No public API
//            sets it (CoreGraphics only reads it: CGDisplayRotation); the setter used is MonitorPanel's (a private framework):
//            +[MPDisplayMgr sharedMgr], -displayWithID:, -[MPDisplay canChangeOrientation], -orientation, -setOrientation:, found by
//            their runtime names and checked against the type encodings they had on macOS 26.6 before any call; if anything differs
//            the agent rotates nothing and says why. Every rotation is read back with the public CGDisplayRotation;
//   tells    PianoTouch the rotation shown (its "piano-touch-rotation" property, TouchRotation.h), never one only asked for.
// Nothing runs unless the kernel boot argument piano-rotate is 1 or more (sysctl kern.piano_bootargs):
//   piano-rotate=1   observe: inputs and decisions logged, nothing changed
//   piano-rotate=2   + WindowServer's rotation
//   piano-rotate=3   + PianoTouch told
//   piano-rotate-map=T,L,B,R   degrees for the top/left/bottom/right edge up (default 0,90,180,270: CGDisplayRotation's sense, 90 = the
//                              panel's left edge up)
//   piano-rotate-settle=MS (600), -interval=MS (1000), -detach=MS (1500)   the policy's times (AutoRotate.h)
//   piano-rotate-cover=N       2 (default) held at 0 while the cover is attached, 1 turned to 0 at attach only, 0 the cover ignored
//   piano-rotate-test=1        the edge from the preference TestEdge instead of PianoSensors (a test of everything but the sensor)
// The user's rotation lock: `defaults write com.unixnu.piano-rotated Locked -bool true`, then `notifyutil -p
// com.unixnu.piano-rotated.prefs` (or SIGHUP). Command line, for the USB root shell: --probe (what it sees; changes nothing),
// --set DEGREES [--touch] (one rotation, read back), --level N (instead of the boot argument), --stderr (the log on stderr too).
// Log: os_log subsystem com.unixnu.piano-rotated (`log show --last 5m --predicate 'subsystem == "com.unixnu.piano-rotated"'`).
// Build: tooling/k27/build-piano-rotated.sh (the host tests, then arm64, ad-hoc signed by the linker).
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "AutoRotate.h"
#include "PianoBootArgs.h"
#include "TouchRotation.h"

#define APP "com.unixnu.piano-rotated"
#define PREFS_NOTIFICATION APP ".prefs"
#define EDGE_PROPERTY "piano-sensors-edge"
#define SEQ_PROPERTY "piano-sensors-seq"
#define SENSORS_LEVEL_PROPERTY "piano-sensors"
#define COVER_PROPERTY "piano-keyboard-attached"
// NativeGopDisplay's ProductAttributes: ManufacturerID "XMI" (EISA: X=24, M=13, I=9), ProductID 0x8750.
static const uint32_t kPanelVendor = (24u << 10) | (13u << 5) | 9u, kPanelModel = 0x8750;

static os_log_t gLog;
static int gStderr, gLevel, gTestEdges, gRunning;   // gRunning: the agent's loop (not --probe / --set)
static ParAgent gAgent;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    os_log(gLog, "%{public}s", line);
    if (gStderr) fprintf(stderr, "piano-rotated: %s\n", line);
}

static uint64_t nowMs(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

// ------------------------------------------------------------------------------------------------------------- boot arguments
static char gBootArgs[PIANO_BOOT_ARGS_MAX];

// The whole merged boot-args (kern.piano_bootargs, k27 0076): the rotation keys can sit past kern.bootargs' 1023-byte clip.
static void readBootArgs(void)
{
    pianoBootArgs(gBootArgs);
}

// NAME=VALUE (or a bare NAME, "1") from the boot arguments, or NULL.
static const char *bootArg(const char *name, char *out, size_t cap)
{
    size_t length;
    const char *value = pianoBootArgValue(gBootArgs, name, &length);
    if (!value || length >= cap) return NULL;
    memcpy(out, value, length);
    out[length] = 0;
    return out;
}

static int parseNumber(const char *value, int base, long *out)
{
    char *end;
    errno = 0;
    const long n = strtol(value, &end, base);
    if (end == value || *end || errno == ERANGE) return 0;
    *out = n;
    return 1;
}

static long bootNumber(const char *name, long fallback, long lo, long hi)
{
    char value[64];
    long n;
    if (!bootArg(name, value, sizeof value) || !parseNumber(value, 0, &n)) return fallback;
    return n < lo ? lo : n > hi ? hi : n;
}

static void configure(ParConfig *c)
{
    par_defaults(c);
    char map[64];
    if (bootArg("piano-rotate-map", map, sizeof map)) {
        int d[4], k = 0;
        for (char *p = map, *end; k < 4; ++k, p = end + 1) {
            const long angle = strtol(p, &end, 10);
            if (end == p || *end != (k < 3 ? ',' : '\0') || angle < 0 || angle > 270 || !par_valid_degrees((int)angle)) break;
            d[k] = (int)angle;
        }
        if (k == 4)
            for (int e = 0; e < 4; ++e) c->rot.degrees[e] = (uint16_t)d[e];
        else
            note("piano-rotate-map=%s is not four of 0/90/180/270: the default map", map);
    }
    c->rot.settle_ms = (uint32_t)bootNumber("piano-rotate-settle", c->rot.settle_ms, 0, 10000);
    c->rot.min_interval_ms = (uint32_t)bootNumber("piano-rotate-interval", c->rot.min_interval_ms, 0, 60000);
    c->detach_hold_ms = (uint32_t)bootNumber("piano-rotate-detach", c->detach_hold_ms, 0, 60000);
    c->cover_lock = (int)bootNumber("piano-rotate-cover", PAR_LOCK_HOLD, PAR_LOCK_IGNORE, PAR_LOCK_HOLD);
    gTestEdges = gTestEdges || bootNumber("piano-rotate-test", 0, 0, 1);
}

// ------------------------------------------------------------------------------------------------------------- the display
static CGDirectDisplayID gDisplayOverride;

static CGDirectDisplayID panelDisplay(void)
{
    if (gDisplayOverride) return gDisplayOverride;
    CGDirectDisplayID ids[8];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(8, ids, &n) != kCGErrorSuccess) n = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (CGDisplayVendorNumber(ids[i]) == kPanelVendor && CGDisplayModelNumber(ids[i]) == kPanelModel) return ids[i];
    return 0;
}

static int degreesShown(CGDirectDisplayID d)
{
    if (!d) return -1;
    const double rotation = CGDisplayRotation(d);
    if (!isfinite(rotation)) return -1;
    const int deg = ((int)lround(fmod(rotation, 360.0)) + 360) % 360;
    return par_valid_degrees(deg) ? deg : -1;
}

static int onConsole(void)
{
    CFDictionaryRef s = CGSessionCopyCurrentDictionary();
    static int told;
    if (!s) {
        if (!told++) note("no session dictionary (the login window?): acting as the console's session");
        return 1;
    }
    CFBooleanRef b = (CFBooleanRef)CFDictionaryGetValue(s, kCGSessionOnConsoleKey);
    const int on = b && CFGetTypeID(b) == CFBooleanGetTypeID() && CFBooleanGetValue(b);
    CFRelease(s);
    return on;
}

// ------------------------------------------------------------------------------------------------------------- WindowServer's rotation
// MonitorPanel (System Settings' display code), by its runtime names; the type encodings are compared without their offsets.
static struct {
    int ready;
    char why[200];
    Class mgr, display;
    SEL shared, withID, can, get, set, sensor;
} gMp;

static int typesAre(Method m, const char *want)
{
    if (!m) return 0;
    const char *t = method_getTypeEncoding(m);
    char s[64];
    size_t k = 0;
    for (; t && *t && k + 1 < sizeof s; ++t)
        if (*t < '0' || *t > '9') s[k++] = *t;
    s[k] = 0;
    return !strcmp(s, want);
}

static void backendInit(void)
{
    if (!dlopen("/System/Library/PrivateFrameworks/MonitorPanel.framework/MonitorPanel", RTLD_LAZY)) {
        snprintf(gMp.why, sizeof gMp.why, "MonitorPanel does not load (%s)", dlerror());
        return;
    }
    gMp.mgr = objc_getClass("MPDisplayMgr");
    gMp.display = objc_getClass("MPDisplay");
    gMp.shared = sel_registerName("sharedMgr");
    gMp.withID = sel_registerName("displayWithID:");
    gMp.can = sel_registerName("canChangeOrientation");
    gMp.get = sel_registerName("orientation");
    gMp.set = sel_registerName("setOrientation:");
    gMp.sensor = sel_registerName("hasRotationSensor");
    const char *missing = !gMp.mgr ? "class MPDisplayMgr" : !gMp.display ? "class MPDisplay"
                        : !typesAre(class_getClassMethod(gMp.mgr, gMp.shared), "@@:") ? "+[MPDisplayMgr sharedMgr] as @@:"
                        : !typesAre(class_getInstanceMethod(gMp.mgr, gMp.withID), "@@:i") ? "-[MPDisplayMgr displayWithID:] as @@:i"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.can), "B@:") ? "-[MPDisplay canChangeOrientation] as B@:"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.get), "i@:") ? "-[MPDisplay orientation] as i@:"
                        : !typesAre(class_getInstanceMethod(gMp.display, gMp.set), "v@:i") ? "-[MPDisplay setOrientation:] as v@:i"
                        : NULL;
    if (missing) {
        snprintf(gMp.why, sizeof gMp.why, "MonitorPanel has no %s (another macOS build?): no rotation", missing);
        return;
    }
    gMp.ready = 1;
    snprintf(gMp.why, sizeof gMp.why, "MonitorPanel MPDisplay -setOrientation:");
}

static id mpDisplay(CGDirectDisplayID d)
{
    if (!gMp.ready) return nil;
    id mgr = ((id (*)(Class, SEL))objc_msgSend)(gMp.mgr, gMp.shared);
    return mgr ? ((id (*)(id, SEL, int))objc_msgSend)(mgr, gMp.withID, (int)d) : nil;
}

static int mpBool(id display, SEL sel, int fallback)
{
    if (!display || !class_getInstanceMethod(object_getClass(display), sel) ||
        !typesAre(class_getInstanceMethod(object_getClass(display), sel), "B@:"))
        return fallback;
    return ((BOOL (*)(id, SEL))objc_msgSend)(display, sel) ? 1 : 0;
}

static int applyRotation(CGDirectDisplayID d, int degrees, char *why, size_t cap)
{
    if (!d) {
        snprintf(why, cap, "panel display is unavailable");
        return 0;
    }
    if (!gMp.ready) {
        snprintf(why, cap, "%s", gMp.why);
        return 0;
    }
    id display = mpDisplay(d);
    if (!display) {
        snprintf(why, cap, "MonitorPanel has no display %#x", d);
        return 0;
    }
    if (!mpBool(display, gMp.can, 0)) {
        snprintf(why, cap, "WindowServer offers no rotation for display %#x (canChangeOrientation NO)", d);
        return 0;
    }
    ((void (*)(id, SEL, int))objc_msgSend)(display, gMp.set, degrees);
    snprintf(why, cap, "asked");
    return 1;
}

// ------------------------------------------------------------------------------------------------------------- the drivers
typedef struct {
    const char *cls;
    io_service_t service;
    io_object_t interest;
    io_iterator_t matching;
} Watched;
static Watched gSensors = {"PianoSensors", 0, 0, 0}, gKeyboard = {"PianoKeyboard", 0, 0, 0}, gTouch = {"PianoTouch", 0, 0, 0};
static IONotificationPortRef gPort;

static int readNumber(io_service_t s, const char *key, long *out)
{
    if (!s) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = k ? IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0) : NULL;
    int ok = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) ok = CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, out);
    else if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) *out = CFBooleanGetValue((CFBooleanRef)v), ok = 1;
    if (v) CFRelease(v);
    if (k) CFRelease(k);
    return ok;
}

// PianoSensors' edge: its name (ps_edge_name) or, from an earlier draft, the number; anything else is none.
static int readEdge(io_service_t s)
{
    if (!s) return PS_EDGE_NONE;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR(EDGE_PROPERTY), kCFAllocatorDefault, 0);
    int edge = PS_EDGE_NONE;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        char name[16];
        if (CFStringGetCString((CFStringRef)v, name, sizeof name, kCFStringEncodingUTF8))
            for (int e = PS_EDGE_TOP; e <= PS_EDGE_RIGHT; ++e)
                if (!strcmp(name, ps_edge_name(e))) edge = e;
    } else if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        long n = -1;
        if (CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &n) && n >= PS_EDGE_TOP && n <= PS_EDGE_RIGHT) edge = (int)n;
    }
    if (v) CFRelease(v);
    return edge;
}

// A service by class even when it never called registerService (matching finds registered ones only): the service plane, walked.
static io_service_t findByClass(const char *cls)
{
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    io_iterator_t it = 0;
    io_service_t found = 0;
    if (root && IORegistryEntryCreateIterator(root, kIOServicePlane, kIORegistryIterateRecursively, &it) == KERN_SUCCESS) {
        for (io_object_t o; !found && (o = IOIteratorNext(it));) {
            if (IOObjectConformsTo(o, cls)) found = o;
            else IOObjectRelease(o);
        }
        IOObjectRelease(it);
    }
    if (root) IOObjectRelease(root);
    return found;
}

// Three failures at one angle end retries until the angle or the service changes.
static int gTouchFailures;

static int pushTouch(int degrees)
{
    kern_return_t kr = KERN_FAILURE;
    if (gTouch.service) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &degrees);
        if (n) {
            kr = IORegistryEntrySetCFProperty(gTouch.service, CFSTR(PAT_PROPERTY), n);
            CFRelease(n);
        }
    }
    if (kr == KERN_SUCCESS) {
        note("PianoTouch told %d degrees: taken", degrees);
        gTouchFailures = 0;
        return 1;
    }
    if (++gTouchFailures < 3) {
        note("PianoTouch told %d degrees: %s (%#x)", degrees, gTouch.service ? "REFUSED" : "not found", kr);
        return 0;
    }
    note("PianoTouch did not take %d degrees three times: waiting for a new angle or service", degrees);
    return 1;
}

// ------------------------------------------------------------------------------------------------------------- the policy
static CFRunLoopTimerRef gTimer, gSensorsTimer;
static long gSensorsSeq = -1;
static unsigned gSensorsLookups;
static int gInPolicy, gAgain, gLastShown = -2, gLastWant = -2, gLastEdge = -2, gLastCover = -2;
static const char *gLastWhy;
static void readInputs(int active);

static const char *coverName(int c)
{
    return c == PAR_COVER_ATTACHED ? "attached" : c == PAR_COVER_DETACHED ? "detached" : "unknown (no PianoKeyboard)";
}

static void schedule(uint64_t deadline)
{
    if (!gTimer) return;
    const uint64_t now = nowMs();
    const double in = !deadline ? 1e9 : deadline <= now ? 0.001 : (double)(deadline - now) / 1000.0;
    CFRunLoopTimerSetNextFireDate(gTimer, CFAbsoluteTimeGetCurrent() + in);
}

static void runPolicy(void)
{
    if (gInPolicy) {
        gAgain = 1;
        return;
    }
    gInPolicy = 1;
    int can = 0;
    for (int pass = 0; pass < 6; ++pass) @autoreleasepool {
        gAgain = 0;
        const CGDirectDisplayID display = panelDisplay();
        const int active = display && !CGDisplayIsAsleep(display) && onConsole();
        const int shown = degreesShown(display);
        if (shown != gLastShown) {
            note("display %#x shows %d degrees", display, shown);
            gLastShown = shown;
            gTouchFailures = 0;
        }
        can = gLevel >= 2 && gMp.ready && active;
        readInputs(active);
        const uint64_t now = nowMs();
        par_observed(&gAgent, active ? shown : -1, now);
        const ParDecision d = par_step(&gAgent, now, can);
        if (d.want != gLastWant || d.why != gLastWhy) {
            if (d.want >= 0) note("wants %d degrees: %s%s", d.want, d.why, d.apply < 0 && gLevel < 2 ? " (observing only)" : "");
            else note("%s", d.why);
            gLastWant = d.want;
            gLastWhy = d.why;
        }
        if (d.touch >= 0) {
            par_touch_done(&gAgent, d.touch, gLevel >= 3 ? pushTouch(d.touch) : 1, nowMs());
            continue;
        }
        if (d.apply >= 0) {
            char why[240];
            const int ok = applyRotation(display, d.apply, why, sizeof why);
            note("rotate %d -> %d degrees: %s", shown, d.apply, why);
            par_applied(&gAgent, d.apply, ok, nowMs());
            continue;
        }
        if (!gAgain) break;
    }
    gInPolicy = 0;
    schedule(par_deadline(&gAgent, nowMs(), can));
}

static void readInputs(int active)
{
    const uint64_t now = nowMs();
    long v = -1;
    int edge = PS_EDGE_NONE;
    if (active && gTestEdges) {
        Boolean valid = false;
        const CFIndex e = CFPreferencesGetAppIntegerValue(CFSTR("TestEdge"), CFSTR(APP), &valid);
        edge = valid && e >= PS_EDGE_TOP && e <= PS_EDGE_RIGHT ? (int)e : PS_EDGE_NONE;
    } else if (active && gSensors.service && readNumber(gSensors.service, SENSORS_LEVEL_PROPERTY, &v) && v >= 4) {
        edge = readEdge(gSensors.service);
    }
    int cover = PAR_COVER_UNKNOWN;
    if (readNumber(gKeyboard.service, COVER_PROPERTY, &v)) cover = v ? PAR_COVER_ATTACHED : PAR_COVER_DETACHED;
    if (edge != gLastEdge) {
        note("edge up: %s%s", ps_edge_name(edge), gTestEdges ? " (test preference)" : gSensors.service ? "" : " (no PianoSensors)");
        gLastEdge = edge;
    }
    if (cover != gLastCover) {
        note("keyboard cover %s", coverName(cover));
        gLastCover = cover;
        if (gSensorsTimer) CFRunLoopTimerSetNextFireDate(gSensorsTimer, CFAbsoluteTimeGetCurrent());   // polling starts or stops
    }
    par_cover(&gAgent, cover, now);
    par_edge(&gAgent, edge, now);
}

static void takeService(Watched *w, io_service_t service);

// Also poll PianoSensors' sequence while an edge can matter. Look for unregistered services every 30 s, up to 20 times.
static void sensorsTick(void)
{
    double next = 0;
    if (!gSensors.service && !gTestEdges && gSensorsLookups < 20) {
        ++gSensorsLookups;
        io_service_t service = findByClass(gSensors.cls);
        if (service) takeService(&gSensors, service);
        else next = 30;
    }
    long level = 0, seq = -1;
    const int wanted = gRunning && !gTestEdges && gSensors.service && readNumber(gSensors.service, SENSORS_LEVEL_PROPERTY, &level) &&
                       level >= 4 && (gLastCover != PAR_COVER_ATTACHED || gAgent.cfg.cover_lock == PAR_LOCK_IGNORE);
    if (wanted) {
        const CGDirectDisplayID d = panelDisplay();
        next = d && CGDisplayIsAsleep(d) ? 5 : 0.25;
        if (readNumber(gSensors.service, SEQ_PROPERTY, &seq) && seq != gSensorsSeq) {
            gSensorsSeq = seq;
            runPolicy();
        }
    }
    if (gSensorsTimer) CFRunLoopTimerSetNextFireDate(gSensorsTimer, CFAbsoluteTimeGetCurrent() + (next > 0 ? next : 1e9));
}

static void reloadPrefs(void)
{
    CFPreferencesAppSynchronize(CFSTR(APP));
    Boolean valid = false;
    const int locked = CFPreferencesGetAppBooleanValue(CFSTR("Locked"), CFSTR(APP), &valid) && valid;
    if (locked != gAgent.locked) note("rotation lock %s", locked ? "on" : "off");
    par_lock(&gAgent, locked);
}

static void interest(void *ctx, io_service_t service, natural_t type, void *argument)
{
    (void)argument;
    Watched *w = ctx;
    if (service != w->service) return;
    if (type == kIOMessageServiceIsTerminated) {
        note("%s went away", w->cls);
        if (w->interest) IOObjectRelease(w->interest);
        if (w->service) IOObjectRelease(w->service);
        w->interest = w->service = 0;
    }
    if (gRunning && (type == kIOMessageServicePropertyChange || type == kIOMessageServiceIsTerminated)) {
        if (w == &gSensors && !w->service) {
            gSensorsLookups = 0;
            CFRunLoopTimerSetNextFireDate(gSensorsTimer, CFAbsoluteTimeGetCurrent());
        }
        runPolicy();
    }
}

static void takeService(Watched *w, io_service_t service)
{
    w->service = service;
    IOServiceAddInterestNotification(gPort, service, kIOGeneralInterest, interest, w, &w->interest);
    if (w == &gSensors) {
        gSensorsSeq = -1;
        if (gSensorsTimer) CFRunLoopTimerSetNextFireDate(gSensorsTimer, CFAbsoluteTimeGetCurrent());
    } else if (w == &gTouch) {
        gTouchFailures = 0;
        gAgent.touch = -1;
        gAgent.touch_retry_at = 0;
    }
    note("found %s", w->cls);
}

static void matched(void *ctx, io_iterator_t iterator)
{
    Watched *w = ctx;
    for (io_service_t s; (s = IOIteratorNext(iterator));) {
        if (w->service) {
            IOObjectRelease(s);
            continue;
        }
        takeService(w, s);
    }
    if (gRunning) runPolicy();
}

static void watch(Watched *w)
{
    if (IOServiceAddMatchingNotification(gPort, kIOFirstMatchNotification, IOServiceMatching(w->cls), matched, w, &w->matching) ==
        KERN_SUCCESS) {
        matched(w, w->matching);   // arms the notification and takes what is there now
    }
}

static void reconfigured(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *ctx)
{
    (void)display;
    (void)ctx;
    if (!(flags & kCGDisplayBeginConfigurationFlag)) dispatch_async(dispatch_get_main_queue(), ^{ runPolicy(); });
}

// ------------------------------------------------------------------------------------------------------------- one-shot modes
static void probe(void)
{
    CGDirectDisplayID ids[8];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(8, ids, &n) != kCGErrorSuccess) n = 0;
    printf("piano-rotated --probe: level %d (boot arguments: piano-rotate=%ld), test edges %d\n", gLevel, bootNumber("piano-rotate", 0, 0, 3),
           gTestEdges);
    for (uint32_t i = 0; i < n; ++i) {
        const CGRect b = CGDisplayBounds(ids[i]);
        printf("  display %#x: vendor %#x model %#x%s, %.0fx%.0f points, rotation %.0f degrees, %s%s\n", ids[i], CGDisplayVendorNumber(ids[i]),
               CGDisplayModelNumber(ids[i]),
               CGDisplayVendorNumber(ids[i]) == kPanelVendor && CGDisplayModelNumber(ids[i]) == kPanelModel ? " (the panel)" : "",
               b.size.width, b.size.height, CGDisplayRotation(ids[i]), CGDisplayIsAsleep(ids[i]) ? "asleep" : "awake",
               ids[i] == CGMainDisplayID() ? ", main" : "");
    }
    const CGDirectDisplayID d = panelDisplay();
    printf("  panel display: %#x\n  backend: %s\n", d, gMp.why);
    if (gMp.ready) {
        id display = mpDisplay(d);
        printf("  MPDisplay for %#x: %s; canChangeOrientation %d, orientation %d, hasRotationSensor %d\n", d, display ? "found" : "NONE",
               mpBool(display, gMp.can, -1),
               display ? ((int (*)(id, SEL))objc_msgSend)(display, gMp.get) : -1, mpBool(display, gMp.sensor, -1));
    }
    long v = -1;
    if (!gSensors.service) gSensors.service = findByClass(gSensors.cls);
    long level = 0, seq = -1;
    printf("  PianoSensors: %s, level %ld, %s %s, %s %ld\n", gSensors.service ? "found" : "not found",
           readNumber(gSensors.service, SENSORS_LEVEL_PROPERTY, &level) ? level : 0L, EDGE_PROPERTY,
           ps_edge_name(readEdge(gSensors.service)), SEQ_PROPERTY, readNumber(gSensors.service, SEQ_PROPERTY, &seq) ? seq : -1L);
    printf("  PianoKeyboard: %s, %s %s\n", gKeyboard.service ? "found" : "not found", COVER_PROPERTY,
           readNumber(gKeyboard.service, COVER_PROPERTY, &v) ? (v ? "1 (attached)" : "0 (detached)") : "absent");
    printf("  PianoTouch: %s, %s %s\n", gTouch.service ? "found" : "not found", PAT_PROPERTY,
           readNumber(gTouch.service, PAT_PROPERTY, &v) ? (v == 0 ? "0" : v == 90 ? "90" : v == 180 ? "180" : v == 270 ? "270" : "?") : "absent");
    printf("  session on the console: %d; rotation lock (preference Locked): %d\n", onConsole(), gAgent.locked);
}

static int setOnce(int degrees, int touch)
{
    const CGDirectDisplayID d = panelDisplay();
    char why[240];
    const int before = degreesShown(d);
    if (!applyRotation(d, degrees, why, sizeof why)) {
        printf("piano-rotated --set %d: display %#x at %d degrees: NOT asked: %s\n", degrees, d, before, why);
        return 1;
    }
    int shown = before;
    for (int i = 0; i < 30 && shown != degrees; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
        shown = degreesShown(d);
    }
    printf("piano-rotated --set %d: display %#x %d -> %d degrees: %s\n", degrees, d, before, shown, shown == degrees ? "shown" : "NOT shown in 3 s");
    if (touch && shown == degrees) pushTouch(shown);
    return shown == degrees ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------------------- main
int main(int argc, char **argv)
{
    @autoreleasepool {
        gLog = os_log_create(APP, "agent");
        int doProbe = 0, set = -1, touch = 0, level = -1;
        for (int i = 1; i < argc; ++i) {
            long number;
            if (!strcmp(argv[i], "--probe")) doProbe = 1;
            else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
                if (!parseNumber(argv[++i], 10, &number) || number < 0 || number > 270 || !par_valid_degrees((int)number)) {
                    fprintf(stderr, "piano-rotated: --set takes 0, 90, 180 or 270\n");
                    return 2;
                }
                set = (int)number;
            } else if (!strcmp(argv[i], "--touch")) touch = 1;
            else if (!strcmp(argv[i], "--level") && i + 1 < argc && parseNumber(argv[i + 1], 10, &number) && number >= 0) {
                level = number > 3 ? 3 : (int)number;
                ++i;
            } else if (!strcmp(argv[i], "--display") && i + 1 < argc) gDisplayOverride = (CGDirectDisplayID)strtoul(argv[++i], NULL, 0);
            else if (!strcmp(argv[i], "--test-edges")) gTestEdges = 1;
            else if (!strcmp(argv[i], "--stderr")) gStderr = 1;
            else {
                fprintf(stderr, "usage: piano-rotated [--probe | --set DEGREES [--touch]] [--level N] [--display ID] [--test-edges] [--stderr]\n");
                return 2;
            }
        }
        readBootArgs();
        gLevel = level >= 0 ? level : (int)bootNumber("piano-rotate", 0, 0, 3);
        if (!gLevel && !doProbe && set < 0) {
            note("piano-rotate is 0 or absent: not running");
            return 0;
        }
        ParConfig config;
        configure(&config);
        par_init(&gAgent, &config);
        backendInit();
        gPort = IONotificationPortCreate(kIOMainPortDefault);
        CFRunLoopSourceRef source = gPort ? IONotificationPortGetRunLoopSource(gPort) : NULL;
        if (!source) {
            note("IOKit notification source unavailable");
            return 1;
        }
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
        watch(&gSensors);
        watch(&gKeyboard);
        watch(&gTouch);
        reloadPrefs();
        if (doProbe) {
            probe();
            return 0;
        }
        if (set >= 0) return setOnce(set, touch);
        const CGDirectDisplayID d = panelDisplay();
        id display = mpDisplay(d);
        note("start: level %d (%s), map %u/%u/%u/%u, settle %u ms, interval %u ms, detach hold %u ms, cover %s; display %#x (vendor %#x "
             "model %#x) at %d degrees; backend %s (canChangeOrientation %d, hasRotationSensor %d)%s",
             gLevel, gLevel == 1 ? "observe" : gLevel == 2 ? "rotate" : "rotate + touch", (unsigned)config.rot.degrees[0],
             (unsigned)config.rot.degrees[1], (unsigned)config.rot.degrees[2], (unsigned)config.rot.degrees[3], config.rot.settle_ms, config.rot.min_interval_ms, config.detach_hold_ms,
             config.cover_lock == PAR_LOCK_HOLD ? "held at 0 while attached" : config.cover_lock == PAR_LOCK_AT_ATTACH ? "0 at attach" : "ignored",
             d, CGDisplayVendorNumber(d), CGDisplayModelNumber(d), degreesShown(d), gMp.why, mpBool(display, gMp.can, -1),
             mpBool(display, gMp.sensor, -1), gTestEdges ? "; edges from the test preference TestEdge" : "");
        gTimer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, ^(CFRunLoopTimerRef t) {
            (void)t;
            runPolicy();
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), gTimer, kCFRunLoopDefaultMode);
        gSensorsTimer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, ^(CFRunLoopTimerRef t) {
            (void)t;
            sensorsTick();
        });
        CFRunLoopAddTimer(CFRunLoopGetMain(), gSensorsTimer, kCFRunLoopDefaultMode);
        CGDisplayRegisterReconfigurationCallback(reconfigured, NULL);
        signal(SIGHUP, SIG_IGN);
        dispatch_source_t hup = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(hup, ^{
            reloadPrefs();
            runPolicy();
        });
        dispatch_resume(hup);
        int token = 0;
        notify_register_dispatch(PREFS_NOTIFICATION, &token, dispatch_get_main_queue(), ^(int t) {
            (void)t;
            reloadPrefs();
            runPolicy();
        });
        gRunning = 1;
        runPolicy();
        sensorsTick();
        CFRunLoopRun();
    }
    return 0;
}
