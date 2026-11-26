// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plugin 0012 on the build Mac, over the hostsim (as plugin_host): the plugin's own device path (NativeAdrenoMetalDevice
// -initWithAcceleratorPort:, i.e. deviceForPort with its m2v cache-directory probe), not native_bridge_metal_open directly.
// test-cache-fallback-host.sh runs it once under a sandbox that denies every write in the plugin's five candidates (the user cache and
// temp directories and /private/var/tmp: what Chrome's gpu.sb leaves the GPU helper) and once without, and reads its stderr for the
// plugin's lines.
//
//   cache_fallback_host PLUGIN_EXECUTABLE [--expect-no-device]
//
// It dlopens the plugin, asks the plugin's class for a device, prints `device: NAME` (or `no device`), and with a device makes a
// compute pipeline from the bridge's fixture library twice (the second from the runtime's in-memory memo) and dispatches it, which
// must complete (the hostsim's CP executes nothing, so no value is checked): `RESULT PASS` when all hold. With --expect-no-device a missing device is the pass (the known-negative: plugin 0011).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using LibraryWithAir = id (*)(const void *, size_t);
using FixtureAir = const void *(*)(size_t *);
static unsigned gFailures;
#define CHECK(cond, ...) do { if (!(cond)) { ++gFailures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2)
            return 2;
        const bool expectNone = argc > 2 && !strcmp(argv[2], "--expect-no-device");
        void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!plugin) {
            printf("FAIL: dlopen %s\n", dlerror());
            return 1;
        }
        auto libraryWithAir = (LibraryWithAir)dlsym(plugin, "native_bridge_metal_library_with_air");
        auto fixture = (FixtureAir)dlsym(plugin, "nmb_fixture_air");
        Class cls = objc_getClass("NativeAdrenoMetalDevice");
        if (!libraryWithAir || !fixture || !cls) {
            printf("FAIL: entry points (fixture, library, NativeAdrenoMetalDevice)\n");
            return 1;
        }
        // As Metal's plugin loader: alloc the principal class, then -initWithAcceleratorPort: (port 0 on the host: registry ID 0).
        id raw = ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc"));
        id<MTLDevice> device = ((id (*)(id, SEL, unsigned))objc_msgSend)(raw, sel_registerName("initWithAcceleratorPort:"), 0u);
        if (!device) {
            printf("no device\n");
            printf(expectNone ? "RESULT PASS\n" : "RESULT FAIL\n");
            return expectNone ? 0 : 1;
        }
        printf("device: %s\n", device.name.UTF8String);
        CHECK(!expectNone, "a device where none was expected");
        size_t bytes = 0;
        const void *air = fixture(&bytes);
        id<MTLLibrary> library = air ? libraryWithAir(air, bytes) : nil;
        CHECK(library, "the bridge's fixture library");
        if (!library)
            return 1;
        NSError *error = nil;
        id<MTLComputePipelineState> compute = nil;
        for (int round = 0; round < 2; round++) {
            compute = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scale_and_offset"] error:&error];
            CHECK(compute, "compute pipeline %d: %s", round, error.localizedDescription.UTF8String ?: "");
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        CHECK(queue, "command queue");
        if (!compute || !queue)
            return 1;
        std::vector<float> in(256);
        for (unsigned i = 0; i < in.size(); ++i)
            in[i] = (float)i;
        id<MTLBuffer> input = [device newBufferWithBytes:in.data() length:in.size() * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:in.size() * 4 options:MTLResourceStorageModeShared];
        const float scale = 2;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:compute];
        [ce setBuffer:input offset:0 atIndex:0];
        [ce setBuffer:output offset:0 atIndex:1];
        [ce setBytes:&scale length:4 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(in.size(), 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "compute: status %ld error %s", (long)cb.status,
              cb.error.localizedDescription.UTF8String ?: "none");
        // The hostsim's software CP executes nothing: the dispatch must complete, its numbers are not checked (as plugin_host).
        printf("compute: status %ld\n", (long)cb.status);
        printf(gFailures ? "RESULT FAIL\n" : "RESULT PASS\n");
        return gFailures ? 1 : 0;
    }
}
