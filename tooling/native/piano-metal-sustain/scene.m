// SPDX-License-Identifier: MIT
#import "scene.h"
#import <simd/simd.h>

// Keep the runtime shader inside the binary so the runner stages a single file.
#define MSL(...) @#__VA_ARGS__
static NSString *const sceneSource = @"#include <metal_stdlib>\n" MSL(
    using namespace metal;
    struct Vertex { float4 position, normal; float2 uv; };
    struct Camera { float4 eye, right, up, forward; };
    struct Varying {
        float4 clip [[position]];
        float3 world, normal;
        float2 uv;
        float material [[flat]];
    };

    vertex Varying arenaVertex(uint id [[vertex_id]], device const Vertex *mesh [[buffer(0)]],
                               constant Camera &camera [[buffer(1)]], constant float4 &object [[buffer(2)]])
    {
        const Vertex v = mesh[id];
        const float a = object.w < 0 ? 0.0 : camera.forward.w * (0.24 + 0.11 * fract(object.w)) + object.w;
        const float c = cos(a), s = sin(a);
        const float3x3 rotate = float3x3(float3(c,0,-s), float3(0,1,0), float3(s,0,c));
        const float height = object.w < 0 ? 0.0 : object.z + 0.2 * sin(camera.forward.w * 1.6 + object.w);
        const float3 world = rotate * v.position.xyz * object.z + float3(object.x, height, object.y);
        const float3 relative = world - camera.eye.xyz;
        const float z = dot(relative, camera.forward.xyz);
        Varying out;
        out.clip = float4(dot(relative, camera.right.xyz) * camera.right.w,
                          dot(relative, camera.up.xyz) * camera.up.w, (z - 0.4) * (120.0 / 119.6), z);
        out.world = world;
        out.normal = rotate * v.normal.xyz;
        out.uv = v.uv;
        out.material = object.w;
        return out;
    }

    fragment half4 arenaFragment(Varying in [[stage_in]], constant Camera &camera [[buffer(1)]],
                                 texture2d<half> tiles [[texture(0)]])
    {
        constexpr sampler tileSampler(address::repeat, filter::linear);
        const float3 texel = float3(tiles.sample(tileSampler, in.uv).rgb);
        const float tint = fract(in.material * 0.371);
        const float3 material = in.material < 0 ? float3(0.35,0.42,0.48) :
            mix(float3(1.0,0.42,0.16), float3(0.16,0.65,1.0), tint);
        const float3 albedo = texel * material;
        const float3 n = normalize(in.normal);
        const float3 view = normalize(camera.eye.xyz - in.world);
        float3 color = albedo * (0.17 + 0.65 * max(dot(n, normalize(float3(-0.6,0.9,0.4))), 0.0));
        const float t = camera.forward.w;
        const float3 lightPositions[2] = {float3(11*cos(t), 6, 11*sin(t)), float3(-8, 8, 3)};
        const float3 lightColors[2] = {float3(1.0,0.53,0.20), float3(0.23,0.60,1.0)};
        for (uint i = 0; i < 2; ++i) {
            const float3 delta = lightPositions[i] - in.world;
            const float3 light = normalize(delta);
            const float diffuse = max(dot(n, light), 0.0);
            const float specular = pow(max(dot(n, normalize(light + view)), 0.0), 24.0);
            color += (albedo * diffuse + specular * 0.28) * lightColors[i] / (1 + 0.025 * dot(delta, delta));
        }
        const float fog = smoothstep(24.0, 66.0, distance(camera.eye.xyz, in.world));
        return half4(half3(mix(color, float3(0.055,0.095,0.16), fog)), 1.0h);
    }
);
#undef MSL

typedef struct { vector_float4 position, normal; vector_float2 uv; } Vertex;
typedef struct { vector_float4 eye, right, up, forward; } Camera;

enum {
    meshGrid = 12,
    crateVertices = 6 * (meshGrid + 1) * (meshGrid + 1),
    crateIndices = 6 * meshGrid * meshGrid * 6,
    tileSize = 64,
};

static void build_mesh(Vertex vertices[crateVertices + 4], uint16_t indices[crateIndices])
{
    size_t index = 0;
    for (unsigned face = 0; face < 6; ++face) {
        const unsigned axis = face / 2, u = (axis + 1) % 3, v = (axis + 2) % 3;
        const unsigned stride = meshGrid + 1, base = face * stride * stride;
        const bool reverse = face % 2;
        for (unsigned y = 0; y <= meshGrid; ++y) {
            for (unsigned x = 0; x <= meshGrid; ++x) {
                const float s = (float)x / meshGrid, t = (float)y / meshGrid;
                vector_float3 p = {0, 0, 0};
                p[axis] = reverse ? -1.0f : 1.0f;
                p[u] = 2 * s - 1;
                p[v] = 2 * t - 1;
                const vector_float3 core = simd_clamp(p, -0.68f, 0.68f);
                const vector_float3 normal = simd_normalize(p - core);
                vertices[base + y * stride + x] = (Vertex){
                    simd_make_float4(core + 0.32f * normal, 1), simd_make_float4(normal, 0), {s, t}};
                if (x == meshGrid || y == meshGrid)
                    continue;
                const uint16_t a = base + y * stride + x, b = a + 1, c = a + stride, d = c + 1;
                indices[index++] = a;
                indices[index++] = reverse ? c : b;
                indices[index++] = reverse ? b : c;
                indices[index++] = b;
                indices[index++] = reverse ? c : d;
                indices[index++] = reverse ? d : c;
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        const float x = i & 1 ? 1 : -1, z = i & 2 ? 1 : -1;
        vertices[crateVertices + i] = (Vertex){{50*x, 0, 50*z, 1}, {0, 1, 0, 0}, {20*x, 20*z}};
    }
}

static id<MTLTexture> make_tiles(id<MTLDevice> device)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                      width:tileSize height:tileSize mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    uint8_t rgba[tileSize][tileSize][4];
    for (unsigned y = 0; y < tileSize; ++y) {
        for (unsigned x = 0; x < tileSize; ++x) {
            const unsigned u = x % 16, v = y % 16;
            const bool grout = u < 2 || v < 2;
            const bool detail = (u == 3 || u == 12) && (v == 4 || v == 12);
            const unsigned grain = (x * 17 + y * 23 + x * y * 7) % 29;
            const unsigned value = grout ? 46 : detail ? 235 : 132 + 35 * ((x/16 + y/16) % 2);
            rgba[y][x][0] = value + grain / 2;
            rgba[y][x][1] = value + grain / 3;
            rgba[y][x][2] = value + grain / 4;
            rgba[y][x][3] = 255;
        }
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, tileSize, tileSize) mipmapLevel:0
                 withBytes:rgba bytesPerRow:sizeof rgba[0]];
    return texture;
}

@implementation SustainScene {
    id<MTLRenderPipelineState> _pipeline;
    id<MTLDepthStencilState> _depthState;
    id<MTLTexture> _depth, _tiles;
    id<MTLBuffer> _vertices, _indices;
    NSData *_objects;
    float _aspect;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device width:(NSUInteger)width height:(NSUInteger)height
                         draws:(NSUInteger)draws error:(NSError **)error
{
    self = [super init];
    if (!self)
        return nil;
    id<MTLLibrary> library = [device newLibraryWithSource:sceneSource options:nil error:error];
    if (!library)
        return nil;
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@"arenaVertex"];
    pipeline.fragmentFunction = [library newFunctionWithName:@"arenaFragment"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    _pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:error];
    if (!_pipeline)
        return nil;

    MTLDepthStencilDescriptor *depthState = [MTLDepthStencilDescriptor new];
    depthState.depthWriteEnabled = YES;
    depthState.depthCompareFunction = MTLCompareFunctionLess;
    _depthState = [device newDepthStencilStateWithDescriptor:depthState];
    MTLTextureDescriptor *depth = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                    width:width height:height mipmapped:NO];
    depth.storageMode = MTLStorageModePrivate;
    depth.usage = MTLTextureUsageRenderTarget;
    // One command queue orders writes to the shared depth attachment.
    _depth = [device newTextureWithDescriptor:depth];
    _tiles = make_tiles(device);

    Vertex vertices[crateVertices + 4];
    uint16_t indices[crateIndices];
    build_mesh(vertices, indices);
    _vertices = [device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
    _indices = [device newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];
    NSMutableData *data = [NSMutableData dataWithLength:draws * sizeof(vector_float4)];
    vector_float4 *objects = data.mutableBytes;
    const NSUInteger side = (NSUInteger)ceil(sqrt((double)draws));
    const float shift = ((float)side - 1) * 0.5f;
    for (NSUInteger i = 0; i < draws; ++i) {
        const float x = (float)(i % side) - shift, z = (float)(i / side) - shift;
        const float seed = i * 1.618f;
        objects[i] = (vector_float4){x * 2.1f + 0.21f * sinf(seed), z * 2.1f + 0.21f * cosf(seed),
                                    0.48f + (i % 6) * 0.09f, seed};
    }
    _objects = data;
    _aspect = (float)width / height;
    return _depthState && _depth && _tiles && _vertices && _indices ? self : nil;
}

- (BOOL)encode:(id<MTLCommandBuffer>)command color:(id<MTLTexture>)color time:(double)time
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.055, 0.095, 0.16, 1);
    pass.depthAttachment.texture = _depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder)
        return NO;

    const float t = (float)time;
    const vector_float3 eye = {26 * sinf(t * 0.19f), 12 + 2 * sinf(t * 0.27f), 26 * cosf(t * 0.19f)};
    const vector_float3 forward = simd_normalize((vector_float3){0, 1, 0} - eye);
    const vector_float3 right = simd_normalize(simd_cross(forward, (vector_float3){0, 1, 0}));
    const vector_float3 up = simd_cross(right, forward);
    const Camera camera = {simd_make_float4(eye, 1), simd_make_float4(right, 1.55f / _aspect),
                           simd_make_float4(up, 1.55f), simd_make_float4(forward, t)};
    [encoder setRenderPipelineState:_pipeline];
    [encoder setDepthStencilState:_depthState];
    [encoder setVertexBuffer:_vertices offset:0 atIndex:0];
    [encoder setVertexBytes:&camera length:sizeof camera atIndex:1];
    [encoder setFragmentBytes:&camera length:sizeof camera atIndex:1];
    [encoder setFragmentTexture:_tiles atIndex:0];
    const vector_float4 *objects = _objects.bytes;
    for (NSUInteger i = 0; i < _objects.length / sizeof(*objects); ++i) {
        [encoder setVertexBytes:&objects[i] length:sizeof objects[i] atIndex:2];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:crateIndices indexType:MTLIndexTypeUInt16
                            indexBuffer:_indices indexBufferOffset:0];
    }
    const vector_float4 floor = {0, 0, 1, -1};
    [encoder setVertexBytes:&floor length:sizeof floor atIndex:2];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:crateVertices vertexCount:4];
    [encoder endEncoding];
    return YES;
}
@end
