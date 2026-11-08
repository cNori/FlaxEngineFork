// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GBufferCommon.hlsl"

#define MAX_DIRECTIONAL_SLICES 24u
#define MIN_SHADOW_PAGE_RESOLUTION 64u

META_CB_BEGIN(0, GridView)
float4x4 CameraInverseProjection;
float4x4 CameraInverseView;
uint2 DepthSize;
uint DebugMode;
uint WorldShadowTableMask;
float4 WorldPageOriginAndSpan;
uint WorldPageResolution;
float3 PagePadding;
float CameraDepthClear;
float3 CameraForward;
META_CB_END

META_CB_BEGIN(1, GridLight)
float4x4 LightViewProjections[MAX_DIRECTIONAL_SLICES];
float4x4 InverseLightProjections[MAX_DIRECTIONAL_SLICES];
float4x4 LightViews[MAX_DIRECTIONAL_SLICES];
float4 LightPositionAndRange;
float4 LightDirectionAndOuterCos;
float4 LightColorAndFalloff;
float4 LightOptions;
float4 DirectionalSlices[MAX_DIRECTIONAL_SLICES];
int4 ShadowPages[MAX_DIRECTIONAL_SLICES]; // Atlas origin, resolution and array layer.
uint LightKind; // Point, directional, spot.
uint2 LightDepthSize;
uint LightFaceMask;
META_CB_END

Texture2D<float> CameraDepth : register(t0);
Texture2D<float4> SceneNormals : register(t1);
Texture2D<float4> SceneBaseColor : register(t2);
Texture2D<float4> SceneEmissive : register(t3);
Texture2DArray<float> BlockerDepth : register(t6);

float4 ReceiverScreenPosition(uint2 pixel)
{
    float2 ndc = (pixel + 0.5) / DepthSize * float2(2, -2) + float2(-1, 1);
    return float4(ndc, CameraDepth.Load(int3(pixel, 0)), 1);
}

bool ReadReceiver(float4 screen, out float3 position, out float4 view)
{
    position = 0;
    view = 0;
    if (screen.z == CameraDepthClear)
        return false;
    // Divide in camera space before adding the view translation.
    view = PROJECT_POINT(screen, CameraInverseProjection);
    if (view.w <= 0)
        return false;
    position = PROJECT_POINT(float4(view.xyz / view.w, 1), CameraInverseView).xyz;
    return all(isfinite(position));
}

bool ReadReceiver(float4 screen, out float3 position)
{
    float4 view;
    return ReadReceiver(screen, position, view);
}

bool ReadReceiver(uint2 pixel, out float3 position)
{
    return ReadReceiver(ReceiverScreenPosition(pixel), position);
}

uint DominantAxis(float3 direction)
{
    float3 size = abs(direction);
    if (size.x >= max(size.y, size.z))
        return 0;
    return size.y >= size.z ? 1 : 2;
}

float3 ReceiverNormal(float3 dx, float3 dy, float3 fallback)
{
    float3 normal = cross(dx, dy);
    float lengthSquared = dot(normal, normal);
    return lengthSquared > 1e-20 && isfinite(lengthSquared) ? normal * rsqrt(lengthSquared) : fallback;
}

bool IsInShadowMap(float2 uv, float z)
{
    return all(uv >= 0) && all(uv <= 1) && z >= 0 && z <= 1;
}

bool DirectionalPage(float3 position, uint page, out float3 ndc)
{
    ndc = 0;
    if ((LightFaceMask & (1u << page)) == 0)
        return false;
    ndc = PROJECT_POINT(float4(position, 1), LightViewProjections[page]).xyz;
    return all(abs(ndc.xy) <= 1) && ndc.z >= 0 && ndc.z <= 1;
}

float CameraViewDepth(float3 position)
{
    return dot(position - PROJECT_POINT(float4(0, 0, 0, 1), CameraInverseView).xyz, CameraForward);
}

uint DirectionalSlice(float cameraDepth)
{
    // Find the first slice whose far plane includes the receiver.
    uint first = 0;
    uint last = max(uint(LightOptions.w), 1u) - 1u;
    [loop]
    while (first < last)
    {
        uint middle = (first + last) / 2u;
        if (cameraDepth <= DirectionalSlices[middle].y)
            last = middle;
        else
            first = middle + 1u;
    }
    return first;
}

bool IsBlocked(float3 position, float3 normal, uint face, float3 ndc, float inverseClipW)
{
    float3 fromLight = position - LightPositionAndRange.xyz;
    float4 receiverView = LightKind == 1 ? PROJECT_POINT(float4(position, 1), LightViews[face]) :
        float4(PROJECT_POINT(float4(fromLight, 0), LightViews[face]).xyz, 1);
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (!IsInShadowMap(uv, ndc.z))
        return false;
    int4 page = ShadowPages[face];
    uint2 texel = min(uint2(uv * page.z), page.z - 1);
    float depth = BlockerDepth.Load(int4(page.xy + int2(texel), page.w, 0));
    if (depth == DEPTH_RANGE_MAX)
        return false;

    // Unproject the difference from the receiver to the sampled texel. Working
    // relative to the receiver avoids subtracting two reconstructed world positions.
    float2 tap = (float2(texel) + 0.5) / page.z;
    float2 tapNdc = tap * float2(2, -2) + float2(-1, 1);
    float4x4 inverseProjection = InverseLightProjections[face];
    float4 offset = PROJECT_POINT(float4(tapNdc - ndc.xy, depth - ndc.z, 0), inverseProjection);
    float blockerW = inverseClipW + offset.w;
    if (blockerW <= 0)
        return false;
    float3 toBlocker = (offset.xyz - receiverView.xyz * offset.w) / blockerW;
    float3 lightNormal = PROJECT_POINT(float4(normal, 0), LightViews[face]).xyz;
    float3 direction = LightKind == 1 ? float3(0, 0, 1) : normalize(receiverView.xyz + toBlocker);
    float incidence = dot(lightNormal, direction);
    if (abs(incidence) <= 1e-4)
        return false;

    // Compare in receiver-plane distance. Convert depth roundoff to that same
    // unit so the tolerance tracks perspective depth precision at long range.
    float separation = -dot(lightNormal, toBlocker) * sign(incidence);
    float depthMagnitude = max(depth, ndc.z);
    float depthStep = asfloat(asuint(depthMagnitude) + 1u) - depthMagnitude;
    float4 depthAxis = PROJECT_POINT(float4(0, 0, 1, 0), inverseProjection);
    float planeDepthError = abs(dot(lightNormal, depthAxis.xyz - receiverView.xyz * depthAxis.w)) * (2.0 * depthStep / blockerW);
    float tolerance = 0.02 + planeDepthError;
    if (LightKind == 1)
    {
        // The receiver is reconstructed from rasterized camera depth. Allow
        // for float roundoff in that position as well as in the shadow depth.
        float3 toCamera = position - PROJECT_POINT(float4(0, 0, 0, 1), CameraInverseView).xyz;
        float distance = max(abs(toCamera.x), max(abs(toCamera.y), abs(toCamera.z)));
        float positionStep = asfloat(asuint(distance) + 1u) - distance;
        // Hardware interpolates depth using rasterized subpixel coordinates.
        // Bound a subpixel of error in the shadow projection as a distance
        // along the receiver normal before comparing the two surfaces.
        float2 texelSize = 2.0 * abs(float2(inverseProjection[0][0], inverseProjection[1][1])) / page.z;
        float rasterError = dot(abs(lightNormal.xy), texelSize) * (1.0 / 128.0);
        float projectionStep = 1.192092896e-7 * abs(inverseProjection[2][2]);
        tolerance += 32.0 * positionStep + abs(incidence) * (4.0 * projectionStep) + rasterError;
    }
    if (LightKind != 1)
    {
        // A local light's float transform also rounds its distance from the
        // receiver. Account for that in world units after fitting the depth range.
        float lightDistance = max(abs(receiverView.x), max(abs(receiverView.y), abs(receiverView.z)));
        float positionStep = asfloat(asuint(lightDistance) + 1u) - lightDistance;
        tolerance += 8.0 * positionStep;
    }

    return separation > tolerance;
}

bool LocalShadowPage(float3 position, out uint face, out float4 clip)
{
    face = 0;
    clip = 0;
    float3 fromLight = position - LightPositionAndRange.xyz;
    float distanceSquared = dot(fromLight, fromLight);
    if (distanceSquared >= LightPositionAndRange.w * LightPositionAndRange.w)
        return false;
    if (LightKind == 2 && dot(fromLight, LightDirectionAndOuterCos.xyz) < sqrt(distanceSquared) * LightDirectionAndOuterCos.w)
        return false;
    if (LightKind == 0)
    {
        uint axis = DominantAxis(fromLight);
        face = 2u * axis + (fromLight[axis] < 0 ? 1u : 0u);
    }
    if ((LightFaceMask & (1u << face)) == 0)
        return false;
    clip = PROJECT_POINT(float4(position, 1), LightViewProjections[face]);
    return clip.w > 0 && all(abs(clip.xy) <= clip.w) && clip.z >= 0 && clip.z <= clip.w;
}

bool IsLocalLightBlocked(float3 position, float3 normal)
{
    uint face;
    float4 clip;
    if (!LocalShadowPage(position, face, clip))
        return false;
    float inverseClipW = rcp(clip.w);
    return IsBlocked(position, normal, face, clip.xyz * inverseClipW, inverseClipW);
}
float DirectionalOcclusion(float3 position, float3 normal, float cameraDepth)
{
    uint page = DirectionalSlice(cameraDepth);
    float3 ndc;
    if (!DirectionalPage(position, page, ndc))
        return 0;
    float blocked = IsBlocked(position, normal, page, ndc, 1) ? 1 : 0;
    float3 nextNdc;
    float4 range = DirectionalSlices[page];
    if (page + 1u < uint(LightOptions.w) && cameraDepth > range.z && DirectionalPage(position, page + 1u, nextNdc))
    {
        float next = IsBlocked(position, normal, page + 1u, nextNdc, 1) ? 1 : 0;
        return lerp(blocked, next, smoothstep(range.z, range.y, cameraDepth));
    }
    return blocked;
}

// Each address is a texel within an absolute world page. The dominant
// geometric normal picks the projection plane. Height distinguishes receivers
// sharing the same lateral address, such as a ceiling above a floor.
struct WorldShadowTexel
{
    float3 Position;
    uint Fingerprint;
    float3 Normal;
};

struct WorldShadowAddress
{
    WorldShadowTexel Texel;
    uint Key;
    uint Level;
    uint Axis;
    uint2 Pixel;
    float2 PagePosition;
};

uint MixWorldShadowKey(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float3 ToWorldShadowPlane(float3 position, uint axis)
{
    return axis == 0 ? position.yzx : (axis == 1 ? position.zxy : position);
}

float3 FromWorldShadowPlane(float3 position, uint axis)
{
    return axis == 0 ? position.zxy : (axis == 1 ? position.yzx : position);
}

uint WorldShadowLevel(float3 dx, float3 dy, uint axis)
{
    float2 footprintX = ToWorldShadowPlane(dx, axis).xy;
    float2 footprintY = ToWorldShadowPlane(dy, axis).xy;
    float a = dot(footprintX, footprintX);
    float b = dot(footprintX, footprintY);
    float c = dot(footprintY, footprintY);
    // Use the smaller screen-pixel dimension, with a bounded anisotropy ratio,
    // so a shallow camera angle alone cannot force a coarse shadow.
    float major = sqrt(max(0, 0.5 * (a + c + sqrt((a - c) * (a - c) + 4 * b * b))));
    float determinant = abs(footprintX.x * footprintY.y - footprintX.y * footprintY.x);
    float footprint = max(determinant / max(major, 1e-12), major * 0.25);
    float span = WorldPageOriginAndSpan.w;
    return (uint)clamp(floor(log2(max(1.0, footprint * WorldPageResolution / span))), 0.0, float(firstbithigh(WorldPageResolution)));
}

WorldShadowAddress GetWorldShadowAddress(float3 position, float3 normal, float3 dx, float3 dy)
{
    WorldShadowAddress address;
    address.Axis = DominantAxis(normal);
    float3 planeNormal = ToWorldShadowPlane(normal, address.Axis);
    float3 planePosition = ToWorldShadowPlane(position - WorldPageOriginAndSpan.xyz, address.Axis);
    float span = WorldPageOriginAndSpan.w;
    address.Level = WorldShadowLevel(dx, dy, address.Axis);
    uint resolution = WorldPageResolution >> address.Level;

    address.PagePosition = planePosition.xy / span;
    float2 page = floor(address.PagePosition);
    address.Pixel = min(uint2(frac(address.PagePosition) * resolution), resolution - 1u);
    float2 center = (page + (float2(address.Pixel) + 0.5) / resolution) * span;
    float2 delta = center - planePosition.xy;
    float height = planePosition.z - dot(planeNormal.xy, delta) / planeNormal.z;
    address.Texel.Position = position + FromWorldShadowPlane(float3(delta, height - planePosition.z), address.Axis);
    address.Texel.Normal = normal;

    // Position is stored at full precision. The height key only divides
    // layers of the same texel, using the finest pitch even in the coarse LODs.
    float heightCell = height * (4.0 * WorldPageResolution / span);
    uint heightKey = asuint(floor(heightCell + 0.5));
    uint gx = asuint(int(page.x)) * resolution + address.Pixel.x;
    uint gy = asuint(int(page.y)) * resolution + address.Pixel.y;
    uint baseKey = MixWorldShadowKey(gx) ^ MixWorldShadowKey(gy ^ 0x9e3779b9u);
    uint layerKey = MixWorldShadowKey(heightKey ^ (address.Level | (address.Axis << 5)));
    address.Key = max(1u, MixWorldShadowKey(baseKey ^ layerKey));
    address.Texel.Fingerprint = MixWorldShadowKey(gx ^ 0x85ebca6bu) ^ MixWorldShadowKey(gy ^ 0xc2b2ae35u) ^ layerKey;
    return address;
}

float WorldShadowOcclusion(WorldShadowTexel texel)
{
    if (LightKind == 1)
        return DirectionalOcclusion(texel.Position, texel.Normal, CameraViewDepth(texel.Position));
    if (IsLocalLightBlocked(texel.Position, texel.Normal))
        return 1;
    return 0;
}

float3 WorldShadowTangent(float4 screen, float4 view, int2 pixel, int2 axis)
{
    int2 lo = pixel - axis;
    int2 hi = pixel + axis;
    float loDepth = all(lo >= 0) ? CameraDepth.Load(int3(lo, 0)) : CameraDepthClear;
    float hiDepth = all(hi < int2(DepthSize)) ? CameraDepth.Load(int3(hi, 0)) : CameraDepthClear;
    float loChange = loDepth == CameraDepthClear ? 1e10 : screen.z - loDepth;
    float hiChange = hiDepth == CameraDepthClear ? 1e10 : hiDepth - screen.z;
    float depthChange = abs(loChange) < abs(hiChange) ? loChange : hiChange;
    float2 ndcStep = float2(axis) * float2(2, -2) / DepthSize;
    float3 viewPosition = view.xyz / view.w;
    float4 flatStep = PROJECT_POINT(float4(ndcStep, 0, 0), CameraInverseProjection);
    float3 tangent = (flatStep.xyz - viewPosition * flatStep.w) / view.w;
    if (abs(depthChange) < 1e10)
    {
        float4 step = PROJECT_POINT(float4(ndcStep, depthChange, 0), CameraInverseProjection);
        float3 surfaceTangent = (step.xyz - viewPosition * step.w) / view.w;
        if (dot(surfaceTangent, surfaceTangent) <= 64.0 * dot(tangent, tangent))
            tangent = surfaceTangent;
    }
    return PROJECT_POINT(float4(tangent, 0), CameraInverseView).xyz;
}

#if defined(_CS_GatherWorldShadowTexels)
RWBuffer<uint> WorldShadowKeys : register(u0);
RWStructuredBuffer<WorldShadowTexel> WorldShadowTexelOutput : register(u1);
RWByteAddressBuffer WorldShadowDispatchOutput : register(u2);
RWTexture2D<uint2> WorldShadowScreenOutput : register(u3);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 1)]
void CS_GatherWorldShadowTexels(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= DepthSize))
        return;
    WorldShadowScreenOutput[pixel] = 0;
    float4 screen = ReceiverScreenPosition(pixel);
    float4 view;
    float3 position;
    if (!ReadReceiver(screen, position, view))
        return;
    float4 surface = SceneNormals.Load(int3(pixel, 0));
    if (surface.a < 0.15)
        return;
    float3 dx = WorldShadowTangent(screen, view, int2(pixel), int2(1, 0));
    float3 dy = WorldShadowTangent(screen, view, int2(pixel), int2(0, 1));
    float3 shadingNormal = DecodeNormal(surface.xyz);
    float3 geometric = ReceiverNormal(dx, dy, shadingNormal);
    float3 normal = abs(dot(geometric, shadingNormal)) >= 0.5 ? geometric : shadingNormal;
    WorldShadowAddress address = GetWorldShadowAddress(position, normal, dx, dy);
    uint slot = address.Key & WorldShadowTableMask;
    [loop]
    for (uint attempt = 0; attempt < 16; ++attempt)
    {
        uint previous;
        InterlockedCompareExchange(WorldShadowKeys[slot], 0u, address.Key, previous);
        if (previous == 0)
        {
            WorldShadowTexelOutput[slot] = address.Texel;
            uint index;
            WorldShadowDispatchOutput.InterlockedAdd(12, 1, index);
            // The second half of the key buffer stores the compact work list.
            WorldShadowKeys[WorldShadowTableMask + 1u + index] = slot;
        }
        if (previous == 0 || previous == address.Key)
        {
            WorldShadowScreenOutput[pixel] = uint2(slot + 1u, address.Texel.Fingerprint);
            return;
        }
        slot = (slot + 1u) & WorldShadowTableMask;
    }
}
#endif

#if defined(_CS_SetWorldShadowDispatch)
ByteAddressBuffer WorldShadowDispatch : register(t10);
RWByteAddressBuffer WorldShadowArgs : register(u0);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(1, 1, 1)]
void CS_SetWorldShadowDispatch()
{
    uint groups = (WorldShadowDispatch.Load(12) + 63u) / 64u;
    WorldShadowArgs.Store3(0, uint3(min(groups, 256u), (groups + 255u) / 256u, 1u));
}
#endif

Texture2D<uint2> WorldShadowScreen : register(t4);
StructuredBuffer<WorldShadowTexel> WorldShadowTexels : register(t5);
Buffer<float> WorldShadowVisibility : register(t7);

#if defined(_CS_ProjectWorldShadowPages)
Buffer<uint> WorldShadowWork : register(t9);
ByteAddressBuffer WorldShadowDispatch : register(t10);
RWBuffer<float> WorldShadowVisibilityOutput : register(u0);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_ProjectWorldShadowPages(uint2 id : SV_DispatchThreadID)
{
    uint index = id.x + id.y * (256u * 64u);
    if (index >= WorldShadowDispatch.Load(12))
        return;
    uint slot = WorldShadowWork[WorldShadowTableMask + 1u + index];
    WorldShadowVisibilityOutput[slot] = WorldShadowOcclusion(WorldShadowTexels[slot]);
}
#endif

float ReadWorldShadowOcclusion(uint2 pixel, float4 screen, float4 view, float3 position, float3 shadingNormal)
{
    if (WorldShadowTableMask != 0)
    {
        uint2 entry = WorldShadowScreen.Load(int3(pixel, 0));
        if (entry.x != 0)
        {
            uint slot = entry.x - 1u;
            if (WorldShadowTexels[slot].Fingerprint == entry.y)
                return WorldShadowVisibility[slot];
        }
    }
    // A saturated page table can still evaluate the same world texel.
    float3 dx = WorldShadowTangent(screen, view, int2(pixel), int2(1, 0));
    float3 dy = WorldShadowTangent(screen, view, int2(pixel), int2(0, 1));
    float3 geometric = ReceiverNormal(dx, dy, shadingNormal);
    float3 normal = abs(dot(geometric, shadingNormal)) >= 0.5 ? geometric : shadingNormal;
    return WorldShadowOcclusion(GetWorldShadowAddress(position, normal, dx, dy).Texel);
}

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ShadowAmbient(Quad_VS2PS input) : SV_Target
{
    uint2 pixel = uint2(input.Position.xy);
    float depth = CameraDepth.Load(int3(pixel, 0));
    if (depth == CameraDepthClear)
        return float4(0.08, 0.09, 0.11, 1);
    float4 surface = SceneNormals.Load(int3(pixel, 0));
    float4 base = SceneBaseColor.Load(int3(pixel, 0));
    float3 emissive = SceneEmissive.Load(int3(pixel, 0)).rgb;
    return float4(base.rgb * (surface.a < 0.15 ? 1.0 : 0.24 * base.a) + emissive, 1);
}

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ResolveWorldGridLight(Quad_VS2PS input) : SV_Target
{
    uint2 pixel = uint2(input.Position.xy);
    float4 screen = ReceiverScreenPosition(pixel);
    float3 position;
    float4 view;
    bool valid = ReadReceiver(screen, position, view);
    if (!valid)
        return 0;

    float4 surface = SceneNormals.Load(int3(pixel, 0));
    if (surface.a < 0.15)
        return 0;
    float3 toLight = -LightDirectionAndOuterCos.xyz;
    float attenuation = 1;
    if (LightKind != 1)
    {
        toLight = LightPositionAndRange.xyz - position;
        float distanceSquared = dot(toLight, toLight);
        float radiusFraction = distanceSquared / (LightPositionAndRange.w * LightPositionAndRange.w);
        if (radiusFraction >= 1)
            return 0;
        toLight *= rsqrt(max(distanceSquared, 1e-6));
        if (LightOptions.y > 0)
        {
            float fade = saturate(1 - radiusFraction * radiusFraction);
            attenuation = fade * fade / (1 + distanceSquared * 0.0001);
        }
        else
            attenuation = pow(saturate(1 - sqrt(radiusFraction)), LightColorAndFalloff.w);
        if (LightKind == 2)
        {
            float cone = saturate((dot(-toLight, LightDirectionAndOuterCos.xyz) - LightDirectionAndOuterCos.w) /
                max(LightOptions.x - LightDirectionAndOuterCos.w, 1e-4));
            attenuation *= cone * cone;
        }
    }

    float3 shadingNormal = DecodeNormal(surface.xyz);
    float ndotl = saturate(dot(shadingNormal, toLight));
    float visibility = 1;
    if (LightOptions.z > 0 && ndotl * attenuation > 0)
        visibility -= LightOptions.z * ReadWorldShadowOcclusion(pixel, screen, view, position, shadingNormal);

    float3 base = SceneBaseColor.Load(int3(pixel, 0)).rgb;
    return float4(base * LightColorAndFalloff.rgb * (ndotl * attenuation * visibility), 0);
}

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ShowShadowPages(Quad_VS2PS input) : SV_Target
{
    uint2 pixel = uint2(input.Position.xy);
    float4 screen = ReceiverScreenPosition(pixel);
    float3 position;
    float4 view;
    float4 surface = SceneNormals.Load(int3(pixel, 0));
    if (!ReadReceiver(screen, position, view) || surface.a < 0.15)
        return 0;
    float3 shadingNormal = DecodeNormal(surface.xyz);
    float3 dx = WorldShadowTangent(screen, view, int2(pixel), int2(1, 0));
    float3 dy = WorldShadowTangent(screen, view, int2(pixel), int2(0, 1));
    float3 geometric = ReceiverNormal(dx, dy, shadingNormal);
    float3 normal = abs(dot(geometric, shadingNormal)) >= 0.5 ? geometric : shadingNormal;
    WorldShadowAddress address = GetWorldShadowAddress(position, normal, dx, dy);
    uint resolution = WorldPageResolution >> address.Level;
    float2 cells = frac(address.PagePosition) * resolution;
    float2 width = (abs(ToWorldShadowPlane(dx, address.Axis).xy) + abs(ToWorldShadowPlane(dy, address.Axis).xy)) *
        (resolution / WorldPageOriginAndSpan.w);

    float3 color = 0;
    float alpha = 0;
    if (DebugMode & 2u)
    {
        bool blocked = ReadWorldShadowOcclusion(pixel, screen, view, position, shadingNormal) >= 0.5;
        color = blocked ? float3(1, 0.35, 0.14) : float3(0.24, 0.95, 0.45);
        alpha = blocked ? 0.75 : 0.35;
    }

    if (DebugMode & 1u)
    {
        float2 pixelWidth = max(width, 1e-5);
        float2 fineDistance = abs(frac(cells + 0.5) - 0.5) / pixelWidth;
        float2 majorDistance = abs(frac(cells / 8.0 + 0.5) - 0.5) * 8.0 / pixelWidth;
        float2 fine = (1.0 - smoothstep(0.25, 1.0, fineDistance)) * (1.0 - smoothstep(0.16, 0.45, width));
        float2 major = (1.0 - smoothstep(0.4, 1.3, majorDistance)) * (1.0 - smoothstep(1.2, 3.5, width));
        float2 lines = smoothstep(1e-5, 1e-4, width) * max(fine * 0.35, major * 0.8);
        float2 edgeDistance = min(frac(address.PagePosition), 1.0 - frac(address.PagePosition)) * resolution / pixelWidth;
        float pageEdge = 1.0 - smoothstep(0.3, 1.5, min(edgeDistance.x, edgeDistance.y));
        float gridLine = max(pageEdge, max(lines.x, lines.y));

        const float3 pageColors[6] = {
            float3(0.4, 0.9, 1.0), float3(0.45, 1.0, 0.55), float3(1.0, 0.9, 0.3),
            float3(1.0, 0.55, 0.2), float3(1.0, 0.4, 0.85), float3(0.65, 0.55, 1.0)
        };
        float3 pageColor = pageColors[min(address.Level, 5u)];
        color = alpha > 0 ? lerp(color, pageColor, gridLine) : pageColor;
        alpha = max(alpha, max(gridLine, 0.13));
    }
    return float4(color, alpha);
}
#define CAMERA_TILE_SIZE 16u
#define SHADOW_TILE_SIZE 16u
#define SHADOW_TILE_COUNT (SHADOW_TILE_SIZE * SHADOW_TILE_SIZE)
#define BLOCKER_DEMAND_MAX_LOG2 20u
#define BLOCKER_DEMAND_LIMIT (1u << BLOCKER_DEMAND_MAX_LOG2)
#define SHADOW_RECEIVER_FAR (SHADOW_TILE_COUNT + 1u + BLOCKER_DEMAND_MAX_LOG2 + 1u)
#define SHADOW_FACE_STRIDE (SHADOW_RECEIVER_FAR + 1u)

META_CB_BEGIN(2, ShadowReceiverMask)
uint ShadowMaskOffset;
uint ShadowFaceCount;
uint ReceiverMaskPadding0;
uint ReceiverMaskPadding1;
META_CB_END

struct CameraReceiverTile
{
    float4 Minimum;
    float4 Maximum;
    float4 Position;
    float4 Surface; // Geometric normal and selected world texel pitch.
};

#ifdef _CS_BuildCameraReceiverTiles
RWStructuredBuffer<CameraReceiverTile> CameraTileOutput : register(u0);
groupshared uint3 ReceiverMinimum;
groupshared uint3 ReceiverMaximum;
groupshared uint NearestReceiver;

uint3 EncodeOrderedFloat(float3 value)
{
    uint3 bits = asuint(value);
    return bits ^ (((bits >> 31) * 0x7fffffffu) | 0x80000000u);
}

float3 DecodeOrderedFloat(uint3 value)
{
    return asfloat(value ^ ((((value >> 31) ^ 1u) * 0x7fffffffu) | 0x80000000u));
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 1)]
void CS_BuildCameraReceiverTiles(uint2 group : SV_GroupID, uint2 lane : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if (index == 0)
    {
        ReceiverMinimum = 0xffffffffu;
        ReceiverMaximum = 0u;
        NearestReceiver = 0xffffffffu;
    }
    GroupMemoryBarrierWithGroupSync();

    uint3 minimum = 0xffffffffu;
    uint3 maximum = 0u;
    uint nearest = 0xffffffffu;
    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 4; sampleIndex++)
    {
        uint2 pixel = group * CAMERA_TILE_SIZE + lane * 2u + uint2(sampleIndex & 1u, sampleIndex >> 1);
        if (any(pixel >= DepthSize))
            continue;
        float4 screen = ReceiverScreenPosition(pixel);
        float3 position;
        float4 view;
        if (ReadReceiver(screen, position, view))
        {
            // View depth is positive. Keep its high bits plus the pixel index
            // within the 16-by-16 tile to choose one reproducible sample.
            float depth = max(0, view.z / view.w);
            uint2 localPixel = pixel - group * CAMERA_TILE_SIZE;
            uint pixelIndex = localPixel.y * CAMERA_TILE_SIZE + localPixel.x;
            nearest = min(nearest, (asuint(depth) & ~255u) | pixelIndex);
            uint3 bits = EncodeOrderedFloat(position);
            minimum = min(minimum, bits);
            maximum = max(maximum, bits);
        }
    }
    if (minimum.x != 0xffffffffu)
    {
        InterlockedMin(ReceiverMinimum.x, minimum.x);
        InterlockedMin(ReceiverMinimum.y, minimum.y);
        InterlockedMin(ReceiverMinimum.z, minimum.z);
        InterlockedMax(ReceiverMaximum.x, maximum.x);
        InterlockedMax(ReceiverMaximum.y, maximum.y);
        InterlockedMax(ReceiverMaximum.z, maximum.z);
        InterlockedMin(NearestReceiver, nearest);
    }
    GroupMemoryBarrierWithGroupSync();

    if (index == 0)
    {
        CameraReceiverTile bounds;
        bool valid = ReceiverMinimum.x != 0xffffffffu;
        bounds.Minimum = float4(valid ? DecodeOrderedFloat(ReceiverMinimum) : 0, valid ? 1 : 0);
        bounds.Maximum = float4(valid ? DecodeOrderedFloat(ReceiverMaximum) : 0, 0);
        bounds.Position = 0;
        bounds.Surface = 0;
        if (valid)
        {
            uint2 pixel = group * CAMERA_TILE_SIZE + uint2(NearestReceiver & 15u, (NearestReceiver >> 4) & 15u);
            float4 screen = ReceiverScreenPosition(pixel);
            float4 view = PROJECT_POINT(screen, CameraInverseProjection);
            bounds.Position = PROJECT_POINT(float4(view.xyz / view.w, 1), CameraInverseView);
            float3 dx = WorldShadowTangent(screen, view, int2(pixel), int2(1, 0));
            float3 dy = WorldShadowTangent(screen, view, int2(pixel), int2(0, 1));
            float3 fallback = DecodeNormal(SceneNormals.Load(int3(pixel, 0)).xyz);
            float3 normal = ReceiverNormal(dx, dy, fallback);
            normal = abs(dot(normal, fallback)) >= 0.5 ? normal : fallback;
            uint level = WorldShadowLevel(dx, dy, DominantAxis(normal));
            bounds.Surface = float4(normal, WorldPageOriginAndSpan.w / (WorldPageResolution >> level));

            // Bound the snap for every receiver in the tile, including those
            // with a coarser LOD than the representative used for atlas sizing.
            float2 pixelStep = 2.0 / float2(DepthSize);
            float2 viewStep = float2(
                PROJECT_POINT(float4(pixelStep.x, 0, 0, 0), CameraInverseProjection).x,
                PROJECT_POINT(float4(0, pixelStep.y, 0, 0), CameraInverseProjection).y);
            float inverseW = rcp(view.w);
            if (PROJECT_POINT(float4(0, 0, 1, 0), CameraInverseProjection).w != 0)
            {
                float3 center = (bounds.Minimum.xyz + bounds.Maximum.xyz) * 0.5;
                float3 extent = (bounds.Maximum.xyz - bounds.Minimum.xyz) * 0.5;
                float forwardDepth = view.z * inverseW + dot(center - bounds.Position.xyz, CameraForward);
                inverseW = max(inverseW, forwardDepth + dot(extent, abs(CameraForward)));
            }
            // WorldShadowTangent permits up to 8x the flat step. The
            // minor axis is bounded by the shorter step, and the major axis by both.
            float span = WorldPageOriginAndSpan.w;
            float maxFootprint = max(8.0 * min(abs(viewStep.x), abs(viewStep.y)), 2.0 * length(viewStep)) * inverseW;
            bounds.Maximum.w = clamp(maxFootprint, span / WorldPageResolution, span);
        }
        CameraTileOutput[group.y * ((DepthSize.x + CAMERA_TILE_SIZE - 1u) / CAMERA_TILE_SIZE) + group.x] = bounds;
    }
}
#endif

#ifdef _CS_MarkShadowReceiverDepth
StructuredBuffer<CameraReceiverTile> CameraTiles : register(t8);
RWBuffer<uint> ShadowReceiverDepth : register(u0);

uint RequestedBlockerResolution(float4 clip, float4x4 projection, float4 surface)
{
    // Project one texel of the selected world LOD along both receiver-plane axes.
    uint axis = DominantAxis(surface.xyz);
    float3 normal = ToWorldShadowPlane(surface.xyz, axis);
    float3 tangentX = FromWorldShadowPlane(float3(1, 0, -normal.x / normal.z), axis);
    float3 tangentY = FromWorldShadowPlane(float3(0, 1, -normal.y / normal.z), axis);
    float4 dx = PROJECT_POINT(float4(tangentX, 0), projection);
    float4 dy = PROJECT_POINT(float4(tangentY, 0), projection);
    float2 ndc = clip.xy / clip.w;
    float scale = 0.5 * surface.w / clip.w;
    float2 u = (dx.xy - ndc * dx.w) * scale;
    float2 v = (dy.xy - ndc * dy.w) * scale;
    float a = dot(u, u);
    float b = dot(u, v);
    float c = dot(v, v);
    float major = sqrt(max(1e-20, 0.5 * (a + c + length(float2(a - c, 2 * b)))));
    float determinant = abs(u.x * v.y - u.y * v.x);
    float uvPerTexel = max(determinant / major, major * 0.25);
    // Keep the full projection demand until the CPU knows the occupied rectangle.
    return uint(ceil(rcp(max(rcp(float(BLOCKER_DEMAND_LIMIT)), uvPerTexel))));
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_MarkShadowReceiverDepth(uint index : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    uint2 count = (DepthSize + CAMERA_TILE_SIZE - 1u) / CAMERA_TILE_SIZE;
    if (index >= count.x * count.y || (ShadowFaceCount & (1u << group.z)) == 0)
        return;
    CameraReceiverTile bounds = CameraTiles[index];
    if (bounds.Minimum.w == 0)
        return;

    // A snapped world texel can lie just outside the camera pixel bounds.
    float snapMargin = max(2.0 * bounds.Surface.w, bounds.Maximum.w);
    float3 receiverMin = bounds.Minimum.xyz - snapMargin;
    float3 receiverMax = bounds.Maximum.xyz + snapMargin;
    if (LightKind != 1)
    {
        float3 delta = clamp(LightPositionAndRange.xyz, receiverMin, receiverMax) - LightPositionAndRange.xyz;
        if (dot(delta, delta) > LightPositionAndRange.w * LightPositionAndRange.w)
            return;
    }

    float4x4 projection = LightViewProjections[group.z];
    float2 minUV = 1.0;
    float2 maxUV = 0.0;
    float furthestDepth = 0.0;
    uint outside = 63u;
    bool crossesEye = false;
    float minCameraDepth = 3.402823466e+38;
    float maxCameraDepth = -3.402823466e+38;
    float3 cameraPosition = PROJECT_POINT(float4(0, 0, 0, 1), CameraInverseView).xyz;
    [unroll]
    for (uint corner = 0; corner < 8; corner++)
    {
        float3 position = float3((corner & 1u) ? receiverMax.x : receiverMin.x,
                                 (corner & 2u) ? receiverMax.y : receiverMin.y,
                                 (corner & 4u) ? receiverMax.z : receiverMin.z);
        if (LightKind == 1)
        {
            float cameraDepth = dot(position - cameraPosition, CameraForward);
            minCameraDepth = min(minCameraDepth, cameraDepth);
            maxCameraDepth = max(maxCameraDepth, cameraDepth);
        }

        float4 clip = PROJECT_POINT(float4(position, 1), projection);
        float epsilon = 1e-5 * max(1.0, abs(clip.w));
        outside &= (clip.x < -clip.w - epsilon ? 1u : 0u) |
                   (clip.x >  clip.w + epsilon ? 2u : 0u) |
                   (clip.y < -clip.w - epsilon ? 4u : 0u) |
                   (clip.y >  clip.w + epsilon ? 8u : 0u) |
                   (clip.z < -epsilon ? 16u : 0u) |
                   (clip.z > clip.w + epsilon ? 32u : 0u);
        crossesEye = crossesEye || clip.w <= epsilon;
        float3 ndc = clip.xyz / max(clip.w, epsilon);
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        minUV = min(minUV, uv);
        maxUV = max(maxUV, uv);
        furthestDepth = max(furthestDepth, DEPTH_01(ndc.z));
    }
    if (outside != 0)
        return;
    if (LightKind == 1)
    {
        const float start = DirectionalSlices[group.z].x;
        const float end = DirectionalSlices[group.z].y;
        const float guard = max(0.1, end * 1e-5);
        if (maxCameraDepth < start - guard || minCameraDepth > end + guard)
            return;
    }

    // The bounds can overlap pages with no corresponding pixel in
    // the camera tile. Keep that overlap for caster culling and measure LOD
    // from real receivers whose lookup actually uses this page.
    float3 samplePosition = bounds.Position.xyz;
    float4 sampleClip = PROJECT_POINT(bounds.Position, projection);
    float3 sampleNdc = sampleClip.xyz / max(1e-5, sampleClip.w);
    bool inside = sampleClip.w > 1e-5 && all(abs(sampleNdc.xy) <= 1.0) &&
        sampleNdc.z >= 0.0 && sampleNdc.z <= 1.0;
    if (LightKind == 1)
    {
        float depth = dot(samplePosition - cameraPosition, CameraForward);
        inside = inside && depth >= DirectionalSlices[group.z].x && depth <= DirectionalSlices[group.z].y;
    }
    else
    {
        float3 fromLight = samplePosition - LightPositionAndRange.xyz;
        float distanceSquared = dot(fromLight, fromLight);
        inside = inside && distanceSquared < LightPositionAndRange.w * LightPositionAndRange.w;
        if (LightKind == 0)
        {
            uint axis = DominantAxis(fromLight);
            inside = inside && group.z == 2u * axis + (fromLight[axis] < 0 ? 1u : 0u);
        }
        else
        {
            inside = inside && dot(fromLight, LightDirectionAndOuterCos.xyz) >=
                sqrt(distanceSquared) * LightDirectionAndOuterCos.w;
        }
    }
    float4 demandClip = sampleClip;
    float4 demandSurface = bounds.Surface;
    if (!inside)
    {
        // The tile can overlap a neighboring face with no representative on it.
        float3 center = (receiverMin + receiverMax) * 0.5;
        demandClip = PROJECT_POINT(float4(center, 1), projection);
        float depth = dot(samplePosition - cameraPosition, CameraForward);
        float centerDepth = dot(center - cameraPosition, CameraForward);
        if (abs(PROJECT_POINT(float4(0, 0, 1, 0), CameraInverseProjection).w) > 0)
            demandSurface.w *= max(1.0, centerDepth / max(depth, 1e-5));
    }
    uint resolution = demandClip.w > 1e-5 ? RequestedBlockerResolution(demandClip, projection, demandSurface) : 0;
    float2 guard = 2.0 / MIN_SHADOW_PAGE_RESOLUTION;
    uint2 first = min(uint2(saturate(minUV - guard) * SHADOW_TILE_SIZE), SHADOW_TILE_SIZE - 1u);
    uint2 last = min(uint2(saturate(maxUV + guard) * SHADOW_TILE_SIZE), SHADOW_TILE_SIZE - 1u);
    if (crossesEye)
    {
        first = 0;
        last = SHADOW_TILE_SIZE - 1u;
        furthestDepth = 1.0;
    }
    // Positive float bit patterns sort in depth order. Zero is reserved for an empty tile.
    uint encoded = asuint(1.0 + saturate(furthestDepth));
    uint faceOffset = ShadowMaskOffset + group.z * SHADOW_FACE_STRIDE;
    // Keep a linear Z bound from the padded camera tile. Clip-space depth
    // loses precision near the far end of large perspective shadow faces.
    float3 center = (receiverMin + receiverMax) * 0.5;
    float3 extent = (receiverMax - receiverMin) * 0.5;
    float4x4 lightView = LightViews[group.z];
    float3 axis = float3(lightView[0][2], lightView[1][2], lightView[2][2]);
    float farDepth = PROJECT_POINT(float4(center, 1), lightView).z + dot(extent, abs(axis));
    if (isfinite(farDepth))
        InterlockedMax(ShadowReceiverDepth[faceOffset + SHADOW_RECEIVER_FAR], asuint(max(0.0, farDepth)));
    else
        InterlockedMax(ShadowReceiverDepth[faceOffset + SHADOW_RECEIVER_FAR], 0x7f800000u);
    uint demandOffset = faceOffset + SHADOW_TILE_COUNT;
    if (inside)
    {
        // Round up to the bin covering this full-frustum demand.
        uint tier = resolution > 1u ? uint(1 + firstbithigh(resolution - 1u)) : 0u;
        InterlockedAdd(ShadowReceiverDepth[demandOffset + 1u + min(tier, BLOCKER_DEMAND_MAX_LOG2)], 1u);
    }
    else
    {
        InterlockedMax(ShadowReceiverDepth[demandOffset], max(1u, resolution));
    }
    for (uint y = first.y; y <= last.y; y++)
        for (uint x = first.x; x <= last.x; x++)
            InterlockedMax(ShadowReceiverDepth[faceOffset + y * SHADOW_TILE_SIZE + x], encoded);
}
#endif
