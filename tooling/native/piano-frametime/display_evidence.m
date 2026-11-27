// SPDX-License-Identifier: MIT
#import "display_evidence.h"

#import <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static const char snapshotName[] = "machdep.piano_dpu_snapshot";

static bool take(const char **cursor, const char *literal)
{
    const size_t len = strlen(literal);
    if (strncmp(*cursor, literal, len) != 0)
        return false;
    *cursor += len;
    return true;
}

static bool read_number(const char **cursor, int base, uint64_t *value)
{
    const char *digits = base == 10 ? "0123456789" : "0123456789abcdefABCDEF";
    if (!**cursor || !strchr(digits, **cursor))
        return false;
    char *end;
    errno = 0;
    const uint64_t parsed = strtoull(*cursor, &end, base);
    if (errno == ERANGE)
        return false;
    *cursor = end;
    *value = parsed;
    return true;
}

static bool read_decimal(const char **cursor, const char *prefix, uint64_t *value)
{
    return take(cursor, prefix) && read_number(cursor, 10, value);
}

static bool token_end(const char *cursor)
{
    return strchr(" \t\r;", *cursor) != NULL; // Also matches the terminating NUL.
}

static const char *line_text(NSString *line)
{
    const char *text = line.UTF8String;
    return text && strlen(text) == [line lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ? text : NULL;
}

static NSDictionary *snapshot(NSString *body)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSArray<NSString *> *lines = [body componentsSeparatedByString:@"\n"];
    for (NSString *line in lines) {
        const char *text = line_text(line);
        if (!text)
            continue;
        const char *cursor = text;
        uint64_t serial, time, mode, link, enabled, frame;
        if (read_decimal(&cursor, "snapshot ", &serial) && read_decimal(&cursor, " at ", &time) &&
            take(&cursor, " us:")) {
            result[@"serial"] = @(serial);
            result[@"time_us"] = @(time);
        }
        cursor = text;
        if (read_decimal(&cursor, "mode: live ", &mode) && mode <= UINT32_MAX &&
            take(&cursor, " Hz") && token_end(cursor))
            result[@"classified_mode_hz"] = @(mode);

        cursor = text;
        if (!read_decimal(&cursor, "intf", &link) || (link != 1 && link != 2) || !take(&cursor, ":"))
            continue;
        cursor = strstr(cursor, "; counters en ");
        if (cursor && take(&cursor, "; counters en ") && read_number(&cursor, 16, &enabled) &&
            enabled <= UINT32_MAX && (enabled & 3) == 3 &&
            read_decimal(&cursor, " frame ", &frame) && frame <= UINT32_MAX && token_end(cursor)) {
            result[link == 1 ? @"intf1_frame" : @"intf2_frame"] = @(frame);
        }
    }
    result[@"status"] = [result[@"serial"] unsignedLongLongValue] && [result[@"time_us"] unsignedLongLongValue] &&
        result[@"intf1_frame"] && result[@"intf2_frame"]
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

static NSDictionary *cadence_window(const char *cursor)
{
    uint64_t seq, window, flips, ignored, again, gaps, n, p50, p95, p99, max;
    if (!read_decimal(&cursor, "cadence ", &seq) || !read_decimal(&cursor, ": ", &window) || !window ||
        !read_decimal(&cursor, " ms: ", &flips) || !read_decimal(&cursor, " flips (", &ignored) ||
        !read_decimal(&cursor, ".", &ignored) || !read_decimal(&cursor, "/s), ", &again) ||
        !read_decimal(&cursor, " again, ", &gaps) || !read_decimal(&cursor, " gaps; iv n ", &n) ||
        !read_decimal(&cursor, " p50 ", &p50) || !read_decimal(&cursor, " p95 ", &p95) ||
        !read_decimal(&cursor, " p99 ", &p99) || !read_decimal(&cursor, " max ", &max))
        return nil;
    if (take(&cursor, " mean ") && !read_number(&cursor, 10, &ignored))
        return nil;
    if (!take(&cursor, " us") || !token_end(cursor))
        return nil;
    return @{@"sequence": @(seq), @"window_ms": @(window), @"new_surface_flips": @(flips),
             @"same_surface": @(again), @"gaps_ge_100ms": @(gaps), @"new_surface_hz": @(1000.0 * flips / window),
             @"intervals": @(n), @"p50_us": @(p50), @"p95_us": @(p95), @"p99_us": @(p99), @"max_us": @(max)};
}

static NSDictionary *log_evidence(NSString *body)
{
    static const char dpu_prefix[] = "PianoDpu: ";
    NSMutableArray *cadence = [NSMutableArray array];
    NSMutableArray *vbl = [NSMutableArray array];
    NSMutableArray *totals = [NSMutableArray array];
    NSUInteger positive = 0;
    for (NSString *line in [body componentsSeparatedByString:@"\n"]) {
        const char *text = line_text(line);
        if (!text)
            continue;
        const char *dpu = strstr(text, dpu_prefix);
        if (dpu) {
            dpu += sizeof(dpu_prefix) - 1;
            NSDictionary *sample = cadence_window(dpu);
            if (sample) {
                positive += [sample[@"intervals"] unsignedLongLongValue] > 0 &&
                    [sample[@"new_surface_flips"] unsignedLongLongValue] > 0;
                [cadence addObject:sample];
                continue;
            }
            uint64_t ignored, flips;
            if (read_decimal(&dpu, "flips: ", &ignored) && read_decimal(&dpu, " in the last ", &ignored) &&
                read_decimal(&dpu, " s (", &flips) && take(&dpu, " in all:")) {
                [totals addObject:@(flips)];
                continue;
            }
        }
        const char *gop = strstr(text, "NativeGopDisplay: period ");
        uint64_t window, ticks, idle;
        if (gop && read_decimal(&gop, "NativeGopDisplay: period ", &window) &&
            read_decimal(&gop, " ms: ", &ticks) && read_decimal(&gop, " VBL ticks, ", &idle) &&
            take(&gop, " times the tick stopped") && token_end(gop)) {
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
