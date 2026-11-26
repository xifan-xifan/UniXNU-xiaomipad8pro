// SPDX-License-Identifier: MIT
#import "display_evidence.h"

#import <IOKit/IOKitLib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static const char snapshotName[] = "machdep.piano_dpu_snapshot";

static NSDictionary *snapshot(NSString *body)
{
    uint64_t serial = 0, time = 0;
    unsigned mode = 0;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSArray<NSString *> *lines = [body componentsSeparatedByString:@"\n"];
    for (NSString *line in lines) {
        const char *s = line.UTF8String;
        if (sscanf(s, "snapshot %" SCNu64 " at %" SCNu64 " us:", &serial, &time) == 2) {
            result[@"serial"] = @(serial);
            result[@"time_us"] = @(time);
        }
        if (sscanf(s, "mode: live %u Hz", &mode) == 1)
            result[@"classified_mode_hz"] = @(mode);
        unsigned link, enabled;
        uint32_t frame;
        const char *counters = strstr(s, "; counters en ");
        if (counters && sscanf(s, "intf%u:", &link) == 1 && (link == 1 || link == 2) &&
            sscanf(counters, "; counters en %x frame %" SCNu32, &enabled, &frame) == 2 &&
            (enabled & 3) == 3) {
            result[link == 1 ? @"intf1_frame" : @"intf2_frame"] = @(frame);
        }
    }
    result[@"status"] = serial && time && result[@"intf1_frame"] && result[@"intf2_frame"]
        ? @"ok" : @"missing_snapshot_or_enabled_counters";
    return result;
}

NSDictionary *ft_read_snapshot(void)
{
    // DpuSnapshot.cpp bounds its text to 24 * 480 bytes. A single call avoids a cached size probe.
    char buffer[16384] = {0};
    size_t size = sizeof(buffer) - 1;
    if (sysctlbyname(snapshotName, buffer, &size, NULL, 0) != 0) {
        const int error = errno;
        NSString *status = error == EPERM || error == EACCES ? @"permission_denied"
            : error == ENOENT ? @"unavailable" : @"read_error";
        return @{@"status": status, @"errno": @(error)};
    }
    return snapshot(@(buffer));
}

NSDictionary *ft_scanout_report(NSDictionary *before, NSDictionary *after)
{
    NSMutableDictionary *result = [@{
        @"source": @(snapshotName), @"before": before, @"after": after,
        @"status": @"missing_counter_evidence"
    } mutableCopy];
    if (![before[@"status"] isEqual:@"ok"] || ![after[@"status"] isEqual:@"ok"])
        return result;
    const uint64_t start = [before[@"time_us"] unsignedLongLongValue];
    const uint64_t end = [after[@"time_us"] unsignedLongLongValue];
    if (end <= start || [after[@"serial"] unsignedLongLongValue] <= [before[@"serial"] unsignedLongLongValue]) {
        result[@"status"] = @"stale_or_reversed_snapshot";
        return result;
    }
    const double span = (end - start) / 1e6;
    result[@"span_s"] = @(span);
    NSString *status = @"ok";
    for (NSString *name in @[@"intf1", @"intf2"]) {
        NSString *key = [name stringByAppendingString:@"_frame"];
        // The hardware counter wraps at 32 bits.
        const uint32_t delta = [after[key] unsignedIntValue] - [before[key] unsignedIntValue];
        result[name] = @{@"frame_delta": @(delta), @"scanout_hz": @(delta / span)};
        if (delta > span * 1000 + 2)
            status = @"counter_reset_or_implausible_delta";
        else if (!delta && [status isEqual:@"ok"])
            status = @"counter_not_advancing";
    }
    result[@"status"] = status;
    return result;
}

NSDictionary *ft_gop_properties(void)
{
    io_iterator_t services = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("NativeGopDisplay"),
                                     &services) != KERN_SUCCESS)
        return @{};
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    io_service_t service;
    while ((service = IOIteratorNext(services))) {
        // The panel can be advertised as external. Match the driver's fixed panel UUID.
        NSString *uuid = CFBridgingRelease(IORegistryEntryCreateCFProperty(
            service, CFSTR("IOMFBUUID"), kCFAllocatorDefault, 0));
        if ([uuid isEqual:@"4E474F50-4449-5350-0000-000000008750"]) {
            id timing = CFBridgingRelease(IORegistryEntryCreateCFProperty(
                service, CFSTR("DPTimingModeId"), kCFAllocatorDefault, 0));
            id refresh = CFBridgingRelease(IORegistryEntryCreateCFProperty(
                service, CFSTR("IOMFBDisplayRefresh"), kCFAllocatorDefault, 0));
            if ([timing isKindOfClass:NSNumber.class])
                result[@"timing_id"] = timing;
            id min = [refresh isKindOfClass:NSDictionary.class] ? refresh[@"displayMinRefreshInterval"] : nil;
            if ([min isKindOfClass:NSNumber.class])
                result[@"display_min_refresh_interval_raw"] = min;
            NSDictionary *attributes = CFBridgingRelease(IORegistryEntryCreateCFProperty(
                service, CFSTR("DisplayAttributes"), kCFAllocatorDefault, 0));
            id product = [attributes isKindOfClass:NSDictionary.class] ? attributes[@"ProductAttributes"] : nil;
            if ([product isKindOfClass:NSDictionary.class]) {
                for (NSString *key in @[@"LegacyManufacturerID", @"ProductID"])
                    if ([product[key] isKindOfClass:NSNumber.class])
                        result[key] = product[key];
                // Only the fixed public product name is retained, never a user-assigned display name.
                result[@"panel_product_name"] = @([product[@"ProductName"] isEqual:@"Xiaomi Pad 8 Pro"]);
            }
            if ([attributes isKindOfClass:NSDictionary.class])
                for (NSString *key in @[@"NativeFormatHorizontalPixels", @"NativeFormatVerticalPixels"])
                    if ([attributes[key] isKindOfClass:NSNumber.class])
                        result[key] = attributes[key];
            id external = CFBridgingRelease(IORegistryEntryCreateCFProperty(
                service, CFSTR("external"), kCFAllocatorDefault, 0));
            if ([external isKindOfClass:NSNumber.class])
                result[@"builtin"] = @(![external boolValue]);
            IOObjectRelease(service);
            break;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(services);
    return result;
}

static NSDictionary *log_evidence(NSString *body)
{
    NSMutableArray *cadence = [NSMutableArray array];
    NSMutableArray *vbl = [NSMutableArray array];
    NSMutableArray *totals = [NSMutableArray array];
    NSUInteger positive = 0;
    for (NSString *line in [body componentsSeparatedByString:@"\n"]) {
        const char *text = line.UTF8String;
        const char *dpu = strstr(text, "PianoDpu: ");
        const char *gop = strstr(text, "NativeGopDisplay: period ");
        uint64_t seq, window, flips, again, gaps, n, p50, p95, p99, max, ticks, idle;
        if (dpu && sscanf(dpu, "PianoDpu: cadence %" SCNu64 ": %" SCNu64 " ms: %" SCNu64
                          " flips (%*u.%*u/s), %" SCNu64 " again, %" SCNu64 " gaps; iv n %" SCNu64
                          " p50 %" SCNu64 " p95 %" SCNu64 " p99 %" SCNu64 " max %" SCNu64 " us",
                          &seq, &window, &flips, &again, &gaps, &n, &p50, &p95, &p99, &max) == 10 && window) {
            positive += n > 0 && flips > 0;
            [cadence addObject:@{@"sequence": @(seq), @"window_ms": @(window),
                @"new_surface_flips": @(flips), @"same_surface": @(again), @"gaps_ge_100ms": @(gaps),
                @"new_surface_hz": @(1000.0 * flips / window), @"intervals": @(n),
                @"p50_us": @(p50), @"p95_us": @(p95), @"p99_us": @(p99), @"max_us": @(max)}];
        } else if (dpu && sscanf(dpu, "PianoDpu: flips: %*u in the last %*u s (%" SCNu64 " in all:",
                                 &flips) == 1) {
            [totals addObject:@(flips)];
        } else if (gop && sscanf(gop, "NativeGopDisplay: period %" SCNu64 " ms: %" SCNu64
                                 " VBL ticks, %" SCNu64 " times the tick stopped", &window, &ticks, &idle) == 3) {
            [vbl addObject:@{@"window_ms": @(window), @"software_ticks": @(ticks), @"idle_stops": @(idle)}];
        }
    }
    return @{@"dpu_cadence_windows": cadence, @"dpu_positive_cadence_windows": @(positive),
             @"dpu_new_surface_flip_totals": totals,
             @"gop_software_vbl_windows": vbl};
}

BOOL ft_write_json(NSDictionary *report)
{
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&error];
    if (!data) {
        fprintf(stderr, "piano-frametime: JSON serialization failed (%ld)\n", (long)error.code);
        return NO;
    }
    if (fwrite(data.bytes, 1, data.length, stdout) == data.length && fputc('\n', stdout) != EOF &&
        fflush(stdout) == 0)
        return YES;
    fprintf(stderr, "piano-frametime: JSON write failed\n");
    return NO;
}

static NSString *read_capture(const char *path)
{
    NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
    if (!text)
        fprintf(stderr, "piano-frametime: cannot read a kernel capture\n");
    return text;
}

int ft_kernel_report(const char *beforePath, const char *afterPath, const char *logPath)
{
    NSString *before = read_capture(beforePath);
    NSString *after = read_capture(afterPath);
    NSString *log = logPath ? read_capture(logPath) : nil;
    if (!before || !after || (logPath && !log))
        return 1;
    NSDictionary *scanout = ft_scanout_report(snapshot(before), snapshot(after));
    NSDictionary *driverLog = log ? log_evidence(log) : nil;
    NSDictionary *report = @{@"schema_version": @1, @"scanout": scanout,
                             @"driver_log": driverLog ?: NSNull.null};
    if (!ft_write_json(report))
        return 1;
    fprintf(stderr, "piano-frametime: scanout %s", [scanout[@"status"] UTF8String]);
    if ([scanout[@"status"] isEqual:@"ok"])
        fprintf(stderr, ", INTF1 %.3f Hz, INTF2 %.3f Hz", [scanout[@"intf1"][@"scanout_hz"] doubleValue],
                [scanout[@"intf2"][@"scanout_hz"] doubleValue]);
    fputc('\n', stderr);
    const NSUInteger positive = [driverLog[@"dpu_positive_cadence_windows"] unsignedIntegerValue];
    if (driverLog)
        fprintf(stderr, "kernel log: %lu positive DPU cadence windows, %lu software VBL windows\n",
                (unsigned long)positive, (unsigned long)[driverLog[@"gop_software_vbl_windows"] count]);
    return [scanout[@"status"] isEqual:@"ok"] && (!driverLog || positive) ? 0 : 1;
}
