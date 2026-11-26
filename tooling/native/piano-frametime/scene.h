// SPDX-License-Identifier: MIT
#pragma once

// Embed the shader so the probe can be copied as a single executable.
#define MSL(...) @#__VA_ARGS__
static NSString *const sceneSource = @"#include <metal_stdlib>\n" MSL(
    using namespace metal;

    struct Scene {
        float time;
        float aspect;
        uint iterations;
    };

    struct Vertex {
        float4 position [[position]];
        float3 color;
        float3 local;
    };

    constant float3 corners[] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1}
    };
    constant ushort indices[] = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,4,7, 0,7,3,
        1,2,6, 1,6,5, 0,1,5, 0,5,4, 3,7,6, 3,6,2
    };
    constant float3 faceColor[] = {
        {0.95,0.4,0.1}, {0.3,0.75,1}, {0.13,0.8,0.5},
        {0.8,0.32,0.15}, {0.3,0.48,0.95}, {0.9,0.7,0.2}
    };

    vertex Vertex cube_vertex(uint vertexID [[vertex_id]], uint instanceID [[instance_id]],
                              constant Scene &scene [[buffer(0)]])
    {
        const float spin = scene.time * 1.35 + float(instanceID) * 0.6;
        const float tilt = spin * 0.73;
        const float3 local = corners[indices[vertexID]];
        const float3 yaw = float3(local.x * cos(spin) + local.z * sin(spin), local.y,
                                 local.z * cos(spin) - local.x * sin(spin));
        const float3 p = float3(yaw.x, yaw.y * cos(tilt) - yaw.z * sin(tilt),
                                yaw.y * sin(tilt) + yaw.z * cos(tilt));
        const float depth = 4.5 + p.z;
        const float2 center = float2(float(instanceID % 4) * 0.46 - 0.69,
                                     float(instanceID / 4) * 0.61 - 0.61);
        const float yScale = min(0.59, 0.39 * scene.aspect);
        const float2 xy = p.xy * float2(yScale / scene.aspect, yScale);
        Vertex out;
        out.position = float4(center * depth + xy, depth - 1.0, depth);
        out.color = faceColor[vertexID / 6];
        out.local = local;
        return out;
    }

    fragment half4 cube_fragment(Vertex in [[stage_in]], constant Scene &scene [[buffer(0)]])
    {
        float2 wave = in.local.xy * 2.0 + in.local.z + scene.time * 0.5;
        for (uint i = 0; i < scene.iterations; ++i)
            wave = sin(wave.yx * float2(2.11, 1.91) + float2(1.8, 0.9));
        const float light = 0.78 + 0.13 * wave.x / (1.0 + abs(wave.x)) + 0.09 * in.local.y;
        return half4(half3(in.color * light), 1.0h);
    }
);
#undef MSL
