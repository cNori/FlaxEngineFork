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

#if defined(_CS_V2ProjectCameraReceivers) || defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)
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
    // Depth extrapolation above validates the plane. Use the shading normal
    // only to orient it; normal-map detail must not replace a valid geometric slope.
    if (lengthSquared <= 1e-20)
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
    float radius, float2 rotation)
{
    float total = 0;
    uint blockers = 0;
    LOOP
    for (uint i = 0; i < 32; i++)
    {
        float2 texel = clamp(floor(receiverTexel + V2RotateBlockerTap(V2BlockerDisk[i], rotation) * radius + 0.5), 0, tile.zw - 1);
        float blocker = LightingLightDepth.Load(int3(int2(tile.xy + texel), 0));
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
    float2 invAtlasSize, float4x4 projection, float2 rotation, float maximumRadius, float angularTangent, float suppliedGap)
{
    maximumRadius = max(maximumRadius, 0.0);
    float gap = suppliedGap >= -1.0 ? suppliedGap : V2AverageBlockerGap(receiverTexel, depth, gradient, bias, tile, max(1.0, maximumRadius), rotation);
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
        shadow += SampleV2ReceiverPlane(tap, receiverTexel, tile, invAtlasSize, depth, gradient, bias);
    }
    return shadow / 32.0;
}

#endif

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

// Also use the adaptive filter when a legacy view needs an explicit mask.
float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier);

float2 SampleV2MaskTileSettings(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier, float angularTangent)
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

    float penumbra = (separation - bias) * LightingShadowFilter.x * angularTangent;
    return float2(coverage, clamp(penumbra * V2PixelsPerWorldUnit(receiver), 0.5, 4.0));
}

float2 SampleV2MaskTile(float3 receiver, float3 normal, float3 uncertainty, float3 coord, uint tier)
{
    return SampleV2MaskTileSettings(receiver, normal, uncertainty, coord, tier, LightingShadowFilter.z);
}
// Pixel mode: nearest texel, one raw comparison. No bilinear coverage or blocker search.
float V2RawShadow(float3 coord, float3 normal, float3 uncertainty, uint tier)
{
    float4 tile = LightingShadowTiles[tier];
    float4x4 projection = LightingLightViewProjection;
    if (tier != 0) projection = LightingNearLightViewProjection;
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float2 texel = clamp(floor(receiverTexel + 0.5), 0, tile.zw - 1);
    float2 gradient = V2ShadowDepthPerTexel(normal, projection, tile.zw);
    float bias = V2LightingReceiverBias(gradient, tile.zw, projection, uncertainty);
    float blocker = LightingLightDepth.Load(int3(int2(tile.xy + texel), 0));
    // DEPTH_DIFF does not parenthesize its arguments in reversed depth.
    float receiverDepth = coord.z + dot(gradient, texel - receiverTexel);
    float gap = DEPTH_DIFF(receiverDepth, blocker);
    return blocker == DEPTH_RANGE_MAX || gap <= bias ? 1.0 : 0.0;
}

float2 V2ResolveBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier);


#endif



#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

#include "./Flax/Math/Noise.hlsl"

float SampleV2ShadowTileSettings(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier, float radiusSetting, bool pcss, float angularTangent, float suppliedGap)
{
    float4 tile = LightingShadowTiles[tier];
    uint width, height;
    LightingLightDepth.GetDimensions(width, height);
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
        return V2PCSSVisibility(receiverTexel, depth, depthPerTexel, bias, tile, invAtlasSize, projection, float2(cosine, sine), radiusSetting, angularTangent, suppliedGap);
    // Large kernels use an irregular disk instead of concentric eight-spoke rings.
    // Keep every sample on the same receiver plane and preserve the requested radius.
    if (radiusSetting > 1.0)
    {
        float visibility = 0.0;
        LOOP
        for (uint tap = 0; tap < 32; tap++)
        {
            float2 offset = V2RotateBlockerTap(V2BlockerDisk[tap], float2(cosine, sine)) * radiusSetting;
            visibility += SampleV2ReceiverPlane(receiverTexel + offset, receiverTexel, tile, invAtlasSize, depth, depthPerTexel, bias);
        }
        return visibility / 32.0;
    }
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
        offsets[i] = float2(cosine * direction.x - sine * direction.y, sine * direction.x + cosine * direction.y) * radiusSetting;
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

float SampleV2ShadowTile(float2 uv, float depth, float3 normal, float3 uncertainty, float2 pixel, uint tier)
{
    return SampleV2ShadowTileSettings(uv, depth, normal, uncertainty, pixel, tier, LightingShadowFilter.w, LightingBlockerParameters.x != 0.0, LightingShadowFilter.z, -2.0);
}

float2 V2EvaluateBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier, float4 settings, float gap)
{
    uint mode = (uint)settings.x;
    float4 tile = LightingShadowTiles[tier];
    float radius = min(32.0, settings.y * tile.z / max(LightingBlockerParameters.z, 1.0));
    if ((mode == 2 || mode == 4) && radius > 0)
        return float2(SampleV2ShadowTileSettings(coord.xy, coord.z, normal, uncertainty, pixel, tier, radius, mode == 4, settings.z, gap), 0.5);
    float2 mask = SampleV2MaskTileSettings(receiver, normal, uncertainty, coord, tier, settings.z);
    // Hard is unblurred; ScreenSpace uses the selected fixed screen-pixel support.
    mask.y = mode == 3 ? clamp(settings.y, 0.5, 4.0) : 0.5;
    return mask;
}

// Returns normalized blocker gap and its world-space length along the light ray.
// Both normal rendering and debug use this lookup. Camera position is not involved.
// A single-layer shadow map only exposes the first surface seen by the light.
float2 V2BlurBlockerDistance(float3 coord, float3 normal, float3 uncertainty, uint tier)
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
    float gap = V2AverageBlockerGap(coord.xy * tile.zw - 0.5, coord.z, gradient, bias, tile, radius, float2(1, 0));
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

float4 V2BlurBandDebug(float3 coord, float3 normal, float3 uncertainty)
{
    float2 blocker = V2BlurBlockerDistance(coord, normal, uncertainty, 0);
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

float2 V2ResolveBlur(float3 receiver, float3 coord, float3 normal, float3 uncertainty, float2 pixel, uint tier)
{
    uint count = (uint)LightingBlockerParameters.y;
    float2 blocker = V2BlurBlockerDistance(coord, normal, uncertainty, tier);
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
        float2 a = V2EvaluateBlur(receiver, coord, normal, uncertainty, pixel, tier, aSettings, blocker.x);
        if (i + 1 >= count || distance <= band.Range.y)
            return a;
        float weight = 1.0 - smoothstep(band.Range.y, band.Range.z, distance);
        V2BlurBand next = V2BlurBands[bIndex];
        if (aIndex == bIndex || all(next.Settings == aSettings)) return a;
        float2 b = V2EvaluateBlur(receiver, coord, normal, uncertainty, pixel, tier, next.Settings, blocker.x);
        return lerp(b, a, weight);
    }
    return float2(1, 0.5);
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
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverH.w, LightingInverseViewProjection);
    if (LightingDebugOptions.x == 5.0)
    {
        float3 color = V2BlurBandDebug(coord, normal, uncertainty).rgb;
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
        float gap = V2AverageBlockerGap(receiverTexel, coord.z, gradient, bias, tile, max(1.0, LightingShadowFilter.w), float2(1, 0));
        if (gap < 0)
            return float4(0.02, 0.1, 0.3, weight);
        float value = saturate(gap * LightingShadowFilter.x / max(LightingDebugOptions.w, 0.01));
        return float4(value.xxx, weight);
    }
    float2 mask;
    BRANCH
    if (LightingBlockerParameters.w != 0)
        mask = float2(V2RawShadow(coord, normal, uncertainty, 0), 0.5);
    else if (LightingBlockerParameters.y > 0)
        mask = V2ResolveBlur(biasedReceiver, coord, normal, uncertainty, position.xy, 0);
    else if (LightingShadowFilter.w > 0)
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
            if (LightingBlockerParameters.w != 0)
                shadow = V2RawShadow(coord, normal, uncertainty, tier);
            else if (LightingBlockerParameters.y > 0)
                shadow = V2ResolveBlur(receiver, coord, normal, uncertainty, pixel, tier).x;
            else if (LightingShadowFilter.w > 0)
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
float V2BlockerDistanceCm(float3 receiver, float3 normal, float3 uncertainty)
{
    float3 coord = ProjectV2Shadow(receiver, LightingLightViewProjection);
    if (!IsV2ShadowCoordValid(coord))
        return -2.0;
    float4 tile = LightingShadowTiles[0];
    float2 receiverTexel = coord.xy * tile.zw - 0.5;
    float2 gradient = V2ShadowDepthPerTexel(normal, LightingLightViewProjection, tile.zw);
    float bias = V2LightingReceiverBias(gradient, tile.zw, LightingLightViewProjection, uncertainty);
    float gap = V2AverageBlockerGap(receiverTexel, coord.z, gradient, bias, tile, max(1.0, LightingShadowFilter.w), float2(1, 0));
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
        float distanceCm = V2BlockerDistanceCm(receiver, normal, uncertainty);
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
        return V2BlurBandDebug(coord, normal, uncertainty);
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
