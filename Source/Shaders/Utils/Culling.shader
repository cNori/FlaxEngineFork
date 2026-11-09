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

#if defined(_CS_V2ProjectCameraReceivers) || defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowDistanceReceivers)
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

#if defined(_VS_ShadowCasterBoundsDebug) || defined(_CS_V2LightCasterVisibility) || defined(_CS_V2ProjectCameraReceivers) || defined(_CS_V2ReceiverMaskCasters) || defined(_CS_V2ReceiverTileMax)

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
uint SelectionPadding0;
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
float SelectionPadding1;
uint2 SelectionPadding2;
META_CB_END

struct ShadowCasterBounds
{
    float3 Minimum;
    uint UseProxy;
    float3 Maximum;
    uint Padding;
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

#if defined(_CS_V2ReceiverMaskCasters) || defined(_CS_V2ReceiverTileMax)
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




#if defined(_CS_V2DepthOcclusion) || defined(_PS_V2ShadowDepthReduce)
META_CB_BEGIN(2, V2OcclusionData)
float4x4 OcclusionViewProjection;
uint OcclusionWidth;
uint OcclusionHeight;
uint OcclusionCount;
uint OcclusionGroupsPerRow;
META_CB_END
#endif

#ifdef _CS_V2DepthOcclusion
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

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowDistanceReceivers) || defined(_PS_V2ShadowFilter) || defined(_PS_V2ShadowDistanceJump) || defined(_PS_V2ShadowDistanceResolve)

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
float4 LightingDebugOptions; // X: lighting mode (0 material, 1 white, 2 cel), Y: distance field requested, Z: jump step, W: distance debug range (world centimeters).
float4 LightingCelParameters; // X: band count, Y: use receiver SDF, Z: world distance range, W: core fill strength.
float4 LightingShadowSampling; // X: adaptive jitter, Y: SDF side (0 both, 1 inner, 2 outer), Z: seed-only filter pass, W: world-origin noise phase.
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

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowDistanceReceivers)
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

#if defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowDistanceReceivers)

// Also use the adaptive filter when a legacy view needs an explicit mask.
float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier);

float2 SampleV2MaskTile(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier)
{
    float4 tile = LightingShadowTiles[tier];
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float4x4 projection = V2LightingShadowProjection(tier);
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
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

float FilterV2ShadowVisibility(float4 position)
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

#if defined(_PS_V2ShadowFilter) || defined(_PS_V2ShadowDistanceJump) || defined(_PS_V2ShadowDistanceResolve)
// RGB = geometric receiver normal; A = filtered shadow coverage, -1 = unreliable/background.
Texture2D<float4> ShadowDistanceReceiverInput : register(t15);

// Geometry validity is separate from shadow classification. The red/core region is
// shadow amount 1, i.e. VISIBILITY 0 after filtering; penumbra pixels remain outside.
float4 GetV2CoreReceiverData(int2 pixel)
{
    float4 receiver = ShadowDistanceReceiverInput.Load(int3(pixel, 0));
    if (receiver.w >= 0.0)
        receiver.w = FilteredShadowMask.Load(int3(pixel, 0)) <= 0.0 ? 0.0 : 1.0;
    return receiver;
}

float DistanceV2Footprint(float3 receiver, float depth, float2 pixel, float2 size)
{
    float3 x = ReconstructV2Receiver(pixel + float2(1, 0), depth, size) - receiver;
    float3 y = ReconstructV2Receiver(pixel + float2(0, 1), depth, size) - receiver;
    return max(max(length(x), length(y)), 0.001);
}

// Compare geometry in world/origin-relative space, never material shading normals.
// Plane tolerance permits quantization and gentle curvature, but rejects depth layers.
bool SameV2DistanceSurface(float3 p, float3 n, float3 q, float3 m, float footprint)
{
    float3 delta = q - p;
    float agreement = clamp(dot(n, m), -1.0, 1.0);
    // A curved receiver is not one infinite plane. Normal rotation bounds its
    // expected departure from the endpoint tangent planes. Parallel depth layers
    // get no curvature allowance, so a wall/foreground jump still fails.
    float curvature = sqrt(max(0.0, 2.0 * (1.0 - agreement)));
    float tolerance = max(0.005, footprint * 0.25 + length(delta) * curvature);
    return agreement > 0.5 && abs(dot(delta, n)) <= tolerance && abs(dot(delta, m)) <= tolerance;
}

// Pack 3x7-bit normal into an exactly representable float integer. Zero is invalid.
float PackV2DistanceNormal(float3 normal)
{
    uint3 q = (uint3)round(saturate(normal * 0.5 + 0.5) * 127.0);
    return (float)(1u + q.x + (q.y << 7) + (q.z << 14));
}
float3 UnpackV2DistanceNormal(float packed)
{
    uint q = (uint)packed - 1u;
    return normalize(float3(q & 127u, (q >> 7) & 127u, (q >> 14) & 127u) / 127.0 * 2.0 - 1.0);
}
#endif

#ifdef _PS_V2ShadowFilter
struct V2ShadowFilterOutput
{
    float Visibility : SV_Target0;
    float4 EdgeSeed : SV_Target1;
};

// Store the boundary's WORLD position and geometric normal, not screen coordinates.
float4 FindV2ShadowEdgeSeed(int2 pixel)
{
    uint width, height;
    ShadowDistanceReceiverInput.GetDimensions(width, height);
    float2 size = float2(width, height);
    float4 center = GetV2CoreReceiverData(pixel);
    if (center.w < 0.0)
        return 0.0;
    float depth = LightingReceiverDepth.Load(int3(pixel, 0));
    float3 receiver = ReconstructV2Receiver(float2(pixel) + 0.5, depth, size);
    float footprint = DistanceV2Footprint(receiver, depth, float2(pixel) + 0.5, size);
    float4 nearest = 0.0;
    float best = 1e30;
    static const int2 neighbours[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    UNROLL
    for (uint i = 0; i < 4; i++)
    {
        int2 other = pixel + neighbours[i];
        if (any(other < 0) || any(other >= int2(width, height)))
            continue;
        float4 sample = GetV2CoreReceiverData(other);
        if (sample.w < 0.0 || ((center.w < 0.5) == (sample.w < 0.5)))
            continue;
        float otherDepth = LightingReceiverDepth.Load(int3(other, 0));
        float3 otherReceiver = ReconstructV2Receiver(float2(other) + 0.5, otherDepth, size);
        if (!SameV2DistanceSurface(receiver, center.xyz, otherReceiver, sample.xyz, footprint))
            continue;
        float fraction = saturate((0.5 - center.w) / (sample.w - center.w));
        float3 seed = lerp(receiver, otherReceiver, fraction);
        float d = dot(seed - receiver, seed - receiver);
        if (d < best)
        {
            best = d;
            nearest = float4(seed, PackV2DistanceNormal(normalize(lerp(center.xyz, sample.xyz, fraction))));
        }
    }
    return nearest;
}

META_PS(true, FEATURE_LEVEL_SM5)
V2ShadowFilterOutput PS_V2ShadowFilter(float4 position : SV_Position)
{
    V2ShadowFilterOutput output;
    output.Visibility = 0.0;
    output.EdgeSeed = 0.0;
    // Seeds must read COMPLETED filtered coverage in a separate draw, never its render target.
    BRANCH
    if (LightingShadowSampling.z != 0.0)
        output.EdgeSeed = FindV2ShadowEdgeSeed(int2(position.xy));
    else
        output.Visibility = FilterV2ShadowVisibility(position);
    return output;
}
#endif

#if defined(_PS_V2ShadowDistanceJump) || defined(_PS_V2ShadowDistanceResolve)
Texture2D<float4> ShadowDistanceSeedInput : register(t14);
#endif

#ifdef _PS_V2ShadowDistanceJump
// JFA+1 is only the transport mechanism. Distances and surface compatibility are world-space.
// It cannot reconstruct off-screen/hidden surfaces or provide an exact geodesic distance.
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2ShadowDistanceJump(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    ShadowDistanceSeedInput.GetDimensions(width, height);
    int2 size = int2(width, height);
    int2 pixel = int2(position.xy);
    float4 surface = ShadowDistanceReceiverInput.Load(int3(pixel, 0));
    if (surface.w < 0.0)
        return 0.0;
    float depth = LightingReceiverDepth.Load(int3(pixel, 0));
    float3 receiver = ReconstructV2Receiver(position.xy, depth, size);
    float footprint = DistanceV2Footprint(receiver, depth, position.xy, size);
    int step = (int)LightingDebugOptions.z;
    // A valid seed already owned by this receiver is never discarded by a later jump.
    // Previously even the centre candidate was re-tested through quantized-normal gates.
    float4 nearest = ShadowDistanceSeedInput.Load(int3(pixel, 0));
    float3 retainedDelta = nearest.xyz - receiver;
    float best = nearest.w != 0.0 ? dot(retainedDelta, retainedDelta) : 1e30;
    UNROLL
    for (int y = -1; y <= 1; y++)
    {
        UNROLL
        for (int x = -1; x <= 1; x++)
        {
            int2 tap = pixel + int2(x, y) * step;
            if (any(tap < 0) || any(tap >= size))
                continue;
            float4 seed = ShadowDistanceSeedInput.Load(int3(tap, 0));
            if (seed.w == 0.0)
                continue;
            float4 tapSurface = GetV2CoreReceiverData(tap);
            float tapDepth = LightingReceiverDepth.Load(int3(tap, 0));
            float3 tapReceiver = ReconstructV2Receiver(float2(tap) + 0.5, tapDepth, size);
            if (tapSurface.w < 0.0 || !SameV2DistanceSurface(receiver, surface.xyz, tapReceiver, tapSurface.xyz, footprint) ||
                !SameV2DistanceSurface(receiver, surface.xyz, seed.xyz, UnpackV2DistanceNormal(seed.w), footprint))
                continue;
            // A jump must not hop over a foreground silhouette or an empty gap.
            int2 middle = int2((uint2(pixel) + uint2(tap)) >> 1);
            float4 middleSurface = GetV2CoreReceiverData(middle);
            float middleDepth = LightingReceiverDepth.Load(int3(middle, 0));
            float3 middleReceiver = ReconstructV2Receiver(float2(middle) + 0.5, middleDepth, size);
            if (middleSurface.w < 0.0 || !SameV2DistanceSurface(receiver, surface.xyz, middleReceiver, middleSurface.xyz, footprint))
                continue;
            float3 delta = seed.xyz - receiver;
            float d = dot(delta, delta);
            bool stableTie = d == best && (seed.x < nearest.x ||
                (seed.x == nearest.x && (seed.y < nearest.y || (seed.y == nearest.y && seed.z < nearest.z))));
            if (d < best || stableTie)
            {
                best = d;
                nearest = seed;
            }
        }
    }
    return nearest;
}
#endif

#ifdef _PS_V2ShadowDistanceResolve
struct V2ShadowDistanceOutput
{
    float Distance : SV_Target0;
    float4 Debug : SV_Target1;
};
META_PS(true, FEATURE_LEVEL_SM5)
V2ShadowDistanceOutput PS_V2ShadowDistanceResolve(float4 position : SV_Position)
{
    int3 pixel = int3(int2(position.xy), 0);
    V2ShadowDistanceOutput output;
    output.Distance = 0.0;
    output.Debug = float4(0, 0, 0, 1);
    if (LightingReceiverDepth.Load(pixel) == DEPTH_RANGE_MAX)
        return output;
    float4 surface = GetV2CoreReceiverData(pixel.xy);
    if (surface.w < 0.0)
    {
        // Insufficient geometric depth support; never substitute a noisy material normal.
        output.Debug = float4(0.12, 0, 0.12, 1);
        return output;
    }
    uint width, height;
    ShadowDistanceSeedInput.GetDimensions(width, height);
    float3 receiver = ReconstructV2Receiver(position.xy, LightingReceiverDepth.Load(pixel), float2(width, height));
    float4 seed = ShadowDistanceSeedInput.Load(pixel);
    float magnitude = length(seed.xyz - receiver);
    bool inside = surface.w < 0.5;
    uint side = (uint)LightingShadowSampling.y;
    if ((side == 1 && !inside) || (side == 2 && inside))
    {
        // Excluded halves do not contribute a distance ramp. Keep their sign for cel classification.
        output.Distance = inside ? -1e20 : 1e20;
        return output;
    }
    if (seed.w == 0.0)
    {
        // Unavailable is not a very large valid distance. Mark it distinctly and never fill from it.
        output.Distance = inside ? -1e30 : 1e30;
        output.Debug = float4(0.5, 0.0, 0.5, 1);
        return output;
    }
    output.Distance = inside ? -magnitude : magnitude;
    float ramp = saturate(magnitude / max(LightingDebugOptions.w, 0.001));
    float3 tint = inside ? float3(1.0, 0.28, 0.04) : float3(0.05, 0.5, 1.0);
    output.Debug = float4(lerp(float3(1, 1, 1), tint * 0.25, ramp), 1);
    return output;
}
#endif

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowDistanceReceivers) || defined(_PS_V2ShadowMask)

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
    float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
    // Debug comparison: fixed disk/rings by default; random rotation/radii only when requested.
    bool jitterEnabled = LightingShadowSampling.x != 0.0;
    float noise = 0.0;
    BRANCH
    if (jitterEnabled)
    {
        uint receiverWidth, receiverHeight;
        LightingReceiverDepth.GetDimensions(receiverWidth, receiverHeight);
        float receiverDepth = LightingReceiverDepth.Load(int3(int2(pixel), 0));
        float3 receiver = ReconstructV2Receiver(pixel, receiverDepth, float2(receiverWidth, receiverHeight));
        float2 worldAnchor = float2(receiver.x + receiver.y * 0.754877666, receiver.z + receiver.y * 0.569840296);
        float phase = dot(worldAnchor, float2(0.06711056, 0.00583715)) + LightingShadowSampling.w;
        noise = frac(52.9829189 * frac(phase));
    }
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
        float radius = jitterEnabled ? 0.84 + 0.16 * frac(noise + i * 0.618034) : 1.0;
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
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * (jitterEnabled ? lerp(0.18, 0.55, jitter) : 0.35), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
            total += SampleV2ReceiverPlane(receiverTexel + offsets[tap] * (jitterEnabled ? lerp(0.58, 0.81, 1.0 - jitter) : 0.7), receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
        }
        sampleCount = 25.0;
    }
    return total / sampleCount;
}

#endif

#ifdef _PS_V2ShadowDistanceReceivers
// Camera depth supplies geometric receivers. The seed pass later reads completed shadow coverage.
// Geometric normals only: albedo, normal maps, emissive and material noise are not read.
META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_V2ShadowDistanceReceivers(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float depth = LightingReceiverDepth.Load(int3(pixel, 0));
    if (depth == DEPTH_RANGE_MAX)
        return float4(0, 0, 0, -1);
    uint width, height;
    LightingReceiverDepth.GetDimensions(width, height);
    float2 size = float2(width, height);
    float4 receiverH = ReconstructV2ReceiverH(position.xy, depth, size);
    float3 receiver = receiverH.xyz / receiverH.w;
    float2 gx = V2DepthAxisGradient(LightingReceiverDepth, depth, pixel, int2(1, 0), int2(size));
    float2 gy = V2DepthAxisGradient(LightingReceiverDepth, depth, pixel, int2(0, 1), int2(size));
    if (gx.y == 0.0 || gy.y == 0.0)
        return float4(0, 0, 0, -1);
    float4 dx = PROJECT_POINT(float4(2.0 / size.x, 0, gx.x, 0), LightingInverseViewProjection);
    float4 dy = PROJECT_POINT(float4(0, -2.0 / size.y, gy.x, 0), LightingInverseViewProjection);
    float3 rawNormal = cross(dx.xyz - receiver * dx.w, dy.xyz - receiver * dy.w);
    float squared = dot(rawNormal, rawNormal);
    if (squared <= 1e-20)
        return float4(0, 0, 0, -1);
    float3 normal = rawNormal * rsqrt(squared);
    float3 toCamera = LightingIsOrthographic ? -LightingCameraForward : LightingCameraPosition - receiver;
    normal *= dot(normal, toCamera) < 0 ? -1.0 : 1.0;
    // Classification is deferred until jitter/filtering has completed.
    return float4(normal, 1.0);
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
Texture2D<float> CelShadowDistance : register(t14);
Texture2D<float4> CelShadowReceivers : register(t15);

float3 V2CelPalette(float value)
{
    float bands = clamp(round(LightingCelParameters.x), 2.0, 16.0);
    float level = min(floor(saturate(value) * bands), bands - 1.0) / (bands - 1.0);
    return lerp(float3(0.35, 0.7, 1.0), float3(0.05, 0.2, 0.9), level);
}

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
                float bias = V2LightingReceiverBias(depthPerTexel, tile.zw, projection, uncertainty);
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
    bool whiteLighting = LightingDebugOptions.x == 1.0;
    bool celLighting = LightingDebugOptions.x == 2.0;
    if (shadingModel == SHADING_MODEL_UNLIT && !whiteLighting && !celLighting)
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
    float3 result = emissive + GetDiffuseColor(surface) * surface.AO * max(LightingAmbientIntensity, 0.0);
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
            visibility = SampleV2Shadow(surface.WorldPos, planeNormal, uncertainty, position.xy);
        }
        BRANCH
        if (celLighting)
        {
            float value = nDotL * saturate(visibility);
            // Keep the outer filtered gradient; apply SDF fill only inside its opaque core.
            BRANCH
            if (LightingCelParameters.y != 0.0 && CelShadowReceivers.Load(pixel).w >= 0.0 &&
                abs(CelShadowDistance.Load(pixel)) < 1e29)
            {
                float distance = CelShadowDistance.Load(pixel);
                float range = max(LightingCelParameters.z, 0.001);
                uint side = (uint)LightingShadowSampling.y;
                float filteredVisibility = saturate(visibility);
                float combinedVisibility = filteredVisibility;
                // Keep the jitter curve's shape while moving its shadow-end height to the SDF join.
                // Core fill then DECREASES with inward distance, rather than brightening the deep core.
                if (side != 2)
                {
                    float joinVisibility = saturate(LightingCelParameters.w);
                    combinedVisibility = lerp(joinVisibility, 1.0, filteredVisibility);
                    if (filteredVisibility <= 0.0 && distance < 0.0)
                    {
                        float inwardWeight = smoothstep(0.0, range, -distance);
                        combinedVisibility = joinVisibility * (1.0 - inwardWeight);
                    }
                }
                value = nDotL * combinedVisibility;
            }
            return float4(V2CelPalette(value), 1);
        }
        float3 toEye = LightingIsOrthographic ? -LightingCameraForward : normalize(LightingCameraPosition - surface.WorldPos);
        LightSample response = StandardShading(surface, 1.0, light, toEye, surface.Normal);
        result += (whiteLighting ? response.Diffuse : response.Diffuse + response.Specular) * visibility *
            nDotL * LightingRadiance.rgb * max(LightingRadiance.a, 0.0);
    }
    return float4(celLighting ? V2CelPalette(0.0) : result, 1);
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
    bool show = box.UseProxy != 0 && (CasterDebugFlags & (selected ? 1u : 2u)) != 0;
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

// Derive lower-resolution light depth without rerendering any geometry.
// Nearest-blocker reduction is conservative: ordinary and reversed depth use
// opposite extrema. Depth values must never be averaged across silhouettes.
#ifdef _PS_V2ShadowDepthReduce
Texture2D<float> ReductionSourceDepth : register(t7);
META_PS(true, FEATURE_LEVEL_SM5)
float PS_V2ShadowDepthReduce(float4 position : SV_Position) : SV_Depth
{
    uint2 ReductionTargetSize = uint2(OcclusionWidth,OcclusionHeight);
    uint2 sourceSize;
    ReductionSourceDepth.GetDimensions(sourceSize.x, sourceSize.y);
    uint2 pixel = uint2(position.xy);
    uint2 first, last;
    // Uniform branch: avoid divisions for the normal 2:1 power-of-two chain.
    if (all(sourceSize == ReductionTargetSize * 2))
    {
        first = pixel * 2;
        last = first + 2;
    }
    else
    {
        // Non-power-of-two footprints may overlap a third source texel.
        first = (pixel * sourceSize) / ReductionTargetSize;
        last = min(((pixel + 1) * sourceSize + ReductionTargetSize - 1) / ReductionTargetSize, sourceSize);
    }
    float result = DEPTH_RANGE_MAX;
    LOOP
    for (uint y = first.y; y < last.y; y++)
    {
        LOOP
        for (uint x = first.x; x < last.x; x++)
        {
            float sampleDepth = ReductionSourceDepth.Load(int3(x,y,0));
#if REVERSE_Z
            result = max(result,sampleDepth);
#else
            result = min(result,sampleDepth);
#endif
        }
    }
    return result;
}
#endif
