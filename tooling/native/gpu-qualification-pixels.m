// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
// A fresh Metal client and completed render-pass clear/readback, no screen capture or Automation API.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue=[device newCommandQueue];
        if(!device || !queue) {puts("PIXELS_FAIL device/queue");return 1;}
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:256 height:256 mipmapped:NO];
        d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> texture=[device newTextureWithDescriptor:d];
        if(!texture) {puts("PIXELS_FAIL texture");return 1;}
        uint8_t *bytes=malloc(256*256*4);if(!bytes)return 1;
        for(unsigned round=0;round<3;++round) {
            const double colors[3][4]={{.25,.5,.75,1},{1,0,.5,1},{0,1,.25,1}};
            const double *c=colors[round];
            MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
            pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(c[0],c[1],c[2],c[3]);
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
            if(!command || !encoder) {puts("PIXELS_FAIL encoder");free(bytes);return 1;}
            [encoder endEncoding];[command commit];[command waitUntilCompleted];
            if(command.status!=MTLCommandBufferStatusCompleted) {puts("PIXELS_FAIL completion");free(bytes);return 1;}
            [texture getBytes:bytes bytesPerRow:256*4 fromRegion:MTLRegionMake2D(0,0,256,256) mipmapLevel:0];
            for(unsigned i=0;i<256*256*4;++i) {
                int expected=(int)(c[i%4]*255+.5);
                if(abs((int)bytes[i]-expected)>1) {printf("PIXELS_FAIL round=%u byte=%u got=%u expected=%d\n",round,i,bytes[i],expected);free(bytes);return 1;}
            }
        }
        free(bytes);puts("PIXELS_OK commands=3 pixels=196608 fresh-client=1");return 0;
    }
}
