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

#if defined(_CS_ShadowCasterSelection) || defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask)
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

float2 V2DepthAxisGradient(Texture2D<float> depthTexture, float depth, int2 pixel, int2 axis, int2 size)
{
    // Fit device depth, which is affine over a rasterized triangle. A wider
    // stencil reduces D24 quantization, while extrapolation rejects silhouettes.
    // Edge selection follows https://atyuwen.github.io/posts/normal-reconstruction/
    const int radius = 4;
    int2 l1 = pixel - axis * radius;
    int2 l2 = pixel - axis * (2 * radius);
    int2 r1 = pixel + axis * radius;
    int2 r2 = pixel + axis * (2 * radius);
    float a = depthTexture.Load(int3(clamp(l1, 0, size - 1), 0));
    float b = depthTexture.Load(int3(clamp(l2, 0, size - 1), 0));
    float c = depthTexture.Load(int3(clamp(r1, 0, size - 1), 0));
    float d = depthTexture.Load(int3(clamp(r2, 0, size - 1), 0));
    const float tolerance = 4.0 / 16777215.0;
    bool left = all(l2 >= 0) && a != DEPTH_RANGE_MAX && b != DEPTH_RANGE_MAX &&
        abs((a - depth) * 2.0 - (b - depth)) <= tolerance;
    bool right = all(r2 < size) && c != DEPTH_RANGE_MAX && d != DEPTH_RANGE_MAX &&
        abs((c - depth) * 2.0 - (d - depth)) <= tolerance;
    if (left && right)
        return float2(((c - a) + 2.0 * (d - b)) / (10.0 * radius), 1);
    if (left)
        return float2(((depth - a) + 2.0 * (depth - b)) / (5.0 * radius), 1);
    if (right)
        return float2(((c - depth) + 2.0 * (d - depth)) / (5.0 * radius), 1);
    return 0;
}

float3 V2ReceiverPlaneNormal(Texture2D<float> depthTexture, float3 receiver, float depth, int2 pixel, float2 size,
    float4x4 inverseViewProjection, float3 shadingNormal)
{
    float2 xGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(1, 0), int2(size));
    float2 yGradient = V2DepthAxisGradient(depthTexture, depth, pixel, int2(0, 1), int2(size));
    if (xGradient.y == 0 || yGradient.y == 0)
        return shadingNormal;
    float2 gradient = float2(xGradient.x, yGradient.x);

    // Differentiate homogeneous unprojection without subtracting large world positions.
    float4 dx = PROJECT_POINT(float4(2.0 / size.x, 0, gradient.x, 0), inverseViewProjection);
    float4 dy = PROJECT_POINT(float4(0, -2.0 / size.y, gradient.y, 0), inverseViewProjection);
    float3 planeNormal = cross(dx.xyz - receiver * dx.w, dy.xyz - receiver * dy.w);
    float lengthSquared = dot(planeNormal, planeNormal);
    float agreement = dot(planeNormal, shadingNormal);
    // At a silhouette the quad may straddle two surfaces.
    if (lengthSquared <= 0 || agreement * agreement < 0.36 * lengthSquared)
        return shadingNormal;
    return planeNormal * ((agreement < 0 ? -1.0 : 1.0) * rsqrt(lengthSquared));
}

float3 V2OffsetShadowReceiver(float3 receiver, float3 normal, float3 directionToLight, float normalBias)
{
    float cosine = saturate(dot(normal, directionToLight));
    float offset = normalBias * sqrt(saturate(1.0 - cosine * cosine));
    return receiver + normal * offset;
}

float3 V2CameraDepthUncertainty(float3 receiver, float homogeneousW, float4x4 inverseViewProjection)
{
    // The opaque GBuffer depth is D24. Allow two steps for rasterization and storage error.
    const float cameraDepthError = 2.0 / 16777215.0;
    float4 depthDerivative = PROJECT_POINT(float4(0, 0, 1, 0), inverseViewProjection);
    return (depthDerivative.xyz - receiver * depthDerivative.w) * (cameraDepthError / homogeneousW);
}

float V2ReceiverDepthBias(float precisionBias, float2 depthPerTexel, float2 size,
    float4x4 lightProjection, float3 receiverUncertainty)
{
    // Carry camera-depth error through both light depth and the receiver-plane UV correction.
    float4 lightError = PROJECT_POINT(float4(receiverUncertainty, 0), lightProjection);
    float2 texelError = lightError.xy * size * float2(0.5, -0.5);
    float cameraBias = abs(lightError.z - dot(depthPerTexel, texelError));

    float slope = dot(abs(depthPerTexel), float2(1, 1));
    return precisionBias + cameraBias + 0.125 * min(slope, rcp(min(size.x, size.y)));
}
#endif

#ifdef _CS_ShadowCasterSelection

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
float4 SelectionReceiverRange; // Inner and outer camera radii. Zero end selects the legacy path.
META_CB_END

struct ShadowCasterBounds
{
    float3 Minimum;
    uint UseProxy;
    float3 Maximum;
    uint DebugCategory;
};

Texture2D<float> ReceiverDepth : register(t2);
Texture2D<float> LightDepth : register(t7);
Texture2D<float4> ShadowReceiverNormals : register(t8);
StructuredBuffer<ShadowCasterBounds> ShadowCasterBoxes : register(t3);
RWStructuredBuffer<uint> CasterSelection : register(u1);
RWTexture2D<uint> ReceiverHits : register(u2);
groupshared uint ProxyHit;

bool IsAlreadyShadowed(float3 receiver, float3 normal, uint tier, uint2 pixel, float cameraDepth, float receiverW)
{
    if (!HasLightDepth)
        return false;
    float4x4 projection = tier == 0 ? LightViewProjection : SelectionNearLightViewProjection;
    if (HasReceiverNormals)
        normal = V2ReceiverPlaneNormal(ReceiverDepth, receiver, cameraDepth, int2(pixel), float2(Width, Height),
            InverseViewProjection, normal);
    float3 biasedReceiver = V2OffsetShadowReceiver(receiver, normal, DirectionToLight, SelectionCameraPosition.w);
    float3 coord = ProjectV2Shadow(biasedReceiver, projection);
    if (!IsV2ShadowCoordValid(coord))
        return false;

    float4 tile = SelectionShadowTiles[tier];
    float2 size = tile.zw;
    int2 origin = int2(tile.xy);
    float2 receiverTexel = coord.xy * size - 0.5;
    int2 first = int2(floor(receiverTexel));
    if (any(first < 0) || any(first + 1 >= int2(size)))
        return false;
    int margin = (int)ceil(ShadowSelectionFilterRadius);
    float2 depthPerTexel = V2ShadowDepthPerTexel(normal, projection, size);
    float3 uncertainty = V2CameraDepthUncertainty(receiver, receiverW, InverseViewProjection);
    float bias = V2ReceiverDepthBias(SelectionShadowSplit.w, depthPerTexel, size, projection, uncertainty);
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        int2 texel = first + int2(tap & 1, tap >> 1);
        float blocker = LightDepth.Load(int3(origin + texel, 0));
        float depth = coord.z + dot(depthPerTexel, texel - receiverTexel);
        if (blocker == DEPTH_RANGE_MAX || DEPTH_DIFF(depth, blocker) <= bias)
            return false;
    }

    // Adaptive sampling can see outside the central PCF footprint.
    if (margin > 0)
    {
        if (any(first - margin < 0) || any(first + 1 + margin >= int2(size)))
            return false;
        LOOP
        for (int y = -margin; y <= margin + 1; y++)
        {
            LOOP
            for (int x = -margin; x <= margin + 1; x++)
            {
                int2 texel = first + int2(x, y);
                float blocker = LightDepth.Load(int3(origin + texel, 0));
                float depth = coord.z + dot(depthPerTexel, texel - receiverTexel);
                if (blocker == DEPTH_RANGE_MAX || DEPTH_DIFF(depth, blocker) <= bias)
                    return false;
            }
        }
    }
    return true;
}

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
            float a = (boxMin[axis] - receiver[axis]) / direction;
            float b = (boxMax[axis] - receiver[axis]) / direction;
            enter = max(enter, min(a, b));
            leave = min(leave, max(a, b));
        }
    }
    return leave >= enter;
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_ShadowCasterSelection(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint caster = group.y * GroupsPerRow + group.x;
    if (caster >= CasterCount)
        return;
    bool hasNearTier = SelectionShadowSplit.z != 0;

    ShadowCasterBounds box = ShadowCasterBoxes[caster];
    if (!box.UseProxy)
    {
        if (thread == 0)
            CasterSelection[caster] = hasNearTier ? 3 : 1; // Cheap geometry goes directly to shadow drawing.
        return;
    }

    // Bound the receiver scan by the screen rectangle of the swept box.
    float3 offset = -DirectionToLight * ProjectionDistance;
    float3 boundsMin = box.Minimum + min(offset, 0);
    float3 boundsMax = box.Maximum + max(offset, 0);
    if (SelectionReceiverRange.y > 0.0)
    {
        float3 nearOffset = max(max(boundsMin - SelectionCameraPosition.xyz, SelectionCameraPosition.xyz - boundsMax), 0.0);
        float3 farOffset = max(abs(boundsMin - SelectionCameraPosition.xyz), abs(boundsMax - SelectionCameraPosition.xyz));
        if (dot(farOffset, farOffset) < SelectionReceiverRange.x * SelectionReceiverRange.x ||
            dot(nearOffset, nearOffset) >= SelectionReceiverRange.y * SelectionReceiverRange.y)
        {
            if (thread == 0)
                CasterSelection[caster] = 0;
            return;
        }
    }
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
    // Use half of the group for each tier so one group can publish the complete mask.
    uint tier = hasNearTier ? thread / 32 : 0;
    uint tierBit = 1u << tier;
    uint start = hasNearTier ? thread % 32 : thread;
    uint stride = hasNearTier ? 32 : 64;
    for (uint index = start; index < extent.x * extent.y; index += stride)
    {
        uint2 pixel = first + uint2(index % extent.x, index / extent.x);
        float depth = ReceiverDepth.Load(int3(pixel, 0));
        if (depth == DEPTH_RANGE_MAX)
            continue;
        float2 uv = (float2(pixel) + 0.5) / size;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 receiver = PROJECT_POINT(float4(ndc, depth, 1), InverseViewProjection);
        float3 receiverPosition = receiver.xyz / receiver.w;
        if (hasNearTier)
        {
            float viewDepth = dot(receiverPosition - SelectionCameraPosition.xyz, SelectionCameraForward.xyz);
            float3 nearCoord = ProjectV2Shadow(receiverPosition, SelectionNearLightViewProjection);
            float nearWeight = V2ShadowTierWeight(nearCoord, viewDepth, SelectionShadowSplit.xy,
                SelectionShadowTiles[1].zw, ShadowSelectionFilterRadius);
            if ((tier == 1 && nearWeight <= 0.0) || (tier == 0 && nearWeight >= 1.0))
                continue;
        }
        if (SelectionReceiverRange.y > 0.0)
        {
            float3 delta = receiverPosition - SelectionCameraPosition.xyz;
            float distanceSquared = dot(delta, delta);
            if (distanceSquared < SelectionReceiverRange.x * SelectionReceiverRange.x ||
                distanceSquared >= SelectionReceiverRange.y * SelectionReceiverRange.y)
                continue;
        }
        if (!IntersectsCaster(receiverPosition, box.Minimum, box.Maximum))
            continue;
        float3 normal = 0;
        if (HasReceiverNormals)
        {
            normal = normalize(ShadowReceiverNormals.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
            if (dot(normal, DirectionToLight) <= 0.01)
                continue;
        }
        if (IsAlreadyShadowed(receiverPosition, normal, tier, pixel, depth, receiver.w))
            continue;

        InterlockedOr(ProxyHit, tierBit);
        if (WriteDebugMask)
            InterlockedOr(ReceiverHits[pixel], box.DebugCategory);
        else
            break;
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0)
        CasterSelection[caster] = ProxyHit;
}

#endif

#ifdef _PS_ShadowReceiverHits

Texture2D<uint> ShadowReceiverHitMask : register(t4);

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_ShadowReceiverHits(float4 position : SV_Position) : SV_Target
{
    uint hit = ShadowReceiverHitMask.Load(int3(int2(position.xy), 0));
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

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2ShadowMask) || defined(_PS_V2CascadeShadowMask) || defined(_PS_V2ShadowFilter)

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
            farMask = SampleV2MaskTile(receiver, normal, uncertainty, coord, 0);
    }
    if (nearSample.w <= 0.0)
        return farMask;
    return lerp(farMask, SampleV2MaskTile(receiver, normal, uncertainty, nearSample.xyz, 1), nearSample.w);
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

#if defined(_PS_V2DirectionalLighting) || defined(_PS_V2CascadeShadowMask)

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
