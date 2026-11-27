/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Atomic administrator-only PianoPower configuration. Never exposes MCA writes.
 * --save persists only daily configuration, never the bounded test inhibit.
 * --apply-saved waits for the service and reapplies it after a boot. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const keys[] = {"ChargeLimitEnabled", "ChargeLimitStop", "ChargeLimitResume", "ChargeMode",
                                  "ChargeTestInhibitSeconds", "ChargeConfigRevision"};
static const char *config_dir = "/private/var/db/piano-charge";
static const char *config = "/private/var/db/piano-charge/config";
static int number(CFDictionaryRef d, const char *key, unsigned *out) {
    int64_t n;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef value = k ? CFDictionaryGetValue(d, k) : NULL;
    int valid = value && CFGetTypeID(value) == CFNumberGetTypeID() && !CFNumberIsFloatType(value) &&
                CFNumberGetValue(value, kCFNumberSInt64Type, &n) && n >= 0 && n <= UINT32_MAX;
    if (k) CFRelease(k);
    if (valid) *out = (unsigned)n;
    return valid;
}
static CFDictionaryRef state(io_service_t s) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("PianoCharge"), NULL, 0);
    if (v && CFGetTypeID(v) != CFDictionaryGetTypeID()) {CFRelease(v); return NULL;}
    return (CFDictionaryRef)v;
}
static int parse_unsigned(const char *s, char **end, unsigned *out) {
    if (*s < '0' || *s > '9') return 0;
    errno = 0;
    unsigned long n = strtoul(s, end, 10);
    if (errno || n > UINT32_MAX) return 0;
    *out = (unsigned)n; return 1;
}
static int parse(const char *s, unsigned *out) {
    char *end;
    return parse_unsigned(s, &end, out) && !*end;
}
// Return 0 for a complete configuration, 1 for a refused file, 2 for absence.
static int load_config(const char *path, uid_t owner, unsigned v[6]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 2 : 1;
    struct stat st;
    char line[96];
    size_t length = 0;
    int bad = fstat(fd, &st) || st.st_uid != owner || (st.st_mode & 022) || !S_ISREG(st.st_mode);
    while (!bad && length < sizeof line) {
        ssize_t n = read(fd, line + length, sizeof line - length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            bad = n < 0;
            break;
        }
        length += (size_t)n;
    }
    close(fd);
    if (bad || !length || length == sizeof line || memchr(line, 0, length)) return 1;
    line[length] = 0;
    char *at = line;
    for (unsigned i = 0; i < 4; ++i) {
        while (isspace((unsigned char)*at)) ++at;
        if (!parse_unsigned(at, &at, &v[i]) || (*at && !isspace((unsigned char)*at))) return 1;
    }
    while (isspace((unsigned char)*at)) ++at;
    return *at ? 1 : 0;
}
static int persist(const char *path, const unsigned v[6]) {
    char tmp[PATH_MAX];
    int size = snprintf(tmp, sizeof tmp, "%s.new.XXXXXX", path);
    if (v[4] || size < 0 || (size_t)size >= sizeof tmp) return 1;
    int fd = mkstemp(tmp);
    if (fd < 0) return 1;
    char line[80]; int n = snprintf(line, sizeof line, "%u %u %u %u\n", v[0], v[1], v[2], v[3]);
    size_t written = 0;
    while (written < (size_t)n) {
        ssize_t count = write(fd, line + written, (size_t)n - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += (size_t)count;
    }
    int bad = written != (size_t)n || fsync(fd);
    if (close(fd)) bad = 1;
    if (!bad && !rename(tmp, path)) return 0;
    unlink(tmp);
    return 1;
}
int main(int argc, char **argv) {
    unsigned v[6] = {1,80,75,1,0,0};
    int save = argc > 1 && !strcmp(argv[1], "--save");
    int apply = argc == 2 && !strcmp(argv[1], "--apply-saved");
    int show = argc == 2 && !strcmp(argv[1], "--show");
    if (!show && geteuid()) {fputs("administrator required\n", stderr); return 1;}
    if (apply) {
        int loaded = load_config(config, 0, v);
        if (loaded) return loaded == 2 ? 0 : 1;
    } else if (!show) {
        int first = save ? 2 : 1, count = argc - first;
        if (count != 4 && count != 5) {
            fputs("usage: piano-charge-config [--save] enabled stop resume mode [inhibitSeconds]\n"
                  "       piano-charge-config --show | --apply-saved\n", stderr); return 2;
        }
        for (int i = 0; i < count; ++i) if (!parse(argv[first+i], &v[i])) return 2;
    }
    if (v[0] > 1 || v[1] < 50 || v[1] > 100 || v[2] < 40 || v[2] >= v[1] || v[3] > 2 || v[4] > 180 || (save && v[4])) {
        fputs("configuration range refused\n", stderr); return 2;
    }
    io_service_t service = 0;
    CFDictionaryRef d = NULL;
    // Matching service registration precedes charger firmware/telemetry. Saved
    // settings wait for that readiness too, rather than exiting at early boot.
    for (int i = 0; i < (apply ? 180 : 1); ++i) {
        service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoPower"));
        if (service) d = state(service);
        if (d) break;
        if (service) {IOObjectRelease(service); service = 0;}
        if (apply) sleep(1);
    }
    if (!service || !d) {fputs("PianoCharge state unavailable\n", stderr); return 1;}
    if (show) {
        CFDataRef xml = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        int ok = 0;
        if (xml) {
            size_t size = (size_t)CFDataGetLength(xml);
            ok = size && fwrite(CFDataGetBytePtr(xml), 1, size, stdout) == size && !fflush(stdout);
            CFRelease(xml);
        }
        CFRelease(d); IOObjectRelease(service); return ok ? 0 : 1;
    }
    int have_revision = number(d, "ConfigRevision", &v[5]);
    CFRelease(d);
    if (!have_revision) {
        fputs("PianoCharge revision unavailable\n", stderr);
        IOObjectRelease(service);
        return 1;
    }
    CFMutableDictionaryRef request = CFDictionaryCreateMutable(NULL, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!request) {IOObjectRelease(service); return 1;}
    for (unsigned i = 0; i < 6; ++i) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        int64_t value = v[i];
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt64Type, &value);
        if (!k || !n) {if(k)CFRelease(k);if(n)CFRelease(n);CFRelease(request);IOObjectRelease(service);return 1;}
        CFDictionarySetValue(request, k, n); CFRelease(k); CFRelease(n);
    }
    kern_return_t kr = IORegistryEntrySetCFProperties(service, request); CFRelease(request);
    if (kr) {fprintf(stderr, "transaction refused code=0x%x\n", kr); IOObjectRelease(service); return 1;}
    int rc = 1;
    for (unsigned i = 0; i < 100; ++i) {
        usleep(100000); d = state(service);
        if (!d) continue;
        unsigned revision, error;
        int accepted = number(d, "ConfigRevision", &revision) && revision == v[5]+1 &&
                       number(d, "ConfigError", &error) && !error;
        CFRelease(d);
        if (accepted) {
            printf("config accepted revision=%u enabled=%u stop=%u resume=%u mode=%u inhibit=%u\n", revision,v[0],v[1],v[2],v[3],v[4]);
            rc = save ? ((mkdir(config_dir, 0700) && errno != EEXIST) || persist(config, v)) : 0;
            break;
        }
    }
    IOObjectRelease(service);
    if (rc) fputs("configuration not confirmed; inspect readback\n", stderr);
    return rc;
}
