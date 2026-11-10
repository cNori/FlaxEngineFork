// Copyright (c) Wojciech Figat. All rights reserved.

// Combined mask sampling reuses the cascade helper implementation.
#ifdef _PS_V2CombinedShadowMask
#define _PS_V2CascadeShadowMask 1
#endif

#include "./Flax/Common.hlsl"
#include "./Flax/Math/Math.hlsl"

#include "./Flax/Utils/ShadowProjection.hlsl"
#include "./Flax/Utils/ShadowReceiver.hlsl"

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

#include "./Flax/Lighting/Lighting.hlsl"

META_CB_BEGIN(3, V2DirectionalLightingData)
float4x4 LightingInverseViewProjection;
float4x4 LightingLightViewProjection;
float3 LightingDirectionToLight;
float LightingAmbientIntensity;
float3 LightingCameraPosition;
uint LightingIsOrthographic;
float3 LightingCameraForward;
uint LightingUseFilteredShadow;
float4 LightingRadiance;
float4 LightingShadowFilter; // Light depth range, projection scale in pixels, tan(light angular radius), adaptive radius in texels
float4x4 LightingNearLightViewProjection;
float4 LightingShadowTiles[2];
float4 LightingShadowSplit;
float4 LightingCascadeRange; // Inner radius, fade start radius, outer radius.
float4 LightingCascadeParameters; // Normal bias in world units, cascade index, debug enabled, screen filter radius.
float4 LightingReductionFootprint; // XY: maximum reduced-sample displacement in normalized UV.
float4 LightingDebugOptions; // X: lighting mode (0 material, 1 white, 3 blocker distance, 4 contact, 5 blur bands); W: blocker display range in centimeters.
float4 LightingShadowSampling; // X: adaptive jitter; W: world-origin noise phase.
float4x4 LightingCameraViewProjection;
float4 LightingBlockerParameters; // X: legacy PCSS flag, Y: blur band count, Z: full light-map resolution.
float4 LightingContactParameters; // World length, thickness, camera fade distance, reserved.
META_CB_END

Texture2D<float> LightingReceiverDepth : register(t2);
Texture2D<float> LightingLightDepth : register(t7);
Texture2D<float4> LightingNormals : register(t8);
Texture2D<float4> LightingBaseColor : register(t9);
Texture2D<float4> LightingMaterial : register(t10);
Texture2D<float4> LightingEmissive : register(t11);
Texture2D<float2> RawShadowMask : register(t12);
Texture2D<float> FilteredShadowMask : register(t13);
struct V2BlurBand { float4 Range; float4 Settings; };
StructuredBuffer<V2BlurBand> V2BlurBands : register(t0);

#ifdef _PS_V2CombinedShadowMask
// One descriptor per radial band. Every depth map shares the light projection.
struct V2CascadeDescriptor { float4 Range; float4 Sampling; float4 Footprint; };
StructuredBuffer<V2CascadeDescriptor> V2CascadeDescriptors : register(t1);
static uint V2CascadeCount;
static uint V2FirstCascade;
static float4 V2CurrentShadowFilter;
static float4 V2CurrentShadowTiles[2];
static float4 V2CurrentShadowSplit;
static float4 V2CurrentCascadeRange;
static float4 V2CurrentCascadeParameters;
static float4 V2CurrentReductionFootprint;

void V2InitializeCascadeCount() { V2CascadeCount = (uint)LightingCascadeRange.w; V2FirstCascade = (uint)LightingCascadeRange.x; }

void V2SelectCascade(uint index)
{
    V2CascadeDescriptor band = V2CascadeDescriptors[V2FirstCascade + index];
    V2CurrentShadowFilter = LightingShadowFilter;
    V2CurrentShadowFilter.w = band.Sampling.w;
    V2CurrentShadowTiles[0] = float4(0, 0, band.Sampling.xx);
    V2CurrentShadowTiles[1] = 0;
    V2CurrentShadowSplit = LightingShadowSplit;
    V2CurrentShadowSplit.w = band.Sampling.y;
    V2CurrentCascadeRange = band.Range;
    V2CurrentCascadeParameters = float4(band.Sampling.z, band.Footprint.z, 0, 0);
    V2CurrentReductionFootprint = float4(band.Footprint.xy, 0, 0);
}
Texture2D<float> V2CascadeDepth0 : register(t20);
Texture2D<float> V2CascadeDepth1 : register(t21);
Texture2D<float> V2CascadeDepth2 : register(t22);
Texture2D<float> V2CascadeDepth3 : register(t23);
#define LightingShadowFilter V2CurrentShadowFilter
#define LightingShadowTiles V2CurrentShadowTiles
#define LightingShadowSplit V2CurrentShadowSplit
#define LightingCascadeRange V2CurrentCascadeRange
#define LightingCascadeParameters V2CurrentCascadeParameters
#define LightingReductionFootprint V2CurrentReductionFootprint
#endif

float V2LoadLightDepth(int3 texel, Texture2D<float> lightDepth) { return lightDepth.Load(texel); }
#if CAN_USE_GATHER && !defined(WGSL)
float4 V2GatherLightDepth(float2 uv, Texture2D<float> lightDepth) { return lightDepth.GatherRed(SamplerPointClamp, uv); }
#endif
void V2LightDepthDimensions(out uint width, out uint height, Texture2D<float> lightDepth) { lightDepth.GetDimensions(width, height); }

float4 ReconstructV2ReceiverH(float2 pixelCenter, float depth, float2 size)
{
    float2 ndc = pixelCenter / size * float2(2, -2) + float2(-1, 1);
    return PROJECT_POINT(float4(ndc, depth, 1), LightingInverseViewProjection);
}

float3 ReconstructV2Receiver(float2 pixelCenter, float depth, float2 size)
{
    float4 receiver = ReconstructV2ReceiverH(pixelCenter, depth, size);
    return receiver.xyz / receiver.w;
}

float V2PixelsPerWorldUnit(float3 receiver)
{
    float viewDepth = LightingIsOrthographic ? 1.0 :
        max(dot(receiver - LightingCameraPosition, LightingCameraForward), 0.001);
    return LightingShadowFilter.y / viewDepth;
}

float4 V2NearShadowSample(float3 receiver)
{
    if (LightingShadowSplit.z == 0)
        return 0;
    float viewDepth = dot(receiver - LightingCameraPosition, LightingCameraForward);
    if (viewDepth >= LightingShadowSplit.y)
        return 0;
    float3 coord = ProjectV2Shadow(receiver, LightingNearLightViewProjection);
    return float4(coord, V2ShadowTierWeight(coord, viewDepth, LightingShadowSplit.xy,
        LightingShadowTiles[1].zw, LightingShadowFilter.w));
}

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)
float2 V2InvAtlasSize(Texture2D<float> lightDepth)
{
    uint width, height;
    V2LightDepthDimensions(width, height, lightDepth);
    return 1.0 / float2(width, height);
}

float4x4 V2LightingShadowProjection(uint tier)
{
    return tier == 0 ? LightingLightViewProjection : LightingNearLightViewProjection;
}

// A reduced texel stores the NEAREST sample across its source footprint,
// not the depth at its centre. On the receiver plane this difference is bounded
// by |depth gradient| dot footprint radius. Compensate that displacement only;
// the unreduced map keeps its original comparison and bias.
float V2LightingReceiverBias(float2 depthPerTexel, float2 size, float4x4 projection, float3 uncertainty)
{
    float bias = V2ReceiverDepthBias(LightingShadowSplit.w, depthPerTexel, size, projection, uncertainty);
    return bias + dot(abs(depthPerTexel) * size, LightingReductionFootprint.xy);
}

float4 GatherV2ReceiverSeparation(float2 texel, float2 receiverTexel, float4 tile, float2 invAtlasSize,
    float depth, float2 depthPerTexel, float bias, out float2 blend, Texture2D<float> lightDepth)
{
    // Gather order is bottom-left, bottom-right, top-right, top-left.
    // Clamp the footprint to this tile before gathering, including atlas seams.
    float2 clamped = clamp(texel, 0, tile.zw - 1);
    float2 first = min(floor(clamped), tile.zw - 2);
    blend = clamped - first;
#if CAN_USE_GATHER && !defined(WGSL)
    float4 blockers = V2GatherLightDepth((tile.xy + first + 1.0) * invAtlasSize, lightDepth);
#else
    int2 origin = int2(tile.xy + first);
    float4 blockers = float4(
        V2LoadLightDepth(int3(origin + int2(0, 1), 0), lightDepth),
        V2LoadLightDepth(int3(origin + int2(1, 1), 0), lightDepth),
        V2LoadLightDepth(int3(origin + int2(1, 0), 0), lightDepth),
        V2LoadLightDepth(int3(origin, 0), lightDepth));
#endif
    float2 x = (first.x + float2(0, 1) - receiverTexel.x) * depthPerTexel.x;
    float2 y = (first.y + float2(0, 1) - receiverTexel.y) * depthPerTexel.y;
    float4 receiverDepth = depth + x.xyyx + y.yyxx;
    float4 separation = max(DEPTH_DIFF(receiverDepth, blockers) - bias, 0.0);
    return separation * float4(blockers != DEPTH_RANGE_MAX);
}

float BlendV2ShadowQuad(float4 values, float2 blend)
{
    return lerp(lerp(values.w, values.z, blend.x), lerp(values.x, values.y, blend.x), blend.y);
}

float SampleV2ReceiverPlane(float2 texel, float2 receiverTexel, float4 tile, float2 invAtlasSize,
    float depth, float2 depthPerTexel, float bias, Texture2D<float> lightDepth)
{
    float2 blend;
    float4 separation = GatherV2ReceiverSeparation(texel, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, blend, lightDepth);
    return BlendV2ShadowQuad(float4(separation <= 0.0), blend);
}

// Same 32-point Vogel disk used by Flax ShadowsSampling.hlsl.
static const float2 V2BlockerDisk[32] = { 
    
    float2(0.12500000, 0.00000000),
    float2(-0.15965044, 0.14624202),
    float2(0.02445674, -0.27843648),
    float2(0.20119321, 0.26248097),
    float2(-0.36925793, -0.06537271),
    float2(0.34984338, -0.22245139),
    float2(-0.11709833, 0.43521595),
    float2(-0.22302461, -0.42969179),
    float2(0.48406291, 0.17694080),
    float2(-0.50370997, 0.20772886),
    float2(0.24297905, -0.51873517),
    float2(0.17918217, 0.57207417),
    float2(-0.54061830, -0.31361890),
    float2(0.63443625, -0.13916063),
    float2(-0.38743055, 0.55047488),
    float2(-0.08905894, -0.69024885),
    float2(0.54879880, 0.46308208),
    float2(-0.73889750, 0.03009081),
    float2(0.53931046, -0.53597510),
    float2(-0.03660476, 0.77976608),
    float2(-0.51236552, -0.61490381),
    float2(0.81227481, 0.10993028),
    float2(-0.68869931, 0.47834957),
    float2(0.18879366, -0.83590192),
    float2(0.43436146, 0.75957572),
    float2(-0.85019755, -0.27210116),
    float2(0.82646751, -0.38088906),
    float2(-0.35873961, 0.85479879),
    float2(-0.31848884, -0.88836360),
    float2(0.84942913, 0.44759941),
    float2(-0.94430852, 0.24780297),
    float2(0.53754807, -0.83391672),
 };

float2 V2RotateBlockerTap(float2 tap, float2 rotation)
{
    return float2(rotation.x * tap.x - rotation.y * tap.y, rotation.y * tap.x + rotation.x * tap.y);
}

// Flax-style PCSS blocker average, with V2's receiver-plane correction.
// Only occluding depths contribute; no hidden depth layers are inferred.
float V2AverageBlockerGap(float2 receiverTexel, float depth, float2 gradient, float bias, float4 tile,
    float radius, float2 rotation, Texture2D<float> lightDepth)
{
    float total = 0;
    uint blockers = 0;
    LOOP
    for (uint i = 0; i < 32; i++)
    {
        float2 texel = clamp(floor(receiverTexel + V2RotateBlockerTap(V2BlockerDisk[i], rotation) * radius + 0.5), 0, tile.zw - 1);
        float blocker = V2LoadLightDepth(int3(int2(tile.xy + texel), 0), lightDepth);
        float receiverDepth = depth + dot(gradient, texel - receiverTexel);
        float gap = DEPTH_DIFF(receiverDepth, blocker);
        if (blocker != DEPTH_RANGE_MAX && gap > bias)
        {
            total += gap;
            blockers++;
        }
    }
    return blockers > 0 ? total / blockers : -1.0;
}

float V2PCSSVisibility(float2 receiverTexel, float depth, float2 gradient, float bias, float4 tile,
    float2 invAtlasSize, float4x4 projection, float2 rotation, float maximumRadius, float angularTangent, float suppliedGap, Texture2D<float> lightDepth)
{
    maximumRadius = max(maximumRadius, 0.0);
    float gap = suppliedGap >= -1.0 ? suppliedGap : V2AverageBlockerGap(receiverTexel, depth, gradient, bias, tile, max(1.0, maximumRadius), rotation, lightDepth);
    if (gap < 0)
        return 1.0;
#if defined(WGSL)
    projection = transpose(projection);
#endif
    float3 axisX = float3(projection[0][0], projection[1][0], projection[2][0]);
    float3 axisY = float3(projection[0][1], projection[1][1], projection[2][1]);
    float3 axisZ = float3(projection[0][2], projection[1][2], projection[2][2]);
    float worldTexel = max(2.0 / (length(axisX) * tile.z), 2.0 / (length(axisY) * tile.w));
    float separationCm = gap / max(length(axisZ), 1e-12);
    float penumbra = separationCm * angularTangent / max(worldTexel, 1e-6);
    float radius = min(max(penumbra, 0.3), maximumRadius);
    float shadow = 0;
    LOOP
    for (uint i = 0; i < 32; i++)
    {
        float2 tap = receiverTexel + V2RotateBlockerTap(V2BlockerDisk[i], rotation) * radius;
        shadow += SampleV2ReceiverPlane(tap, receiverTexel, tile, invAtlasSize, depth, gradient, bias, lightDepth);
    }
    return shadow / 32.0;
}

#endif

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

// Also use the adaptive filter when a legacy view needs an explicit mask.
float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier, Texture2D<float> lightDepth);

float2 SampleV2MaskTileSettings(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier, float angularTangent, Texture2D<float> lightDepth)
{
    float4 tile = LightingShadowTiles[tier];
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float4x4 projection = V2LightingShadowProjection(tier);
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
    float coverage = SampleV2ReceiverPlane(receiverTexel, receiverTexel, tile, V2InvAtlasSize(lightDepth), coord.z, depthPerTexel, bias, lightDepth);
    if (coverage >= 1.0)
        return float2(1, 0.5);

    float2 texel = floor(receiverTexel + 0.5);
    float blocker = V2LoadLightDepth(int3(int2(tile.xy + texel), 0), lightDepth);
    float receiverDepth = coord.z + dot(depthPerTexel, texel - receiverTexel);
    float separation = DEPTH_DIFF(receiverDepth, blocker);
    if (blocker == DEPTH_RANGE_MAX || separation <= bias)
        return float2(coverage, 0.5);

    float penumbra = (separation - bias) * LightingShadowFilter.x * angularTangent;
    return float2(coverage, clamp(penumbra * V2PixelsPerWorldUnit(receiver), 0.5, 4.0));
}

float2 SampleV2MaskTile(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier, Texture2D<float> lightDepth)
{
    return SampleV2MaskTileSettings(receiver, normal, uncertainty, coord, tier, LightingShadowFilter.z, lightDepth);
}
// Pixel mode: nearest texel, one raw comparison. No bilinear coverage or blocker search.
float V2RawShadow(float3 coord, float3 normal, float3 uncertainty, uint tier, Texture2D<float> lightDepth)
{
    float4 tile = LightingShadowTiles[tier];
    float4x4 projection = LightingLightViewProjection;
    if (tier != 0) projection = LightingNearLightViewProjection;
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float2 texel = clamp(floor(receiverTexel + 0.5), 0, tile.zw - 1);
    float2 gradient = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(gradient, tile.zw, projection, uncertainty);
    float blocker = V2LoadLightDepth(int3(int2(tile.xy + texel), 0), lightDepth);
    // DEPTH_DIFF does not parenthesize its arguments in reversed depth.
    float receiverDepth = coord.z + dot(gradient, texel - receiverTexel);
    float gap = DEPTH_DIFF(receiverDepth, blocker);
    return blocker == DEPTH_RANGE_MAX || gap <= bias ? 1.0 : 0.0;
}

float2 V2ResolveBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier, Texture2D<float> lightDepth);


#endif



#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

#include "./Flax/Math/Noise.hlsl"

float SampleV2ShadowTileSettings(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier, float radiusSetting, bool pcss, float angularTangent, float suppliedGap, Texture2D<float> lightDepth)
{
    float4 tile = LightingShadowTiles[tier];
    uint width, height;
    V2LightDepthDimensions(width, height, lightDepth);
    float2 invAtlasSize = 1.0 / float2(width, height);
    float2 receiverTexel = uv * tile.zw - 0.5;
    float4x4 projection = LightingLightViewProjection; if (tier != 0) projection = LightingNearLightViewProjection;
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
    // Debug comparison: fixed disk/rings by default; random rotation/radii only when requested.
    bool jitterEnabled = LightingShadowSampling.x != 0.0;
    float noise = 0.0;
    BRANCH
    if (jitterEnabled)
    {
        // Camera-depth reconstruction must not steer the sampling pattern.
        // Integer light texels give stable, decorrelated rotations at a fixed map location.
        uint2 cell = (uint2)max(floor(receiverTexel), 0.0);
        uint hash = cell.x * 0x8da6b343u ^ cell.y * 0xd8163841u;
        hash ^= hash >> 16;
        hash *= 0x7feb352du;
        hash ^= hash >> 15;
        noise = (hash & 0x00ffffffu) * (1.0 / 16777216.0);
    }
    float sine = 0.0, cosine = 1.0;
    sincos(noise * 6.2831853, sine, cosine);
    BRANCH
    if (pcss)
        return V2PCSSVisibility(receiverTexel, depth, depthPerTexel, bias, tile, invAtlasSize, projection, float2(cosine, sine), radiusSetting, angularTangent, suppliedGap, lightDepth);
    // Large kernels use an irregular disk instead of concentric eight-spoke rings.
    // Keep every sample on the same receiver plane and preserve the requested radius.
    if (radiusSetting > 1.0)
    {
        float visibility = 0.0;
        LOOP
        for (uint tap = 0; tap < 32; tap++)
        {
            float2 offset = V2RotateBlockerTap(V2BlockerDisk[tap], float2(cosine, sine)) * radiusSetting;
            visibility += SampleV2ReceiverPlane(receiverTexel + offset, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, lightDepth);
        }
        return visibility / 32.0;
    }
    static const float2 directions[8] = {
        float2(1, 0), float2(0.7071068, 0.7071068), float2(0, 1), float2(-0.7071068, 0.7071068),
        float2(-1, 0), float2(-0.7071068, -0.7071068), float2(0, -1), float2(0.7071068, -0.7071068)
    };

    // GPU Gems 2, chapter 17. Probe the disk perimeter before refining mixed coverage.
    float2 offsets[8];
    float total = SampleV2ReceiverPlane(receiverTexel, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, lightDepth);
    UNROLL
    for (uint i = 0; i < 8; i++)
    {
        float2 direction = directions[i];
        offsets[i] = float2(cosine * direction.x - sine * direction.y, sine * direction.x + cosine * direction.y) * radiusSetting;
        float radius = jitterEnabled ? 0.84 + 0.16 * frac(noise + i * 0.618034) : 1.0;
        total += SampleV2ReceiverPlane(receiverTexel + offsets[i] * radius, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, lightDepth);
    }
    float sampleCount = 9.0;
    BRANCH
    if (total > 0.0 && total < 9.0)
    {
        UNROLL
        for (uint tap = 0; tap < 8; tap++)
        {
            float jitter = frac(noise + tap * 0.381966);
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * (jitterEnabled ? lerp(0.18, 0.55, jitter) : 0.35), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, lightDepth);
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * (jitterEnabled ? lerp(0.58, 0.81, 1.0 - jitter) : 0.7), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, lightDepth);
        }
        sampleCount = 25.0;
    }
    return total / sampleCount;
}

float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier, Texture2D<float> lightDepth)
{
    return SampleV2ShadowTileSettings(uv, depth, normal, uncertainty, pixel, tier, LightingShadowFilter.w,
#ifdef _PS_V2CombinedShadowMask
        false,
#else
        LightingBlockerParameters.x != 0.0,
#endif
        LightingShadowFilter.z, -2.0, lightDepth);
}

float2 V2EvaluateBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier, float4 settings, float gap, Texture2D<float> lightDepth)
{
    uint mode = (uint)settings.x;
    float4 tile = LightingShadowTiles[tier];
    float radius = min(32.0, settings.y * tile.z / max(LightingBlockerParameters.z, 1.0));
#ifdef _PS_V2CombinedShadowMask
    // Native normal-lighting band records always select the adaptive PCF path.
    if (radius > 0)
        return float2(SampleV2ShadowTileSettings(coord.xy, coord.z, normal, uncertainty, pixel, tier, radius, false, settings.z, gap, lightDepth), 0.5);
#else
    if ((mode == 2 || mode == 4) && radius > 0)
        return float2(SampleV2ShadowTileSettings(coord.xy, coord.z, normal, uncertainty, pixel, tier, radius, mode == 4, settings.z, gap, lightDepth), 0.5);
#endif
    float2 mask = SampleV2MaskTileSettings(receiver, normal, uncertainty, coord, tier, settings.z, lightDepth);
    // Hard is unblurred; ScreenSpace uses the selected fixed screen-pixel support.
    mask.y = mode == 3 ? clamp(settings.y, 0.5, 4.0) : 0.5;
    return mask;
}

// Returns normalized blocker gap and its world-space length along the light ray.
// Both normal rendering and debug use this lookup. Camera position is not involved.
// A single-layer shadow map only exposes the first surface seen by the light.
float2 V2BlurBlockerDistance(float3 coord, float3 normal, float3 uncertainty, uint tier, Texture2D<float> lightDepth)
{
    float4 tile = LightingShadowTiles[tier];
    float4x4 projection = LightingLightViewProjection;
    if (tier != 0) projection = LightingNearLightViewProjection;
    float2 gradient = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(gradient, tile.zw, projection, uncertainty);
    float radius = 1.0;
    uint count = (uint)LightingBlockerParameters.y;
    LOOP
    for (uint i = 0; i < count; i++)
    {
        float4 settings = V2BlurBands[i].Settings;
        if (settings.x == 2.0 || settings.x == 4.0)
            radius = max(radius, min(32.0, settings.y * tile.z / max(LightingBlockerParameters.z, 1.0)));
    }
    // Fixed light-map taps keep the distance estimate independent of frame jitter.
    float gap = V2AverageBlockerGap(coord.xy * tile.zw - 0.5, coord.z, gradient, bias, tile, radius, float2(1, 0), lightDepth);
#if defined(WGSL)
    projection = transpose(projection);
#endif
    float3 depthAxis = float3(projection[0][2], projection[1][2], projection[2][2]);
    return float2(gap, gap >= 0 ? gap / max(length(depthAxis), 1e-12) : -1.0);
}

// The triangular SC/DB matrix clamps BOTH endpoints of a blur transition.
// Outside a cascade pass there is no camera LOD floor.
uint V2EffectiveBlurBand(uint distanceBand)
{
    uint cameraCascade = 0;
#ifdef _PS_V2CascadeShadowMask
    cameraCascade = (uint)max(LightingCascadeParameters.y, 0.0);
#endif
    return min(max(distanceBand, cameraCascade), (uint)LightingBlockerParameters.y - 1);
}

float4 V2BlurBandDebug(float3 coord, float3 normal, float3 uncertainty, Texture2D<float> lightDepth)
{
    float2 blocker = V2BlurBlockerDistance(coord, normal, uncertainty, 0, lightDepth);
    if (blocker.x < 0) return float4(0.02, 0.1, 0.3, 1);
    float distance = blocker.y;
    static const float3 colors[8] = {
        float3(0.95, 0.2, 0.18), float3(0.2, 0.85, 0.3),
        float3(0.2, 0.5, 1.0), float3(1.0, 0.75, 0.15),
        float3(0.85, 0.25, 0.95), float3(0.1, 0.85, 0.9),
        float3(1.0, 0.45, 0.1), float3(0.7, 0.7, 0.7)
    };
    uint count = (uint)LightingBlockerParameters.y;
    LOOP
    for (uint i = 0; i < count; i++)
    {
        float4 range = V2BlurBands[i].Range;
        if (distance >= range.z) continue;
        uint a = V2EffectiveBlurBand(i);
        uint b = V2EffectiveBlurBand(min(i + 1, count - 1));
        float3 color = colors[a % 8];
        if (i + 1 < count && distance > range.y)
            color = lerp(color, colors[b % 8], smoothstep(range.y, range.z, distance));
        return float4(color, 1);
    }
    return float4(0.08, 0.08, 0.08, 1);
}

float2 V2ResolveBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier, Texture2D<float> lightDepth)
{
    uint count = (uint)LightingBlockerParameters.y;
    float2 blocker = V2BlurBlockerDistance(coord, normal, uncertainty, tier, lightDepth);
    if (blocker.x < 0) return float2(1, 0.5);
    float distance = blocker.y;
    LOOP
    for (uint i = 0; i < count; i++)
    {
        V2BlurBand band = V2BlurBands[i];
        if (distance >= band.Range.z) continue;
        uint aIndex = V2EffectiveBlurBand(i);
        uint bIndex = V2EffectiveBlurBand(min(i + 1, count - 1));
        float4 aSettings = V2BlurBands[aIndex].Settings;
        float2 a = V2EvaluateBlur(receiver, coord, normal, uncertainty, pixel, tier, aSettings, blocker.x, lightDepth);
        if (i + 1 >= count || distance <= band.Range.y)
            return a;
        float weight = 1.0 - smoothstep(band.Range.y, band.Range.z, distance);
        V2BlurBand next = V2BlurBands[bIndex];
        if (aIndex == bIndex || all(next.Settings == aSettings)) return a;
        float2 b = V2EvaluateBlur(receiver, coord, normal, uncertainty, pixel, tier, next.Settings, blocker.x, lightDepth);
        return lerp(b, a, weight);
    }
    return float2(1, 0.5);
}


#endif


#ifdef _PS_V2CascadeShadowMask

float4 V2CascadeMaskAtReceiver(float4 position, float3 receiver, float3 normal, float3 uncertainty, float receiverDistance, Texture2D<float> lightDepth)
{
    float3 biasedReceiver = V2OffsetShadowReceiver(receiver, normal, LightingDirectionToLight, LightingCascadeParameters.x);
    float3 coord = ProjectV2Shadow(biasedReceiver, LightingLightViewProjection);
    if (!IsV2ShadowCoordValid(coord))
        return 0;

    float weight = LightingCascadeRange.y < LightingCascadeRange.z
        ? 1.0 - smoothstep(LightingCascadeRange.y, LightingCascadeRange.z, receiverDistance) : 1.0;
#ifndef _PS_V2CombinedShadowMask
    if (LightingCascadeParameters.z == 1.0)
    {
        static const float3 colors[8] = {
            float3(0.95, 0.2, 0.18), float3(0.2, 0.85, 0.3),
            float3(0.2, 0.5, 1.0), float3(1.0, 0.75, 0.15),
            float3(0.85, 0.25, 0.95), float3(0.1, 0.85, 0.9),
            float3(1.0, 0.45, 0.1), float3(0.7, 0.7, 0.7)
        };
        return float4(colors[(uint)LightingCascadeParameters.y % 8], weight);
    }
    if (LightingDebugOptions.x == 5.0)
    {
        float3 color = V2BlurBandDebug(coord, normal, uncertainty, lightDepth).rgb;
        float alpha = weight;
        if (LightingCascadeParameters.z == 2.0)
        {
            // Blur colour is blocker-relative; contours show the independent camera ranges.
            float width = max(fwidth(receiverDistance), 1e-3);
            float endLine = 1.0 - smoothstep(width, 2.5 * width, abs(receiverDistance - LightingCascadeRange.z));
            float fadeLine = LightingCascadeRange.y < LightingCascadeRange.z
                ? 1.0 - smoothstep(width, 2.0 * width, abs(receiverDistance - LightingCascadeRange.y)) : 0.0;
            color = lerp(color, float3(0.02, 0.02, 0.02), fadeLine);
            color = lerp(color, float3(1, 1, 1), endLine);
            // The near band's fade reaches zero at its end: keep the boundary visible there.
            alpha = max(alpha, endLine);
        }
        return float4(color, alpha);
    }
    // Distance debug follows the SAME radial ranges, depth LOD, projection,
    // receiver bias and alpha blend as the shadow result. No parameter interpolation.
    if (LightingDebugOptions.x == 3.0)
    {
        float4 tile = LightingShadowTiles[0];
        float2 receiverTexel = coord.xy * tile.zw - 0.5;
        float2 gradient = V2ShadowDepthPerTexel(normal, LightingLightViewProjection, tile.zw);
        float bias = V2LightingReceiverBias(gradient, tile.zw, LightingLightViewProjection, uncertainty);
        float gap = V2AverageBlockerGap(receiverTexel, coord.z, gradient, bias, tile, max(1.0, LightingShadowFilter.w), float2(1, 0), lightDepth);
        if (gap < 0)
            return float4(0.02, 0.1, 0.3, weight);
        float value = saturate(gap * LightingShadowFilter.x / max(LightingDebugOptions.w, 0.01));
        return float4(value.xxx, weight);
    }
#endif
    float2 mask;
    BRANCH
    if (LightingBlockerParameters.w != 0)
        mask = float2(V2RawShadow(coord, normal, uncertainty, 0, lightDepth), 0.5);
    else if (LightingBlockerParameters.y > 0)
        mask = V2ResolveBlur(biasedReceiver, coord, normal, uncertainty, position.xy, 0, lightDepth);
    else if (LightingShadowFilter.w > 0)
        mask = float2(SampleV2ShadowTile(coord.xy, coord.z, normal, uncertainty, position.xy, 0, lightDepth), 0.5);
    else
    {
        mask = SampleV2MaskTile(receiver, normal, uncertainty, coord, 0, lightDepth);
        // Blend radius alongside visibility so filter changes use the same
        // radial transition, including transitions into a hard or adaptive band.
        mask.y = clamp(mask.y, 0.5, max(LightingCascadeParameters.w, 0.5));
    }
    return float4(mask, 0, weight);
}

#ifdef _PS_V2CombinedShadowMask
float4 V2SelectedCascadeMask(uint index, float4 position, float3 receiver, float3 normal, float3 uncertainty, float distance)
{
    // Branch once per contributing band, outside every blocker/filter tap.
    switch (index)
    {
    case 0: return V2CascadeMaskAtReceiver(position, receiver, normal, uncertainty, distance, V2CascadeDepth0);
    case 1: return V2CascadeMaskAtReceiver(position, receiver, normal, uncertainty, distance, V2CascadeDepth1);
    case 2: return V2CascadeMaskAtReceiver(position, receiver, normal, uncertainty, distance, V2CascadeDepth2);
    case 3: return V2CascadeMaskAtReceiver(position, receiver, normal, uncertainty, distance, V2CascadeDepth3);
    default: return 0;
    }
}
#endif

#ifdef _PS_V2CombinedShadowMask
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2CombinedShadowMask(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = LightingReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX) return 0;
    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, size);
    float3 receiver = receiverH.xyz / receiverH.w;
    float receiverDistance = length(receiver - LightingCameraPosition);
    float3 normal = V2ReceiverPlaneNormal(LightingReceiverDepth, receiver, depth, pixel.xy, size,
        LightingInverseViewProjection, DecodeNormal(LightingNormals.Load(pixel).xyz));
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
    // Front to back is equivalent to the old far-to-near alpha composition.
    // A fully contributing near band hides every farther one, so stop there.
    float2 visibility = 0;
    float remaining = 1;
    V2InitializeCascadeCount();
    uint count = V2CascadeCount;
    LOOP
    for (uint i = 0; i < count; i++)
    {
        V2SelectCascade(i);
        if (receiverDistance < LightingCascadeRange.x || receiverDistance >= LightingCascadeRange.z) continue;
        float4 band = V2SelectedCascadeMask(i, position, receiver, normal, uncertainty, receiverDistance);
        visibility += remaining * band.a * band.rg;
        remaining *= 1 - band.a;
        if (remaining == 0) break;
    }
    // Composite groups far to near in the render target, just like the reference.
    // Uncovered pixels retain the already-resolved farther group's visibility.
    float coverage = 1 - remaining;
    return coverage > 0 ? float4(visibility / coverage, 0, coverage) : 0;
}
#else
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2CascadeShadowMask(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = LightingReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX)
        discard;

    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, size);
    float3 receiver = receiverH.xyz / receiverH.w;
    float receiverDistance = length(receiver - LightingCameraPosition);
    if (receiverDistance < LightingCascadeRange.x || receiverDistance >= LightingCascadeRange.z)
        discard;

    float3 normal = V2ReceiverPlaneNormal(LightingReceiverDepth, receiver, depth, pixel.xy, size,
        LightingInverseViewProjection, DecodeNormal(LightingNormals.Load(pixel).xyz));
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
    float4 mask = V2CascadeMaskAtReceiver(position, receiver, normal, uncertainty, receiverDistance, LightingLightDepth);
    if (mask.a == 0) discard;
    return mask;
}
#endif

#endif

#ifdef _PS_V2DirectionalLighting
float SampleV2Shadow(float3 receiver, float3 normal, float3 uncertainty, float2 pixel, Texture2D<float> lightDepth)
{
    float4 nearSample = V2NearShadowSample(receiver);
    uint firstTier = nearSample.w >= 1.0 ? 1 : 0;
    uint lastTier = nearSample.w > 0.0 ? 1 : 0;
    float visibility = 0.0;
    LOOP
    for (uint tier = firstTier; tier <= lastTier; tier++)
    {
        float3 coord = tier == 0 ? ProjectV2Shadow(receiver, LightingLightViewProjection) : nearSample.xyz;
        float weight = tier == 0 ? 1.0 - nearSample.w : nearSample.w;
        float shadow = 1.0;
        if (IsV2ShadowCoordValid(coord))
        {
            BRANCH
            if (LightingBlockerParameters.w != 0)
                shadow = V2RawShadow(coord, normal, uncertainty, tier, lightDepth);
            else if (LightingBlockerParameters.y > 0)
                shadow = V2ResolveBlur(receiver, coord, normal, uncertainty, pixel, tier, lightDepth).x;
            else if (LightingShadowFilter.w > 0)
                shadow = SampleV2ShadowTile(coord.xy, coord.z, normal, uncertainty, pixel, tier, lightDepth);
            else
            {
                float4 tile = LightingShadowTiles[tier];
                float2 texel = coord.xy * tile.zw - 0.5;
                float4x4 projection = V2LightingShadowProjection(tier);
                float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
                float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
                shadow = SampleV2ReceiverPlane(texel, texel, tile, V2InvAtlasSize(lightDepth), coord.z, depthPerTexel, bias, lightDepth);
            }
        }
        visibility += shadow * weight;
    }
    return visibility;
}

// Adapted from Flax Shadows.shader::RayCastScreenSpaceShadow. This returns
// contact VISIBILITY only; it never replaces the PCSS blocker-distance estimate.
// Length/thickness here are world centimeters rather than Flax's screen-relative length.
float V2ContactVisibility(float3 receiver, float2 size)
{
    float lengthCm = LightingContactParameters.x;
    if (lengthCm <= 0 || dot(LightingDirectionToLight, LightingDirectionToLight) < 0.5)
        return 1.0;
    float cameraDistance = length(receiver - LightingCameraPosition);
    float fade = 1.0 - saturate(pow(cameraDistance / max(LightingContactParameters.z, 0.01), 2));
    if (fade <= 0)
        return 1.0;
    float4 startCS = PROJECT_POINT(float4(receiver, 1), LightingCameraViewProjection);
    float4 endCS = PROJECT_POINT(float4(receiver + normalize(LightingDirectionToLight) * lengthCm * fade, 1), LightingCameraViewProjection);
    if (startCS.w <= 1e-8 || endCS.w <= 1e-8)
        return 1.0;
    float3 start = startCS.xyz / startCS.w;
    float3 delta = endCS.xyz / endCS.w - start;
    float pixelSpan = max(abs(delta.x) * size.x, abs(delta.y) * size.y) * 0.5;
    if (pixelSpan < 1.0)
        return 1.0;
    float3 step = delta / 16.0;
    // Fixed midpoint start avoids adding animated noise to this short correction.
    float3 ray = start + delta / max(min(16.0, pixelSpan), 1.0) * 1.5;
    float occlusion = 0;
    LOOP
    for (uint i = 0; i < 16; i++)
    {
        float2 uv = ray.xy * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0) || any(uv >= 1) || ray.z < 0 || ray.z > 1)
            break;
        int2 pixel = int2(uv * size);
        float sceneDepth = LightingReceiverDepth.Load(int3(pixel, 0));
        if (sceneDepth != DEPTH_RANGE_MAX)
        {
            float3 surface = ReconstructV2Receiver(float2(pixel) + 0.5, sceneDepth, size);
            float4 rayH = PROJECT_POINT(float4(ray, 1), LightingInverseViewProjection);
            float3 rayWorld = rayH.xyz / rayH.w;
            float difference = dot(rayWorld - surface, LightingCameraForward) - 0.01;
            float thickness = max(LightingContactParameters.y, 0.01);
            float hardness = max(0.001, thickness * 0.1);
            float hit = saturate(difference / hardness) * saturate((thickness - difference) / hardness);
            occlusion = max(occlusion, hit);
        }
        ray += step;
    }
    return 1.0 - occlusion * fade;
}

// Distance along a directional-light ray, not a distance to the shadow edge.
// A standard shadow map stores only the first surface encountered from the light.
float V2BlockerDistanceCm(float3 receiver, float3 normal, float3 uncertainty, Texture2D<float> lightDepth)
{
    float3 coord = ProjectV2Shadow(receiver, LightingLightViewProjection);
    if (!IsV2ShadowCoordValid(coord))
        return -2.0;
    float4 tile = LightingShadowTiles[0];
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float2 gradient = V2ShadowDepthPerTexel(normal, LightingLightViewProjection, tile.zw);
    float bias = V2LightingReceiverBias(gradient, tile.zw, LightingLightViewProjection, uncertainty);
    float gap = V2AverageBlockerGap(receiverTexel, coord.z, gradient, bias, tile, max(1.0, LightingShadowFilter.w), float2(1, 0), lightDepth);
    return gap >= 0 ? gap * LightingShadowFilter.x : -1.0;
}
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2DirectionalLighting(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = LightingReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX)
        return 0;

    BRANCH
    if (LightingDebugOptions.x == 3.0)
    {
        uint receiverWidth, receiverHeight;
        LightingReceiverDepth.GetDimensions(receiverWidth, receiverHeight);
        float2 size = float2(receiverWidth, receiverHeight);
        float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, size);
        float3 receiver = receiverH.xyz / receiverH.w;
        // Orient the wide depth-plane fit using the local geometric plane, not
        // world-up: wheels, walls and other vertical receivers have valid normals too.
        // Derivatives use camera geometry only, so material normal maps cannot add
        // fake blocker distances. At silhouettes the checked fit may fall back here.
        float3 localPlane = cross(ddx(receiver), ddy(receiver));
        float localLengthSquared = dot(localPlane, localPlane);
        if (localLengthSquared <= 1e-20)
            return float4(1, 0.5, 0, 1);
        float3 geometricNormal = localPlane * rsqrt(localLengthSquared);
        float3 normal = V2ReceiverPlaneNormal(LightingReceiverDepth, receiver, depth, pixel.xy, size,
            LightingInverseViewProjection, geometricNormal);
        float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
        float distanceCm = V2BlockerDistanceCm(receiver, normal, uncertainty, LightingLightDepth);
        if (distanceCm == -2.0)
            return float4(1, 0, 1, 1);
        if (distanceCm < 0.0)
            return float4(0.02, 0.1, 0.3, 1);
        float value = saturate(distanceCm / max(LightingDebugOptions.w, 0.01));
        return float4(value.xxx, 1);
    }
    BRANCH
    if (LightingDebugOptions.x == 5.0)
    {
        uint width, height;
        LightingReceiverDepth.GetDimensions(width, height);
        float3 receiver = ReconstructV2Receiver(position.xy, depth, float2(width, height));
        float3 coord = ProjectV2Shadow(receiver, LightingLightViewProjection);
        if (!IsV2ShadowCoordValid(coord)) return float4(1, 0, 1, 1);
        float3 plane = cross(ddx(receiver), ddy(receiver));
        float planeLength = dot(plane, plane);
        if (planeLength <= 1e-20) return float4(0.08, 0.08, 0.08, 1);
        float3 normal = V2ReceiverPlaneNormal(LightingReceiverDepth, receiver, depth, pixel.xy,
            float2(width, height), LightingInverseViewProjection, plane * rsqrt(planeLength));
        float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, float2(width, height));
        float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
        return V2BlurBandDebug(coord, normal, uncertainty, LightingLightDepth);
    }
    BRANCH
    if (LightingDebugOptions.x == 4.0)
    {
        uint receiverWidth, receiverHeight;
        LightingReceiverDepth.GetDimensions(receiverWidth, receiverHeight);
        float2 size = float2(receiverWidth, receiverHeight);
        float3 receiver = ReconstructV2Receiver(position.xy, depth, size);
        float contact = V2ContactVisibility(receiver, size);
        return float4(contact.xxx, 1);
    }
    float4 packedNormal = LightingNormals.Load(pixel);
    float3 emissive = LightingEmissive.Load(pixel).rgb;
    int shadingModel = (int)(packedNormal.a * 3.999);
    bool whiteLighting = LightingDebugOptions.x == 1.0;
    if (shadingModel == SHADING_MODEL_UNLIT && !whiteLighting)
        return float4(LightingDebugOptions.y != 0.0 ? float3(0, 0, 0) : emissive, 1);

    float4 albedo = LightingBaseColor.Load(pixel);
    float3 material = LightingMaterial.Load(pixel).rgb;
    GBufferSample surface = (GBufferSample)0;
    surface.Normal = DecodeNormal(packedNormal.xyz);
    surface.Color = albedo.rgb;
    surface.AO = albedo.a;
    surface.Roughness = max(material.r, 0.04);
    surface.Metalness = material.g;
    surface.Specular = material.b;
    surface.ShadingModel = shadingModel;

    // Override only the lighting inputs: geometry, normal maps and shadow visibility stay intact.
    // Unlit and emissive materials also participate so the whole scene is comparable.
    BRANCH
    if (whiteLighting)
    {
        emissive = 0.0;
        surface.Color = 1.0;
        surface.AO = 1.0;
        surface.Roughness = 1.0;
        surface.Metalness = 0.0;
        surface.Specular = 0.0;
        surface.ShadingModel = SHADING_MODEL_LIT;
    }

    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, float2(width, height));
    surface.WorldPos = receiverH.xyz / receiverH.w;

    float3 light = LightingDirectionToLight;
    float nDotL = saturate(dot(surface.Normal, light));
    float3 result = LightingDebugOptions.y != 0.0 ? float3(0, 0, 0) :
        emissive + GetDiffuseColor(surface) * surface.AO * max(LightingAmbientIntensity, 0.0);
    if (nDotL > 0)
    {
        float visibility;
        BRANCH
        if (LightingUseFilteredShadow)
            visibility = FilteredShadowMask.Load(pixel);
        else if (LightingCascadeRange.w != 0.0)
            visibility = 1.0;
        else
        {
            float3 planeNormal = V2ReceiverPlaneNormal(LightingReceiverDepth, surface.WorldPos, depth, pixel.xy, float2(width, height),
                LightingInverseViewProjection, surface.Normal);
            float3 uncertainty = V2CameraDepthUncertainty(surface.WorldPos, receiverH.w, LightingInverseViewProjection);
            visibility = SampleV2Shadow(surface.WorldPos, planeNormal, uncertainty, position.xy, LightingLightDepth);
        }
        // Contact visibility supplements PCSS; it cannot brighten mapped shadows.
        float contactVisibility = 1.0;
        BRANCH
        if (visibility > 0.0)
            contactVisibility = V2ContactVisibility(surface.WorldPos, float2(width, height));
        visibility = min(visibility, contactVisibility);
        float3 toEye = LightingIsOrthographic ? -LightingCameraForward : normalize(LightingCameraPosition - surface.WorldPos);
        LightSample response = StandardShading(surface, 1.0, light, toEye, surface.Normal);
        result += (whiteLighting ? response.Diffuse : response.Diffuse + response.Specular) * visibility *
            nDotL * LightingRadiance.rgb * max(LightingRadiance.a, 0.0);
    }
    return float4(result, 1);
}

#endif

#endif
