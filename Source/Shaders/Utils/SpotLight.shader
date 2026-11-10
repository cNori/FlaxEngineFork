// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/Lighting/Lighting.hlsl"
#include "./Flax/Utils/ShadowReceiver.hlsl"
#include "./Flax/Utils/ShadowProjection.hlsl"

META_CB_BEGIN(0, V2SpotData)
float4x4 SpotInverseViewProjection;
float4x4 SpotLightViewProjection;
float4 SpotCameraPosition;
float4 SpotCameraForward;
float4 SpotLightPositionRange;
float4 SpotLightDirectionOuter;
float4 SpotLightRadiance;
float4 SpotConeFalloff;
float4 SpotShadowInfo;
float4 SpotOptions;
float4 SpotDefaultFilter;
META_CB_END

struct SpotFilterBand { float4 Range; float4 Parameters; };
StructuredBuffer<SpotFilterBand> SpotFilterBands : register(t0);
Texture2D<float> SpotReceiverDepth : register(t2);
#ifdef _PS_V2PointLighting
StructuredBuffer<float4x4> PointFaceMatrices : register(t12);
Texture2DArray<float> PointShadowDepth : register(t13);
static uint PointFace;
static float4x4 PointProjection;
static float3 PointForward;
uint SelectV2PointFace(float3 delta)
{
    float3 a = abs(delta);
    if (a.x >= a.y && a.x >= a.z) return delta.x >= 0 ? 0u : 1u;
    if (a.y >= a.z) return delta.y >= 0 ? 2u : 3u;
    return delta.z >= 0 ? 4u : 5u;
}
#define PUNCTUAL_SHADOW_PROJECTION PointProjection
#define PUNCTUAL_SHADOW_FORWARD PointForward
#else
Texture2D<float> SpotShadowDepth : register(t7);
#define PUNCTUAL_SHADOW_PROJECTION SpotLightViewProjection
#define PUNCTUAL_SHADOW_FORWARD SpotLightDirectionOuter.xyz
#endif
Texture2D<float4> SpotNormals : register(t8);
Texture2D<float4> SpotBaseColour : register(t9);
Texture2D<float4> SpotMaterial : register(t10);

float SpotCompare(int2 texel, float2 receiverTexel, float depth, float2 gradient, float bias)
{
    texel = clamp(texel, 0, (int)SpotShadowInfo.x - 1);
#ifdef _PS_V2PointLighting
    float blocker = PointShadowDepth.Load(int4(texel, PointFace, 0));
#else
    float blocker = SpotShadowDepth.Load(int3(texel, 0));
#endif
    if (blocker == DEPTH_RANGE_MAX) return 1;
    // Keep expressions outside DEPTH_DIFF: its reversed-Z macro arguments are
    // not parenthesized, so an inline depth+offset would reverse only half of it.
    float receiverDepth = depth + dot(gradient, float2(texel) - receiverTexel);
    return DEPTH_DIFF(receiverDepth, blocker) <= bias ? 1.0 : 0.0;
}

float SpotBilinearCompare(float2 texel, float2 receiverTexel, float depth, float2 gradient, float bias)
{
    int2 first = int2(floor(texel));
    float2 blend = frac(texel);
    float a = SpotCompare(first, receiverTexel, depth, gradient, bias);
    float b = SpotCompare(first + int2(1, 0), receiverTexel, depth, gradient, bias);
    float c = SpotCompare(first + int2(0, 1), receiverTexel, depth, gradient, bias);
    float d = SpotCompare(first + int2(1, 1), receiverTexel, depth, gradient, bias);
    return lerp(lerp(a, b, blend.x), lerp(c, d, blend.x), blend.y);
}

float SpotBandVisibility(float3 receiver, float3 normal, float3 uncertainty, float3 toLight, float4 settings)
{
    float axialDistance = max(dot(receiver - SpotLightPositionRange.xyz, PUNCTUAL_SHADOW_FORWARD), SpotShadowInfo.y);
    float worldTexel = 2 * axialDistance * SpotDefaultFilter.w / SpotShadowInfo.x;
    float normalOffset = settings.z * worldTexel * (1 - saturate(dot(normal, toLight)));
    receiver += normal * normalOffset;
    float3 coord = ProjectV2Shadow(receiver, PUNCTUAL_SHADOW_PROJECTION);
    if (any(coord.xy < 0) || any(coord.xy >= 1) || coord.z <= 0 || coord.z >= 1) return 1;
    float2 texel = coord.xy * SpotShadowInfo.x - 0.5;
    float2 gradient = V2PerspectiveShadowGradient(receiver, normal, PUNCTUAL_SHADOW_PROJECTION, SpotShadowInfo.xx);

    float4 h = PROJECT_POINT(float4(receiver, 1), PUNCTUAL_SHADOW_PROJECTION);
    float4 error = PROJECT_POINT(float4(uncertainty, 0), PUNCTUAL_SHADOW_PROJECTION);
    float3 ndc = h.xyz / h.w;
    float3 errorNdc = (error.xyz - ndc * error.w) / h.w;
    float cameraBias = abs(errorNdc.z - dot(gradient * SpotShadowInfo.x, errorNdc.xy * float2(0.5, -0.5)));
    float precision = SpotShadowInfo.w + 2.0 * 1.192092896e-7 * abs(coord.z);
    float slope = 0.125 * min(dot(abs(gradient), float2(1, 1)), 1.0 / SpotShadowInfo.x);
    // Convert a user bias measured in texels to perspective device depth at THIS receiver.
    float derivative = SpotShadowInfo.y * SpotShadowInfo.z /
        ((SpotShadowInfo.z - SpotShadowInfo.y) * axialDistance * axialDistance);
    float bias = precision + cameraBias + slope + settings.y * worldTexel * derivative;
    if (SpotOptions.z != 0)
        return SpotCompare(int2(floor(texel + 0.5)), texel, coord.z, gradient, bias);

    float radius = min(settings.x, 32.0);
    if (radius <= 0)
        return SpotBilinearCompare(texel, texel, coord.z, gradient, bias);
    // Light-texel anchored jitter: camera motion must not choose another pattern.
    uint2 cell = (uint2)max(floor(texel), 0);
    uint hash = cell.x * 0x8da6b343u ^ cell.y * 0xd8163841u;
    hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15;
    float phase = (hash & 0x00ffffffu) * (6.283185307 / 16777216.0);
    float total = 0;
    LOOP
    for (uint i = 0; i < 16; i++)
    {
        float angle = i * 2.39996323 + phase;
        float sine, cosine; sincos(angle, sine, cosine);
        float2 offset = float2(cosine, sine) * sqrt((i + 0.5) / 16.0) * radius;
        total += SpotBilinearCompare(texel + offset, texel, coord.z, gradient, bias);
    }
    return total / 16.0;
}

float SpotVisibility(float3 receiver, float3 normal, float3 uncertainty, float3 toLight)
{
    if (SpotOptions.x == 0) return 1;
    uint count = (uint)SpotOptions.w;
    if (count == 0) return SpotBandVisibility(receiver, normal, uncertainty, toLight, SpotDefaultFilter);
    float distance = length(receiver - SpotCameraPosition.xyz);
    float visibility = 0, remaining = 1;
    LOOP
    for (uint i = 0; i < count; i++)
    {
        SpotFilterBand band = SpotFilterBands[i];
        if (distance < band.Range.x || distance >= band.Range.z) continue;
        float weight = band.Range.y < band.Range.z ? 1 - smoothstep(band.Range.y, band.Range.z, distance) : 1;
        visibility += remaining * weight * SpotBandVisibility(receiver, normal, uncertainty, toLight, band.Parameters);
        remaining *= 1 - weight;
        if (remaining == 0) break;
    }
    return visibility + remaining;
}

float4 V2PunctualLighting(float4 position)
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = SpotReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX) return 0;
    uint width, height; SpotReceiverDepth.GetDimensions(width, height);
    float2 ndc = position.xy / float2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 receiverH = PROJECT_POINT(float4(ndc, depth, 1), SpotInverseViewProjection);
    float3 receiver = receiverH.xyz / receiverH.w;
    float3 fromLight = receiver - SpotLightPositionRange.xyz;
    float distanceSquared = dot(fromLight, fromLight);
    if (distanceSquared >= SpotLightPositionRange.w * SpotLightPositionRange.w || distanceSquared <= 1e-12) return 0;
    float3 toLight = -fromLight * rsqrt(distanceSquared);
    float cone = 1;
#ifndef _PS_V2PointLighting
    float cosine = dot(-toLight, SpotLightDirectionOuter.xyz);
    cone = saturate((cosine - SpotLightDirectionOuter.w) / max(SpotConeFalloff.x - SpotLightDirectionOuter.w, 1e-6));
    cone *= cone;
    if (cone <= 0) return 0;
#endif
    float normalizedDistanceSquared = distanceSquared / (SpotLightPositionRange.w * SpotLightPositionRange.w);
    float falloff;
    if (SpotConeFalloff.z != 0)
    {
        float cutoff = saturate(1 - normalizedDistanceSquared * normalizedDistanceSquared);
        falloff = cutoff * cutoff / max(distanceSquared * 0.0001, 0.01);
    }
    else
        falloff = pow(saturate(1 - normalizedDistanceSquared), SpotConeFalloff.y);

    float4 packedNormal = SpotNormals.Load(pixel);
    GBufferSample surface = (GBufferSample)0;
    surface.ShadingModel = (int)(packedNormal.a * 3.999);
    bool white = SpotOptions.y != 0;
    if (surface.ShadingModel == SHADING_MODEL_UNLIT && !white) return 0;
    surface.Normal = DecodeNormal(packedNormal.xyz);
    float4 albedo = SpotBaseColour.Load(pixel);
    float3 material = SpotMaterial.Load(pixel).rgb;
    surface.WorldPos = receiver;
    surface.Color = white ? float3(1, 1, 1) : albedo.rgb;
    surface.AO = white ? 1 : albedo.a;
    surface.Roughness = white ? 1 : max(material.r, 0.04);
    surface.Metalness = white ? 0 : material.g;
    surface.Specular = white ? 0 : material.b;
    if (white) surface.ShadingModel = SHADING_MODEL_LIT;
    float nDotL = saturate(dot(surface.Normal, toLight));
    if (nDotL <= 0) return 0;
    float visibility = 1;
    BRANCH
    if (SpotOptions.x != 0)
    {
#ifdef _PS_V2PointLighting
        PointFace = SelectV2PointFace(fromLight);
        PointProjection = PointFaceMatrices[PointFace];
        PointForward = PointFace < 2 ? float3(PointFace == 0 ? 1 : -1, 0, 0) :
            PointFace < 4 ? float3(0, PointFace == 2 ? 1 : -1, 0) : float3(0, 0, PointFace == 4 ? 1 : -1);
#endif
        float3 geometricNormal = V2ReceiverPlaneNormal(SpotReceiverDepth, receiver, depth, pixel.xy,
            float2(width, height), SpotInverseViewProjection, surface.Normal);
        float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, SpotInverseViewProjection);
        visibility = SpotVisibility(receiver, geometricNormal, uncertainty, toLight);
        visibility = lerp(1, visibility, SpotConeFalloff.w);
    }
    float3 toEye = SpotCameraPosition.w != 0 ? -SpotCameraForward.xyz : normalize(SpotCameraPosition.xyz - receiver);
    LightSample response = StandardShading(surface, 1, toLight, toEye, surface.Normal);
    float3 direct = white ? response.Diffuse : response.Diffuse + response.Specular;
    return float4(direct * nDotL * SpotLightRadiance.rgb * max(SpotLightRadiance.a, 0) * cone * falloff * visibility, 0);
}

#ifdef _PS_V2SpotLighting
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2SpotLighting(float4 position : SV_Position) : SV_Target
{
    return V2PunctualLighting(position);
}
#endif

#ifdef _PS_V2PointLighting
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2PointLighting(float4 position : SV_Position) : SV_Target
{
    return V2PunctualLighting(position);
}
#endif
