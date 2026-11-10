// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __V2_SHADOW_PROJECTION__
#define __V2_SHADOW_PROJECTION__

float3 ProjectV2Shadow(float3 receiver, float4x4 projection)
{
    float4 clip = PROJECT_POINT(float4(receiver, 1), projection);
    float3 ndc = clip.xyz / clip.w;
    return float3(ndc.xy * float2(0.5, -0.5) + 0.5, ndc.z);
}

bool IsV2ShadowCoordValid(float3 coord)
{
    return all(coord.xy >= 0) && all(coord.xy < 1) && coord.z > 0 && coord.z < 1;
}

float V2ShadowTierWeight(float3 coord, float viewDepth, float2 split, float2 tileSize, float filterRadius)
{
    if (!IsV2ShadowCoordValid(coord) || viewDepth >= split.y)
        return 0.0;
    float2 edge = min(coord.xy, 1.0 - coord.xy) * tileSize;
    float borderWeight = smoothstep(0.0, filterRadius + 1.0, min(edge.x, edge.y));
    return borderWeight * (1.0 - smoothstep(split.x, split.y, viewDepth));
}

// Receiver-plane correction: how much LIGHT device depth changes for one light-map texel.
// Inputs: unit world-space normal, world-to-light clip matrix, light-map size in texels.
// Output: (depth change per +X texel, depth change per +Y texel), not world distance.
// Used to compare a sloped receiver at each neighboring texel's position instead of
// comparing every texel against the receiver's centre depth (which produces self-hits).
float2 V2ShadowDepthPerTexel(float3 normal, float4x4 lightProjection, float2 size)
{
    // The directional projection has orthogonal axes. Invert each scale to
    // express one texel of UV and one unit of depth in receiver world space.
#if defined(WGSL)
    lightProjection = transpose(lightProjection);
#endif
    float3 x = float3(lightProjection[0][0], lightProjection[1][0], lightProjection[2][0]);
    float3 y = float3(lightProjection[0][1], lightProjection[1][1], lightProjection[2][1]);
    float3 z = float3(lightProjection[0][2], lightProjection[1][2], lightProjection[2][2]);
    float normalZ = dot(normal, z);
    if (abs(normalZ) <= 1e-6 * length(z) * length(normal))
        return 0;
    float2 plane = float2(-2.0 * dot(normal, x) / dot(x, x), 2.0 * dot(normal, y) / dot(y, y));
    return plane * (dot(z, z) / normalZ) / size;
}

#endif
