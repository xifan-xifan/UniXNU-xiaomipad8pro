// SPDX-License-Identifier: BSD-2-Clause-Patent
// family_host's stand-in for a browser's GPU image: the plugin answers supportsFamily: by the image the call comes from (frontend
// 0011), so family_host asks through this library built under the name of that image (libGLESv2.dylib: ANGLE's) and under a
// neutral name (libFamilyOther.dylib) for the control. The call below is this image's own code, as ANGLE's is in its image; it must
// not be a tail call (the plugin would see family_host's return address), so the answer is read back after the call.
#import <Metal/Metal.h>
__attribute__((visibility("default"))) BOOL family_asker_supports(id<MTLDevice> device, NSInteger family)
{
    volatile BOOL answer = [device supportsFamily:(MTLGPUFamily)family];
    return answer;
}
