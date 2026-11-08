// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/Math/Math.hlsl"

META_CB_BEGIN(0, OcclusionCullingData)
float4x4 ViewProjectionMatrix;
float2 RTSize;
float MaxMipLevel;
uint CullCount;
META_CB_END

// Vertex Shader function for Hardware Occlusion Culling queries bounds projection
META_VS(true, FEATURE_LEVEL_ES2)
float4 VS_HardwareOcclusionCulling(float3 Position : POSITION0) : SV_Position
{
    return mul(float4(Position, 1), ViewProjectionMatrix);
}

#ifdef _CS_HZBCull

RWBuffer<uint> HZBResults : register(u0);
Buffer<float3> BoundsBuffer : register(t0);
Texture2D<float> HiZ : register(t1);

// Compute Shader for HZB culling
// [Reference: https://interplayoflight.wordpress.com/2017/11/15/experiments-in-gpu-based-occlusion-culling/]
// [Reference: https://blog.selfshadow.com/publications/practical-visibility/]
META_CS(true, AUTO)
[numthreads(64, 1, 1)]
void CS_HZBCull(uint DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId >= CullCount)
        return;

    // Load object bounds
    float3 bondsMin = BoundsBuffer[DispatchThreadId * 2];
    float3 bondsMax = BoundsBuffer[DispatchThreadId * 2 + 1];
    float3 bondsSize = bondsMax - bondsMin;

    // Project bounds onto the screen
    float3 boundsCorners[] = {
        bondsMin.xyz,
        bondsMin.xyz + float3(bondsSize.x,0,0),
        bondsMin.xyz + float3(0, bondsSize.y,0),
        bondsMin.xyz + float3(0, 0, bondsSize.z),
        bondsMin.xyz + float3(bondsSize.xy,0),
        bondsMin.xyz + float3(0, bondsSize.yz),
        bondsMin.xyz + float3(bondsSize.x, 0, bondsSize.z),
        bondsMax.xyz
    };
    float closestZ = DEPTH_RANGE_MAX;
    float2 minUV = 1, maxUV = 0;
    UNROLL
    for (uint i = 0; i < 8; i++)
    {
        // Transform world-space bounds to NDC
        float4 clipPos = PROJECT_POINT(float4(boundsCorners[i], 1), ViewProjectionMatrix);
        clipPos.xyz = clipPos.xyz / clipPos.w;

        // Get min/max UVs
        clipPos.xy = clipPos.xy * float2(0.5, -0.5) + float2(0.5, 0.5);
        clipPos.xy = saturate(clipPos.xy);
        minUV = min(clipPos.xy, minUV);
        maxUV = max(clipPos.xy, maxUV);

        // Get the closest depth
#if REVERSE_Z
        if (clipPos.z < 0)
            clipPos.z = 1; // Point is behind the camera
        closestZ = saturate(max(closestZ, clipPos.z));
#else
        closestZ = saturate(min(closestZ, clipPos.z));
#endif
    }

    // Calculate Hi-Z buffer mip (assumes HZB is power of two)
#if VULKAN || defined(WGSL) // For some reason, Vulkan needs different mip selection math
    float2 pixelSize = RTSize * (maxUV - minUV) * 2.0f;
    float mip = floor(log2(max(max(pixelSize.x, pixelSize.y), 1.0f)));
#else
    int2 size = (maxUV - minUV) * RTSize;
    float mip = ceil(log2(max(max(size.x, size.y), 1)));
#endif
    mip = clamp(mip, 0, MaxMipLevel);
    float4 boundsUVs = float4(minUV, maxUV);

#if 0 // TODO: figure out why it causes minor artifacts (eg. in Bistro)
    // Texel footprint for the lower (finer-grained) level
    float mipUp = max(mip - 1, 0);
    float2 scale = exp2(-mipUp);
    float2 a = floor(boundsUVs.xy * scale);
    float2 b = ceil(boundsUVs.zw * scale);
    float2 dims = b - a;

    // Use the lower level if we only touch <= 2 texels in both dimensions
    if (dims.x <= 2 && dims.y <= 2)
        mip = mipUp;
#endif

    // Load depths from Hi-Z buffer
    float4 depths = {
        SAMPLE_RT_DEPTH_LEVEL(HiZ, boundsUVs.xy, mip),
        SAMPLE_RT_DEPTH_LEVEL(HiZ, boundsUVs.zy, mip),
        SAMPLE_RT_DEPTH_LEVEL(HiZ, boundsUVs.xw, mip),
        SAMPLE_RT_DEPTH_LEVEL(HiZ, boundsUVs.zw, mip)
    };

    // Find the furthest depth and test it
#if REVERSE_Z
    float furthestDepth = Min4(depths);
    bool visible = closestZ >= furthestDepth;
#else
    float furthestDepth = Max4(depths);
    bool visible = closestZ <= furthestDepth;
#endif

    // Write culling result
    HZBResults[DispatchThreadId] = visible ? 1u : 0u;
}

#endif

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

#if defined(_CS_V2ProjectCameraReceivers) || defined(_PS_V2ReceiverDepthCross) || defined(_CS_ShadowCasterSelection) || defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask)
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
    if (abs(normalZ) <= 0.02 * length(z) * length(normal))
        return 0;
    float2 plane = float2(-2.0 * dot(normal, x) / dot(x, x), 2.0 * dot(normal, y) / dot(y, y));
    return plane * (dot(z, z) / normalZ) / size;
}

// Fit the CAMERA device-depth slope along one screen axis. Return (slope, valid).
// All sampled depths and the fit tolerance below are raw device depth, not centimetres.
// This assumes samples chosen for a fit belong to the same planar surface.
float2 V2DepthAxisGradient(Texture2D<float> depthTexture, float depth, int2 pixel, int2 axis, int2 size)
{
    // Fit device depth, which is affine over a rasterized triangle. A wider
    // stencil reduces D24 quantization, while extrapolation rejects silhouettes.
    // Edge selection follows https://atyuwen.github.io/posts/normal-reconstruction/
    // Sample at -8, -4, +4 and +8 pixels. A planar triangle's device depth is affine:
    // depth(x +/- 8) - depth(x) should be twice depth(x +/- 4) - depth(x).
    const int radius = 4;
    int2 l1 = pixel - axis * radius;
    int2 l2 = pixel - axis * (2 * radius);
    int2 r1 = pixel + axis * radius;
    int2 r2 = pixel + axis * (2 * radius);
    float a = depthTexture.Load(int3(clamp(l1, 0, size - 1), 0));
    float b = depthTexture.Load(int3(clamp(l2, 0, size - 1), 0));
    float c = depthTexture.Load(int3(clamp(r1, 0, size - 1), 0));
    float d = depthTexture.Load(int3(clamp(r2, 0, size - 1), 0));
    // Allow four D24 quantization steps in that extrapolation check.
    // left/right indicate an acceptable fit on that side, not camera visibility.
    const float tolerance = 4.0 / 16777215.0;
    bool left = all(l2 >= 0) && a != DEPTH_RANGE_MAX && b != DEPTH_RANGE_MAX &&
        abs((a - depth) * 2.0 - (b - depth)) <= tolerance;
    bool right = all(r2 < size) && c != DEPTH_RANGE_MAX && d != DEPTH_RANGE_MAX &&
        abs((c - depth) * 2.0 - (d - depth)) <= tolerance;
    // Weighted least-squares slopes. Dividing by the sample spacing converts the
    // differences back to device-depth change per ONE camera pixel.
    // When both sides pass, they are combined; passing does not prove both sides
    // are the same physical surface at a wall/floor boundary.
    if (left && right)
        return float2(((c - a) + 2.0 * (d - b)) / (10.0 * radius), 1);
    if (left)
        return float2(((depth - a) + 2.0 * (depth - b)) / (5.0 * radius), 1);
    if (right)
        return float2(((c - depth) + 2.0 * (d - depth)) / (5.0 * radius), 1);
    return 0;
}

// Reconstruct a geometric world-space normal from CAMERA depth.
// receiver is already unprojected into the same world/origin space as the matrices.
// shadingNormal is the decoded, normalized GBuffer normal used for orientation/fallback.
// Return XYZ = chosen normal; W = 1 if the depth-plane checks pass, otherwise 0.
// W is a binary confidence flag, not a probability or an occlusion result.
float4 V2ReceiverPlaneNormalChecked(Texture2D<float> depthTexture, float3 receiver, float depth, int2 pixel, float2 size,
    float4x4 inverseViewProjection, float3 shadingNormal)
{
    float2 xGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(1, 0), int2(size));
    float2 yGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(0, 1), int2(size));
    if (xGradient.y == 0 || yGradient.y == 0)
        return float4(shadingNormal, 0);
    float2 gradient = float2(xGradient.x, yGradient.x);

    // Differentiate homogeneous unprojection without subtracting large world positions.
    // A camera pixel moves NDC by (2/width, -2/height). Combine that screen
    // displacement with the fitted depth change, then differentiate P = q.xyz/q.w.
    // dx.xyz - receiver*dx.w and dy.xyz - receiver*dy.w are proportional to
    // world-space surface tangents; their cross product gives the plane normal.
    float4 dx = PROJECT_POINT(float4(2.0 / size.x, 0, gradient.x, 0), inverseViewProjection);
    float4 dy = PROJECT_POINT(float4(0, -2.0 / size.y, gradient.y, 0), inverseViewProjection);
    float3 planeNormal = cross(dx.xyz - receiver * dx.w, dy.xyz - receiver * dy.w);
    float lengthSquared = dot(planeNormal, planeNormal);
    float agreement = dot(planeNormal, shadingNormal);
    // At a silhouette the quad may straddle two surfaces.
    // With a unit shading normal, agreement^2 / lengthSquared is cos(angle)^2.
    // Require |cos(angle)| >= 0.6; then flip the plane normal to the GBuffer side.
    // A mixed-surface fit can still pass this threshold. That is what the
    // Surface Confidence and Surface Normal debug views expose.
    if (lengthSquared <= 0 || agreement * agreement < 0.36 * lengthSquared)
        return float4(shadingNormal, 0);
    return float4(planeNormal * ((agreement < 0 ? -1.0 : 1.0) * rsqrt(lengthSquared)), 1);
}

float3 V2ReceiverPlaneNormal(Texture2D<float> depthTexture, float3 receiver, float depth, int2 pixel, float2 size,
    float4x4 inverseViewProjection, float3 shadingNormal)
{
    return V2ReceiverPlaneNormalChecked(depthTexture, receiver, depth, pixel, size, inverseViewProjection, shadingNormal).xyz;
}

float3 V2OffsetShadowReceiver(float3 receiver, float3 normal, float3 directionToLight, float normalBias)
{
    float cosine = saturate(dot(normal, directionToLight));
    float offset = normalBias * sqrt(saturate(1.0 - cosine * cosine));
    return receiver + normal * offset;
}

// Estimate WORLD-SPACE position error caused by two CAMERA D24 depth steps.
// homogeneousW is q.w BEFORE the perspective divide, where q = clip*inverseVP.
// dP/dDepth = (inverseVP_depth.xyz - P*inverseVP_depth.w) / q.w.
// Return a world-space error vector (engine units/cm), not a scalar depth bias.
float3 V2CameraDepthUncertainty(float3 receiver, float homogeneousW, float4x4 inverseViewProjection)
{
    // The opaque GBuffer depth is D24. Allow two steps for rasterization and storage error.
    const float cameraDepthError = 2.0 / 16777215.0;
    float4 depthDerivative = PROJECT_POINT(float4(0, 0, 1, 0), inverseViewProjection);
    return (depthDerivative.xyz - receiver * depthDerivative.w) * (cameraDepthError / homogeneousW);
}

// Convert camera position uncertainty to a tolerance in LIGHT device depth.
// depthPerTexel is the light-space receiver-plane slope; size is light-map texels.
// precisionBias, cameraBias and the returned sum are normalized light depth.
// Debug converts the result to cm using SelectionReceiverRange.z.
float V2ReceiverDepthBias(float precisionBias, float2 depthPerTexel, float2 size,
    float4x4 lightProjection, float3 receiverUncertainty)
{
    // Carry camera-depth error through both light depth and the receiver-plane UV correction.
    float4 lightError = PROJECT_POINT(float4(receiverUncertainty, 0), lightProjection);
    float2 texelError = lightError.xy * size * float2(0.5, -0.5);
    // The same position error changes both the light depth and the light UV.
    // Remove the depth change explained by motion along the fitted receiver plane.
    float cameraBias = abs(lightError.z - dot(depthPerTexel, texelError));

    // Add a small slope allowance (1/8 of the summed per-texel slopes), capped
    // before scaling so a grazing plane cannot produce an unlimited allowance.
    float slope = dot(abs(depthPerTexel), float2(1, 1));
    return precisionBias + cameraBias + 0.125 * min(slope, rcp(min(size.x, size.y)));
}
#endif

#if defined(_CS_ShadowCasterSelection) || defined(_VS_ShadowCasterBoundsDebug) || defined(_PS_V2ShadowResourceDebug) || defined(_PS_V2LightPOVShaded) || defined(_CS_V2LightCasterVisibility) || defined(_VS_V2ShadowProxyDepth) || defined(_PS_V2ReceiverDepthCross) || defined(_PS_V2ReceiverTraceProbe) || defined(_CS_V2ProjectCameraReceivers) || defined(_CS_V2ReceiverMaskCasters) || defined(_CS_V2ReceiverTileMax)

META_CB_BEGIN(1, ShadowCasterSelectionData)
float4x4 InverseViewProjection;
float4x4 ShadowViewProjection;
float4x4 LightViewProjection;
float3 DirectionToLight;
float ProjectionDistance;
uint Width;
uint Height;
uint CasterCount;
uint GroupsPerRow;
uint WriteDebugMask;
uint HasLightDepth;
uint HasReceiverNormals;
float ShadowSelectionFilterRadius;
float4x4 SelectionNearLightViewProjection;
float4 SelectionShadowTiles[2];
float4 SelectionShadowSplit;
float4 SelectionCameraPosition;
float4 SelectionCameraForward;
float4 SelectionReceiverRange; // Retained for shared shadow resolve data. Candidate selection is view-wide.
uint CasterDebugFlags; // Bit 0: selected expensive casters. Bit 1: rejected expensive casters.
float SelectionReceiverBias;
uint ReceiverTraceStep;
uint ReceiverTraceCaster;
META_CB_END

struct ShadowCasterBounds
{
    float3 Minimum;
    uint UseProxy;
    float3 Maximum;
    uint DebugCategory;
    uint CameraIndex;
    uint3 Padding;
};

#endif

// Camera-visible receiver coverage, expressed in the SHARED LIGHT projection.
// This pass visits the camera depth once, independently of casters and cascades.
// Light-hidden receivers MUST remain present: those are possible shadow receivers.
#ifdef _CS_V2ProjectCameraReceivers
Texture2D<float> CameraReceiverDepth : register(t2);
Texture2D<float4> CameraReceiverNormals : register(t8);
RWTexture2D<uint> CameraReceiverCoverageOut : register(u0);
RWTexture2D<uint> CameraReceiverNearOut : register(u1);
RWTexture2D<uint> CameraReceiverFarOut : register(u2);
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 1)]
void CS_V2ProjectCameraReceivers(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;
    if (pixel.x >= Width || pixel.y >= Height) return;
    float depth = CameraReceiverDepth.Load(int3(pixel, 0));
    if (depth == DEPTH_RANGE_MAX) return;
    float2 ndc = (float2(pixel) + 0.5) / float2(Width, Height) * float2(2,-2) + float2(-1,1);
    float4 worldH = PROJECT_POINT(float4(ndc, depth, 1), InverseViewProjection);
    if (abs(worldH.w) < 1e-12) return;
    float3 world = worldH.xyz / worldH.w;
    float3 light = ProjectV2Shadow(world, LightViewProjection);
    if (!IsV2ShadowCoordValid(light)) return;

    // Coverage bit 1 means camera-visible. Bit 2 additionally means potentially
    // front-facing to this light. A back face remains visible in the coverage
    // debug view, but cannot receive this light's direct illumination or shadow.
    bool eligible = true;
    bool uncertainPlane = !HasReceiverNormals;
    float3 receiverNormal = 0;
    if (HasReceiverNormals)
    {
        float3 normal = CameraReceiverNormals.Load(int3(pixel,0)).xyz * 2 - 1;
        bool validNormal = dot(normal,normal) >= 0.01;
        receiverNormal = validNormal ? normalize(normal) : 0;
        eligible = !validNormal || dot(receiverNormal,DirectionToLight) >= -0.0001;
        uncertainPlane = !validNormal || abs(dot(receiverNormal,DirectionToLight)) < 0.02;
    }
    uint bits = eligible ? (uncertainPlane ? 7u : 3u) : 1u;
    // Canonical light depth is ALWAYS 0 near, 1 far, including reversed Z.
    // Positive finite float bit patterns preserve order for integer atomics.
    int2 size = int2(SelectionShadowTiles[0].zw);
    // A camera point is splatted over neighboring LIGHT texels. Their depth can
    // differ on a sloped plane. Store a conservative interval spanning that
    // footprint, rather than treating the point's depth as exact at every texel.
    float2 gradient = HasReceiverNormals ? V2ShadowDepthPerTexel(receiverNormal,LightViewProjection,size) : 0;
    float footprint = dot(abs(gradient),float2(1,1)) + SelectionShadowSplit.w * 4;
    float canonicalDepth = DEPTH_01(light.z);
    uint nearBits = asuint(saturate(canonicalDepth-footprint));
    uint farBits = asuint(saturate(canonicalDepth+footprint));
    int2 first = int2(floor(light.xy * size - 0.5));
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        int2 texel = first + int2(tap & 1, tap >> 1);
        if (any(texel < 0) || any(texel >= size)) continue;
        InterlockedOr(CameraReceiverCoverageOut[texel], bits);
        if (eligible)
        {
            // Retain BOTH extremes when different camera surfaces land in the
            // same light texel. Choosing only the nearest would lose receivers
            // behind other surfaces. The interval may conservatively overselect.
            InterlockedMin(CameraReceiverNearOut[texel], nearBits);
            InterlockedMax(CameraReceiverFarOut[texel], farBits);
        }
    }
}
#endif

#if defined(_CS_V2ReceiverMaskCasters) || defined(_CS_V2ReceiverTileMax) || defined(_PS_V2ShadowResourceDebug)
Texture2D<uint> CameraReceiverCoverage : register(t24);
Texture2D<uint> CameraReceiverNear : register(t25);
Texture2D<uint> CameraReceiverFar : register(t26);
#endif

#ifdef _CS_V2ReceiverTileMax
Texture2D<float> ReceiverTileLightDepth : register(t7);
RWTexture2D<uint> ReceiverTileMaxOutput : register(u0);
groupshared uint TileMaximumReceiver;
// One tile summary is shared by every caster. Zero means no eligible receiver
// needing an expensive shadow; otherwise it stores the farthest canonical depth.
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8,8,1)]
void CS_V2ReceiverTileMax(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint thread : SV_GroupIndex)
{
    if (thread == 0) TileMaximumReceiver = 0;
    GroupMemoryBarrierWithGroupSync();
    uint2 pixel = group.xy * 8 + local.xy;
    uint2 size = uint2(SelectionShadowTiles[0].zw);
    if (all(pixel < size))
    {
        uint coverage = CameraReceiverCoverage.Load(int3(pixel,0));
        if ((coverage & 2u) != 0)
        {
            float nearDepth = asfloat(CameraReceiverNear.Load(int3(pixel,0)));
            float cheap = ReceiverTileLightDepth.Load(int3(pixel,0));
            bool alreadyShadowed = (coverage & 4u) == 0 && cheap != DEPTH_RANGE_MAX &&
                nearDepth > DEPTH_01(cheap) + SelectionShadowSplit.w * 4;
            if (!alreadyShadowed)
                InterlockedMax(TileMaximumReceiver,CameraReceiverFar.Load(int3(pixel,0)));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) ReceiverTileMaxOutput[group.xy] = TileMaximumReceiver;
}
#endif

#ifdef _CS_V2ReceiverMaskCasters
Texture2D<float> ReceiverMaskLightDepth : register(t7);
StructuredBuffer<ShadowCasterBounds> ReceiverMaskBoxes : register(t3);
StructuredBuffer<uint> ReceiverMaskLightVisibility : register(t21);
Texture2D<uint> ReceiverTileMaximum : register(t28);
RWStructuredBuffer<uint> ReceiverMaskCasterResults : register(u0);
groupshared uint ReceiverMaskHit;
groupshared float3 ReceiverRayOrigin;
groupshared float3 ReceiverRayStepX;
groupshared float3 ReceiverRayStepY;
groupshared float3 ReceiverRayDelta;
groupshared float3 ReceiverRayInverse;
groupshared float3 ReceiverRayPadding;
groupshared float ReceiverBoxNearest;

// One group per caster, scanning only its light-space rectangle at fixed map size.
// This produces the final conservative caster flags. The old camera-ray search
// remains available for explicit diagnostics, not as a second normal scan.
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64,1,1)]
void CS_V2ReceiverMaskCasters(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint index = group.y * GroupsPerRow + group.x;
    if (index >= CasterCount) return;
    ShadowCasterBounds box = ReceiverMaskBoxes[index];
    if (ReceiverMaskLightVisibility[index] == 0)
    {
        if (thread == 0) ReceiverMaskCasterResults[index] = 0;
        return;
    }
    // Cheap real geometry already has a low-cost depth draw. Do not scan a
    // potentially huge light-map rectangle just to avoid a few triangles.
    // The earlier light-visibility pass can still reject a fully blocked box.
    if (!box.UseProxy)
    {
        if (thread == 0) ReceiverMaskCasterResults[index] = 3;
        return;
    }
    float2 lo = 1, hi = 0;
    bool outside = false;
    float nearest = 1;
    UNROLL
    for (uint c=0;c<8;c++)
    {
        float3 p = float3((c&1)?box.Maximum.x:box.Minimum.x,
            (c&2)?box.Maximum.y:box.Minimum.y,(c&4)?box.Maximum.z:box.Minimum.z);
        float3 light = ProjectV2Shadow(p,LightViewProjection);
        outside = outside || !IsV2ShadowCoordValid(light);
        nearest = min(nearest,DEPTH_01(light.z));
        lo=min(lo,light.xy); hi=max(hi,light.xy);
    }
    // The map cannot prove absence outside its projection. Keep such casters.
    if (outside)
    {
        if (thread == 0) ReceiverMaskCasterResults[index] = 3;
        return;
    }
    uint2 size=uint2(SelectionShadowTiles[0].zw);
    int2 first=max(int2(floor(lo*size))-2,0);
    int2 last=min(int2(ceil(hi*size))+2,int2(size));
    uint2 extent=uint2(max(last-first,0));
    if (thread == 0)
    {
        ReceiverMaskHit = 0;
        // Directional light projection is orthographic: every texel ray has the
        // same direction, and its origin changes affinely across the map.
        // Reconstruct this basis ONCE per caster, rather than four matrices and
        // three divisions for every occupied texel in the rectangle.
        float4 originH = PROJECT_POINT(float4(-1,1,DEPTH_RANGE_MIN,1),SelectionNearLightViewProjection);
        float4 deltaH = PROJECT_POINT(float4(0,0,DEPTH_RANGE_MAX-DEPTH_RANGE_MIN,0),SelectionNearLightViewProjection);
        float4 xH = PROJECT_POINT(float4(2.0/size.x,0,0,0),SelectionNearLightViewProjection);
        float4 yH = PROJECT_POINT(float4(0,-2.0/size.y,0,0),SelectionNearLightViewProjection);
        ReceiverRayOrigin = originH.xyz / originH.w;
        ReceiverRayStepX = xH.xyz / originH.w;
        ReceiverRayStepY = yH.xyz / originH.w;
        ReceiverRayDelta = deltaH.xyz / originH.w;
        ReceiverRayInverse = rcp(ReceiverRayDelta);
        ReceiverRayPadding = abs(ReceiverRayStepX) + abs(ReceiverRayStepY);
        float3 lightDepthAxis = float3(LightViewProjection[0][2],LightViewProjection[1][2],LightViewProjection[2][2]);
        ReceiverBoxNearest = max(0,nearest-dot(abs(lightDepthAxis),ReceiverRayPadding));
    }
    GroupMemoryBarrierWithGroupSync();
    float3 paddedMinimum = box.Minimum - ReceiverRayPadding;
    float3 paddedMaximum = box.Maximum + ReceiverRayPadding;
    bool tiled = (CasterDebugFlags & 262144u) != 0;
    int2 firstTile = first >> 3;
    uint2 tileExtent = uint2(((last + 7) >> 3) - firstTile);
    uint scanCount = tiled ? tileExtent.x * tileExtent.y : extent.x * extent.y;
    for(uint n=tiled?0:thread;n<scanCount;n+=tiled?1:64)
    {
        // A stale zero only causes extra work; seeing a hit lets other lanes stop.
        // The final group barrier still precedes publication of the atomic flag.
        if (ReceiverMaskHit != 0) break;
        int2 pixel;
        if (tiled)
        {
            int2 tile = firstTile + int2(n % tileExtent.x,n / tileExtent.x);
            uint farthestBits = ReceiverTileMaximum.Load(int3(tile,0));
            // Max reduction proves ALL receivers in the tile are in front of the
            // padded box. No centre/corner heuristic or averaged depth is used.
            if (farthestBits == 0 || asfloat(farthestBits) <= ReceiverBoxNearest + SelectionShadowSplit.w) continue;
            pixel = tile * 8 + int2(thread & 7u,thread >> 3);
            if (any(pixel < first) || any(pixel >= last)) continue;
        }
        else pixel=first+int2(n%extent.x,n/extent.x);
        uint coverage=CameraReceiverCoverage.Load(int3(pixel,0));
        if ((coverage&2u)==0) continue;
        float receiverNear=asfloat(CameraReceiverNear.Load(int3(pixel,0)));
        float receiverFar=asfloat(CameraReceiverFar.Load(int3(pixel,0)));
        float3 start = ReceiverRayOrigin + ReceiverRayStepX * (pixel.x + 0.5) + ReceiverRayStepY * (pixel.y + 0.5);
        float3 delta = ReceiverRayDelta;
        float enter=0,leave=1;
        bool hit=true;
        UNROLL
        for(uint axis=0;axis<3;axis++)
        {
            if(abs(delta[axis])<1e-8)
                hit=hit && start[axis]>=paddedMinimum[axis] && start[axis]<=paddedMaximum[axis];
            else
            {
                float a=(paddedMinimum[axis]-start[axis])*ReceiverRayInverse[axis];
                float b=(paddedMaximum[axis]-start[axis])*ReceiverRayInverse[axis];
                enter=max(enter,min(a,b)); leave=min(leave,max(a,b));
            }
        }
        // Directional orthographic projection: ray t equals canonical light depth.
        // A visible receiver must be behind the caster's entry, not in front of it.
        if(!hit || leave<enter || receiverFar<=enter+SelectionShadowSplit.w) continue;
        float cheap=ReceiverMaskLightDepth.Load(int3(pixel,0));
        // A cheap blocker BEFORE the box prevents this caster receiving the light.
        // Do not reject a receiver merely because it is behind cheap depth: that
        // receiver may be the cheap surface itself receiving this caster's shadow.
        if(cheap!=DEPTH_RANGE_MAX)
        {
            float cheapCanonical=DEPTH_01(cheap);
            if(cheapCanonical<enter-SelectionShadowSplit.w*4) continue;
            // Cheap real geometry has its own shadow draw. An expensive caster
            // cannot add a visible hard shadow when ALL camera receivers in this
            // texel are already behind that cheap blocker. Do not apply this
            // rule to the cheap caster itself, or its shadow would disappear.
            // Mixed/uncertain planes stay conservative instead of proving a cull.
            if(box.UseProxy && (coverage&4u)==0 && receiverNear>cheapCanonical+SelectionShadowSplit.w*4) continue;
        }
        InterlockedOr(ReceiverMaskHit,3u);
        break;
    }
    GroupMemoryBarrierWithGroupSync();
    if(thread==0) ReceiverMaskCasterResults[index]=ReceiverMaskHit;
}
#endif


#ifdef _CS_ShadowCasterSelection
// Selection inputs (these are separate projections; their raw depths cannot be subtracted):
//   ReceiverDepth: nearest opaque surface seen by the CAMERA, device depth.
//   LightDepth: cheap real geometry rendered from the LIGHT, device depth.
//   ProxySelectionDepth: expensive geometry's AABB proxies rendered from the LIGHT.
//   LightCasterVisibility: earlier light-POV visibility result, indexed by caster.
// All world positions/bounds use the same render-origin offset as the view matrices.
Texture2D<float> ReceiverDepth : register(t2);
Texture2D<float> LightDepth : register(t7);
Texture2D<float4> ShadowReceiverNormals : register(t8);
StructuredBuffer<uint> LightCasterVisibility : register(t21);
Texture2D<float> ProxySelectionDepth : register(t22);
StructuredBuffer<uint> CameraCasterVisibility : register(t23);
StructuredBuffer<uint> CameraReceiverCasterVisibility : register(t27);
StructuredBuffer<ShadowCasterBounds> ShadowCasterBoxes : register(t3);
RWStructuredBuffer<uint> CasterSelection : register(u1);
RWTexture2D<uint> ReceiverHits : register(u2);
#if 1 // Region: receiver selection diagnostics
// Diagnostic output only. Step IDs match Receiver Debug Step in pipeline settings:
// 0 decision, 1 camera depth, 2 world position, 3 normal, 4 receiver bias,
// 5 light UV/depth, 6 proxy separation, 7 cheap separation, 8 tolerance,
// 9 surface confidence, 10 cheap taps, 11 proxy taps.
// Each selected view stores RAW values. The resource-debug pixel shader below
// converts them to colours; its RGB can differ from the numeric probe's raw RGB.
RWTexture2D<float4> ReceiverTraceOutput : register(u3);
void TraceReceiverValue(bool trace, uint2 pixel, uint step, float4 value)
{
    if (trace && ReceiverTraceStep == step) ReceiverTraceOutput[pixel] = value;
}
// Decision view: R stores the reason. Other views: negative A means the pixel
// exited before producing that view's values. Earlier completed views stay intact
// where the caller explicitly guards this write.
// Reasons: 1 outside sweep (cleared by CPU), 2 no camera surface, 3 light-hidden,
// 4 ray misses AABB, 5 back-facing, 7 proxy miss, 8 cheap-occluded,
// 9 accepted, 10 accepted with uncertain/mixed samples.
void TraceReceiverExit(bool trace, uint2 pixel, uint reason)
{
    if (trace) ReceiverTraceOutput[pixel] = ReceiverTraceStep == 0 ? float4(reason, 0, 0, 0) : float4(0, 0, 0, -float(reason));
}
#endif // Region: receiver selection diagnostics

// One shared flag for the caster's 64-thread group. Any accepted receiver sets it.
groupshared uint ProxyHit;

// This map contains cheap occluders for the entire view/light, never a cascade.
// Reject only when all four neighboring light-depth samples prove the receiver blocked.
// receiver/normal: camera-visible surface in world/origin space, BEFORE selection bias.
// receiverW: its homogeneous camera unprojection W, used for precision estimation.
// Result true means all four LIGHT depth taps prove a cheap occluder is in front.
// Missing/out-of-map taps keep the receiver potential (false): conservative policy.
// Diagnostic outputs: differences=(min gap cm, max gap cm, tolerance cm, valid bits);
// coverage=(valid bits, blocked bits, tolerance cm, 1). Bits 1,2,4,8 form a 2x2 quad.
bool IsReceiverBlockedByCheapOccluders(float3 receiver, float3 normal, uint2 pixel, float cameraDepth, float receiverW, bool trace, out float4 differences, out float4 coverage)
{
    differences = 0;
    coverage = 0;
    if (!HasLightDepth)
        return false;
    // Move the CAMERA surface into LIGHT UV/depth so both depths share a projection.
    float3 coord = ProjectV2Shadow(receiver, LightViewProjection);
    if (!IsV2ShadowCoordValid(coord))
        return false;
    float2 size = SelectionShadowTiles[0].zw;
    // Texel centres are integer coordinates after subtracting 0.5. floor() gives
    // the upper-left member of the four surrounding samples. This is NOT a
    // bilinear filtered depth fetch: all four samples are tested independently.
    float2 receiverTexel = coord.xy * size - 0.5;
    int2 first = int2(floor(receiverTexel));
    if (any(first < 0) || any(first + 1 >= int2(size)))
        return false;
    float2 depthPerTexel = HasReceiverNormals ? V2ShadowDepthPerTexel(normal, LightViewProjection, size) : 0;
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverW, InverseViewProjection);
    float bias = V2ReceiverDepthBias(SelectionShadowSplit.w, depthPerTexel, size, LightViewProjection, uncertainty);
    float minimum = 1e30, maximum = -1e30;
    uint validMask = 0, blockedMask = 0;
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        int2 texel = first + int2(tap & 1, tap >> 1);
        float blocker = LightDepth.Load(int3(texel, 0));
        // Extrapolate the receiver PLANE to this light texel's centre.
        // The normal therefore affects both this depth correction and the bias.
        float receiverDepth = coord.z + dot(depthPerTexel, texel - receiverTexel);
        bool valid = blocker != DEPTH_RANGE_MAX;
        // Positive gap = receiver is farther from the light than the blocker.
        // DEPTH_DIFF swaps subtraction for reversed Z; do not compare raw camera
        // depth against raw light depth. Both operands here are LIGHT depth.
        float separation = DEPTH_DIFF(receiverDepth, blocker);
        if (valid)
        {
            validMask |= 1u << tap;
            minimum = min(minimum, separation);
            maximum = max(maximum, separation);
        }
        // A tap blocks only beyond the allowed precision/slope error.
        // Preserve the existing negated <= comparison, including its NaN behaviour.
        if (valid && !(separation <= bias)) blockedMask |= 1u << tap;
        // Normal execution stops at the first inconclusive/clear tap. Diagnostics
        // visit all four so the min/max gaps and tap masks can be inspected.
        else if (!trace) return false;
    }
    float span = SelectionReceiverRange.z;
    differences = float4(validMask ? minimum * span : 0, validMask ? maximum * span : 0, bias * span, validMask);
    coverage = float4(validMask, blockedMask, bias * span, 1);
    // 15 = 1|2|4|8: ONE clear or missing tap is enough to keep this receiver.
    return blockedMask == 15u;
}

// Slab ray/AABB intersection. Start at a receiver and travel TOWARD the light:
// P(t) = receiver + DirectionToLight*t, for 0 <= t <= ProjectionDistance.
// If the ray crosses the caster box, that caster could shadow this receiver.
// enter/leave are distances along the ray in world units, not device depths.
bool IntersectsCaster(float3 receiver, float3 boxMin, float3 boxMax)
{
    float enter = 0.0;
    float leave = ProjectionDistance;
    UNROLL
    for (uint axis = 0; axis < 3; axis++)
    {
        float direction = DirectionToLight[axis];
        if (abs(direction) < 1e-8)
        {
            if (receiver[axis] < boxMin[axis] || receiver[axis] > boxMax[axis])
                return false;
        }
        else
        {
            // Solve where the ray crosses the two planes for this axis; intersect
            // that interval with the intervals from the other axes.
            float a = (boxMin[axis] - receiver[axis]) / direction;
            float b = (boxMax[axis] - receiver[axis]) / direction;
            enter = max(enter, min(a, b));
            leave = min(leave, max(a, b));
        }
    }
    return leave >= enter;
}

// LEGACY CAMERA-RAY SEARCH: used for explicit receiver diagnostics, or as a
// fallback when no shared light-depth map is available. With the receiver map
// present, CS_V2ReceiverMaskCasters supplies final flags; this trace cannot
// override them. It intentionally remains available to inspect the old tests.
// CASTER SELECTION FLOW (one 64-thread group per caster):
// 1 light visibility -> 2 swept camera rectangle -> 3 camera surface reconstruction
// -> 4 normal/facing -> 5 biased ray/AABB -> 6 proxy light-depth check
// -> 7 cheap occluder light-depth check -> 8 retain caster if ANY receiver survives.
// This finds potential shadow contribution, not the final shadow mask/lighting.
// Debug views write intermediate values; they do not define the acceptance policy.
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_ShadowCasterSelection(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint caster = group.y * GroupsPerRow + group.x;
    if (caster >= CasterCount)
        return;

    // 1. Read this caster and the earlier LIGHT-POV visibility result.
    ShadowCasterBounds box = ShadowCasterBoxes[caster];
    bool trace = (CasterDebugFlags & 128u) != 0 && caster == ReceiverTraceCaster;
    bool lightVisible = !HasLightDepth || LightCasterVisibility[caster] != 0;
    // Shared light-space mask broad phase; the camera-ray checks below remain the fine phase.
    bool receiverVisible = (CasterDebugFlags & 131072u) == 0 || CameraReceiverCasterVisibility[caster] != 0;
    if ((CasterDebugFlags & 64u) != 0 && box.CameraIndex != 0xffffffffu && CameraCasterVisibility[box.CameraIndex] == 0)
        box.DebugCategory = (box.DebugCategory & ~1u) | 2u;
    bool writeDebugHits = WriteDebugMask != 0 && ((CasterDebugFlags & 4u) == 0 || (box.DebugCategory & 128u) != 0);
    if ((!lightVisible || !receiverVisible) && !trace)
    {
        if (thread == 0)
            CasterSelection[caster] = 0;
        return;
    }
    // Cheap geometry also needs a visible receiver. A camera-hidden caster is
    // retained when its light-space sweep reaches a camera-visible surface.

    // 2. Bound the CAMERA pixels worth testing. Extrude the AABB AWAY from the
    // light by ProjectionDistance, then project that swept volume into the camera.
    // This rectangle is a broad phase: pixels inside still need the exact ray test.
    // A corner behind the camera falls back to the whole screen to avoid misses.
    float3 offset = -DirectionToLight * ProjectionDistance;
    float3 boundsMin = box.Minimum + min(offset, 0);
    float3 boundsMax = box.Maximum + max(offset, 0);
    float2 minUV = 1, maxUV = 0;
    UNROLL
    for (uint corner = 0; corner < 8; corner++)
    {
        float3 cornerPosition = float3(
            (corner & 1) ? boundsMax.x : boundsMin.x,
            (corner & 2) ? boundsMax.y : boundsMin.y,
            (corner & 4) ? boundsMax.z : boundsMin.z);
        float4 clip = PROJECT_POINT(float4(cornerPosition, 1), ShadowViewProjection);
        if (clip.w <= 0)
        {
            minUV = 0;
            maxUV = 1;
            break;
        }
        float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        minUV = min(minUV, uv);
        maxUV = max(maxUV, uv);
    }

    uint2 size = uint2(Width, Height);
    uint2 first = uint2(floor(saturate(minUV) * size));
    uint2 last = uint2(ceil(saturate(maxUV) * size));
    uint2 extent = last - first;
    if (thread == 0)
        ProxyHit = 0;
    GroupMemoryBarrierWithGroupSync();
    // Distribute the rectangle's pixels over 64 threads (thread, thread+64, ...).
    for (uint index = thread; index < extent.x * extent.y; index += 64)
    {
        uint2 pixel = first + uint2(index % extent.x, index / extent.x);
        float depth = ReceiverDepth.Load(int3(pixel, 0));
        if (depth == DEPTH_RANGE_MAX)
        {
            TraceReceiverExit(trace, pixel, 2);
            continue;
        }
        if (!lightVisible)
        {
            TraceReceiverExit(trace, pixel, 3);
            continue;
        }
        // 3. Reconstruct the CAMERA-visible surface. Pixel centre -> UV -> NDC;
        // then inverse camera VP gives q, and q.xyz/q.w gives world position.
        // Empty camera depth above is sky, so it cannot receive a visible shadow.
        if (!receiverVisible)
        {
            TraceReceiverExit(trace, pixel, 11); // No light-space camera-visible receiver behind this caster.
            continue;
        }
        float2 uv = (float2(pixel) + 0.5) / size;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 receiver = PROJECT_POINT(float4(ndc, depth, 1), InverseViewProjection);
        float3 receiverPosition = receiver.xyz / receiver.w;
        float3 cameraUncertainty = V2CameraDepthUncertainty(receiverPosition, receiver.w, InverseViewProjection);
        TraceReceiverValue(trace, pixel, 1, float4(depth, length(receiverPosition - SelectionCameraPosition.xyz), length(cameraUncertainty), 1));
        TraceReceiverValue(trace, pixel, 2, float4(receiverPosition, 1));
        // Cheap broad phase only; the acceptance test below uses the original box.
        if (!IntersectsCaster(receiverPosition, box.Minimum - SelectionReceiverBias, box.Maximum + SelectionReceiverBias))
        {
            if (ReceiverTraceStep == 0 || ReceiverTraceStep > 2) TraceReceiverExit(trace, pixel, 4);
            continue;
        }

        // 4. Recover a geometric receiver normal. A reliable back-facing surface
        // cannot receive this direct light; an unreliable edge fit stays potential.
        // Surface Normal shows XYZ; Surface Confidence shows the fit flag and N.L.
        float3 normal = 0;
        float planeConfidence = 0;
        if (HasReceiverNormals)
        {
            normal = normalize(ShadowReceiverNormals.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
            float4 plane = V2ReceiverPlaneNormalChecked(ReceiverDepth, receiverPosition, depth, int2(pixel), float2(Width, Height),
                InverseViewProjection, normal);
            normal = plane.xyz;
            planeConfidence = plane.w;
            TraceReceiverValue(trace, pixel, 3, plane);
            TraceReceiverValue(trace, pixel, 9, float4(plane.w, dot(normal, DirectionToLight), length(cameraUncertainty), 1));
            // Matching the occluder's depth only identifies its surface. Its
            // back face cannot receive direct light or a shadow from that light.
            // Reject only a reliable geometric plane; silhouettes stay potential.
            if (box.UseProxy && plane.w != 0 && dot(normal, DirectionToLight) < -0.0001)
            {
                if (ReceiverTraceStep != 3 && ReceiverTraceStep != 9 && ReceiverTraceStep > 2 || ReceiverTraceStep == 0)
                    TraceReceiverExit(trace, pixel, 5);
                continue;
            }
        }


        // 5. Test the original AABB using an adjusted receiver. This changes the
        // ray START, not the box, light direction or actual rendered geometry.
        // Bias only the selection ray's start, opposite the surface normal.
        // Reconstruct the geometric plane from the original, unbiased depth.
        float3 testReceiver = receiverPosition - normal * SelectionReceiverBias;
        TraceReceiverValue(trace, pixel, 4, float4(testReceiver - receiverPosition, SelectionReceiverBias));
        if (!IntersectsCaster(testReceiver, box.Minimum, box.Maximum))
        {
            if (ReceiverTraceStep == 0 || ReceiverTraceStep > 4 && ReceiverTraceStep != 9) TraceReceiverExit(trace, pixel, 4);
            continue;
        }

        bool uncertain = planeConfidence == 0;
        if (HasLightDepth && box.UseProxy)
        {
            // 6. Test expensive AABB proxies in the shared LIGHT depth map.
            // A proxy is potential when it lies in front of/near this receiver.
            // Missing map data also stays potential rather than causing a false cull.
            // Cross in light space, never subtract depths from different projections.
            // Only reject when all four proxy-map neighbors are at/behind the receiver.
            float3 coord = ProjectV2Shadow(testReceiver, LightViewProjection);
            TraceReceiverValue(trace, pixel, 5, float4(coord, IsV2ShadowCoordValid(coord) ? 1 : 0));
            float2 mapSize = SelectionShadowTiles[0].zw;
            int2 firstTexel = int2(floor(coord.xy * mapSize - 0.5));
            if (IsV2ShadowCoordValid(coord) && all(firstTexel >= 0) && all(firstTexel + 1 < int2(mapSize)))
            {
                bool potential = false;
                uint proxyValidMask = 0, proxyPotentialMask = 0;
                float minimum = 1e30, maximum = -1e30;
                UNROLL
                for (uint tap = 0; tap < 4; tap++)
                {
                    float blocker = ProxySelectionDepth.Load(int3(firstTexel + int2(tap & 1, tap >> 1), 0));
                    bool valid = blocker != DEPTH_RANGE_MAX;
                    float separation = DEPTH_DIFF(coord.z, blocker);
                    // Current experimental threshold: TWO times the base light-depth
                    // allowance. Debug B below still reports ONE base allowance.
                    // Keep this distinction in mind when comparing the raw values.
                    bool possible = !valid || separation >= -(SelectionShadowSplit.w + SelectionShadowSplit.w);
                    potential = potential || possible;
                    if (valid) { proxyValidMask |= 1u << tap; minimum = min(minimum, separation); maximum = max(maximum, separation); }
                    if (possible) proxyPotentialMask |= 1u << tap;
                }
                float span = SelectionReceiverRange.z;
                TraceReceiverValue(trace, pixel, 6, float4(proxyValidMask ? minimum * span : 0, proxyValidMask ? maximum * span : 0, SelectionShadowSplit.w * span, proxyValidMask));
                TraceReceiverValue(trace, pixel, 11, float4(proxyValidMask, proxyPotentialMask, SelectionShadowSplit.w * span, 1));
                uncertain = uncertain || proxyValidMask != 15u || (proxyPotentialMask != 0u && proxyPotentialMask != 15u);
                if (!potential)
                {
                    if (ReceiverTraceStep == 0 || ReceiverTraceStep == 7 || ReceiverTraceStep == 8 || ReceiverTraceStep == 10) TraceReceiverExit(trace, pixel, 7);
                    continue;
                }
            }
            else uncertain = true;
        }
        // 7. A cheap real occluder can invalidate the proxy's potential hit.
        // Use the ORIGINAL receiver here: a normal offset inside a wall must not
        // become evidence of a real blocker. See the four-tap test above.
        float4 cheapDifference = 0, cheapCoverage = 0;
        bool blocked = box.UseProxy && IsReceiverBlockedByCheapOccluders(receiverPosition, normal, pixel, depth, receiver.w, trace, cheapDifference, cheapCoverage);
        TraceReceiverValue(trace, pixel, 7, cheapDifference);
        TraceReceiverValue(trace, pixel, 8, float4(SelectionShadowSplit.w * SelectionReceiverRange.z, length(cameraUncertainty), cheapDifference.z, 1));
        TraceReceiverValue(trace, pixel, 10, cheapCoverage);
        if (blocked)
        {
            if (ReceiverTraceStep == 0) TraceReceiverExit(trace, pixel, 8);
            continue;
        }
        // 8. This pixel survived all rejection tests, so retain the caster.
        // uncertain only chooses the DEBUG decision colour; it does not reject it.
        uncertain = uncertain || cheapCoverage.x != 15 || (cheapCoverage.y != 0 && cheapCoverage.y != 15);
        if (ReceiverTraceStep == 0) TraceReceiverExit(trace, pixel, uncertain ? 10u : 9u);
        InterlockedOr(ProxyHit, 1u);
        if (writeDebugHits && box.UseProxy && ((CasterDebugFlags & 16u) == 0 || (box.DebugCategory & 6u) != 0))
        {
            if ((CasterDebugFlags & 8u) != 0)
                InterlockedMin(ReceiverHits[pixel], box.DebugCategory);
            else
                InterlockedOr(ReceiverHits[pixel], box.DebugCategory & 7u);
        }
        else if (!trace)
            break;
    }
    GroupMemoryBarrierWithGroupSync();
    // One accepted pixel anywhere in the rectangle keeps the entire caster.
    // The group must finish its atomic writes before thread 0 publishes the flags.
    if (thread == 0)
        CasterSelection[caster] = ProxyHit != 0 ? 3u : 0u;
}

#endif

#ifdef _PS_ShadowReceiverHits

Texture2D<uint> ShadowReceiverHitMask : register(t4);

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ShadowReceiverHits(float4 position : SV_Position) : SV_Target
{
    uint hit = ShadowReceiverHitMask.Load(int3(int2(position.xy), 0));
    if ((hit & 0x80000000u) != 0)
    {
        if (hit == 0xffffffffu)
            return 0;
        float hue = (float)((hit >> 8) & 65535u) / 65536.0;
        float3 color = saturate(abs(frac(hue + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1);
        return float4(color, 0.65);
    }
    float3 color = 0;
    uint count = 0;
    if (hit & 1)
    {
        color += float3(1.0, 0.2, 0.04); // Camera-visible caster
        count++;
    }
    if (hit & 2)
    {
        color += float3(0.05, 0.8, 1.0); // Camera-occluded caster
        count++;
    }
    if (hit & 4)
    {
        color += float3(0.85, 0.15, 1.0); // Outside the camera frustum
        count++;
    }
    return float4(color / max(count, 1u), count ? 0.7 : 0.0);
}

#endif

#ifdef _CS_V2DepthOcclusion

META_CB_BEGIN(2, V2OcclusionData)
float4x4 OcclusionViewProjection;
uint OcclusionWidth;
uint OcclusionHeight;
uint OcclusionCount;
uint OcclusionGroupsPerRow;
META_CB_END

struct V2OcclusionBounds
{
    float4 Minimum;
    float4 Maximum;
};

Texture2D<float> OccluderDepth : register(t5);
StructuredBuffer<V2OcclusionBounds> V2OcclusionBoxes : register(t6);
RWStructuredBuffer<uint> V2OcclusionResults : register(u0);
groupshared uint HasVisiblePixel;

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_V2DepthOcclusion(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint objectIndex = group.y * OcclusionGroupsPerRow + group.x;
    if (objectIndex >= OcclusionCount)
        return;

    V2OcclusionBounds box = V2OcclusionBoxes[objectIndex];
    float2 minUV = 1, maxUV = 0;
    float nearest = DEPTH_RANGE_MAX;
    bool valid = box.Minimum.w != 0;
    UNROLL
    for (uint corner = 0; corner < 8; corner++)
    {
        float3 position = float3(
            (corner & 1) ? box.Maximum.x : box.Minimum.x,
            (corner & 2) ? box.Maximum.y : box.Minimum.y,
            (corner & 4) ? box.Maximum.z : box.Minimum.z);
        float4 clip = PROJECT_POINT(float4(position, 1), OcclusionViewProjection);
        if (!(clip.w > 0 && clip.z > 0 && clip.z < clip.w))
        {
            valid = false;
            break;
        }
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        minUV = min(minUV, uv);
        maxUV = max(maxUV, uv);
#if REVERSE_Z
        nearest = max(nearest, ndc.z);
#else
        nearest = min(nearest, ndc.z);
#endif
    }

    uint2 size = uint2(OcclusionWidth, OcclusionHeight);
    uint2 first = uint2(floor(saturate(minUV) * size));
    uint2 last = uint2(ceil(saturate(maxUV) * size));
    if (!valid || any(last <= first))
    {
        if (thread == 0)
            V2OcclusionResults[objectIndex] = 1;
        return;
    }

    uint2 extent = last - first;
    if (thread == 0)
        HasVisiblePixel = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint index = thread; index < extent.x * extent.y; index += 64)
    {
        uint2 pixel = first + uint2(index % extent.x, index / extent.x);
        float depth = OccluderDepth.Load(int3(pixel, 0));
        if (depth == DEPTH_RANGE_MAX || !DEPTH_CMP(depth, nearest) || abs(depth - nearest) <= 1e-6)
        {
            InterlockedOr(HasVisiblePixel, 1);
            break;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0)
        V2OcclusionResults[objectIndex] = HasVisiblePixel;
}

#endif

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowFilter) || defined(_PS_V2ShadowBlendReduce) || defined(_PS_V2ShadowBlendFilter) || defined(_PS_V2ShadowBlendCombine)

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
float4 ShadowBlendParameters; // Inverse target size for reduce/combine; filter axis UV step and radius for blur.
uint ShadowBlendInputVisibility;
uint ShadowBlendDebugMode;
float2 ShadowBlendPadding;
META_CB_END

Texture2D<float> LightingReceiverDepth : register(t2);
Texture2D<float> LightingLightDepth : register(t7);
Texture2D<float4> LightingNormals : register(t8);
Texture2D<float4> LightingBaseColor : register(t9);
Texture2D<float4> LightingMaterial : register(t10);
Texture2D<float4> LightingEmissive : register(t11);
Texture2D<float2> RawShadowMask : register(t12);
Texture2D<float> FilteredShadowMask : register(t13);

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

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask)
float2 V2InvAtlasSize()
{
    uint width, height;
    LightingLightDepth.GetDimensions(width, height);
    return 1.0 / float2(width, height);
}

float4x4 V2LightingShadowProjection(uint tier)
{
    return tier == 0 ? LightingLightViewProjection : LightingNearLightViewProjection;
}

float4 GatherV2ReceiverSeparation(float2 texel, float2 receiverTexel, float4 tile, float2 invAtlasSize,
    float depth, float2 depthPerTexel, float bias, out float2 blend)
{
    // Gather order is bottom-left, bottom-right, top-right, top-left.
    // Clamp the footprint to this tile before gathering, including atlas seams.
    float2 clamped = clamp(texel, 0, tile.zw - 1);
    float2 first = min(floor(clamped), tile.zw - 2);
    blend = clamped - first;
#if CAN_USE_GATHER && !defined(WGSL)
    float4 blockers = LightingLightDepth.GatherRed(SamplerPointClamp, (tile.xy + first + 1.0) * invAtlasSize);
#else
    int2 origin = int2(tile.xy + first);
    float4 blockers = float4(
        LightingLightDepth.Load(int3(origin + int2(0, 1), 0)),
        LightingLightDepth.Load(int3(origin + int2(1, 1), 0)),
        LightingLightDepth.Load(int3(origin + int2(1, 0), 0)),
        LightingLightDepth.Load(int3(origin, 0)));
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
    float depth, float2 depthPerTexel, float bias)
{
    float2 blend;
    float4 separation = GatherV2ReceiverSeparation(texel, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias, blend);
    return BlendV2ShadowQuad(float4(separation <= 0.0), blend);
}

#endif

#if defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask)

// Also use the adaptive filter when a legacy view needs an explicit mask.
float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier);

float2 SampleV2MaskTile(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier)
{
    float4 tile = LightingShadowTiles[tier];
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float4x4 projection = V2LightingShadowProjection(tier);
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2ReceiverDepthBias(LightingShadowSplit.w, depthPerTexel, tile.zw, projection, uncertainty);
    float coverage = SampleV2ReceiverPlane(receiverTexel, receiverTexel, tile, V2InvAtlasSize(), coord.z, depthPerTexel, bias);
    if (coverage >= 1.0)
        return float2(1, 0.5);

    float2 texel = floor(receiverTexel + 0.5);
    float blocker = LightingLightDepth.Load(int3(int2(tile.xy + texel), 0));
    float receiverDepth = coord.z + dot(depthPerTexel, texel - receiverTexel);
    float separation = DEPTH_DIFF(receiverDepth, blocker);
    if (blocker == DEPTH_RANGE_MAX || separation <= bias)
        return float2(coverage, 0.5);

    float penumbra = (separation - bias) * LightingShadowFilter.x * LightingShadowFilter.z;
    return float2(coverage, clamp(penumbra * V2PixelsPerWorldUnit(receiver), 0.5, 4.0));
}

#ifdef _PS_V2ShadowMask

META_PS(true, FEATURE_LEVEL_SM5)
float2 PS_V2ShadowMask(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = LightingReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX)
        return float2(1, 0.5);

    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, float2(width, height));
    float3 receiver = receiverH.xyz / receiverH.w;
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
    float4 nearSample = V2NearShadowSample(receiver);
    float3 normal = V2ReceiverPlaneNormal(LightingReceiverDepth, receiver, depth, pixel.xy, float2(width, height),
        LightingInverseViewProjection, DecodeNormal(LightingNormals.Load(pixel).xyz));
    float2 farMask = float2(1, 0.5);
    if (nearSample.w < 1.0)
    {
        float3 coord = ProjectV2Shadow(receiver, LightingLightViewProjection);
        if (IsV2ShadowCoordValid(coord))
            farMask = LightingShadowFilter.w > 0
                ? float2(SampleV2ShadowTile(coord.xy, coord.z, normal, uncertainty, position.xy, 0), 0.5)
                : SampleV2MaskTile(receiver, normal, uncertainty, coord, 0);
    }
    if (nearSample.w <= 0.0)
        return farMask;
    float2 nearMask = LightingShadowFilter.w > 0
        ? float2(SampleV2ShadowTile(nearSample.xy, nearSample.z, normal, uncertainty, position.xy, 1), 0.5)
        : SampleV2MaskTile(receiver, normal, uncertainty, nearSample.xyz, 1);
    return lerp(farMask, nearMask, nearSample.w);
}

#endif

#endif

#ifdef _PS_V2ShadowFilter

META_PS(true, FEATURE_LEVEL_SM5)
float PS_V2ShadowFilter(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float depth = LightingReceiverDepth.Load(int3(pixel, 0));
    if (depth == DEPTH_RANGE_MAX)
        return 1.0;

    uint width, height;
    RawShadowMask.GetDimensions(width, height);
    int2 size = int2(width, height);
    float2 center = RawShadowMask.Load(int3(pixel, 0));
    // Adaptive and hard bands carry a half-pixel support. Their interiors
    // already contain the final shadow result and need no screen-space blur.
    if (LightingCascadeRange.w != 0.0 && center.y <= 0.5)
        return center.x;
    static const int2 offsets[12] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1),
        int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3)
    };

    float2 samples[12];
    bool edge = false;
    UNROLL
    for (uint i = 0; i < 12; i++)
    {
        int2 samplePixel = pixel + offsets[i];
        samples[i] = RawShadowMask.Load(int3(clamp(samplePixel, 0, size - 1), 0));
        edge = edge || samples[i].x != center.x;
    }
    if (!edge)
        return center.x;

    float3 receiver = ReconstructV2Receiver(position.xy, depth, size);
    float3 normal = DecodeNormal(LightingNormals.Load(int3(pixel, 0)).xyz);
    float planeTolerance = 1.5 / max(V2PixelsPerWorldUnit(receiver), 0.0001);
    float visibility = center.x;
    float totalWeight = 1.0;
    UNROLL
    for (uint tap = 0; tap < 12; tap++)
    {
        int2 samplePixel = pixel + offsets[tap];
        if (any(samplePixel < 0) || any(samplePixel >= size))
            continue;
        float2 sampleMask = samples[tap];
        float distance = length(float2(offsets[tap]));
        float radius = LightingCascadeRange.w != 0.0 ? center.y : max(center.y, sampleMask.y);
        float weight = saturate(1.0 - distance / (radius + 0.5));
        if (weight <= 0)
            continue;
        float sampleDepth = LightingReceiverDepth.Load(int3(samplePixel, 0));
        if (sampleDepth == DEPTH_RANGE_MAX)
            continue;
        float3 sampleNormal = DecodeNormal(LightingNormals.Load(int3(samplePixel, 0)).xyz);
        if (dot(normal, sampleNormal) < 0.9)
            continue;
        float3 sampleReceiver = ReconstructV2Receiver(float2(samplePixel) + 0.5, sampleDepth, size);
        // Compare against the receiver plane so grazing angles on a flat floor still filter.
        if (abs(dot(sampleReceiver - receiver, normal)) > planeTolerance)
            continue;
        visibility += sampleMask.x * weight;
        totalWeight += weight;
    }
    return visibility / totalWeight;
}

#endif

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowMask)

#include "./Flax/Math/Noise.hlsl"

float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier)
{
    float4 tile = LightingShadowTiles[tier];
    uint width, height;
    LightingLightDepth.GetDimensions(width, height);
    float2 invAtlasSize = 1.0 / float2(width, height);
    float2 receiverTexel = uv * tile.zw - 0.5;
    float4x4 projection = tier == 0 ? LightingLightViewProjection : LightingNearLightViewProjection;
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2ReceiverDepthBias(LightingShadowSplit.w, depthPerTexel, tile.zw, projection, uncertainty);
    const float noise = InterleavedGradientNoise(pixel);
    float sine, cosine;
    sincos(noise * 6.2831853, sine, cosine);
    static const float2 directions[8] = {
        float2(1, 0), float2(0.7071068, 0.7071068), float2(0, 1), float2(-0.7071068, 0.7071068),
        float2(-1, 0), float2(-0.7071068, -0.7071068), float2(0, -1), float2(0.7071068, -0.7071068)
    };

    // GPU Gems 2, chapter 17. Probe the disk perimeter before refining mixed coverage.
    float2 offsets[8];
    float total = SampleV2ReceiverPlane(receiverTexel, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
    UNROLL
    for (uint i = 0; i < 8; i++)
    {
        float2 direction = directions[i];
        offsets[i] = float2(cosine * direction.x - sine * direction.y, sine * direction.x + cosine * direction.y) * LightingShadowFilter.w;
        float radius = 0.84 + 0.16 * frac(noise + i * 0.618034);
        total += SampleV2ReceiverPlane(receiverTexel + offsets[i] * radius, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
    }
    float sampleCount = 9.0;
    BRANCH
    if (total > 0.0 && total < 9.0)
    {
        UNROLL
        for (uint tap = 0; tap < 8; tap++)
        {
            float jitter = frac(noise + tap * 0.381966);
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * lerp(0.18, 0.55, jitter), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * lerp(0.58, 0.81, 1.0 - jitter), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
        }
        sampleCount = 25.0;
    }
    return total / sampleCount;
}

#endif

#ifdef _PS_V2CascadeShadowMask

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
    float3 biasedReceiver = V2OffsetShadowReceiver(receiver, normal, LightingDirectionToLight, LightingCascadeParameters.x);
    float3 coord = ProjectV2Shadow(biasedReceiver, LightingLightViewProjection);
    if (!IsV2ShadowCoordValid(coord))
        discard;

    float weight = LightingCascadeRange.y < LightingCascadeRange.z
        ? 1.0 - smoothstep(LightingCascadeRange.y, LightingCascadeRange.z, receiverDistance) : 1.0;
    if (LightingCascadeParameters.z != 0)
    {
        static const float3 colors[8] = {
            float3(0.95, 0.2, 0.18), float3(0.2, 0.85, 0.3),
            float3(0.2, 0.5, 1.0), float3(1.0, 0.75, 0.15),
            float3(0.85, 0.25, 0.95), float3(0.1, 0.85, 0.9),
            float3(1.0, 0.45, 0.1), float3(0.7, 0.7, 0.7)
        };
        return float4(colors[(uint)LightingCascadeParameters.y % 8], weight);
    }
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
    float2 mask;
    BRANCH
    if (LightingShadowFilter.w > 0)
        mask = float2(SampleV2ShadowTile(coord.xy, coord.z, normal, uncertainty, position.xy, 0), 0.5);
    else
    {
        mask = SampleV2MaskTile(receiver, normal, uncertainty, coord, 0);
        // Blend radius alongside visibility so filter changes use the same
        // radial transition, including transitions into a hard or adaptive band.
        mask.y = clamp(mask.y, 0.5, max(LightingCascadeParameters.w, 0.5));
    }
    return float4(mask, 0, weight);
}

#endif

#ifdef _PS_V2DirectionalLighting

float SampleV2Shadow(float3 receiver, float3 normal, float3 uncertainty, float2 pixel)
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
            if (LightingShadowFilter.w > 0)
                shadow = SampleV2ShadowTile(coord.xy, coord.z, normal, uncertainty, pixel, tier);
            else
            {
                float4 tile = LightingShadowTiles[tier];
                float2 texel = coord.xy * tile.zw - 0.5;
                float4x4 projection = V2LightingShadowProjection(tier);
                float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
                float bias = V2ReceiverDepthBias(LightingShadowSplit.w, depthPerTexel, tile.zw, projection, uncertainty);
                shadow = SampleV2ReceiverPlane(texel, texel, tile, V2InvAtlasSize(), coord.z, depthPerTexel, bias);
            }
        }
        visibility += shadow * weight;
    }
    return visibility;
}

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2DirectionalLighting(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float depth = LightingReceiverDepth.Load(pixel);
    if (depth == DEPTH_RANGE_MAX)
        return 0;

    float4 packedNormal = LightingNormals.Load(pixel);
    float3 emissive = LightingEmissive.Load(pixel).rgb;
    int shadingModel = (int)(packedNormal.a * 3.999);
    if (shadingModel == SHADING_MODEL_UNLIT)
        return float4(emissive, 1);

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

    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, float2(width, height));
    surface.WorldPos = receiverH.xyz / receiverH.w;

    float3 light = LightingDirectionToLight;
    float nDotL = saturate(dot(surface.Normal, light));
    float3 result = emissive + GetDiffuseColor(surface) * surface.AO * max(LightingAmbientIntensity, 0.0);
    if (nDotL > 0)
    {
        float3 toEye = LightingIsOrthographic ? -LightingCameraForward : normalize(LightingCameraPosition - surface.WorldPos);
        LightSample response = StandardShading(surface, 1.0, light, toEye, surface.Normal);
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
            visibility = SampleV2Shadow(surface.WorldPos, planeNormal, uncertainty, position.xy);
        }
        result += (response.Diffuse + response.Specular) * visibility *
            nDotL * LightingRadiance.rgb * max(LightingRadiance.a, 0.0);
    }
    return float4(result, 1);
}

#endif

#endif

#if defined(_PS_V2ShadowBlendReduce) || defined(_PS_V2ShadowBlendFilter) || defined(_PS_V2ShadowBlendCombine)

Texture2D<float> ShadowBlendInput : register(t14);
Texture2D<float> ShadowBlendLow : register(t15);
Texture2D<float> ShadowBlendMedium : register(t16);

float V2CombineShadowMasks(float a, float b, float c)
{
    // All inputs are shadow amounts, not depth or visibility. A constrains
    // the silhouette, and its unfiltered contribution supplies the 1/3 floor.
    return saturate((a + b * a + c * a) / 3.0);
}

#ifdef _PS_V2ShadowBlendReduce
META_PS(true, FEATURE_LEVEL_SM5)
float PS_V2ShadowBlendReduce(float4 position : SV_Position) : SV_Target
{
    float2 uv = position.xy * ShadowBlendParameters.xy;
    float2 offset = ShadowBlendParameters.xy * 0.25;
    // CPU reduction steps are at most 2:1. Four bilinear taps cover the
    // footprint instead of sampling one point in a large source texture.
    float value = (
        ShadowBlendInput.SampleLevel(SamplerLinearClamp, uv + float2(-offset.x, -offset.y), 0) +
        ShadowBlendInput.SampleLevel(SamplerLinearClamp, uv + float2(offset.x, -offset.y), 0) +
        ShadowBlendInput.SampleLevel(SamplerLinearClamp, uv + float2(-offset.x, offset.y), 0) +
        ShadowBlendInput.SampleLevel(SamplerLinearClamp, uv + offset, 0)) * 0.25;
    return ShadowBlendInputVisibility != 0 ? 1.0 - value : value;
}
#endif

#ifdef _PS_V2ShadowBlendFilter
META_PS(true, FEATURE_LEVEL_SM5)
float PS_V2ShadowBlendFilter(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    ShadowBlendInput.GetDimensions(width, height);
    float2 uv = position.xy / float2(width, height);
    float radius = clamp(ShadowBlendParameters.z, 0.0, 8.0);
    float sigma = max(radius * 0.5, 0.5);
    float sum = 0.0;
    float totalWeight = 0.0;
    LOOP
    for (int tap = -8; tap <= 8; tap++)
    {
        float support = saturate(radius + 1.0 - abs(float(tap)));
        if (support <= 0.0)
            continue;
        float weight = exp(-float(tap * tap) / (2.0 * sigma * sigma)) * support;
        sum += ShadowBlendInput.SampleLevel(SamplerLinearClamp, uv + tap * ShadowBlendParameters.xy, 0) * weight;
        totalWeight += weight;
    }
    return sum / totalWeight;
}
#endif

#ifdef _PS_V2ShadowBlendCombine
META_PS(true, FEATURE_LEVEL_SM5)
float PS_V2ShadowBlendCombine(float4 position : SV_Position) : SV_Target
{
    float2 uv = position.xy * ShadowBlendParameters.xy;
    float a = saturate(1.0 - ShadowBlendInput.Load(int3(int2(position.xy), 0)));
    float b = saturate(ShadowBlendLow.SampleLevel(SamplerLinearClamp, uv, 0));
    float c = saturate(ShadowBlendMedium.SampleLevel(SamplerLinearClamp, uv, 0));
    float shadow = V2CombineShadowMasks(a, b, c);
    if (ShadowBlendDebugMode == 1) return shadow;
    if (ShadowBlendDebugMode == 2) return a;
    if (ShadowBlendDebugMode == 3) return b;
    if (ShadowBlendDebugMode == 4) return c;
    return 1.0 - shadow; // Lighting consumes visibility.
}
#endif

#endif

#ifdef _CS_V2ShadowIndirectArgs

StructuredBuffer<uint> ShadowSelectionFlags : register(t0);
StructuredBuffer<uint2> ShadowIndirectInputs : register(t1);
RWBuffer<uint> ShadowIndirectOutput : register(u0);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_V2ShadowIndirectArgs(uint index : SV_DispatchThreadID)
{
    if (index >= ShadowIndirectInputs[0].x)
        return;
    uint2 input = ShadowIndirectInputs[index + 1];
    uint offset = index * 5;
    ShadowIndirectOutput[offset + 0] = input.y;
    uint mask = ShadowIndirectInputs[0].y;
    uint flags = ShadowSelectionFlags[input.x];
    ShadowIndirectOutput[offset + 1] = (mask != 0 ? (flags & mask) : flags) != 0 ? 1 : 0;
    ShadowIndirectOutput[offset + 2] = 0;
    ShadowIndirectOutput[offset + 3] = 0;
    ShadowIndirectOutput[offset + 4] = 0;
}

#endif

#ifdef _VS_ShadowCasterBoundsDebug
StructuredBuffer<ShadowCasterBounds> DebugCasterBounds : register(t3);
StructuredBuffer<uint> DebugCasterSelection : register(t18);
struct CasterBoundsDebugOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};
META_VS(true, FEATURE_LEVEL_SM5)
CasterBoundsDebugOutput VS_ShadowCasterBoundsDebug(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const uint corners[24] = { 0,1, 1,3, 3,2, 2,0, 4,5, 5,7, 7,6, 6,4, 0,4, 1,5, 2,6, 3,7 };
    ShadowCasterBounds box = DebugCasterBounds[instance];
    bool selected = DebugCasterSelection[instance] != 0;
    bool show = (box.UseProxy != 0 || (CasterDebugFlags & 32u) != 0) && (CasterDebugFlags & (selected ? 1u : 2u)) != 0 &&
        ((CasterDebugFlags & 4u) == 0 || (box.DebugCategory & 128u) != 0);
    uint corner = corners[vertex];
    float3 cornerPosition = float3((corner & 1) ? box.Maximum.x : box.Minimum.x,
                         (corner & 2) ? box.Maximum.y : box.Minimum.y,
                         (corner & 4) ? box.Maximum.z : box.Minimum.z);
    CasterBoundsDebugOutput output;
    output.Position = show ? PROJECT_POINT(float4(cornerPosition, 1), ShadowViewProjection) : float4(0, 0, -1, 1);
    output.Color = selected ? float4(0.1, 1, 0.1, 1) : float4(1, 0.1, 0.1, 1);
    return output;
}
#endif
#ifdef _PS_ShadowCasterBoundsDebug
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ShadowCasterBoundsDebug(float4 position : SV_Position, float4 color : COLOR0) : SV_Target
{
    return color;
}
#endif

float3 V2TraceReasonColor(uint reason)
{
    if (reason == 2) return float3(0.3,0,0.4);
    if (reason == 3) return float3(0.4,0,0);
    if (reason == 4) return float3(0.02,0.08,0.2);
    if (reason == 5) return float3(1,0.85,0.05);
    if (reason == 7) return float3(0,0.6,0.9);
    if (reason == 8) return float3(1,0.08,0.03);
    if (reason == 9) return float3(0.05,1,0.1);
    if (reason == 10) return float3(1,0.4,0.02);
    if (reason == 11) return float3(0.12,0.2,0.65);
    return float3(0.015,0.015,0.015);
}

#ifdef _PS_V2ShadowResourceDebug
Texture2D<float4> ShadowDebugResource : register(t19);
Texture2D<float4> ShadowDebugCameraColor : register(t20);
StructuredBuffer<ShadowCasterBounds> ShadowDebugBounds : register(t3);
StructuredBuffer<uint> ShadowDebugSelection : register(t18);
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2ShadowResourceDebug(float4 position : SV_Position) : SV_Target
{
    uint mode = (CasterDebugFlags >> 8) & 255u;
    bool valid = (CasterDebugFlags & 65536u) != 0;
    float2 outputSize = float2(Width, Height);
    float2 localPosition = position.xy - SelectionReceiverRange.zw;
    if (!valid)
    {
        bool checker = (((uint)localPosition.x / 16u + (uint)localPosition.y / 16u) & 1u) != 0;
        return float4(checker ? float3(0.45, 0, 0.45) : float3(0.04, 0.04, 0.04), 1);
    }
    if (mode == 7)
    {
        bool cameraPane = localPosition.x < outputSize.x * 0.5;
        if (abs(localPosition.x - outputSize.x * 0.5) < 1)
            return float4(0.3, 0.3, 0.3, 1);
        uint sourceWidth, sourceHeight;
        if (cameraPane)
            ShadowDebugCameraColor.GetDimensions(sourceWidth, sourceHeight);
        else
            ShadowDebugResource.GetDimensions(sourceWidth, sourceHeight);
        float2 sourceSize = float2(sourceWidth, sourceHeight);
        float2 paneSize = float2(outputSize.x * 0.5, outputSize.y);
        float2 panePosition = localPosition - float2(cameraPane ? 0 : paneSize.x, 0);
        float scale = min(paneSize.x / sourceSize.x, paneSize.y / sourceSize.y);
        float2 uv = ((panePosition - paneSize * 0.5) / scale + sourceSize * 0.5) / sourceSize;
        if (any(uv < 0) || any(uv >= 1))
            return float4(0.025, 0.025, 0.025, 1);
        int2 texel = min(uint2(uv * sourceSize), uint2(sourceWidth - 1, sourceHeight - 1));
        return float4(cameraPane ? ShadowDebugCameraColor.Load(int3(texel, 0)).rgb : ShadowDebugResource.Load(int3(texel, 0)).rgb, 1);
    }
    uint width, height;
    ShadowDebugResource.GetDimensions(width, height);
    // A cascade debug snapshot uses the shared maximum-size allocation. Only
    // the cascade's logical top-left tile was rendered; crop the display to it.
    if (mode == 4 && (CasterDebugFlags & 131072u) != 0)
    {
        width = min(width, (uint)SelectionShadowTiles[0].z);
        height = min(height, (uint)SelectionShadowTiles[0].w);
    }
    float2 sourceSize = float2(width, height);
    float scale = min(outputSize.x / sourceSize.x, outputSize.y / sourceSize.y);
    float2 uv = ((localPosition - outputSize * 0.5) / scale + sourceSize * 0.5) / sourceSize;
    if (any(uv < 0) || any(uv >= 1))
        return float4(0.025, 0.025, 0.025, 1);
    float4 value = ShadowDebugResource.Load(int3(min(uint2(uv * sourceSize), uint2(width - 1, height - 1)), 0));
#if 1 // Region: receiver trace colour conversion
    // These are DISPLAY colours, not another depth/visibility test.
    // The numeric probe reads the unmodified raw float4 written by the compute shader.
    // In particular, orange in Surface Confidence is (reliable=1, positive N.L, 0),
    // not necessarily Decision's accepted-uncertain colour.
    if (mode >= 13 && mode <= 15)
    {
        int2 texel = min(uint2(uv * sourceSize), uint2(width - 1, height - 1));
        uint coverage = CameraReceiverCoverage.Load(int3(texel,0));
        if (coverage == 0) return float4(0.01,0.01,0.01,1);
        if ((coverage & 2u) == 0) return float4(0.4,0.04,0.5,1); // Camera sees a back-facing receiver.
        if (mode == 13) return float4(0.1,0.85,0.25,1);
        float nearDepth = asfloat(CameraReceiverNear.Load(int3(texel,0)));
        float farDepth = asfloat(CameraReceiverFar.Load(int3(texel,0)));
        if (mode == 14)
            return float4(saturate(nearDepth * ShadowSelectionFilterRadius / max(SelectionReceiverRange.x,1)).xxx,1);
        float cheapDepth = value.x; // CandidateOccluderDepth is bound as the reference image.
        if (cheapDepth == DEPTH_RANGE_MAX) return float4(0.1,0.45,1,1);
        float blocker = DEPTH_01(cheapDepth);
        float tolerance = SelectionShadowSplit.w * 4;
        if (nearDepth > blocker + tolerance) return float4(1,0.18,0.05,1); // All recorded receivers behind cheap depth.
        if (farDepth > blocker + tolerance) return float4(1,0.7,0.05,1); // Multiple depths: at least one behind.
        return float4(0.1,0.85,0.25,1); // Receiver matches or precedes cheap light depth.
    }
    if (mode == 12)
    {
        if (ReceiverTraceStep == 0) return float4(V2TraceReasonColor((uint)value.x), 1);
        if (value.w < 0) return float4(V2TraceReasonColor((uint)(-value.w)) * 0.35, 1);
        float range = max(SelectionReceiverRange.y, 0.001);
        if (ReceiverTraceStep == 1) return float4(saturate(value.y / max(SelectionReceiverRange.x, 1)).xxx, 1);
        if (ReceiverTraceStep == 2) return float4(frac(abs(value.xyz) / range), 1);
        if (ReceiverTraceStep == 3) return float4(value.xyz * 0.5 + 0.5, 1);
        if (ReceiverTraceStep == 4) return float4(saturate(0.5 + value.xyz / range * 0.5), 1);
        if (ReceiverTraceStep == 5) return float4(value.xyz, 1);
        if (ReceiverTraceStep == 6 || ReceiverTraceStep == 7)
            return float4(saturate(value.x / range), saturate(1 - abs(value.x) / range) * 0.2, saturate(-value.x / range), 1);
        if (ReceiverTraceStep == 8) return float4(saturate(value.xyz / range), 1);
        if (ReceiverTraceStep == 9) return float4(value.x, saturate(value.y), saturate(-value.y), 1);
        uint mask = (uint)value.y;
        float2 quad = frac(uv * sourceSize * 0.5);
        uint tap = (quad.x >= 0.5 ? 1u : 0u) + (quad.y >= 0.5 ? 2u : 0u);
        return float4((mask & (1u << tap)) != 0 ? float3(0.1,1,0.2) : float3(0.5,0.03,0.02), 1);
    }
#endif // Region: receiver trace colour conversion
    if (mode == 8 || mode == 10)
        return float4(value.rgb, 1);
    if (mode == 2)
        return float4(value.xyz, 1); // Raw encoded GBuffer normals.
    if (mode == 5)
        return float4(value.rrr, 1); // White = lit, black = shadow.
    if (value.r == DEPTH_RANGE_MAX)
        return float4(0.12, 0, 0.16, 1); // No recorded surface.
    float gray;
    if (mode == 1)
    {
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 receiver = PROJECT_POINT(float4(ndc, value.r, 1), InverseViewProjection);
        float distance = length(receiver.xyz / receiver.w - SelectionCameraPosition.xyz);
        gray = 1 - saturate(distance / max(SelectionReceiverRange.x, 0.001));
    }
    else
    {
#if REVERSE_Z
        gray = value.r;
#else
        gray = 1 - value.r;
#endif
    }
    return float4(gray.xxx, 1);
}
#endif

#ifdef _PS_V2LightPOVShaded
Texture2D<float> LightPOVDebugDepth : register(t2);
Texture2D<float4> LightPOVDebugNormals : register(t8);
Texture2D<float4> LightPOVDebugAlbedo : register(t9);
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2LightPOVShaded(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    if (LightPOVDebugDepth.Load(int3(pixel, 0)) == DEPTH_RANGE_MAX)
        return float4(0.025, 0.025, 0.025, 1);
    float3 normal = normalize(LightPOVDebugNormals.Load(int3(pixel, 0)).xyz * 2 - 1);
    float3 albedo = LightPOVDebugAlbedo.Load(int3(pixel, 0)).rgb;
    float diffuse = saturate(dot(normal, DirectionToLight));
    return float4(albedo * (0.15 + 0.85 * diffuse), 1);
}
#endif

#ifdef _CS_V2LightCasterVisibility
Texture2D<float> LightVisibilityDepth : register(t7);
StructuredBuffer<ShadowCasterBounds> LightVisibilityBoxes : register(t3);
RWStructuredBuffer<uint> LightVisibilityResults : register(u0);
groupshared uint LightBoxVisible;
groupshared uint LightBoxSampled;
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_V2LightCasterVisibility(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint index = group.y * GroupsPerRow + group.x;
    if (index >= CasterCount) return;
    ShadowCasterBounds box = LightVisibilityBoxes[index];
    float2 lo = 1, hi = 0;
    float nearest = DEPTH_RANGE_MAX;
    bool uncertain = !HasLightDepth;
    UNROLL
    for (uint c = 0; c < 8; c++)
    {
        float3 corner = float3((c & 1) ? box.Maximum.x : box.Minimum.x,
            (c & 2) ? box.Maximum.y : box.Minimum.y, (c & 4) ? box.Maximum.z : box.Minimum.z);
        float4 clip = PROJECT_POINT(float4(corner, 1), LightViewProjection);
        uncertain = uncertain || clip.w <= 0 || clip.z <= 0 || clip.z >= clip.w;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        lo = min(lo, uv); hi = max(hi, uv);
#if REVERSE_Z
        nearest = max(nearest, ndc.z);
#else
        nearest = min(nearest, ndc.z);
#endif
    }
    uint2 size = uint2(SelectionShadowTiles[0].zw);
    int2 first = max(int2(floor(lo * size)) - 1, 0);
    int2 last = min(int2(ceil(hi * size)) + 1, int2(size));
    if (uncertain || any(last <= first) || any(lo < 0) || any(hi > 1))
    {
        if (thread == 0) LightVisibilityResults[index] = 3;
        return;
    }
    uint2 extent = uint2(last - first);
    if (thread == 0)
    {
        LightBoxVisible = 0;
        LightBoxSampled = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint n = thread; n < extent.x * extent.y; n += 64)
    {
        int2 pixel = first + int2(n % extent.x, n / extent.x);
        // A rotated AABB is not its enclosing screen rectangle. Only a ray
        // that actually enters the box can establish light visibility.
        float2 uv = (float2(pixel) + 0.5) / size;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 nearH = PROJECT_POINT(float4(ndc, DEPTH_RANGE_MIN, 1), SelectionNearLightViewProjection);
        float4 farH = PROJECT_POINT(float4(ndc, DEPTH_RANGE_MAX, 1), SelectionNearLightViewProjection);
        float3 rayStart = nearH.xyz / nearH.w;
        float3 rayDelta = farH.xyz / farH.w - rayStart;
        float enter = 0, leave = 1;
        bool intersects = true;
        UNROLL
        for (uint axis = 0; axis < 3; axis++)
        {
            if (abs(rayDelta[axis]) < 1e-8)
                intersects = intersects && rayStart[axis] >= box.Minimum[axis] && rayStart[axis] <= box.Maximum[axis];
            else
            {
                float a = (box.Minimum[axis] - rayStart[axis]) / rayDelta[axis];
                float b = (box.Maximum[axis] - rayStart[axis]) / rayDelta[axis];
                enter = max(enter, min(a, b));
                leave = min(leave, max(a, b));
            }
        }
        if (!intersects || leave < enter)
            continue;
        InterlockedOr(LightBoxSampled, 1u);
        float boxDepth = ProjectV2Shadow(rayStart + rayDelta * enter, LightViewProjection).z;
        float blocker = LightVisibilityDepth.Load(int3(pixel, 0));
        if (blocker == DEPTH_RANGE_MAX || DEPTH_DIFF(boxDepth, blocker) <= SelectionShadowSplit.w * 4)
        {
            InterlockedOr(LightBoxVisible, 3u);
            break;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // A subpixel box without a center sample cannot be safely rejected.
    if (thread == 0) LightVisibilityResults[index] = LightBoxSampled != 0 ? LightBoxVisible : 3u;
}
#endif

#ifdef _VS_V2ShadowProxyDepth
StructuredBuffer<ShadowCasterBounds> ProxyCasterBounds : register(t3);
StructuredBuffer<uint> ProxyLightVisibility : register(t21);
META_VS(true, FEATURE_LEVEL_SM5)
float4 VS_V2ShadowProxyDepth(uint vertex : SV_VertexID, uint instance : SV_InstanceID) : SV_Position
{
    const uint corners[36] = {0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5};
    ShadowCasterBounds box = ProxyCasterBounds[instance];
    uint c = corners[vertex];
    float3 corner = float3((c & 1) ? box.Maximum.x : box.Minimum.x,
        (c & 2) ? box.Maximum.y : box.Minimum.y, (c & 4) ? box.Maximum.z : box.Minimum.z);
    return box.UseProxy && ProxyLightVisibility[instance] != 0 ?
        PROJECT_POINT(float4(corner, 1), LightViewProjection) : float4(0, 0, -1, 1);
}
#endif

#ifdef _PS_V2ReceiverDepthCross
Texture2D<float> CrossCameraDepth : register(t2);
Texture2D<float> CrossProxyDepth : register(t22);
Texture2D<float4> CrossReceiverNormals : register(t8);
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2ReceiverDepthCross(float4 position : SV_Position) : SV_Target
{
    uint2 sourceSize; CrossCameraDepth.GetDimensions(sourceSize.x, sourceSize.y);
    float2 uv = position.xy / float2(Width, Height);
    int2 sourcePixel = min(uint2(uv * sourceSize), sourceSize - 1);
    uv = (float2(sourcePixel) + 0.5) / sourceSize;
    float depth = CrossCameraDepth.Load(int3(sourcePixel, 0));
    if (depth == DEPTH_RANGE_MAX) return float4(0.12, 0, 0.16, 1);
    float4 world = PROJECT_POINT(float4(uv * float2(2,-2) + float2(-1,1), depth, 1), InverseViewProjection);
    float3 coord = ProjectV2Shadow(world.xyz / world.w, LightViewProjection);
    if (!IsV2ShadowCoordValid(coord)) return float4(0.5,0.5,0.5,1);
    uint2 mapSize; CrossProxyDepth.GetDimensions(mapSize.x, mapSize.y);
    float3 normal = HasReceiverNormals ? normalize(CrossReceiverNormals.Load(int3(sourcePixel, 0)).xyz * 2 - 1) : 0;
    if (HasReceiverNormals)
    {
        float4 plane = V2ReceiverPlaneNormalChecked(CrossCameraDepth, world.xyz / world.w, depth, sourcePixel, sourceSize,
            InverseViewProjection, normal);
        normal = plane.xyz;
        if (plane.w != 0 && dot(normal, DirectionToLight) < -0.0001)
            return float4(0.12, 0.12, 0.12, 1); // A back face is not a direct-light shadow receiver.
    }
    float3 surfaceCoord = coord;
    // Displace the probe without treating a point inside the receiver as a new surface.
    coord = ProjectV2Shadow(world.xyz / world.w - normal * SelectionReceiverBias, LightViewProjection);
    if (!IsV2ShadowCoordValid(coord)) return float4(0.5, 0.5, 0.5, 1);
    float2 slope = HasReceiverNormals ? V2ShadowDepthPerTexel(normal, LightViewProjection, mapSize) : 0;
    coord.z = surfaceCoord.z + dot(slope, (coord.xy - surfaceCoord.xy) * mapSize);
    float3 uncertainty = V2CameraDepthUncertainty(world.xyz / world.w, world.w, InverseViewProjection);
    float bias = V2ReceiverDepthBias(SelectionShadowSplit.w, slope, mapSize, LightViewProjection, uncertainty);
    float2 receiverTexel = coord.xy * mapSize - 0.5;
    int2 first = int2(floor(receiverTexel));
    bool shadow = false;
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        int2 texel = clamp(first + int2(tap & 1, tap >> 1), 0, int2(mapSize) - 1);
        float blocker = CrossProxyDepth.Load(int3(texel, 0));
        float receiverDepth = coord.z + dot(slope, texel - receiverTexel);
        shadow = shadow || (blocker != DEPTH_RANGE_MAX && DEPTH_DIFF(receiverDepth, blocker) > bias);
    }
    return shadow ? float4(1,0.25,0.02,1) : float4(0.04,0.35,0.7,1);
}
#endif

#ifdef _PS_V2ReceiverTraceProbe
Texture2D<float4> ReceiverTraceProbeSource : register(t19);
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2ReceiverTraceProbe(float4 position : SV_Position) : SV_Target
{
    return ReceiverTraceProbeSource.Load(int3(int2(SelectionCameraForward.xy), 0));
}
#endif
