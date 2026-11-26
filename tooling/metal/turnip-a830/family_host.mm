// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU families and capability answers browsers read, from the staged plugin itself (gpu/family1,
// private analysis 20261124-browser-gpu §9): ANGLE's Metal display (Chrome's WebGL/GL), Dawn's Metal adapter (Chrome's WebGPU and
// Graphite) and WebKit's WebGPU decide on these answers before they draw anything.
//
//   family_host PLUGIN_EXECUTABLE TURNIP_DYLIB CACHE_DIR [METALLIB]     on the build Mac, over the hostsim (as plugin_host)
//   family_host --system [METALLIB]                                     on the tablet: MTLCreateSystemDefaultDevice (the installed plugin)
//
// It prints one `family NAME = 0|1` line per family, the plugin's blocker rows for the families it refuses (droidvmFamilyBlockers:),
// the capability answers ANGLE and Dawn read, and the checks of ANGLE's IsMetalRendererAvailable / DisplayMtl::initializeImpl as lines
// `gate NAME = PASS|FAIL (why)`. With METALLIB (a .metallib file, e.g. the one a browser embeds, carved on the machine that runs this:
// nothing of a browser is in this repository), the library is made with newLibraryWithData: as ANGLE makes its internal shaders, every
// function is made, and every kernel gets a compute pipeline: `metallib: N functions, F failed; K kernels, P pipelines failed`.
// --as LIB: the families and the gates are asked through LIB's family_asker_supports (family_asker.m built under an image name: the
// plugin answers by the calling image since frontend 0011), next to this program's own answers (`self family NAME = 0|1`).
// --expect-mac2 / --expect-no-mac2: LIB must (must not) be told Mac2; this program itself, a caller of no browser image, must never
// be told Mac2 (the answer every other client keeps). Exit 1 on a mismatch.
// Without --as the program's own calls are the caller: run a copy of it under an image's path (e.g. .../as-angle/libGLESv2.dylib, an
// executable of that name) and the plugin answers it as that image. That is the pad's way: its AMFI refuses every ad hoc library in a
// non-platform process (2026-11-24: plain, + disable-library-validation, + hardened runtime: "different Team IDs"), executables run.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <initializer_list>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using Open = id (*)(const char *, const char *, uint64_t, void (*)(const char *, const char *));
static unsigned gMissing;
static void missing(const char *cls, const char *selector) { ++gMissing; fprintf(stderr, "missing: %s %s\n", cls, selector); }

struct Family { const char *name; NSInteger value; };
static const Family kFamilies[] = {
    {"Apple1", 1001}, {"Apple2", 1002}, {"Apple3", 1003}, {"Apple4", 1004}, {"Apple5", 1005}, {"Apple6", 1006}, {"Apple7", 1007},
    {"Apple8", 1008}, {"Apple9", 1009}, {"Apple10", 1010}, {"Mac1", 2001}, {"Mac2", 2002}, {"Common1", 3001}, {"Common2", 3002},
    {"Common3", 3003}, {"MacCatalyst1", 4001}, {"MacCatalyst2", 4002}, {"Metal3", 5001}, {"Metal4", 5002},
};

using Asker = BOOL (*)(id<MTLDevice>, NSInteger);
static Asker gAsker;
static BOOL selfFamily(id<MTLDevice> d, NSInteger f) { return [d supportsFamily:(MTLGPUFamily)f]; }
static BOOL family(id<MTLDevice> d, NSInteger f) { return gAsker ? gAsker(d, f) : selfFamily(d, f); }

static unsigned metallibCheck(id<MTLDevice> device, const char *path)
{
    NSData *bytes = [NSData dataWithContentsOfFile:@(path) options:NSDataReadingMappedIfSafe error:nil];
    if (!bytes) {
        printf("metallib: cannot read %s\n", path);
        return 1;
    }
    // A file that is no metallib (a browser's framework binary) is searched for the first metallib embedded in it ("MTLB", version
    // 1.x, its size at offset 16): ANGLE's prebuilt internal shaders, as the browser hands them to newLibraryWithData:.
    if (bytes.length < 24 || memcmp(bytes.bytes, "MTLB", 4)) {
        const uint8_t *base = (const uint8_t *)bytes.bytes, *end = base + bytes.length;
        const uint8_t *found = nullptr;
        for (const uint8_t *p = base; p + 24 <= end; ++p)
            if (p[0] == 'M' && p[1] == 'T' && p[2] == 'L' && p[3] == 'B' && p[4] == 1 && p[5] == 0x80) {
                uint64_t size = 0;
                memcpy(&size, p + 16, 8);
                if (size >= 24 && size <= (uint64_t)(end - p)) {
                    found = p;
                    bytes = [bytes subdataWithRange:NSMakeRange(p - base, size)];
                    break;
                }
            }
        if (!found) {
            printf("metallib: no embedded metallib in %s\n", path);
            printf("gate angle-internal-shaders = FAIL (no metallib)\n");
            return 1;
        }
        printf("metallib: %lu bytes at offset 0x%lx of %s\n", (unsigned long)bytes.length, (unsigned long)(found - base), path);
    }
    dispatch_data_t data = dispatch_data_create(bytes.bytes, bytes.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    if (!library || error) {
        printf("metallib: newLibraryWithData failed: %s\n", error.localizedDescription.UTF8String ?: "nil library");
        printf("gate angle-internal-shaders = FAIL (newLibraryWithData)\n");
        return 1;
    }
    unsigned functions = 0, functionFailures = 0, kernels = 0, pipelineFailures = 0;
    for (NSString *name in library.functionNames) {
        ++functions;
        id<MTLFunction> function = [library newFunctionWithName:name];
        if (!function) {
            ++functionFailures;
            printf("metallib: function %s: not made\n", name.UTF8String);
            continue;
        }
        if (function.functionType != MTLFunctionTypeKernel)
            continue;
        ++kernels;
        NSError *pipelineError = nil;
        id<MTLComputePipelineState> pipeline = nil;
        // Functions with function constants need values; ANGLE gives them per use. Without values the pipeline is made only for
        // functions that have none: a function with constants is counted apart, not as a failure.
        if (function.functionConstantsDictionary.count) {
            printf("metallib: kernel %s: %lu function constants (pipeline not tried)\n", name.UTF8String,
                   (unsigned long)function.functionConstantsDictionary.count);
            continue;
        }
        pipeline = [device newComputePipelineStateWithFunction:function error:&pipelineError];
        if (!pipeline) {
            ++pipelineFailures;
            printf("metallib: kernel %s: pipeline failed: %s\n", name.UTF8String, pipelineError.localizedDescription.UTF8String ?: "?");
        }
    }
    printf("metallib: %u functions, %u failed; %u kernels, %u pipelines failed\n", functions, functionFailures, kernels, pipelineFailures);
    printf("gate angle-internal-shaders = %s\n", functionFailures || pipelineFailures ? "FAIL" : "PASS");
    return functionFailures || pipelineFailures ? 1 : 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int expectMac2 = -1;   // -1: not checked
        const char *asLib = nullptr;
        int a = 1;
        for (; a < argc; ++a) {
            if (!strcmp(argv[a], "--expect-mac2"))
                expectMac2 = 1;
            else if (!strcmp(argv[a], "--expect-no-mac2"))
                expectMac2 = 0;
            else if (!strcmp(argv[a], "--as") && a + 1 < argc)
                asLib = argv[++a];
            else
                break;
        }
        if (asLib) {
            void *lib = dlopen(asLib, RTLD_NOW | RTLD_LOCAL);
            gAsker = lib ? (Asker)dlsym(lib, "family_asker_supports") : nullptr;
            if (!gAsker) {
                printf("FAIL: asker %s: %s\n", asLib, dlerror());
                return 1;
            }
            printf("asking as: %s\n", asLib);
        }
        id<MTLDevice> device = nil;
        const char *metallib = nullptr;
        if (a < argc && !strcmp(argv[a], "--system")) {
            device = MTLCreateSystemDefaultDevice();
            metallib = a + 1 < argc ? argv[a + 1] : nullptr;
        } else if (argc - a >= 3) {
            void *plugin = dlopen(argv[a], RTLD_NOW | RTLD_LOCAL);
            if (!plugin) {
                printf("FAIL: dlopen %s\n", dlerror());
                return 1;
            }
            auto open = (Open)dlsym(plugin, "native_bridge_metal_open");
            auto lastError = (const char *(*)(void))dlsym(plugin, "nmb_last_error");
            setenv("TU_DARWIN_SERVICE", "AGXAccelerator", 1);
            setenv("METAL2VULKAN_BLOCK_MIB", "16", 0);
            device = open ? open(argv[a + 1], argv[a + 2], 0x4e41, missing) : nil;
            if (!device) {
                printf("FAIL: no device: %s\n", lastError ? lastError() : "no entry point");
                return 1;
            }
            metallib = argc - a >= 4 ? argv[a + 3] : nullptr;
        } else {
            fprintf(stderr, "usage: family_host [--as LIB] [--expect-mac2|--expect-no-mac2] PLUGIN TURNIP CACHE [METALLIB] | ... --system [METALLIB]\n");
            return 2;
        }
        if (!device) {
            printf("FAIL: no system Metal device\n");
            return 1;
        }
        printf("device: %s (registryID 0x%llx, unified %d, lowPower %d, removable %d, headless %d)\n", device.name.UTF8String,
               (unsigned long long)device.registryID, device.hasUnifiedMemory, device.lowPower, device.removable, device.headless);
        for (const Family &f : kFamilies)
            printf("family %s = %d\n", f.name, family(device, f.value));
        if (gAsker)
            for (const Family &f : kFamilies)
                printf("self family %s = %d\n", f.name, selfFamily(device, f.value));
        SEL blockers = sel_registerName("droidvmFamilyBlockers:");
        // The rows of Apple's tables the plugin finds unmet, for the families browsers ask about; for a family it answers true these
        // are the rows its claim waives (Apple1-3 today), so the rows Mac2 would add are the Mac2 lines missing from the Apple3 ones.
        for (NSInteger f : {(NSInteger)1003, (NSInteger)2002, (NSInteger)1004, (NSInteger)3003, (NSInteger)5001})
            if ([device respondsToSelector:blockers]) {
                NSString *text = ((NSString * (*)(id, SEL, NSInteger))objc_msgSend)(device, blockers, f);
                for (NSString *line in [text componentsSeparatedByString:@"\n"])
                    if (line.length)
                        printf("blocker %ld: %s\n", (long)f, line.UTF8String);
            }
        printf("argumentBuffersSupport = %lu\n", (unsigned long)device.argumentBuffersSupport);
        printf("readWriteTextureSupport = %lu\n", (unsigned long)device.readWriteTextureSupport);
        printf("areRasterOrderGroupsSupported = %d\n", device.rasterOrderGroupsSupported);
        printf("depth24Stencil8PixelFormatSupported = %d\n", device.depth24Stencil8PixelFormatSupported);
        printf("supportsBCTextureCompression = %d\n", device.supportsBCTextureCompression);
        printf("supports32BitFloatFiltering = %d\n", device.supports32BitFloatFiltering);
        printf("supports32BitMSAA = %d\n", device.supports32BitMSAA);
        printf("supportsQueryTextureLOD = %d\n", device.supportsQueryTextureLOD);
        printf("programmableSamplePositionsSupported = %d\n", device.programmableSamplePositionsSupported);
        for (NSUInteger s : {1, 2, 4, 8})
            printf("supportsTextureSampleCount %lu = %d\n", (unsigned long)s, [device supportsTextureSampleCount:s]);
        MTLSize t = device.maxThreadsPerThreadgroup;
        printf("maxThreadsPerThreadgroup = %lu %lu %lu, maxThreadgroupMemoryLength = %lu, maxBufferLength = %lu\n", (unsigned long)t.width,
               (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)device.maxThreadgroupMemoryLength,
               (unsigned long)device.maxBufferLength);

        // ANGLE (src/common/apple_platform_utils.mm IsMetalRendererAvailable; DisplayMtl::initializeImpl): on macOS the display exists
        // only for a default device in Mac2; initialize then needs Apple1 or Mac2 (requireGpuFamily2) and a command queue; the GL ES
        // version is 3.0 only with Apple4 or Mac2 (Mac1 in its terms), else 2.0, which leaves Chrome without WebGL2.
        const BOOL mac2 = family(device, 2002), apple1 = family(device, 1001), apple4 = family(device, 1004);
        printf("gate angle-metal-renderer-available (default device in Mac2) = %s\n", mac2 ? "PASS" : "FAIL");
        printf("gate angle-require-gpu-family2 (Apple1 or Mac2) = %s\n", apple1 || mac2 ? "PASS" : "FAIL");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        printf("gate angle-command-queue = %s\n", queue ? "PASS" : "FAIL");
        printf("angle es version = %s\n", apple4 || mac2 ? "3.0 (WebGL2)" : "2.0 (no WebGL2)");
        // Dawn (src/dawn/native/metal/PhysicalDeviceMTL.mm): no family refuses the adapter; GetMTLGPUFamily picks the limits row (Mac2
        // first, then Apple9..Apple1, else Mac1).
        const char *dawnRow = mac2 ? "Mac2" : nullptr;
        for (int f = 1009; !dawnRow && f >= 1001; --f)
            if (family(device, f))
                dawnRow = kFamilies[f - 1001].name;
        printf("dawn limits row = %s\n", dawnRow ?: "Mac1");

        unsigned failures = 0;
        if (metallib)
            failures += metallibCheck(device, metallib);
        if (expectMac2 >= 0 && mac2 != (BOOL)expectMac2) {
            printf("FAIL: Mac2 %s for %s\n", expectMac2 ? "expected" : "not expected", asLib ?: "this program");
            ++failures;
        }
        if (expectMac2 >= 0 && gAsker && selfFamily(device, 2002)) {
            printf("FAIL: Mac2 told to a caller of no browser image (this program)\n");
            ++failures;
        }
        printf("missing selectors: %u\n", gMissing);
        printf("%s\n", failures ? "RESULT FAIL" : "RESULT PASS");
        return failures ? 1 : 0;
    }
}
