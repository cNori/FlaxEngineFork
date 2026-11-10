// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/Math/Math.hlsl"

#include "./Flax/Utils/ShadowProjection.hlsl"
#include "./Flax/Utils/ShadowReceiver.hlsl"

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
uint HasCameraVisibility;
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
float4 SelectionLightPosition; // XYZ origin-relative, W: perspective local light.
META_CB_END

struct ShadowCasterBounds
{
    float3 Minimum;
    uint UseProxy;
    float3 Maximum;
    uint CameraIndex;
};

#endif

#if defined(_CS_V2LightCasterVisibility) || defined(_CS_V2ReceiverMaskCasters)
StructuredBuffer<uint> CameraCasterVisibility : register(t29);

bool IsV2CameraVisibleCaster(ShadowCasterBounds box)
{
    // Keep full geometry for a camera-visible box even if the low-resolution
    // projected shadow has no receiver sample this frame. This prevents popping.
    if (box.CameraIndex == 0xffffffffu) return false;
    if (HasCameraVisibility == 0) return false;
    return CameraCasterVisibility[box.CameraIndex] != 0;
}
#endif

// Camera-visible receiver coverage, expressed in the SHARED LIGHT projection.
// This pass visits the camera depth once, independently of casters and cascades.
// Light-hidden receivers MUST remain present: those are possible shadow receivers.
#ifdef _CS_V2ProjectCameraReceivers
Texture2D<float> CameraReceiverDepth : register(t2);
Texture2D<float> CameraReceiverBlockers : register(t7);
Texture2D<float4> CameraReceiverNormals : register(t8);
RWTexture2D<uint> CameraReceiverCoverageOut : register(u0);
RWTexture2D<uint> CameraReceiverNearOut : register(u1);
RWTexture2D<uint> CameraReceiverFarOut : register(u2);
// Recover the actual camera-visible geometric surface from depth. The early
// receiver prepass has no material normals; treating all such pixels as unknown
// let the hidden/front side of a wall validate shadows on its visible/back side.
#include "./Flax/Utils/ReceiverSurface.hlsl"
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
    bool uncertainPlane = true;
    float3 receiverNormal = 0;
    bool validNormal = false;
    if (HasReceiverNormals)
    {
        float3 normal = CameraReceiverNormals.Load(int3(pixel,0)).xyz * 2 - 1;
        validNormal = dot(normal,normal) >= 0.01;
        receiverNormal = validNormal ? normalize(normal) : 0;
    }
    else
        validNormal = V2ReceiverSurfaceNormal(CameraReceiverDepth,int2(pixel),world,uint2(Width,Height),InverseViewProjection,SelectionCameraPosition.xyz,receiverNormal);
    {
        float3 toLight = SelectionLightPosition.w != 0 ? normalize(SelectionLightPosition.xyz - world) : DirectionToLight;
        eligible = !validNormal || dot(receiverNormal,toLight) >= -0.0001;
        uncertainPlane = !validNormal || abs(dot(receiverNormal,toLight)) < 0.02;
    }
    uint bits = eligible ? (uncertainPlane ? 7u : 3u) : 1u;
    // Canonical light depth is ALWAYS 0 near, 1 far, including reversed Z.
    // Positive finite float bit patterns preserve order for integer atomics.
    int2 size = int2(SelectionShadowTiles[0].zw);
    // A camera point is splatted over neighboring LIGHT texels. Their depth can
    // differ on a sloped plane. Store a conservative interval spanning that
    // footprint, rather than treating the point's depth as exact at every texel.
    float2 gradient = 0;
    if (validNormal)
        gradient = SelectionLightPosition.w != 0 ? V2PerspectiveShadowGradient(world,receiverNormal,LightViewProjection,size) :
            V2ShadowDepthPerTexel(receiverNormal,LightViewProjection,size);
    float footprint = dot(abs(gradient),float2(1,1)) + SelectionShadowSplit.w * 4;
    float canonicalDepth = DEPTH_01(light.z);
    int2 first = int2(floor(light.xy * size - 0.5));
    UNROLL
    for (uint tap = 0; tap < 4; tap++)
    {
        int2 texel = first + int2(tap & 1, tap >> 1);
        if (any(texel < 0) || any(texel >= size)) continue;
        // Validate each actual camera surface BEFORE merging depths. Otherwise
        // a lit wall and its shadowed floor can merge into an artificial interval:
        // the wall supplies 'near', the floor supplies 'far', and a hidden caster
        // between them appears to have a visible receiver that does not exist.
        // Move the receiver plane to this LIGHT texel's centre before comparing;
        // comparing the original camera sample depth creates slope-dependent leaks.
        float2 offset = (float2(texel) + 0.5) - light.xy * size;
        float planeDepth = canonicalDepth + dot(gradient, offset) * (DEPTH_RANGE_MAX - DEPTH_RANGE_MIN);
        float tolerance = SelectionShadowSplit.w * 4;
        float cheap = CameraReceiverBlockers.Load(int3(texel,0));
        bool tapEligible = eligible && (cheap == DEPTH_RANGE_MAX ||
            planeDepth <= DEPTH_01(cheap) + tolerance);
        uint tapBits = tapEligible ? bits : 1u;
        InterlockedOr(CameraReceiverCoverageOut[texel], tapBits);
        if (tapEligible)
        {
            // These extremes now contain only receivers not already shadowed by
            // cheap real geometry. Preserve multiple remaining receiver surfaces.
            float uncertainty = validNormal ? tolerance : footprint;
            InterlockedMin(CameraReceiverNearOut[texel], asuint(saturate(planeDepth - uncertainty)));
            InterlockedMax(CameraReceiverFarOut[texel], asuint(saturate(planeDepth + uncertainty)));
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
    // Camera visibility is not proof of a visible shadow contribution. Every
    // caster must reach at least one eligible camera-visible receiver below.
    // Whole-box light visibility is only a coarse result. A zero here must
    // not veto a confirmed receiver ray elsewhere in this box's projection.
    // Proxy casters therefore continue to the per-ray light/receiver test.
    // Cheap geometry keeps its existing fast path and needs no receiver scan.
    if (!box.UseProxy)
    {
        if (thread == 0) ReceiverMaskCasterResults[index] = ReceiverMaskLightVisibility[index] != 0 ? 3 : 0;
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
        float4 clip = PROJECT_POINT(float4(p,1),LightViewProjection);
        outside = outside || (SelectionLightPosition.w != 0 ?
            (clip.w <= 0 || clip.z <= 0 || clip.z >= clip.w) : !IsV2ShadowCoordValid(light));
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
        if (SelectionLightPosition.w != 0)
        {
            // Perspective rays share an apex; their FAR-plane endpoints form an
            // affine grid. Cache that grid once per box, not matrix work per tap.
            float4 farH = PROJECT_POINT(float4(-1,1,DEPTH_RANGE_MAX,1),SelectionNearLightViewProjection);
            float4 farX = PROJECT_POINT(float4(2.0/size.x,0,0,0),SelectionNearLightViewProjection);
            float4 farY = PROJECT_POINT(float4(0,-2.0/size.y,0,0),SelectionNearLightViewProjection);
            ReceiverRayOrigin = SelectionLightPosition.xyz;
            ReceiverRayDelta = farH.xyz / farH.w - ReceiverRayOrigin;
            ReceiverRayStepX = farX.xyz / farH.w;
            ReceiverRayStepY = farY.xyz / farH.w;
            float3 toFarCorner = max((box.Minimum-ReceiverRayOrigin)*(-DirectionToLight),
                (box.Maximum-ReceiverRayOrigin)*(-DirectionToLight));
            float scale = saturate(dot(toFarCorner,float3(1,1,1))/max(ProjectionDistance,1e-6));
            ReceiverRayPadding = (abs(ReceiverRayStepX)+abs(ReceiverRayStepY))*scale;
            // The optional tile quick-reject keeps only the empty-tile proof for
            // perspective boxes; exact entry depth is computed after the ray hit.
            ReceiverBoxNearest = 0;
        }
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
        if (SelectionLightPosition.w != 0)
        {
            start = ReceiverRayOrigin;
            delta += ReceiverRayStepX*(pixel.x+0.5)+ReceiverRayStepY*(pixel.y+0.5);
        }
        float3 rayInverse = SelectionLightPosition.w != 0 ? rcp(delta) : ReceiverRayInverse;
        float enter=0,leave=1;
        bool hit=true;
        UNROLL
        for(uint axis=0;axis<3;axis++)
        {
            if(abs(delta[axis])<1e-8)
                hit=hit && start[axis]>=paddedMinimum[axis] && start[axis]<=paddedMaximum[axis];
            else
            {
                float a=(paddedMinimum[axis]-start[axis])*rayInverse[axis];
                float b=(paddedMaximum[axis]-start[axis])*rayInverse[axis];
                enter=max(enter,min(a,b)); leave=min(leave,max(a,b));
            }
        }
        // Directional orthographic projection: ray t equals canonical light depth.
        // A visible receiver must be behind the caster's entry, not in front of it.
        if(!hit || leave<enter) continue;
        float entryDepth = enter;
        if (SelectionLightPosition.w != 0)
            entryDepth = DEPTH_01(ProjectV2Shadow(start+delta*max(enter,1e-7),LightViewProjection).z);
        if (receiverFar<=entryDepth+SelectionShadowSplit.w) continue;
        float cheap=ReceiverMaskLightDepth.Load(int3(pixel,0));
        // A cheap blocker BEFORE the box prevents this caster receiving the light.
        // Do not reject a receiver merely because it is behind cheap depth: that
        // receiver may be the cheap surface itself receiving this caster's shadow.
        if(cheap!=DEPTH_RANGE_MAX)
        {
            float cheapCanonical=DEPTH_01(cheap);
            if(cheapCanonical<entryDepth-SelectionShadowSplit.w*4) continue;
            // Cheap real geometry has its own shadow draw. An expensive caster
            // cannot add a visible hard shadow when ALL camera receivers in this
            // texel are already behind that cheap blocker. Do not apply this
            // rule to the cheap caster itself, or its shadow would disappear.
            // Mixed/uncertain planes stay conservative instead of proving a cull.
            if(box.UseProxy && (coverage&4u)==0 && receiverNear>cheapCanonical+SelectionShadowSplit.w*4) continue;
        }
        // Acceptance is monotonic within this light/caster test. Other rays
        // may fail, but cannot clear an already confirmed visible shadow hit.
        InterlockedOr(ReceiverMaskHit,3u);
        break;
    }
    GroupMemoryBarrierWithGroupSync();
    if(thread==0) ReceiverMaskCasterResults[index]=ReceiverMaskHit;
}
#endif

#ifdef _CS_V2ShadowIndirectArgs
#include "./Flax/Utils/IndirectDraw.hlsl"
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_V2ShadowIndirectArgs(uint index : SV_DispatchThreadID)
{
    V2BuildIndirectDraw(index);
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
    if (IsV2CameraVisibleCaster(box))
    {
        if (thread == 0) LightVisibilityResults[index] = 3;
        return;
    }
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
    if (uncertain || any(last <= first) || (SelectionLightPosition.w == 0 && (any(lo < 0) || any(hi > 1))))
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

#ifdef _PS_V2ShadowDepthReduce
META_CB_BEGIN(2, V2OcclusionData)
float4x4 OcclusionViewProjection;
uint OcclusionWidth;
uint OcclusionHeight;
uint OcclusionCount;
uint OcclusionGroupsPerRow;
META_CB_END
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
