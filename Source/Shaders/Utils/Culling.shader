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

static const float V2ShadowDepthBias = 0.0005;

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
uint ShadowSelectionPadding;
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

bool IsAlreadyShadowed(float3 receiver)
{
    if (!HasLightDepth)
        return false;
    float4 clip = PROJECT_POINT(float4(receiver, 1), LightViewProjection);
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv <= 0) || any(uv >= 1) || ndc.z <= 0 || ndc.z >= 1)
        return false;

    uint width, height;
    LightDepth.GetDimensions(width, height);
    int2 first = int2(floor(uv * float2(width, height) - 0.5));
    if (any(first < 0) || any(first + 1 >= int2(width, height)))
        return false;
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        float blocker = LightDepth.Load(int3(first + int2(tap & 1, tap >> 1), 0));
        if (!DEPTH_CMP(blocker, ndc.z) || abs(blocker - ndc.z) <= V2ShadowDepthBias)
            return false;
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

    ShadowCasterBounds box = ShadowCasterBoxes[caster];
    if (!box.UseProxy)
    {
        if (thread == 0)
            CasterSelection[caster] = 2; // Cheap geometry goes directly to shadow drawing.
        return;
    }

    // Bound the receiver scan by the screen rectangle of the swept box.
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
    for (uint index = thread; index < extent.x * extent.y; index += 64)
    {
        uint2 pixel = first + uint2(index % extent.x, index / extent.x);
        float depth = ReceiverDepth.Load(int3(pixel, 0));
        if (depth == DEPTH_RANGE_MAX)
            continue;
        float2 uv = (float2(pixel) + 0.5) / size;
        float2 ndc = uv * float2(2, -2) + float2(-1, 1);
        float4 receiver = PROJECT_POINT(float4(ndc, depth, 1), InverseViewProjection);
        float3 receiverPosition = receiver.xyz / receiver.w;
        if (!IntersectsCaster(receiverPosition, box.Minimum, box.Maximum))
            continue;
        if (HasReceiverNormals)
        {
            float3 normal = ShadowReceiverNormals.Load(int3(pixel, 0)).xyz * 2.0 - 1.0;
            if (dot(normal, DirectionToLight) <= 0.01)
                continue;
        }
        if (IsAlreadyShadowed(receiverPosition))
            continue;

        InterlockedOr(ProxyHit, 1);
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

#ifdef _PS_V2DirectionalLighting

#include "./Flax/Lighting/Lighting.hlsl"

META_CB_BEGIN(3, V2DirectionalLightingData)
float4x4 LightingInverseViewProjection;
float4x4 LightingLightViewProjection;
float3 LightingDirectionToLight;
float LightingAmbientIntensity;
float3 LightingCameraPosition;
uint LightingIsOrthographic;
float3 LightingCameraForward;
float LightingPadding;
float4 LightingRadiance;
META_CB_END

Texture2D<float> LightingReceiverDepth : register(t2);
Texture2D<float> LightingLightDepth : register(t7);
Texture2D<float4> LightingNormals : register(t8);
Texture2D<float4> LightingBaseColor : register(t9);
Texture2D<float4> LightingMaterial : register(t10);
Texture2D<float4> LightingEmissive : register(t11);

float SampleV2Shadow(float3 receiver)
{
    float4 clip = PROJECT_POINT(float4(receiver, 1), LightingLightViewProjection);
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0) || any(uv >= 1) || ndc.z <= 0 || ndc.z >= 1)
        return 1.0;
    // Match the bias and four-texel footprint used by caster selection.
#if REVERSE_Z
    const float receiverDepth = ndc.z + V2ShadowDepthBias;
    if (receiverDepth >= 1.0)
        return 1.0;
#else
    const float receiverDepth = ndc.z - V2ShadowDepthBias;
    if (receiverDepth <= 0.0)
        return 1.0;
#endif
    return LightingLightDepth.SampleCmpLevelZero(ShadowSamplerLinear, uv, receiverDepth);
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
    float2 ndc = position.xy / float2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 receiver = PROJECT_POINT(float4(ndc, depth, 1), LightingInverseViewProjection);
    surface.WorldPos = receiver.xyz / receiver.w;

    float3 light = LightingDirectionToLight;
    float nDotL = saturate(dot(surface.Normal, light));
    float3 result = emissive + GetDiffuseColor(surface) * surface.AO * max(LightingAmbientIntensity, 0.0);
    if (nDotL > 0)
    {
        float3 toEye = LightingIsOrthographic ? -LightingCameraForward : normalize(LightingCameraPosition - surface.WorldPos);
        LightSample response = StandardShading(surface, 1.0, light, toEye, surface.Normal);
        float visibility = SampleV2Shadow(surface.WorldPos);
        result += (response.Diffuse + response.Specular) * nDotL * visibility * LightingRadiance.rgb * max(LightingRadiance.a, 0.0);
    }
    return float4(result, 1);
}

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
    ShadowIndirectOutput[offset + 1] = ShadowSelectionFlags[input.x] != 0 ? 1 : 0;
    ShadowIndirectOutput[offset + 2] = 0;
    ShadowIndirectOutput[offset + 3] = 0;
    ShadowIndirectOutput[offset + 4] = 0;
}

#endif
