// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
//
// piano-tier-workload: a Metal workload that reaches the RAM16G GPU-private tier (docs/drivers/ram16g-tier-crash.md).
//
// The only Turnip allocation that asks for the tier (mesa 0015) is the device scratch BO of tu_copy_image_to_image's two-pass
// staging blit. metal2vulkan records copyFromTexture:toTexture: between two private textures of one copy unit as vkCmdCopyImage of
// the two images' own formats; Turnip stages that copy through tu_get_scratch_bo when both images are tiled and their formats differ
// in one of three ways (R8G8 vs another 2-byte format, a format with a non-identity tiled swap, or two UBWC images). This tool issues
// those copies at staging sizes of 2 MiB and more, keeps the textures alive for a hold period, and checks every copied byte through a
// Shared readback buffer filled by a GPU blit (a private texture is never CPU-mapped).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "tier_stats.h"

enum { PATH_R8G8 = 1, PATH_SWAP = 2, PATH_UBWC = 4 };
typedef struct { unsigned bit; const char *name; MTLPixelFormat src, dst; unsigned cpp; const char *why; } path_desc;
static const path_desc kPaths[] = {
    {PATH_R8G8, "r8g8", MTLPixelFormatRG8Unorm, MTLPixelFormatR16Uint, 2, "R8G8 tile layout vs another cpp=2 format"},
    {PATH_SWAP, "swap", MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA8Unorm, 4, "tiled BGRA swap"},
    {PATH_UBWC, "ubwc", MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint, 4, "UBWC to UBWC of different formats"},
};

static volatile sig_atomic_t gStop;
static void onSignal(int s) { (void)s; gStop = 1; }

static double nowS(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static int readTier(char *buf, size_t len)
{
    size_t n = len;
    if (sysctlbyname("machdep.piano_gpu_tier", buf, &n, NULL, 0) != 0) {
        snprintf(buf, len, "unreadable errno=%d", errno);
        return -1;
    }
    buf[len - 1] = 0;
    return 0;
}

static void printTier(const char *label, tier_line *out)
{
    char line[768];
    int rc = readTier(line, sizeof line);
    printf("tier %s %s\n", label, line);
    fflush(stdout);
    if (out) {
        if (rc || tier_parse(line, out))
            memset(out, 0, sizeof *out);
    }
}

@interface Pair : NSObject
@property const path_desc *path;
@property NSUInteger width, height, bytes;
@property id<MTLTexture> src, dst;
@property id<MTLBuffer> upload, readback, control;
@property uint32_t seed;
@end
@implementation Pair
@end

typedef struct {
    const char *name;
    double *wall, *gpu;
    size_t n, cap;
    uint64_t bytes, mismatches, control_mismatches, errors;
} phase_stats;

static void phaseAdd(phase_stats *p, double wall, double gpu)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->wall = realloc(p->wall, p->cap * sizeof *p->wall);
        p->gpu = realloc(p->gpu, p->cap * sizeof *p->gpu);
        if (!p->wall || !p->gpu) {
            fprintf(stderr, "piano-tier-workload: out of memory\n");
            exit(1);
        }
    }
    p->wall[p->n] = wall;
    p->gpu[p->n] = gpu;
    p->n++;
}

static int cmpDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void phasePrint(phase_stats *p, double seconds)
{
    if (p->n) {
        qsort(p->wall, p->n, sizeof *p->wall, cmpDouble);
        qsort(p->gpu, p->n, sizeof *p->gpu, cmpDouble);
    }
    printf("phase %s iterations=%zu copied-mib=%.1f seconds=%.2f mismatches=%llu control-mismatches=%llu errors=%llu "
           "wall-ms p50=%.2f p95=%.2f max=%.2f gpu-ms p50=%.2f p95=%.2f max=%.2f\n",
           p->name, p->n, (double)p->bytes / 1048576.0, seconds, (unsigned long long)p->mismatches,
           (unsigned long long)p->control_mismatches, (unsigned long long)p->errors, percentile_sorted(p->wall, p->n, 50) * 1e3,
           percentile_sorted(p->wall, p->n, 95) * 1e3, p->n ? p->wall[p->n - 1] * 1e3 : 0, percentile_sorted(p->gpu, p->n, 50) * 1e3,
           percentile_sorted(p->gpu, p->n, 95) * 1e3, p->n ? p->gpu[p->n - 1] * 1e3 : 0);
    fflush(stdout);
}

static void fillPattern(uint32_t *w, size_t words, uint32_t seed)
{
    uint32_t x = seed ? seed : 0x9e3779b9u;
    for (size_t i = 0; i < words; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = x ^ (uint32_t)i;
    }
}

static size_t firstDiff(const uint8_t *a, const uint8_t *b, size_t n, size_t *count)
{
    size_t first = SIZE_MAX;
    *count = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) {
            if (first == SIZE_MAX)
                first = i;
            ++*count;
        }
    return first;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: piano-tier-workload [options]\n"
        "       piano-tier-workload --tier-report BEFORE DURING AFTER [--expect-bos N] [--expect-mib M] [--slack-mib S]\n"
        "\n"
        "Reaches Turnip's GPU-private staging scratch (mesa 0015) through Metal: copyFromTexture:toTexture: between two\n"
        "MTLStorageModePrivate textures of one copy unit and different formats, which Turnip copies in two passes through\n"
        "tu_get_scratch_bo. Every copy is verified byte for byte through a Shared buffer the GPU fills (a direct control\n"
        "copy of the source texture separates upload/readback faults from staging faults).\n"
        "\n"
        "  --paths LIST       r8g8,swap,ubwc or all (default all). r8g8: RG8Unorm -> R16Uint (Turnip's R8G8 tile-layout rule,\n"
        "                     the surest staging trigger); swap: BGRA8Unorm -> RGBA8Unorm (tiled swap rule); ubwc:\n"
        "                     RGBA8Unorm -> R32Uint (UBWC to UBWC). The paths share the scratch classes. On an Apple GPU\n"
        "                     the r8g8 copy itself is unreliable (Apple validation forbids differing formats): host smoke\n"
        "                     with --paths swap,ubwc\n"
        "  --sizes-mib LIST   staging sizes in MiB, each a texture of that many bytes (default 2,4,8,16,32,64; min 2, max 256)\n"
        "                     Taken in ascending order: Turnip keeps one scratch BO per power-of-two class and reuses a larger one,\n"
        "                     so the run asks the tier once per class: expected private requests = number of distinct classes,\n"
        "                     expected tier MiB = their sum (126 for the default)\n"
        "  --pairs N          texture pairs per size and path kept alive (default 1; more pairs raise ordinary BO use only)\n"
        "  --seconds N        hold phase: keep the working set alive and repeat copy + verify for N seconds (default 60)\n"
        "  --linger N         after the hold, keep the process (and its scratch BOs) alive N seconds without GPU work so the\n"
        "                     caller can read machdep.piano_gpu_tier (default 10)\n"
        "  --keep-going       continue after a mismatch (default: stop the hold at the first mismatch)\n"
        "  --require-tier     require mode=1 state=ready (piano-gpu-tier=1) before and during the workload. Exit 4\n"
        "                     if private-req, allocs or used-mib fall short or a tier denial counter changes\n"
        "  --dry-run          print the workload configuration without opening Metal or reading tier telemetry\n"
        "  --help             this text\n"
        "\n"
        "Output: line-oriented on stdout ('tier LABEL <sysctl line>', 'warm ...', 'phase ...', final 'RESULT pass|fail ...').\n"
        "Exit: 0 pass; 1 data mismatch, Metal error, no device or interruption; 2 usage; 4 --require-tier not met.\n"
        "--tier-report reads three files of one machdep.piano_gpu_tier line each (before the run, during --linger, after exit)\n"
        "and exits 0 when all samples say mode=1 state=ready, private-req and allocs rose by --expect-bos, used-mib\n"
        "rose by --expect-mib, used-mib ended at most --slack-mib above BEFORE, and the denial counters stayed fixed.\n"
        "A failed report exits 1 and names the failed checks.\n");
}

static bool parseUint(const char *s, uint64_t max, uint64_t *value)
{
    const char *end;
    return !tier_decimal(s, &end, value) && !*end && *value <= max;
}

static bool parseDuration(const char *s, double *value)
{
    if ((*s < '0' || *s > '9') && *s != '.')
        return false;
    if (s[strspn(s, "0123456789.eE+-")])
        return false;
    char *end;
    errno = 0;
    const double seconds = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(seconds) || seconds < 0)
        return false;
    *value = seconds;
    return true;
}

static int parseList(const char *s, uint64_t *out, size_t max, size_t *n)
{
    *n = 0;
    for (;;) {
        const char *end;
        uint64_t value;
        if (*n == max || tier_decimal(s, &end, &value) || value < 2 || value > 256)
            return -1;
        out[(*n)++] = value;
        if (!*end)
            return 0;
        if (*end != ',')
            return -1;
        s = end + 1;
    }
}

static unsigned parsePaths(const char *s)
{
    if (!strcmp(s, "all"))
        return PATH_R8G8 | PATH_SWAP | PATH_UBWC;
    unsigned paths = 0;
    for (;;) {
        size_t len = strcspn(s, ",");
        unsigned bit = 0;
        for (size_t k = 0; k < sizeof kPaths / sizeof kPaths[0]; k++)
            if (strlen(kPaths[k].name) == len && !strncmp(s, kPaths[k].name, len))
                bit = kPaths[k].bit;
        if (!bit)
            return 0;
        paths |= bit;
        if (!s[len])
            return paths;
        s += len + 1;
    }
}

static int cmpU64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int readLineFile(const char *path, tier_line *t)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f)
        return -1;
    size_t len = fread(line, 1, sizeof line, f);
    bool complete = feof(f) && !ferror(f);
    fclose(f);
    if (!complete || !len || len == sizeof line || memchr(line, '\0', len) || memchr(line, '\n', len - 1))
        return -1;
    line[len] = '\0';
    return tier_parse(line, t);
}

static void printFailures(unsigned fail)
{
    static const char *names[] = {"unreadable", "mode", "private-req", "allocs", "used-mib", "return", "denied"};
    for (size_t b = 0; b < sizeof names / sizeof names[0]; b++)
        if (fail & (1u << b))
            printf(" %s", names[b]);
}

static int tierReport(int argc, const char **argv)
{
    if (argc < 5 || (argc - 5) % 2)
        return 2;
    uint64_t bos = 6, mib = 126, slack = 2;
    for (int i = 5; i < argc; i += 2) {
        const char *arg = argv[i], *value = argv[i + 1];
        bool valid;
        if (!strcmp(arg, "--expect-bos"))
            valid = parseUint(value, UINT_MAX, &bos) && bos;
        else if (!strcmp(arg, "--expect-mib"))
            valid = parseUint(value, UINT64_MAX, &mib) && mib;
        else if (!strcmp(arg, "--slack-mib"))
            valid = parseUint(value, UINT64_MAX, &slack);
        else
            return 2;
        if (!valid)
            return 2;
    }
    tier_line t[3] = {0};
    for (int k = 0; k < 3; k++)
        if (readLineFile(argv[2 + k], &t[k]))
            t[k].valid = 0;
    unsigned fail = tier_verdict(&t[0], &t[1], &t[2], (unsigned)bos, mib, slack);
    printf("tier-report %s", fail ? "FAIL" : "PASS");
    printFailures(fail);
    if (t[0].valid && t[1].valid && t[2].valid)
        printf(" | private-req %llu->%llu allocs %llu->%llu used-mib %llu->%llu->%llu refusals %llu->%llu cpu-map-denied %llu->%llu "
               "import-denied %llu->%llu",
               (unsigned long long)t[0].private_req, (unsigned long long)t[1].private_req,
               (unsigned long long)t[0].allocs, (unsigned long long)t[1].allocs,
               (unsigned long long)t[0].used_mib, (unsigned long long)t[1].used_mib, (unsigned long long)t[2].used_mib,
               (unsigned long long)t[0].refusals, (unsigned long long)t[1].refusals, (unsigned long long)t[0].cpu_map_denied,
               (unsigned long long)t[2].cpu_map_denied, (unsigned long long)t[0].import_denied,
               (unsigned long long)t[2].import_denied);
    printf(" (expected bos %llu mib %llu slack %llu)\n", (unsigned long long)bos, (unsigned long long)mib, (unsigned long long)slack);
    return fail ? 1 : 0;
}

// One upload -> staged copy -> readback round of a pair, plus the direct control readback of the source. Returns 0 on success.
static int runPair(id<MTLCommandQueue> queue, Pair *p, phase_stats *ps, bool verbose)
{
    p.seed = p.seed * 1664525u + 1013904223u;
    fillPattern((uint32_t *)p.upload.contents, p.bytes / 4, p.seed);
    const NSUInteger row = p.width * p.path->cpp;
    double t0 = nowS();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!blit) {
        ps->errors++;
        puts("error no-blit-encoder");
        return 1;
    }
    [blit copyFromBuffer:p.upload sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:p.bytes
              sourceSize:MTLSizeMake(p.width, p.height, 1) toTexture:p.src destinationSlice:0 destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
    // The staged copy: different formats of one copy unit, both private (tiled) images.
    [blit copyFromTexture:p.src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(p.width, p.height, 1) toTexture:p.dst destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit copyFromTexture:p.dst sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(p.width, p.height, 1) toBuffer:p.readback destinationOffset:0 destinationBytesPerRow:row
       destinationBytesPerImage:p.bytes];
    [blit copyFromTexture:p.src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(p.width, p.height, 1) toBuffer:p.control destinationOffset:0 destinationBytesPerRow:row
       destinationBytesPerImage:p.bytes];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    double wall = nowS() - t0;
    if (cb.status != MTLCommandBufferStatusCompleted) {
        ps->errors++;
        printf("error %s %lux%lu status=%ld %s\n", p.path->name, (unsigned long)p.width, (unsigned long)p.height, (long)cb.status,
               cb.error ? cb.error.description.UTF8String : "(no error object)");
        fflush(stdout);
        return 1;
    }
    double gpu = cb.GPUEndTime > cb.GPUStartTime ? cb.GPUEndTime - cb.GPUStartTime : 0;
    phaseAdd(ps, wall, gpu);
    ps->bytes += p.bytes;
    size_t nd = 0, nc = 0;
    size_t fd = firstDiff(p.readback.contents, p.upload.contents, p.bytes, &nd);
    size_t fc = firstDiff(p.control.contents, p.upload.contents, p.bytes, &nc);
    if (nc) {
        ps->control_mismatches++;
        printf("mismatch control %s %lux%lu seed=%#x bytes=%zu first=%zu (upload or readback path, not the staging copy)\n",
               p.path->name, (unsigned long)p.width, (unsigned long)p.height, p.seed, nc, fc);
    }
    if (nd) {
        ps->mismatches++;
        const uint8_t *got = p.readback.contents, *want = p.upload.contents;
        printf("mismatch staged %s %lux%lu seed=%#x bytes=%zu first=%zu row=%zu got=%02x want=%02x%s\n", p.path->name,
               (unsigned long)p.width, (unsigned long)p.height, p.seed, nd, fd, fd / row, got[fd], want[fd],
               nc ? "" : " (control clean: the staged texture copy)");
    }
    if (verbose && !nd && !nc)
        printf("ok %s %lux%lu wall-ms=%.2f gpu-ms=%.2f\n", p.path->name, (unsigned long)p.width, (unsigned long)p.height, wall * 1e3,
               gpu * 1e3);
    fflush(stdout);
    return (nd || nc) ? 1 : 0;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
            usage(stdout);
            return 0;
        }
        if (argc > 1 && !strcmp(argv[1], "--tier-report")) {
            int rc = tierReport(argc, argv);
            if (rc == 2)
                usage(stderr);
            return rc;
        }
        unsigned paths = PATH_R8G8 | PATH_SWAP | PATH_UBWC, pairs = 1;
        uint64_t sizes[32] = {2, 4, 8, 16, 32, 64};
        size_t nsizes = 6;
        double seconds = 60, linger = 10;
        bool keepGoing = false, requireTier = false, dryRun = false;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            const char *v = i + 1 < argc ? argv[i + 1] : NULL;
            uint64_t n;
            if (!strcmp(a, "--paths") && v && (paths = parsePaths(v))) {
                i++;
            } else if (!strcmp(a, "--sizes-mib") && v && !parseList(v, sizes, 32, &nsizes)) {
                i++;
            } else if (!strcmp(a, "--pairs") && v && parseUint(v, 16, &n) && n) {
                i++;
                pairs = (unsigned)n;
            } else if (!strcmp(a, "--seconds") && v && parseDuration(v, &seconds)) {
                i++;
            } else if (!strcmp(a, "--linger") && v && parseDuration(v, &linger)) {
                i++;
            } else if (!strcmp(a, "--keep-going")) {
                keepGoing = true;
            } else if (!strcmp(a, "--require-tier")) {
                requireTier = true;
            } else if (!strcmp(a, "--dry-run")) {
                dryRun = true;
            } else {
                usage(stderr);
                return 2;
            }
        }
        if (dryRun && requireTier) {
            fputs("--dry-run and --require-tier are mutually exclusive\n", stderr);
            usage(stderr);
            return 2;
        }
        qsort(sizes, nsizes, sizeof sizes[0], cmpU64);

        uint64_t stagingBytes[32];
        for (size_t i = 0; i < nsizes; i++)
            stagingBytes[i] = sizes[i] << 20;
        unsigned expectBos;
        uint64_t expectBytes;
        scratch_classes(stagingBytes, nsizes, &expectBos, &expectBytes);
        printf("config paths=");
        const char *separator = "";
        for (size_t k = 0; k < sizeof kPaths / sizeof kPaths[0]; k++) {
            if (paths & kPaths[k].bit) {
                printf("%s%s", separator, kPaths[k].name);
                separator = ",";
            }
        }
        printf(" sizes-mib=");
        for (size_t i = 0; i < nsizes; i++)
            printf("%s%llu", i ? "," : "", (unsigned long long)sizes[i]);
        printf(" pairs=%u seconds=%g linger=%g expect-private-bos=%u expect-tier-mib=%llu pid=%d\n", pairs, seconds, linger,
               expectBos, (unsigned long long)(expectBytes >> 20), getpid());
        if (dryRun)
            return 0;
        signal(SIGTERM, onSignal);
        signal(SIGINT, onSignal);
        tier_line before = {0};
        printTier("before", &before);
        if (requireTier) {
            unsigned fail = tier_snapshot_failure(&before);
            if (fail) {
                printf("RESULT fail-tier (expected mode=1 state=ready)");
                printFailures(fail);
                putchar('\n');
                return 4;
            }
        }

        __attribute__((objc_precise_lifetime)) id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            device = all.firstObject;
        }
        if (!device) {
            printf("RESULT fail no-metal-device\n");
            return 1;
        }
        printf("device name=\"%s\" registry=%#llx\n", device.name.UTF8String, (unsigned long long)device.registryID);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            printf("RESULT fail no-command-queue\n");
            return 1;
        }
        printTier("device-open", NULL);

        // Working set: per size (ascending), per path, `pairs` private texture pairs and their Shared buffers.
        __attribute__((objc_precise_lifetime)) NSMutableArray<Pair *> *set = [NSMutableArray array];
        for (size_t s = 0; s < nsizes; s++)
            for (size_t k = 0; k < sizeof kPaths / sizeof kPaths[0]; k++) {
                if (!(paths & kPaths[k].bit))
                    continue;
                const path_desc *pd = &kPaths[k];
                NSUInteger bytes = (NSUInteger)(sizes[s] << 20), width = 4096 / pd->cpp, height = bytes / 4096;
                while (height > 16384) {
                    width *= 2;
                    height /= 2;
                }
                for (unsigned c = 0; c < pairs; c++) {
                    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pd->src width:width
                                                                                                 height:height mipmapped:NO];
                    d.storageMode = MTLStorageModePrivate;
                    d.usage = MTLTextureUsageShaderRead; // no PixelFormatView / ShaderWrite: not mutable, tiled
                    Pair *p = [Pair new];
                    p.path = pd;
                    p.width = width;
                    p.height = height;
                    p.bytes = bytes;
                    p.seed = (uint32_t)(0x5eed0000u + s * 64 + k * 16 + c);
                    p.src = [device newTextureWithDescriptor:d];
                    d.pixelFormat = pd->dst;
                    p.dst = [device newTextureWithDescriptor:d];
                    p.upload = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
                    p.readback = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
                    p.control = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
                    if (!p.src || !p.dst || !p.upload || !p.readback || !p.control) {
                        printf("RESULT fail allocation %s %lux%lu (%llu MiB)\n", pd->name, (unsigned long)width,
                               (unsigned long)height, (unsigned long long)sizes[s]);
                        return 1;
                    }
                    p.src.label = [NSString stringWithFormat:@"tier-src-%s-%llu-%u", pd->name, (unsigned long long)sizes[s], c];
                    p.dst.label = [NSString stringWithFormat:@"tier-dst-%s-%llu-%u", pd->name, (unsigned long long)sizes[s], c];
                    [set addObject:p];
                }
            }
        printTier("allocated", NULL);

        int bad = 0;
        // Warm: the first staged copy of each size, ascending, which allocates that scratch class.
        phase_stats warm = {.name = "warm"};
        double w0 = nowS();
        for (Pair *p in set) {
            char label[64];
            @autoreleasepool {
                bad |= runPair(queue, p, &warm, true);
            }
            snprintf(label, sizeof label, "after-%s-%lu", p.path->name, (unsigned long)(p.bytes >> 20));
            printTier(label, NULL);
            if (gStop)
                break;
        }
        phasePrint(&warm, nowS() - w0);
        printTier("warm", NULL);

        // Hold: the whole working set alive, copied and verified round robin.
        phase_stats hold = {.name = "hold"};
        double h0 = nowS(), lastTier = h0;
        while (!gStop && nowS() - h0 < seconds && (keepGoing || !bad)) {
            for (Pair *p in set) {
                @autoreleasepool {
                    bad |= runPair(queue, p, &hold, false);
                }
                if (gStop || (!keepGoing && bad))
                    break;
            }
            if (nowS() - lastTier >= 15) {
                lastTier = nowS();
                printTier("hold", NULL);
            }
        }
        phasePrint(&hold, nowS() - h0);

        // Linger: scratch BOs stay with the device until the process exits.
        tier_line lingerLine = {0};
        printTier("linger", &lingerLine);
        printf("linger begin seconds=%g\n", linger);
        fflush(stdout);
        for (double l0 = nowS(); !gStop && nowS() - l0 < linger;)
            usleep(200000);
        printf("linger end\n");

        int rc = bad || gStop ? 1 : 0;
        if (!rc && requireTier) {
            unsigned fail = tier_live_verdict(&before, &lingerLine, expectBos, expectBytes >> 20);
            if (fail) {
                printf("require-tier not met (expected mode=1 state=ready)");
                printFailures(fail);
                putchar('\n');
                rc = 4;
            }
        }
        printf("RESULT %s warm-mismatches=%llu hold-mismatches=%llu control-mismatches=%llu errors=%llu hold-iterations=%zu "
               "hold-copied-mib=%.0f\n",
               rc == 0 ? "pass" : rc == 4 ? "fail-tier" : "fail", (unsigned long long)warm.mismatches,
               (unsigned long long)hold.mismatches, (unsigned long long)(warm.control_mismatches + hold.control_mismatches),
               (unsigned long long)(warm.errors + hold.errors), hold.n, (double)hold.bytes / 1048576.0);
        fflush(stdout);
        free(warm.wall);
        free(warm.gpu);
        free(hold.wall);
        free(hold.gpu);
        return rc;
    }
}
