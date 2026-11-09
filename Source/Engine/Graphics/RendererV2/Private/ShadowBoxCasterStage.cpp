// Copyright (c) Wojciech Figat. All rights reserved.

#include "../ShadowBoxCasterStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../RenderPipelineGraphicSettings.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Content.h"
#include "Engine/Core/Math/BoundingFrustum.h"
#include "Engine/Core/Math/Mathd.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Profiler/Profiler.h"
#include "Engine/Renderer/RenderList.h"

namespace
{
    struct BlurBandData
    {
        Float4 Range;
        Float4 Settings; // Resolved mode, full-map radius (screen pixels for ScreenSpace), angular tangent, reserved.
    };
    constexpr int32 MaximumShadowResolution = 16384;
    constexpr int32 FarShadowResolution = 512;

    int32 InitShadowDepthMap(GPUDevice* device, GPUTexture* depth, int32 resolution, int32 farTileWidth)
    {
        const PixelFormat formats[] = { PixelFormat::D16_UNorm, GPU_DEPTH_BUFFER_PIXEL_FORMAT, PixelFormat::D32_Float };
        const int32 minResolution = Math::Min(resolution, FarShadowResolution);
        for (int32 size = resolution; ; size = Math::Max(minResolution, size / 2))
        {
            // Keep the original depth format for the default size and prefer D16 for large maps.
            for (int32 i = size > 4096 ? 0 : 1; i < ARRAY_COUNT(formats); i++)
            {
                const auto format = formats[i];
                const auto sampleFormat = PixelFormatExtensions::FindShaderResourceFormat(format, false);
                if (!EnumHasAllFlags(device->GetFormatFeatures(format).Support, FormatSupport::DepthStencil | FormatSupport::Texture2D) ||
                    !EnumHasAllFlags(device->GetFormatFeatures(sampleFormat).Support, FormatSupport::ShaderSample | FormatSupport::ShaderSampleComparison))
                    continue;
                const auto desc = GPUTextureDescription::New2D(size + farTileWidth, size, format,
                    GPUTextureFlags::ShaderResource | GPUTextureFlags::DepthStencil);
                if (!depth->Init(desc))
                    return size;
            }
            if (size == minResolution)
                return 0;
        }
    }

    float GetShadowDepthPrecisionBias(PixelFormat format)
    {
        const float roundoff = 4.0f * 1.192092896e-7f;
        if (format == PixelFormat::D16_UNorm)
            return roundoff + 1.0f / 65535.0f;
        if (format == PixelFormat::D24_UNorm_S8_UInt)
            return roundoff + 1.0f / 16777215.0f;
        return roundoff;
    }

    bool FitShadowSphere(const Float3& cameraPosition, const Matrix& lightView, const BoundingBox& lightBounds,
                         float endDistance, float filterRadius, int32 resolution, BoundingBox& bounds)
    {
        PROFILE_CPU_NAMED("V2 Shadow Fit Cascade Bounds");
        Float3 lightCenter;
        Float3::TransformCoordinate(cameraPosition, lightView, lightCenter);
        const float diameter = 2.0f * endDistance;
        const float padding = Math::Max(diameter * ((filterRadius + 2.0f) / resolution), 1.0f);
        const Float2 sceneMin(Float3(lightBounds.Minimum) - Float3::One);
        const Float2 sceneMax(Float3(lightBounds.Maximum) + Float3::One);
        const Float2 center(lightCenter);
        const Float2 radius(diameter * 0.5f + padding);
        if (center.X + radius.X <= sceneMin.X || center.X - radius.X >= sceneMax.X ||
            center.Y + radius.Y <= sceneMin.Y || center.Y - radius.Y >= sceneMax.Y)
            return false;

        const Float2 span = Float2::Min(radius * 2.0f, sceneMax - sceneMin);
        const Float2 texel = span / (float)resolution;
        const Float2 offset = center - span * 0.5f - sceneMin;
        const Float2 snapped = Float2::Clamp(sceneMin + Float2::Round(offset / texel) * texel, sceneMin, sceneMax - span);
        const Float2 end = snapped + span;
        bounds = BoundingBox(Vector3(snapped.X, snapped.Y, lightBounds.Minimum.Z - 1),
                             Vector3(end.X, end.Y, lightBounds.Maximum.Z + 1));
        return true;
    }

    struct ShadowSweepPlane
    {
        Plane Boundary;
        Real SweepDelta;
    };

    struct IndirectShadowInput
    {
        uint32 CasterIndex;
        uint32 IndexCount;
    };

    struct CasterBounds
    {
        Float3 Minimum;
        uint32 UseProxy;
        Float3 Maximum;
        uint32 Padding = 0;
    };
    static_assert(sizeof(CasterBounds) == 32, "Caster bounds must match the shader layout.");

    bool CanReachView(const CasterBounds& box, const ShadowSweepPlane (&planes)[6], float padding)
    {
        Real enter = 0;
        Real leave = 1;
        for (const auto& sweepPlane : planes)
        {
            const auto& plane = sweepPlane.Boundary;
            const auto& normal = plane.Normal;
            const Vector3 furthest(
                normal.X >= 0 ? box.Maximum.X : box.Minimum.X,
                normal.Y >= 0 ? box.Maximum.Y : box.Minimum.Y,
                normal.Z >= 0 ? box.Maximum.Z : box.Minimum.Z);

            // For a hit, the box must reach the inside of every plane at a common
            // point along the sweep. Use its support point to keep this conservative.
            const Real distance = Vector3::Dot(normal, furthest) + plane.D + Plane::DistanceEpsilon + padding;
            const Real delta = sweepPlane.SweepDelta;
            if (delta > 0)
                enter = Math::Max(enter, -distance / delta);
            else if (delta < 0)
                leave = Math::Min(leave, -distance / delta);
            else if (distance < 0)
                return false;

            if (enter > leave)
                return false;
        }
        return true;
    }

    GPU_CB_STRUCT(ResolveConstants
    {
        Matrix InverseViewProjection;
        Matrix LightViewProjection;
        Float3 DirectionToLight;
        float AmbientIntensity;
        Float3 CameraPosition;
        uint32 IsOrthographic;
        Float3 CameraForward;
        uint32 UseFilteredShadow;
        Float4 LightRadiance;
        Float4 ShadowFilter;
        Matrix NearLightViewProjection;
        Float4 ShadowTiles[2];
        Float4 ShadowSplit; // Near blend start, blend end, near tier enabled, depth precision bias.
        Float4 CascadeRange;
        Float4 CascadeParameters; // Normal bias in world units, cascade index, debug enabled, screen filter radius.
        Float4 ReductionFootprint; // XY: reduced depth sample support in normalized light-map UV.
        Float4 DebugOptions; // X: lighting mode (0 material, 1 white, 2 cel), Y: edge seeds, Z: jump step, W: debug range.
        Float4 CelParameters; // X: band count, Y: receiver SDF enabled, Z: world distance range, W: core fill strength.
        Float4 ShadowSampling; // X: adaptive sampling jitter, Y: SDF side (0 both, 1 inner, 2 outer), Z: seed-only pass, W: world-origin noise phase.
        Matrix CameraViewProjection; // World -> camera clip for contact shadows.
        Float4 BlockerParameters; // X: PCSS enabled; other components reserved.
        Float4 ContactParameters; // World length, thickness, camera fade distance, reserved.
    });


    GPU_CB_STRUCT(SelectionConstants
    {
        Matrix InverseViewProjection;
        Matrix ViewProjection;
        Matrix LightViewProjection;
        Float3 DirectionToLight;
        float ProjectionDistance;
        uint32 Width;
        uint32 Height;
        uint32 CasterCount;
        uint32 GroupsPerRow;
        uint32 Padding0;
        uint32 HasLightDepth;
        uint32 HasReceiverNormals;
        float FilterRadius;
        Matrix NearLightViewProjection;
        Float4 ShadowTiles[2];
        Float4 ShadowSplit; // Near blend start, blend end, near tier enabled, depth precision bias.
        Float4 CameraPosition;
        Float4 CameraForward;
        Float4 ReceiverRange; // Shadow resolve metadata. Candidate selection is view-wide.
        uint32 CasterDebugFlags;
        float Padding1;
        uint32 Padding[2];
    });
}

ShadowBoxCasterStage::ShadowBoxCasterStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

ShadowBoxCasterStage::~ShadowBoxCasterStage()
{
    if (SelectionShader)
        SelectionShader->Reloading.Unbind<ShadowBoxCasterStage, &ShadowBoxCasterStage::OnShaderReloading>(this);
    SAFE_DELETE_GPU_RESOURCE(CasterBoundsDebugState);
    SAFE_DELETE_GPU_RESOURCE(ResolveState);
    SAFE_DELETE_GPU_RESOURCE(BlurBandSettings);
    SAFE_DELETE_GPU_RESOURCE(ShadowMaskState);
    SAFE_DELETE_GPU_RESOURCE(CascadeMaskState);
    SAFE_DELETE_GPU_RESOURCE(PixelCascadeMaskState);
    SAFE_DELETE_GPU_RESOURCE(CascadeDebugState);
    SAFE_DELETE_GPU_RESOURCE(CascadeDebug);
    SAFE_DELETE_GPU_RESOURCE(ShadowFilterState);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistanceReceiverState);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistanceReceivers);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistanceJumpState);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistanceResolveState);
    for (auto& seed : ShadowDistanceSeeds)
        SAFE_DELETE_GPU_RESOURCE(seed);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistance);
    SAFE_DELETE_GPU_RESOURCE(ShadowDistanceDebug);
    SAFE_DELETE_GPU_RESOURCE(ShadowMask);
    SAFE_DELETE_GPU_RESOURCE(FilteredShadowMask);
    SAFE_DELETE_GPU_RESOURCE(SelectionReadback);
    SAFE_DELETE_GPU_RESOURCE(IndirectInputs);
    SAFE_DELETE_GPU_RESOURCE(IndirectOutput);
    SAFE_DELETE_GPU_RESOURCE(IndirectArguments);
    SAFE_DELETE_GPU_RESOURCE(CandidateOccluderDepth);
    SAFE_DELETE_GPU_RESOURCE(LightVisibilityBuffer);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverMask);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverNear);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverFar);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverCasters);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverTileMax);
    SAFE_DELETE_GPU_RESOURCE(SharedCascadeDepth);
    SAFE_DELETE_GPU_RESOURCE(DepthReductionState);
    for (auto& map : BandDepthMaps)
        SAFE_DELETE_GPU_RESOURCE(map.Texture);
    if (LightDepthBuffers)
        Delete(LightDepthBuffers);
    SAFE_DELETE_GPU_RESOURCE(BoundsBuffer);
    SAFE_DELETE_GPU_RESOURCE(SelectionBuffer);
}

void ShadowBoxCasterStage::OnShaderReloading(Asset* asset)
{
    if (DepthReductionState)
        DepthReductionState->ReleaseGPU();
    if (CasterBoundsDebugState)
        CasterBoundsDebugState->ReleaseGPU();
    if (ResolveState)
        ResolveState->ReleaseGPU();
    if (ShadowMaskState)
        ShadowMaskState->ReleaseGPU();
    if (CascadeMaskState)
        CascadeMaskState->ReleaseGPU();
    if (PixelCascadeMaskState)
        PixelCascadeMaskState->ReleaseGPU();
    if (CascadeDebugState)
        CascadeDebugState->ReleaseGPU();
    if (ShadowFilterState)
        ShadowFilterState->ReleaseGPU();
    if (ShadowDistanceReceiverState)
        ShadowDistanceReceiverState->ReleaseGPU();
    if (ShadowDistanceJumpState)
        ShadowDistanceJumpState->ReleaseGPU();
    if (ShadowDistanceResolveState)
        ShadowDistanceResolveState->ReleaseGPU();
}

void ShadowBoxCasterStage::SetProjection(const Vector3& lightDirection, float distance)
{
    LightDirection = lightDirection;
    if (!LightDirection.IsZero())
        LightDirection.Normalize();
    ProjectionDistance = distance;
}

int32 ShadowBoxCasterStage::GetAllocatedShadowMapResolution() const
{
    if (SharedCascadeDepth && SharedCascadeDepth->GetMemoryUsage())
        return SharedCascadeDepth->Width();
    const auto* depth = LightDepthBuffers ? LightDepthBuffers->DepthBuffer : nullptr;
    return depth && depth->GetMemoryUsage() ? depth->Height() : 0;
}

int32 ShadowBoxCasterStage::PrepareShadowAtlas(GPUDevice* device, int32 resolution, int32 farTileWidth)
{
    if (!LightDepthBuffers)
        LightDepthBuffers = New<RenderBuffers>();
    auto* depth = LightDepthBuffers->DepthBuffer;
    const int32 width = resolution + farTileWidth;
    if (RequestedAtlasWidth == width && RequestedAtlasHeight == resolution && depth->GetMemoryUsage())
        return depth->Height();

    const int32 size = InitShadowDepthMap(device, depth, resolution, farTileWidth);
    if (size)
    {
        RequestedAtlasWidth = width;
        RequestedAtlasHeight = resolution;
    }
    return size;
}

bool ShadowBoxCasterStage::PrepareSharedShadowDepth(GPUDevice* device, const RenderPipelineGraphicSettings* settings)
{
    const int32 count = settings ? settings->ShadowCascades.Count() : 0;
    if (count == 0)
    {
        if (SharedCascadeDepth)
            SharedCascadeDepth->ReleaseGPU();
        SharedCascadeRequestedResolution = 0;
        return false;
    }

    // All bands resolve one common light projection and depth allocation.
    const int32 limit = Math::Min(MaximumShadowResolution, device->Limits.MaximumTexture2DSize);
    const int32 maximumResolution = Math::Clamp(settings->ShadowDepthResolution, FarShadowResolution, limit);
    if (!SharedCascadeDepth)
        SharedCascadeDepth = device->CreateTexture(TEXT("V2 Shared Cascade Depth"));
    if (!SharedCascadeDepth || ((SharedCascadeRequestedResolution != maximumResolution || !SharedCascadeDepth->GetMemoryUsage()) &&
        !InitShadowDepthMap(device, SharedCascadeDepth, maximumResolution, 0)))
    {
        return false;
    }
    SharedCascadeRequestedResolution = maximumResolution;
    return true;
}

StageResult ShadowBoxCasterStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    Casters.Clear();
    LastCasterCount = 0;
    LastRejectedProxyCount = 0;
    LastProxyCasterCount = 0;
    LastSelectionDispatchCount = 0;
    LastCascadeCount = 0;
    LastReceiverWidth = 0;
    LastReceiverHeight = 0;
    LastUsedSelection = false;
    if (!(ProjectionDistance > 0.0f) || LightDirection.IsZero() || !context->GetDevice()->Limits.HasCompute)
        return StageResult::Skipped;

    auto* depth = resources.Get<GPUTexture>(TEXT("Shadow.ReceiverDepth"));
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    if (!depth || !view)
        return StageResult::Skipped;
    LastReceiverWidth = depth->Width();
    LastReceiverHeight = depth->Height();

    if (!SelectionShader)
    {
        SelectionShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/Culling"));
        if (SelectionShader)
            SelectionShader->Reloading.Bind<ShadowBoxCasterStage, &ShadowBoxCasterStage::OnShaderReloading>(this);
    }
    if (!SelectionShader || !SelectionShader->IsLoaded() || !SelectionShader->GPU)
        return StageResult::Skipped;
    auto* constants = SelectionShader->GPU->GetCB(1);
    if (!constants || constants->GetSize() != sizeof(SelectionConstants))
        return StageResult::Skipped;

    const auto* settings = resources.GetGraphicsSettings();
    const int32 proxyTriangleThreshold = settings ? settings->ShadowProxyTriangleThreshold : ProxyTriangleThreshold;
    const float nearShadowDistance = settings ? 0.0f : NearShadowDistance;
    const int32 requestedShadowResolution = settings ? settings->ShadowDepthResolution : ShadowMapResolution;
    const bool smoothShadows = !settings || settings->ShadowRendering == ShadowRenderingMode::Smooth;
    const bool adaptiveFilter = smoothShadows;
    const float adaptiveRadius = 8.0f;
    const bool screenSpaceFilter = false;
    const float angularRadius = 0.27f;
    const bool indirectShadowDraws = settings ? settings->UseIndirectShadowDraws : UseIndirectShadowDraws;
    const bool cascadeDebugRequested = settings && !settings->ShadowCascades.IsEmpty() && settings->ShowShadowCascades;
    const bool showSelectedCasters = !cascadeDebugRequested && (ShowSelectedExpensiveCasters || (settings && settings->ShowSelectedExpensiveShadowCasters));
    const bool showRejectedCasters = !cascadeDebugRequested && (ShowRejectedExpensiveCasters || (settings && settings->ShowRejectedExpensiveShadowCasters));

    const BoundingFrustum frustum(view->ViewProjection);
    const Vector3 sweepOffset(Float3(LightDirection) * ProjectionDistance);
    ShadowSweepPlane sweepPlanes[6];
    for (int32 i = 0; i < ARRAY_COUNT(sweepPlanes); i++)
    {
        auto& sweepPlane = sweepPlanes[i];
        sweepPlane.Boundary = frustum.GetPlane(i);
        sweepPlane.SweepDelta = Vector3::Dot(sweepPlane.Boundary.Normal, sweepOffset);
    }

    Array<CasterBounds> bounds;
    const auto append = [&](const Array<IVisualData*>& candidates)
    {
        for (int32 cameraIndex = 0; cameraIndex < candidates.Count(); cameraIndex++)
        {
            auto* object = candidates[cameraIndex];
            auto* warped = dynamic_cast<WarpedVisualData*>(object);
            auto* actor = warped ? dynamic_cast<StaticModel*>(warped->GetIVD_Source()) : nullptr;
            auto* model = actor ? actor->Model.Get() : nullptr;
            if (!model || !model->IsLoaded())
                continue;
            const int32 lod = model->HighestResidentLODIndex();
            if (lod < 0 || lod >= model->LODs.Count())
                continue;

            int64 triangles = 0;
            for (const auto& mesh : model->LODs[lod].Meshes)
                triangles += mesh.GetTriangleCount();
            if (!triangles)
                continue;

            CasterBounds entry;
            entry.Minimum = Float3(warped->Bounds.Minimum);
            entry.Maximum = Float3(warped->Bounds.Maximum);
            entry.UseProxy = triangles > proxyTriangleThreshold ? 1 : 0;
            bounds.Add(entry);
            Casters.Add(object);
        }
    };
    append(objects);
    if (FrustumStage)
        append(FrustumStage->GetRejectedObjects());
    if (OcclusionStage)
        append(OcclusionStage->GetOccludedObjects());
    if (bounds.IsEmpty())
        return StageResult::Completed;

    auto* device = context->GetDevice();
    const int32 atlasLimit = Math::Min(MaximumShadowResolution, device->Limits.MaximumTexture2DSize);
    if (atlasLimit < 2)
        return StageResult::Skipped;
    const bool useTiers = nearShadowDistance > 0.0f && atlasLimit >= 2 * FarShadowResolution;
    const int32 farTileWidth = useTiers ? FarShadowResolution : 0;
    int32 resolution = Math::Min(Math::Clamp(requestedShadowResolution, FarShadowResolution, MaximumShadowResolution), atlasLimit - farTileWidth);
    GPUTexture* lightDepth = nullptr;
    bool useConfiguredCascades = false;
    int32 smallestCascade = atlasLimit;
    if (context == device->GetMainContext())
    {
        if (settings && !settings->ShadowCascades.IsEmpty() && (!SharedCascadeDepth || !SharedCascadeDepth->GetMemoryUsage()) && LightDepthBuffers)
            LightDepthBuffers->DepthBuffer->ReleaseGPU();
        useConfiguredCascades = PrepareSharedShadowDepth(device, settings);
        if (useConfiguredCascades)
        {
            lightDepth = SharedCascadeDepth;
            smallestCascade = SharedCascadeDepth->Width();
        }
        if (useConfiguredCascades)
        {
            // Material binding still uses RenderBuffers even when depth targets are separate.
            if (!LightDepthBuffers)
                LightDepthBuffers = New<RenderBuffers>();
            else if (LightDepthBuffers->DepthBuffer->GetMemoryUsage())
                LightDepthBuffers->DepthBuffer->ReleaseGPU();
        }
        else
        {
            resolution = PrepareShadowAtlas(device, resolution, farTileWidth);
            if (resolution == 0)
                return StageResult::Completed;
            lightDepth = LightDepthBuffers->DepthBuffer;
        }
    }
    const int32 farResolution = useTiers ? FarShadowResolution : resolution;
    const float inheritedRadius = adaptiveFilter
        ? Math::Clamp(adaptiveRadius, 0.0f, 32.0f) : 0.0f;
    float filterRadius = useConfiguredCascades ? 0.0f : inheritedRadius;
    struct CascadeFilter
    {
        float AdaptiveRadius;
        float ScreenRadius;
        float AngularTangent;
        bool PCSS;
    };
    Array<CascadeFilter, InlinedAllocation<8>> cascadeFilters;
    float maxNormalBias = 0.0f;
    if (useConfiguredCascades)
    {
        for (const auto& cascade : settings->ShadowCascades)
        {
            CascadeFilter filter;
            filter.AdaptiveRadius = smoothShadows ? Math::Clamp(cascade.FilterRadius, 0.0f, 32.0f) : 0.0f;
            filter.ScreenRadius = 0.0f;
            filter.AngularTangent = Math::Tan(angularRadius * DegreesToRadians);
            filter.PCSS = false;
            cascadeFilters.Add(filter);
            filterRadius = Math::Max(filterRadius, filter.AdaptiveRadius);
            maxNormalBias = Math::Max(maxNormalBias, Math::Clamp(cascade.NormalBias, 0.0f, 10.0f));
        }
    }
    Array<BlurBandData, InlinedAllocation<8>> blurBands;
    bool blurNeedsScreenFilter = false;
    if (settings && smoothShadows)
    {
        float previousEnd = 0, previousFade = 0;
        for (int32 i = 0; i < settings->ShadowCascades.Count(); i++)
        {
            const auto& band = settings->ShadowCascades[i];
            const bool terminal = i == settings->ShadowCascades.Count() - 1;
            const float end = terminal ? MAX_float : (band.BlurEndDistance > 0 ? band.BlurEndDistance : band.EndDistance);
            if (!(end > previousEnd && end <= MAX_float))
                continue;
            const float fade = terminal ? end : Math::Lerp(previousEnd, end, 1.0f - Math::Clamp(band.BlurBlendFraction, 0.0f, 1.0f));
            const int32 referenceSize = band.TextureSize <= 0 ? resolution : Math::Clamp(band.TextureSize, FarShadowResolution, resolution);
            const float radius = Math::Clamp(band.FilterRadius, 0.0f, 32.0f) * (float)resolution / referenceSize;
            blurBands.Add({ Float4(previousFade, fade, end, 0), Float4(2.0f, radius,
                Math::Tan(angularRadius * DegreesToRadians), 0) });
            filterRadius = Math::Max(filterRadius, Math::Min(radius, 32.0f));
            previousEnd = end;
            previousFade = fade;
        }
    }
    if (blurBands.HasItems())
    {
        if (!BlurBandSettings)
            BlurBandSettings = device->CreateBuffer(TEXT("V2 Independent Blur Bands"));
        if (!BlurBandSettings || (BlurBandSettings->GetElementsCount() < (uint32)blurBands.Count() &&
            BlurBandSettings->Init(GPUBufferDescription::Structured(blurBands.Count(), sizeof(BlurBandData)))))
            return StageResult::Failed;
        context->ResetSR();
        context->UpdateBuffer(BlurBandSettings, blurBands.Get(), blurBands.Count() * sizeof(BlurBandData));
        resources.SetOutput(TEXT("Shadow.BlurBands"), BlurBandSettings);
    }
    else
    {
        // The shared shader declares t0 even when Pixel bypasses the band lookup.
        // Keep a valid minimal binding rather than a stale texture/retired buffer view.
        if (!BlurBandSettings)
            BlurBandSettings = device->CreateBuffer(TEXT("V2 Shadow Band Binding"));
        if (!BlurBandSettings || (BlurBandSettings->GetElementsCount() == 0 &&
            BlurBandSettings->Init(GPUBufferDescription::Structured(1, sizeof(BlurBandData)))))
            return StageResult::Failed;
    }
    float sweepPadding = 0.0f;
    if (filterRadius > 0 || maxNormalBias > 0)
    {
        // The world diagonal bounds both fitted light-space axes before caster rejection.
        Float3 minimum = bounds[0].Minimum;
        Float3 maximum = bounds[0].Maximum;
        for (const auto& box : bounds)
        {
            minimum = Float3::Min(minimum, box.Minimum);
            maximum = Float3::Max(maximum, box.Maximum);
        }
        const int32 minResolution = useConfiguredCascades ? smallestCascade : farResolution;
        sweepPadding = (filterRadius + 1.0f + maxNormalBias) * ((maximum - minimum).Length() + 2.0f) / minResolution * 1.415f;
    }
    Matrix lightView;
    BoundingBox lightBounds;
    Array<BoundingBox> lightCasterBounds;
    const Float3 direction(LightDirection);
    if (lightDepth)
    {
        const Float3 up = Math::Abs(direction.Y) > 0.99f ? Float3::Right : Float3::Up;
        Matrix::LookAt(Float3::Zero, direction, up, lightView);
        lightCasterBounds.EnsureCapacity(bounds.Count());
    }


    int32 keptCount = 0;
    for (int32 i = 0; i < bounds.Count(); i++)
    {
        const bool keep = !bounds[i].UseProxy || CanReachView(bounds[i], sweepPlanes, sweepPadding);
        if (lightDepth)
        {
            // Fit before view rejection so a caster entering the camera frustum cannot resize the atlas projection.
            const BoundingBox box(Vector3(bounds[i].Minimum), Vector3(bounds[i].Maximum));
            const BoundingBox transformed = BoundingBox::Transform(box, lightView);
            if (i == 0)
                lightBounds = transformed;
            else
                BoundingBox::Merge(lightBounds, transformed, lightBounds);
            if (keep)
                lightCasterBounds.Add(transformed);
        }
        if (!keep)
            continue;
        bounds[keptCount] = bounds[i];
        LastProxyCasterCount += bounds[i].UseProxy != 0;
        Casters[keptCount++] = Casters[i];
    }
    bounds.Resize(keptCount);
    Casters.Resize(keptCount);
    if (keptCount == 0)
        return StageResult::Completed;

    if (!BoundsBuffer)
        BoundsBuffer = device->CreateBuffer(TEXT("V2 Shadow Caster Bounds"));
    if (!SelectionBuffer)
        SelectionBuffer = device->CreateBuffer(TEXT("V2 Shadow Caster Selection"));
    if (!LightVisibilityBuffer)
        LightVisibilityBuffer = device->CreateBuffer(TEXT("V2 Shadow Light Visibility"));
    if (!LightVisibilityBuffer || (LightVisibilityBuffer->GetElementsCount() < (uint32)bounds.Count() &&
        LightVisibilityBuffer->Init(GPUBufferDescription::Structured(bounds.Count(), sizeof(uint32), true))))
        return StageResult::Failed;
    if (!BoundsBuffer || !SelectionBuffer)
        return StageResult::Failed;
    const int32 count = bounds.Count();
    if (BoundsBuffer->GetElementsCount() < (uint32)count &&
        BoundsBuffer->Init(GPUBufferDescription::Structured(count, sizeof(CasterBounds))))
        return StageResult::Failed;
    if (SelectionBuffer->GetElementsCount() < (uint32)count &&
        SelectionBuffer->Init(GPUBufferDescription::Structured(count, sizeof(uint32), true)))
        return StageResult::Failed;

    auto* sceneColor = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    SelectionConstants data = {};
    float lightDepthRange = 0.0f;
    RenderContext lightContexts[2];
    Viewport tileViewports[2] = {
        Viewport(0.0f, 0.0f, (float)farResolution, (float)farResolution),
        Viewport((float)farResolution, 0.0f, (float)resolution, (float)resolution),
    };
    BoundingBox nearBounds;
    int32 tierCount = 1;
    const auto drawShadowDepth = [&](int32 tier, bool proxies, const uint32* selected, bool indirect)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Depth");
        auto& lightContext = lightContexts[tier];
        const uint32 tierBit = 1u << tier;
        lightContext.List = RenderList::GetFromPool();
        lightContext.List->Init(lightContext);
        Array<IndirectShadowInput> drawInputs;
        if (indirect)
            drawInputs.Add({ 0, 0 }); // Header stores the live draw count.
        {
            PROFILE_CPU_NAMED("V2 Shadow Build Draw List");
            for (int32 i = 0; i < count; i++)
            {
                if ((bounds[i].UseProxy != 0) != proxies || (selected && (selected[i] & tierBit) == 0))
                    continue;
                if (tier == 1 || useConfiguredCascades)
                {
                    const auto& box = lightCasterBounds[i];
                    if (box.Maximum.X < nearBounds.Minimum.X || box.Minimum.X > nearBounds.Maximum.X ||
                        box.Maximum.Y < nearBounds.Minimum.Y || box.Minimum.Y > nearBounds.Maximum.Y)
                        continue;
                }
                auto* warped = static_cast<WarpedVisualData*>(Casters[i]);
                auto* actor = static_cast<StaticModel*>(warped->GetIVD_Source());
                auto* model = actor->Model.Get();
                const auto& entries = actor->GetEntries();
                for (const auto& mesh : model->LODs[model->HighestResidentLODIndex()].Meshes)
                {
                    const int32 slot = mesh.GetMaterialSlotIndex();
                    if (slot < 0 || slot >= model->MaterialSlots.Count())
                        continue;
                    if (slot < entries.Count() && (!entries[slot].Visible || entries[slot].ShadowsMode == ShadowsCastingMode::None))
                        continue;
                    auto* material = actor->GetMaterial(slot);
                    if (!material || !material->IsReady() ||
                        !EnumHasAllFlags(material->GetDrawModes(), DrawPass::Depth | DrawPass::GBuffer))
                        continue;
                    const int32 drawIndex = lightContext.List->DrawCalls.Count();
                    mesh.Draw(lightContext, material, warped->WorldMatrix, actor->GetStaticFlags(),
                              false, DrawPass::Depth, actor->GetPerInstanceRandom());
                    if (indirect && lightContext.List->DrawCalls.Count() > drawIndex)
                        drawInputs.Add({ (uint32)i, (uint32)lightContext.List->DrawCalls.Get()[drawIndex].Draw.IndicesCount });
                }
            }

        }
        if (drawInputs.Count() > 1)
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Indirect Arguments");
            auto* buildArgs = SelectionShader->GPU->GetCS("CS_V2ShadowIndirectArgs");
            if (!IndirectInputs)
                IndirectInputs = device->CreateBuffer(TEXT("V2 Shadow Indirect Inputs"));
            if (!IndirectOutput)
                IndirectOutput = device->CreateBuffer(TEXT("V2 Shadow Indirect Output"));
            if (!IndirectArguments)
                IndirectArguments = device->CreateBuffer(TEXT("V2 Shadow Indirect Arguments"));
            const int32 drawCount = drawInputs.Count() - 1;
            const int32 bytes = drawCount * sizeof(GPUDrawIndexedIndirectArgs);
            if (buildArgs &&
                IndirectInputs && IndirectOutput && IndirectArguments &&
                (IndirectInputs->GetElementsCount() >= (uint32)drawInputs.Count() ||
                 !IndirectInputs->Init(GPUBufferDescription::Structured(drawInputs.Count(), sizeof(IndirectShadowInput)))) &&
                (IndirectOutput->GetSize() >= (uint32)bytes ||
                 !IndirectOutput->Init(GPUBufferDescription::Typed(bytes / sizeof(uint32), PixelFormat::R32_UInt, true))) &&
                (IndirectArguments->GetSize() >= (uint32)bytes ||
                 !IndirectArguments->Init(GPUBufferDescription::Argument(bytes))))
            {
                drawInputs[0].CasterIndex = drawCount;
                drawInputs[0].IndexCount = tierBit;
                context->UpdateBuffer(IndirectInputs, drawInputs.Get(), drawInputs.Count() * sizeof(IndirectShadowInput));
                context->BindSR(0, SelectionBuffer->View());
                context->BindSR(1, IndirectInputs->View());
                context->BindUA(0, IndirectOutput->View());
                context->Dispatch(buildArgs, Math::DivideAndRoundUp(drawCount, 64));
                context->ResetUA();
                context->ResetSR();
                context->CopyBuffer(IndirectArguments, IndirectOutput, bytes);

                auto* draws = lightContext.List->DrawCalls.Get();
                for (int32 i = 0; i < drawCount; i++)
                {
                    draws[i].InstanceCount = 0;
                    draws[i].Draw.IndirectArgsBuffer = IndirectArguments;
                    draws[i].Draw.IndirectArgsOffset = i * sizeof(GPUDrawIndexedIndirectArgs);
                }
                LastUsedSelection = true;
            }
        }

        constexpr auto depthList = DrawCallsListType::Depth;
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->SetViewportAndScissors(tileViewports[tier]);
        context->SetRenderTarget(lightDepth->View(), static_cast<GPUTextureView*>(nullptr));
        if (!lightContext.List->DrawCallsLists[(int32)depthList].IsEmpty())
        {
            {
                PROFILE_CPU_NAMED("V2 Shadow Sort Draw List");
                lightContext.List->SortDrawCalls(lightContext, false, depthList, DrawPass::Depth);
            }
            {
                PROFILE_GPU_CPU_NAMED("V2 Shadow Execute Depth Draws");
                lightContext.List->ExecuteDrawCalls(lightContext, depthList);
            }
        }
        context->ResetRenderTarget();
        RenderList::ReturnToPool(lightContext.List);
        lightContext.List = nullptr;
    };

    if (lightDepth && !useConfiguredCascades)
    {
        const Float3 minimum = Float3(lightBounds.Minimum) - Float3::One;
        const Float3 maximum = Float3(lightBounds.Maximum) + Float3::One;
        lightDepthRange = maximum.Z - minimum.Z;
        Matrix lightProjection;
        Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, lightProjection);

        const Float3 cameraPosition(view->LocalTransform.Translation);
        const Float3 cameraForward(Vector3::Transform(Vector3::Forward, view->LocalTransform.Orientation));
        data.CameraPosition = Float4(cameraPosition, 0.0f);
        data.CameraForward = Float4(cameraForward, 0.0f);
        if (useTiers)
        {
            Matrix inverseProjection;
            Matrix::Invert(view->Projection, inverseProjection);
            const float centerDepth = nearShadowDistance * 0.5f;
            const Float3 sliceCenter(0, 0, centerDepth);
            float radiusSquared = 0.0f;
            for (int32 i = 0; i < 4; i++)
            {
                const Float3 clip(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, GPU_DEPTH_RANGE_MIN);
                Float3 point;
                Float3::TransformCoordinate(clip, inverseProjection, point);
                radiusSquared = Math::Max(radiusSquared, (point - sliceCenter).LengthSquared());
                const float splitDepth = Math::Max(nearShadowDistance, point.Z + 1.0f);
                if (view->Projection.M34 != 0.0f)
                    point *= splitDepth / point.Z;
                else
                    point.Z = splitDepth;
                radiusSquared = Math::Max(radiusSquared, (point - sliceCenter).LengthSquared());
            }
            // A sphere around the slice keeps its scale fixed under rotation.
            // Snap its light-space translation to the same texel grid.
            Float3 lightCenter;
            Float3::TransformCoordinate(cameraPosition + cameraForward * centerDepth, lightView, lightCenter);
            const float diameter = 2.0f * Math::Sqrt(radiusSquared);
            const float padding = Math::Max(diameter * ((filterRadius + 2.0f) / resolution), 1.0f);
            const Float2 sceneMin(minimum);
            const Float2 sceneMax(maximum);
            const Float2 center(lightCenter);
            const Float2 radius(diameter * 0.5f + padding);
            if (center.X + radius.X > sceneMin.X && center.X - radius.X < sceneMax.X &&
                center.Y + radius.Y > sceneMin.Y && center.Y - radius.Y < sceneMax.Y)
            {
                const Float2 span = Float2::Min(radius * 2.0f, sceneMax - sceneMin);
                const Float2 texel = span / (float)resolution;
                const Float2 start = center - span * 0.5f - sceneMin;
                const Float2 snapped = Float2::Clamp(sceneMin + Float2::Round(start / texel) * texel, sceneMin, sceneMax - span);
                const Float2 end = snapped + span;
                nearBounds = BoundingBox(Vector3(snapped.X, snapped.Y, minimum.Z), Vector3(end.X, end.Y, maximum.Z));
                tierCount = 2;
                data.ShadowSplit = Float4(nearShadowDistance * 0.85f, nearShadowDistance, 1.0f, 0.0f);
            }
        }

        data.ShadowSplit.W = GetShadowDepthPrecisionBias(lightDepth->Format());
        data.HasLightDepth = 1;
        context->ResetRenderTarget();
        context->ResetSR();
        context->ClearDepth(lightDepth->View());
        for (int32 tier = 0; tier < tierCount; tier++)
        {
            if (tier == 1)
                Matrix::OrthoOffCenter((float)nearBounds.Minimum.X, (float)nearBounds.Maximum.X,
                    (float)nearBounds.Minimum.Y, (float)nearBounds.Maximum.Y, minimum.Z, maximum.Z, lightProjection);
            auto& lightContext = lightContexts[tier];
            const auto& tile = tileViewports[tier];
            lightContext.Buffers = LightDepthBuffers;
            lightContext.View.Pass = DrawPass::Depth;
            lightContext.View.Flags = ViewFlags::None;
            lightContext.View.Direction = direction;
            lightContext.View.SetUp(lightView, lightProjection);
            lightContext.View.PrevViewProjection = lightContext.View.ViewProjection();
            lightContext.View.Near = minimum.Z;
            lightContext.View.Far = maximum.Z;
            lightContext.View.PrepareCache(lightContext, tile.Width, tile.Height, Float2::Zero);
            data.ShadowTiles[tier] = Float4(tile.Location.X, tile.Location.Y, tile.Width, tile.Height);
            Matrix::Transpose(lightContext.View.ViewProjection(), tier == 0 ? data.LightViewProjection : data.NearLightViewProjection);
            // Final cheap and expensive draws are submitted after selection.
        }
    }
    Matrix inverse;
    Matrix::Invert(view->ViewProjection, inverse);
    Matrix::Transpose(inverse, data.InverseViewProjection);
    Matrix::Transpose(view->ViewProjection, data.ViewProjection);
    data.DirectionToLight = -Float3(LightDirection);
    data.ProjectionDistance = ProjectionDistance;
    data.Width = depth->Width();
    data.Height = depth->Height();
    data.CasterCount = count;
    data.GroupsPerRow = Math::Min(count, 1024);
    auto* receiverNormals = resources.Get<GPUTexture>(TEXT("Shadow.ReceiverNormals"));
    data.HasReceiverNormals = receiverNormals ? 1 : 0;
    data.FilterRadius = filterRadius;
    data.CameraPosition = Float4(Float3(view->LocalTransform.Translation), 0);
    data.CasterDebugFlags = (showSelectedCasters ? 1u : 0u) | (showRejectedCasters ? 2u : 0u);

    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Upload Caster Bounds");
        context->UpdateBuffer(BoundsBuffer, bounds.Get(), count * sizeof(CasterBounds));
    }
    constexpr int32 selectionMapResolution = 1024;
    // Build selection depth once. Final shadow bands share a separate high-resolution depth draw.
    SelectionConstants candidateData = data;
    candidateData.HasLightDepth = 0;
    candidateData.ShadowSplit = Float4::Zero;
    candidateData.ReceiverRange = Float4::Zero;
    candidateData.FilterRadius = 0;
    if (lightDepth)
    {
        if (!CandidateOccluderDepth)
            CandidateOccluderDepth = device->CreateTexture(TEXT("V2 Candidate Occluder Depth"));
        const bool ready = CandidateOccluderDepth &&
            ((CandidateOccluderDepth->Width() == selectionMapResolution && CandidateOccluderDepth->GetMemoryUsage()) ||
                InitShadowDepthMap(device, CandidateOccluderDepth, selectionMapResolution, 0) != 0);
        if (ready)
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Candidate Occluders");
            auto* savedDepth = lightDepth;
            const auto savedViewport = tileViewports[0];
            const auto savedBounds = nearBounds;
            auto& candidateContext = lightContexts[0];
            const auto savedView = candidateContext.View;
            const Float3 minimum = Float3(lightBounds.Minimum) - Float3::One;
            const Float3 maximum = Float3(lightBounds.Maximum) + Float3::One;
            Matrix projection;
            Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, projection);
            lightDepth = CandidateOccluderDepth;
            tileViewports[0] = Viewport(0, 0, (float)selectionMapResolution, (float)selectionMapResolution);
            nearBounds = BoundingBox(Vector3(minimum), Vector3(maximum));
            candidateContext.Buffers = LightDepthBuffers;
            candidateContext.View.Pass = DrawPass::Depth;
            candidateContext.View.Flags = ViewFlags::None;
            candidateContext.View.Direction = direction;
            candidateContext.View.SetUp(lightView, projection);
            candidateContext.View.PrevViewProjection = candidateContext.View.ViewProjection();
            candidateContext.View.Near = minimum.Z;
            candidateContext.View.Far = maximum.Z;
            candidateContext.View.PrepareCache(candidateContext, (float)selectionMapResolution, (float)selectionMapResolution, Float2::Zero);
            context->ClearDepth(CandidateOccluderDepth->View());
            drawShadowDepth(0, false, nullptr, false);
            Matrix::Transpose(candidateContext.View.ViewProjection(), candidateData.LightViewProjection);
            // The shared selection pass uses this otherwise unused tier matrix
            // to reconstruct light-space texel rays through each real box.
            Matrix inverseLight;
            Matrix::Invert(candidateContext.View.ViewProjection(), inverseLight);
            Matrix::Transpose(inverseLight, candidateData.NearLightViewProjection);
            candidateData.ShadowTiles[0] = Float4(0, 0, (float)selectionMapResolution, (float)selectionMapResolution);
            candidateData.ShadowSplit.W = GetShadowDepthPrecisionBias(CandidateOccluderDepth->Format());
            candidateData.HasLightDepth = 1;
            candidateData.ReceiverRange.Z = maximum.Z - minimum.Z;
            lightDepth = savedDepth;
            tileViewports[0] = savedViewport;
            nearBounds = savedBounds;
            candidateContext.View = savedView;
            resources.SetOutput(TEXT("Shadow.CandidateOccluderDepth"), CandidateOccluderDepth);
        }
    }
    // Step 1: prove light occlusion against real cheap geometry. The result is
    // independent of camera visibility and shared by every cascade.
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow 1 Light Visibility");
        auto* lightVisibility = SelectionShader->GPU->GetCS("CS_V2LightCasterVisibility");
        if (!lightVisibility)
            return StageResult::Failed;
        context->UpdateCB(constants, &candidateData);
        context->BindCB(1, constants);
        context->BindSR(3, BoundsBuffer->View());
        context->BindSR(7, candidateData.HasLightDepth ? CandidateOccluderDepth : depth);
        context->BindUA(0, LightVisibilityBuffer->View());
        context->Dispatch(lightVisibility, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
        context->ResetUA();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.LightVisibility"), LightVisibilityBuffer);
    }

    // Reproject camera-visible opaque surfaces ONCE into the shared light map.
    // Atomic min/max preserve multiple receiver depths per texel. No CPU readback.
    // The scatter visits camera pixels once; map/caster tests use the fixed 1024 map.
    if (candidateData.HasLightDepth)
    {
        auto initReceiverMap = [&](GPUTexture*& texture, const Char* name)
        {
            if (!texture)
                texture = device->CreateTexture(name);
            return texture && ((texture->Width() == selectionMapResolution && texture->GetMemoryUsage()) ||
                !texture->Init(GPUTextureDescription::New2D(selectionMapResolution, selectionMapResolution,
                    PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess)));
        };
        if (!initReceiverMap(CameraReceiverMask, TEXT("V2 Camera Receivers Light Coverage")) ||
            !initReceiverMap(CameraReceiverNear, TEXT("V2 Camera Receivers Near Depth")) ||
            !initReceiverMap(CameraReceiverFar, TEXT("V2 Camera Receivers Far Depth")))
            return StageResult::Failed;
        if (!CameraReceiverCasters)
            CameraReceiverCasters = device->CreateBuffer(TEXT("V2 Camera Receiver Caster Candidates"));
        if (!CameraReceiverCasters || (CameraReceiverCasters->GetElementsCount() < (uint32)count &&
            CameraReceiverCasters->Init(GPUBufferDescription::Structured(count, sizeof(uint32), true))))
            return StageResult::Failed;
        // Avoid paying a whole-map reduction for a few small projected boxes.
        // This upper-bound scan estimate uses bounds already transformed for fitting;
        // it never reads GPU results back to the CPU.
        double projectedScanTexels = 0;
        const Float3 lightExtent = Float3(lightBounds.Maximum - lightBounds.Minimum) + Float3(2.0f);
        for (int32 i = 0; i < count; i++)
        {
            if (!bounds[i].UseProxy)
                continue;
            const Float3 extent = Float3(lightCasterBounds[i].Maximum - lightCasterBounds[i].Minimum);
            const float width = Math::Min((float)selectionMapResolution, extent.X / lightExtent.X * selectionMapResolution + 4.0f);
            const float height = Math::Min((float)selectionMapResolution, extent.Y / lightExtent.Y * selectionMapResolution + 4.0f);
            projectedScanTexels += (double)width * height;
        }
        const bool useReceiverTiles = projectedScanTexels >= (double)selectionMapResolution * selectionMapResolution;
        const int32 tileResolution = Math::DivideAndRoundUp(selectionMapResolution,8);
        if (useReceiverTiles)
        {
            if (!CameraReceiverTileMax)
                CameraReceiverTileMax = device->CreateTexture(TEXT("V2 Receiver Tile Maximum"));
            if (!CameraReceiverTileMax || ((CameraReceiverTileMax->Width() != tileResolution || !CameraReceiverTileMax->GetMemoryUsage()) &&
                CameraReceiverTileMax->Init(GPUTextureDescription::New2D(tileResolution,tileResolution,PixelFormat::R32_UInt,
                    GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess))))
                return StageResult::Failed;
        }
        auto* tileReceivers = SelectionShader->GPU->GetCS("CS_V2ReceiverTileMax");
        auto* projectReceivers = SelectionShader->GPU->GetCS("CS_V2ProjectCameraReceivers");
        auto* intersectReceivers = SelectionShader->GPU->GetCS("CS_V2ReceiverMaskCasters");
        if (!projectReceivers || !intersectReceivers || (useReceiverTiles && !tileReceivers))
            return StageResult::Failed;
        const uint32 clear[4] = {};
        const uint32 emptyNear[4] = { MAX_uint32, MAX_uint32, MAX_uint32, MAX_uint32 };
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->ClearUA(CameraReceiverMask, clear);
        context->ClearUA(CameraReceiverNear, emptyNear);
        context->ClearUA(CameraReceiverFar, clear);
        if (useReceiverTiles) candidateData.CasterDebugFlags |= 262144u;
        context->UpdateCB(constants, &candidateData);
        context->BindCB(1, constants);
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Camera To Light Receivers");
            context->BindSR(2, depth);
            context->BindSR(8, receiverNormals ? receiverNormals : depth);
            context->BindUA(0, CameraReceiverMask->View());
            context->BindUA(1, CameraReceiverNear->View());
            context->BindUA(2, CameraReceiverFar->View());
            context->Dispatch(projectReceivers, Math::DivideAndRoundUp(data.Width, 8u), Math::DivideAndRoundUp(data.Height, 8u));
            context->ResetUA();
            context->ResetSR();
        }
        if (useReceiverTiles)
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Receiver Tile Max");
            context->BindSR(7,CandidateOccluderDepth);
            context->BindSR(24,CameraReceiverMask);
            context->BindSR(25,CameraReceiverNear);
            context->BindSR(26,CameraReceiverFar);
            context->BindUA(0,CameraReceiverTileMax->View());
            context->Dispatch(tileReceivers,tileResolution,tileResolution);
            context->ResetUA();
            context->ResetSR();
        }
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Receiver Mask Caster Gate");
            LastSelectionDispatchCount++;
            context->BindSR(3, BoundsBuffer->View());
            context->BindSR(7, CandidateOccluderDepth);
            context->BindSR(21, LightVisibilityBuffer->View());
            context->BindSR(24, CameraReceiverMask);
            context->BindSR(25, CameraReceiverNear);
            context->BindSR(26, CameraReceiverFar);
            context->BindSR(28,useReceiverTiles ? CameraReceiverTileMax : nullptr);
            context->BindUA(0, CameraReceiverCasters->View());
            context->Dispatch(intersectReceivers, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
            context->ResetUA();
            context->ResetSR();
        }
        resources.SetOutput(TEXT("Shadow.CameraReceiverCoverage"), CameraReceiverMask);
        resources.SetOutput(TEXT("Shadow.CameraReceiverNear"), CameraReceiverNear);
        resources.SetOutput(TEXT("Shadow.CameraReceiverFar"), CameraReceiverFar);
        resources.SetOutput(TEXT("Shadow.CameraReceiverCasters"), CameraReceiverCasters);
        if (useReceiverTiles) resources.SetOutput(TEXT("Shadow.CameraReceiverTileMax"),CameraReceiverTileMax);
    }

    // The shared receiver gate is the final selection. If its light map could
    // not be built, retain the conservative light-visibility flags instead.
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Publish Receiver Mask Selection");
        context->CopyBuffer(SelectionBuffer, candidateData.HasLightDepth ? CameraReceiverCasters : LightVisibilityBuffer, count * sizeof(uint32));
        LastUsedSelection = true;
    }
    resources.SetOutput(TEXT("Shadow.VisibleShadowCasters"), SelectionBuffer);

    Array<uint32> selected;
    bool selectionReadbackAttempted = false;
    const auto drawSelectedDepth = [&](int32 tiers)
    {
        const bool indirect = indirectShadowDraws && device->Limits.HasDrawIndirect;
        if (!indirect && !selectionReadbackAttempted && device->GetRendererType() == RendererType::DirectX11)
        {
            selectionReadbackAttempted = true;
            if (!SelectionReadback)
                SelectionReadback = device->CreateBuffer(TEXT("V2 Shadow Selection Readback"));
            const auto bytes = (uint32)(count * sizeof(uint32));
            if (SelectionReadback && (SelectionReadback->GetSize() >= bytes ||
                !SelectionReadback->Init(SelectionBuffer->GetDescription().ToStagingReadback())))
            {
                PROFILE_CPU_NAMED("V2 Shadow Selection Readback");
                context->CopyBuffer(SelectionReadback, SelectionBuffer, bytes);
                if (const auto* flags = static_cast<const uint32*>(SelectionReadback->Map(GPUResourceMapMode::Read)))
                {
                    selected.Add(flags, count);
                    SelectionReadback->Unmap();
                }
            }
        }

        const bool haveSelection = selected.Count() == count;
        LastUsedSelection |= haveSelection;
        if (haveSelection && !useConfiguredCascades)
        {
            for (int32 i = 0; i < count; i++)
                if (bounds[i].UseProxy && selected[i] == 0)
                    LastRejectedProxyCount++;
        }
        PROFILE_GPU_CPU_NAMED("V2 Shadow 4 Selected Geometry");
        for (int32 tier = 0; tier < tiers; tier++)
        {
            drawShadowDepth(tier, false, haveSelection ? selected.Get() : nullptr, indirect);
            drawShadowDepth(tier, true, haveSelection ? selected.Get() : nullptr, indirect);
        }
    };

    auto* resolveConstants = SelectionShader->GPU->GetCB(3);
    const bool haveResolveConstants = resolveConstants && resolveConstants->GetSize() == sizeof(ResolveConstants);
    ResolveConstants resolve = {};
    resolve.InverseViewProjection = data.InverseViewProjection;
    resolve.LightViewProjection = data.LightViewProjection;
    resolve.DirectionToLight = data.DirectionToLight;
    resolve.AmbientIntensity = AmbientIntensity;
    resolve.ShadowSampling.X = smoothShadows ? 1.0f : 0.0f;
    resolve.ShadowSampling.Y = settings ? (float)(int32)settings->DistanceFieldSide : 0.0f;
    // Keep jitter attached to world receivers even if WorldWarp changes the local origin.
    if (view->Source)
    {
        auto* source = view->Source->GetIC_Source();
        const auto* sourcePose = source ? static_cast<const Transform*>(source->GetIC_WorldTransform()) : nullptr;
        if (sourcePose)
        {
            const Vector3 origin = sourcePose->Translation - view->LocalTransform.Translation;
            const double phase = ((double)origin.X + (double)origin.Y * (double)0.754877666f) * (double)0.06711056f +
                ((double)origin.Z + (double)origin.Y * (double)0.569840296f) * (double)0.00583715f;
            resolve.ShadowSampling.W = (float)(phase - Math::Floor(phase));
        }
    }
    resolve.DebugOptions.X = settings && settings->ShowCelShading ? 2.0f :
        settings && settings->ShowWhiteSurfaceLighting ? 1.0f : 0.0f;
    resolve.CameraViewProjection = data.ViewProjection;
    resolve.BlockerParameters = Float4(0.0f, (float)blurBands.Count(), (float)resolution, smoothShadows ? 0.0f : 1.0f);
    resolve.ContactParameters = Float4(smoothShadows && settings && settings->UseContactShadows ? Math::Clamp(settings->ContactShadowLength, 0.0f, 200.0f) : 0.0f,
        settings ? Math::Max(settings->ContactShadowThickness, 0.01f) : 1.0f,
        settings ? Math::Max(settings->ContactShadowFadeDistance, 0.01f) : 2000.0f, 0);
    resolve.CameraPosition = Float3(view->LocalTransform.Translation);
    resolve.IsOrthographic = view->Projection.M34 == 0.0f;
    resolve.CameraForward = Float3(Vector3::Transform(Vector3::Forward, view->LocalTransform.Orientation));
    resolve.LightRadiance = Float4(LightColor.R, LightColor.G, LightColor.B, LightIntensity);
    resolve.ShadowFilter = Float4(lightDepthRange,
        Math::Abs(view->Projection.M22) * 0.5f * depth->Height(),
        Math::Tan(Math::Clamp(angularRadius, 0.0f, 45.0f) * DegreesToRadians), filterRadius);
    resolve.NearLightViewProjection = data.NearLightViewProjection;
    resolve.ShadowTiles[0] = data.ShadowTiles[0];
    resolve.ShadowTiles[1] = data.ShadowTiles[1];
    resolve.ShadowSplit = data.ShadowSplit;

    const auto preparePass = [&](GPUPipelineState*& state, GPUTexture*& target, const char* entry,
                                 const Char* name, PixelFormat format, bool blend = false)
    {
        auto* pixelShader = SelectionShader->GPU->GetPS(entry);
        if (!pixelShader)
            return false;
        if (!state)
            state = device->CreatePipelineState();
        if (!target)
            target = device->CreateTexture(name);
        if (!state || !target)
            return false;
        if (!state->IsValid())
        {
            auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            desc.PS = pixelShader;
            if (blend)
                desc.BlendMode = BlendingMode::AlphaBlend;
            if (state->Init(desc))
                return false;
        }
        return (target->Width() == depth->Width() && target->Height() == depth->Height() && target->GetMemoryUsage()) ||
            !target->Init(GPUTextureDescription::New2D(depth->Width(), depth->Height(), format,
                GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget));
    };

    const bool showCascadeDebug = useConfiguredCascades && cascadeDebugRequested;
    const bool blurDebugRequested = settings && settings->ShowShadowBlurBands && blurBands.HasItems();
    bool cascadeDebugReady = false;
    const bool blockerDebugRequested = settings && settings->ShowBlockerDistance && !showCascadeDebug && !blurDebugRequested;
    const bool showCascadeVisualization = showCascadeDebug || (useConfiguredCascades && (blockerDebugRequested || blurDebugRequested));
    const bool contactDebugRequested = smoothShadows && settings && settings->ShowContactShadows && !showCascadeDebug && !blurDebugRequested && !blockerDebugRequested;
    const bool distanceDebugRequested = smoothShadows && settings && settings->ShowShadowDistanceField && !showCascadeDebug && !blurDebugRequested && !blockerDebugRequested && !contactDebugRequested;
    bool distanceDebugReady = false;
    const bool celRequested = settings && settings->ShowCelShading && !showCascadeDebug && !blurDebugRequested && !blockerDebugRequested && !contactDebugRequested;
    bool needsScreenFilter = blurNeedsScreenFilter;
    GPUTexture* visibilityTexture = nullptr;
    // Shared resolve body; cel invokes it only AFTER the distance field is complete.
    const auto drawLightingResolve = [&](bool cel)
    {
        auto* baseColor = resources.Get<GPUTexture>(TEXT("Scene.BaseColor"));
        auto* material = resources.Get<GPUTexture>(TEXT("Scene.Material"));
        auto* emissive = resources.Get<GPUTexture>(TEXT("Scene.Emissive"));
        if (!sceneColor || !baseColor || !receiverNormals || !material || !emissive || !haveResolveConstants || !ResolveState || !ResolveState->IsValid())
            return;
        context->ResetSR();
        context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
        context->SetRenderTarget(sceneColor->View());
        resolve.UseFilteredShadow = visibilityTexture != nullptr;
        resolve.DebugOptions.X = blurDebugRequested ? 5.0f : blockerDebugRequested ? 3.0f : contactDebugRequested ? 4.0f : cel ? 2.0f : settings && settings->ShowWhiteSurfaceLighting ? 1.0f : 0.0f;
        if (blockerDebugRequested || blurDebugRequested)
        {
            // The final resolve samples the full geometry depth, never a reduced band.
            // Configured bands rebuild this projection after the initial resolve setup.
            resolve.LightViewProjection = data.LightViewProjection;
            resolve.ShadowTiles[0] = data.ShadowTiles[0];
            resolve.ShadowSplit = Float4(0, 0, 0, GetShadowDepthPrecisionBias(lightDepth->Format()));
            resolve.ShadowFilter.X = lightContexts[0].View.Far - lightContexts[0].View.Near;
            resolve.ReductionFootprint = Float4::Zero;
            resolve.DebugOptions.W = Math::Max(settings->BlockerDistanceDisplayRange, 0.01f);

        }
        resolve.CelParameters = Float4(settings ? (float)Math::Clamp(settings->CelShadingBands, 2, 16) : 2.0f,
            cel && distanceDebugReady ? 1.0f : 0.0f, settings ? settings->ShadowDistanceDisplayRange : 32.0f, settings ? Math::Clamp(settings->ShadowFillStrength, 0.0f, 1.0f) : 0.5f);
        context->UpdateCB(resolveConstants, &resolve);
        context->BindCB(3, resolveConstants);
        context->BindSR(2, depth);
        context->BindSR(7, lightDepth);
        context->BindSR(8, receiverNormals);
        if (BlurBandSettings) context->BindSR(0, BlurBandSettings->View());
        context->BindSR(9, baseColor);
        context->BindSR(10, material);
        context->BindSR(11, emissive);
        if (visibilityTexture)
        {
            context->BindSR(13, visibilityTexture);
            resources.SetOutput(TEXT("Shadow.Visibility"), visibilityTexture);
        }
        if (cel && distanceDebugReady)
        {
            context->BindSR(14, ShadowDistance);
            context->BindSR(15, ShadowDistanceReceivers);
        }
        context->SetState(ResolveState);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->ResetSR();
    };
    GPUTexture* publishedDepth = useConfiguredCascades ? nullptr : lightDepth;
    if (useConfiguredCascades)
    {
        // Clear visibility to fully lit. Draw far to near so each map can fade into the one behind it.
        resolve.CascadeRange.W = 1.0f;
        const bool drawMask = sceneColor && receiverNormals && haveResolveConstants &&
            (showCascadeVisualization ?
                preparePass(CascadeDebugState, CascadeDebug, "PS_V2CascadeShadowMask", TEXT("V2 Cascade Debug"), PixelFormat::R8G8B8A8_UNorm, true) :
                preparePass(smoothShadows ? CascadeMaskState : PixelCascadeMaskState, ShadowMask, "PS_V2CascadeShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float, smoothShadows));
        if (drawMask)
        {
            if (showCascadeVisualization)
            {
                context->Clear(CascadeDebug->View(), blockerDebugRequested ? Color(0, 0, 0, 1) : Color(0.08f, 0.08f, 0.08f, 1.0f));
                cascadeDebugReady = true;
            }
            else
            {
                context->Clear(ShadowMask->View(), Color(1.0f, 0.5f, 0.0f, 0.0f));
                visibilityTexture = ShadowMask;
            }
        }

        if (drawMask)
        {
            struct CascadeSlice
            {
                BoundingBox Bounds;
                int32 Resolution;
                GPUTexture* Depth = nullptr;
                Float4 Range;
                Float3 Parameters; // Depth bias, normal bias, original array index.
                CascadeFilter Filter;
            };
            Array<CascadeSlice, InlinedAllocation<8>> slices;
            Matrix inverseProjection;
            Matrix::Invert(view->Projection, inverseProjection);
            Float3 lightCamera;
            Float3::TransformCoordinate(resolve.CameraPosition, lightView, lightCamera);
            const Float3 sceneRadius = Float3::Max(
                Float3::Abs(Float3(lightBounds.Minimum) - lightCamera),
                Float3::Abs(Float3(lightBounds.Maximum) - lightCamera));
            float visibleFar = sceneRadius.Length() + 1.0f;

            // A finite far plane is a rectangle. Its corner radius includes the
            // screen edges without making shell boundaries depend on camera orientation.
            float cameraRadius = 0.0f;
            bool finiteFar = true;
            for (int32 corner = 0; corner < 4; corner++)
            {
                Float3 point;
                Float3::TransformCoordinate(Float3(corner & 1 ? 1.0f : -1.0f,
                    corner & 2 ? 1.0f : -1.0f, GPU_DEPTH_RANGE_MAX), inverseProjection, point);
                const float radius = point.Length();
                finiteFar &= radius > 0.0f && radius < MAX_float;
                cameraRadius = Math::Max(cameraRadius, radius);
            }
            if (finiteFar)
                visibleFar = Math::Min(visibleFar, cameraRadius);

            const int32 lastCascade = settings->ShadowCascades.Count() - 1;
            float previousEnd = 0.0f;
            float previousFade = previousEnd;
            for (int32 i = 0; i < settings->ShadowCascades.Count(); i++)
            {
                const bool terminal = i == lastCascade;
                const float end = terminal ? visibleFar : settings->ShadowCascades[i].EndDistance;
                const float rangeStart = terminal ? previousFade : previousEnd;
                if (!(end > rangeStart && end <= MAX_float))
                    continue;
                const float fitEnd = Math::Min(end, visibleFar);
                if (fitEnd <= previousFade)
                    break;

                CascadeSlice slice;
                const float blend = terminal || !smoothShadows ? 0.0f : Math::Clamp(settings->ShadowCascades[i].BlendFraction, 0.0f, 1.0f);
                const float fade = terminal ? end : Math::Lerp(previousEnd, end, 1.0f - blend);
                if (FitShadowSphere(resolve.CameraPosition, lightView, lightBounds,
                    fitEnd, filterRadius + maxNormalBias, SharedCascadeDepth->Width(), slice.Bounds))
                {
                    const int32 requestedSize = settings->ShadowCascades[i].TextureSize;
                    slice.Resolution = requestedSize > 0 ? Math::Clamp(requestedSize, FarShadowResolution, SharedCascadeDepth->Width()) : SharedCascadeDepth->Width();
                    slice.Range = Float4(previousFade, fade, end, 0.0f);
                    slice.Parameters = Float3(
                        Math::Clamp(settings->ShadowCascades[i].DepthBias, 0.0f, 10.0f),
                        Math::Clamp(settings->ShadowCascades[i].NormalBias, 0.0f, 10.0f), (float)i);
                    slice.Filter = cascadeFilters[i];
                    slices.Add(slice);
                    previousEnd = end;
                    previousFade = fade;
                }
            }

            // All bands sample ONE light projection. Their radial ranges and filters
            // affect resolve only; they never trigger another geometry depth draw.
            if (!slices.IsEmpty())
            {
                nearBounds = slices[0].Bounds;
                for (int32 i = 1; i < slices.Count(); i++)
                {
                    nearBounds.Minimum = Vector3::Min(nearBounds.Minimum, slices[i].Bounds.Minimum);
                    nearBounds.Maximum = Vector3::Max(nearBounds.Maximum, slices[i].Bounds.Maximum);
                }
                lightDepth = SharedCascadeDepth;
                publishedDepth = lightDepth;
                const Float3 minimum(nearBounds.Minimum);
                const Float3 maximum(nearBounds.Maximum);
                const float size = (float)SharedCascadeDepth->Width();
                Matrix lightProjection;
                Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, lightProjection);
                auto& lightContext = lightContexts[0];
                tileViewports[0] = Viewport(0, 0, size, size);
                lightContext.Buffers = LightDepthBuffers;
                lightContext.View.Pass = DrawPass::Depth;
                lightContext.View.Flags = ViewFlags::None;
                lightContext.View.Direction = direction;
                lightContext.View.SetUp(lightView, lightProjection);
                lightContext.View.PrevViewProjection = lightContext.View.ViewProjection();
                lightContext.View.Near = minimum.Z;
                lightContext.View.Far = maximum.Z;
                lightContext.View.PrepareCache(lightContext, size, size, Float2::Zero);
                Matrix::Transpose(lightContext.View.ViewProjection(), data.LightViewProjection);
                data.ShadowTiles[0] = Float4(0, 0, size, size);
                data.CameraPosition = Float4(resolve.CameraPosition, 0.0f);
                data.CameraForward = Float4(resolve.CameraForward, 0.0f);
                data.HasLightDepth = 1;
                data.ReceiverRange = Float4(0.0f, visibleFar, 0.0f, 0.0f);
                LastCascadeCount = slices.Count();
                if (drawMask)
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Shared Light Depth");
                    context->ResetRenderTarget();
                    context->ResetSR();
                    context->ClearDepth(lightDepth->View());
                    drawSelectedDepth(1);
                }
                Array<GPUTexture*> usedBandMaps;
                if (!showCascadeDebug || blurDebugRequested)
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Depth LODs");
                    // Build larger requested maps first so smaller bands can reuse
                    // their reductions. Equal sizes always share the same texture.
                    Array<int32> order;
                    for (int32 i = 0; i < slices.Count(); i++)
                        order.Add(i);
                    for (int32 i = 1; i < order.Count(); i++)
                    {
                        const int32 index = order[i];
                        int32 j = i;
                        while (j > 0 && slices[order[j - 1]].Resolution < slices[index].Resolution)
                        {
                            order[j] = order[j - 1];
                            j--;
                        }
                        order[j] = index;
                    }
                    GPUTexture* previous = lightDepth;
                    for (int32 index : order)
                    {
                        auto& slice = slices[index];
                        slice.Depth = GetBandDepth(context, previous, slice.Resolution, usedBandMaps);
                        if (!slice.Depth)
                            return StageResult::Failed;
                        previous = slice.Depth;
                        resources.SetOutput(String::Format(TEXT("Shadow.BandDepth.{0}"), (int32)slice.Parameters.Z), slice.Depth);
                    }
                }
                // Dispose resolutions no longer needed after editing the band list.
                for (int32 i = BandDepthMaps.Count() - 1; i >= 0; i--)
                {
                    if (!usedBandMaps.Contains(BandDepthMaps[i].Texture))
                    {
                        SAFE_DELETE_GPU_RESOURCE(BandDepthMaps[i].Texture);
                        BandDepthMaps.RemoveAt(i);
                    }
                }
                for (int32 i = slices.Count() - 1; i >= 0; i--)
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Band Resolve");
                    const auto& slice = slices[i];
                    auto* bandDepth = slice.Depth ? slice.Depth : lightDepth;
                    const float bandSize = (float)bandDepth->Width();
                    const float worldTexel = Math::Max(maximum.X - minimum.X, maximum.Y - minimum.Y) / bandSize;
                    if (!drawMask)
                        continue;
                    data.ShadowSplit.W = GetShadowDepthPrecisionBias(bandDepth->Format()) +
                        slice.Parameters.X * worldTexel / (maximum.Z - minimum.Z);
                    data.CameraPosition.W = slice.Parameters.Y * worldTexel;
                    data.FilterRadius = slice.Filter.AdaptiveRadius;
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Resolve Cascade Mask");
                        ResolveConstants cascadeResolve = resolve;
                        cascadeResolve.LightViewProjection = data.LightViewProjection;
                        cascadeResolve.ShadowTiles[0] = Float4(0, 0, bandSize, bandSize);
                        cascadeResolve.ShadowSplit = data.ShadowSplit;
                        const Float2 footprint = GetBandDepthFootprint(bandDepth);
                        cascadeResolve.ReductionFootprint = Float4(footprint.X, footprint.Y, 0, 0);
                        cascadeResolve.ShadowFilter.X = maximum.Z - minimum.Z;
                        cascadeResolve.ShadowFilter.Z = slice.Filter.AngularTangent;
                        cascadeResolve.ShadowFilter.W = slice.Filter.AdaptiveRadius;
                        cascadeResolve.BlockerParameters.X = slice.Filter.PCSS ? 1.0f : 0.0f;
                        if (blurBands.IsEmpty()) needsScreenFilter |= slice.Filter.ScreenRadius > 0.5f;
                        cascadeResolve.CascadeRange = slice.Range;
                        if (blurDebugRequested)
                            cascadeResolve.DebugOptions.X = 5.0f;
                        if (blockerDebugRequested)
                        {
                            // Evaluate distance on THIS band's map and composite the
                            // display with exactly the existing cascade fade weight.
                            cascadeResolve.DebugOptions.X = 3.0f;
                            cascadeResolve.DebugOptions.W = Math::Max(settings->BlockerDistanceDisplayRange, 0.01f);
                        }
                        cascadeResolve.CascadeParameters = Float4(data.CameraPosition.W, slice.Parameters.Z,
                            showCascadeDebug ? (blurDebugRequested ? 2.0f : 1.0f) : 0.0f, slice.Filter.ScreenRadius);
                        context->UpdateCB(resolveConstants, &cascadeResolve);
                        context->BindCB(3, resolveConstants);
                        context->BindSR(2, depth);
                        context->BindSR(7, bandDepth);
                        context->BindSR(8, receiverNormals);
                        if (BlurBandSettings) context->BindSR(0, BlurBandSettings->View());
                        context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                        context->SetRenderTarget(showCascadeVisualization ? CascadeDebug->View() : ShadowMask->View());
                        context->SetState(showCascadeVisualization ? CascadeDebugState : smoothShadows ? CascadeMaskState : PixelCascadeMaskState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                        context->ResetSR();
                    }
                }
            }
        }
    }


    if (!showCascadeVisualization && sceneColor && lightDepth)
    {
        if (!useConfiguredCascades)
            drawSelectedDepth(tierCount);

        auto* baseColor = resources.Get<GPUTexture>(TEXT("Scene.BaseColor"));
        auto* material = resources.Get<GPUTexture>(TEXT("Scene.Material"));
        auto* emissive = resources.Get<GPUTexture>(TEXT("Scene.Emissive"));
        auto* pixelShader = SelectionShader->GPU->GetPS("PS_V2DirectionalLighting");
        if (baseColor && receiverNormals && material && emissive && pixelShader && haveResolveConstants)
        {
            if (!ResolveState)
                ResolveState = device->CreatePipelineState();
            if (ResolveState && !ResolveState->IsValid())
            {
                auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
                desc.PS = pixelShader;
                desc.BlendMode = BlendingMode::AlphaBlend;
                ResolveState->Init(desc);
            }
            if (ResolveState && ResolveState->IsValid())
            {
                const bool distanceReady = distanceDebugRequested &&
                    preparePass(ShadowDistanceReceiverState, ShadowDistanceReceivers, "PS_V2ShadowDistanceReceivers", TEXT("V2 Shadow Distance Receivers"), PixelFormat::R16G16B16A16_Float) &&
                    preparePass(ShadowDistanceJumpState, ShadowDistanceSeeds[0], "PS_V2ShadowDistanceJump", TEXT("V2 Shadow Distance Seeds A"), PixelFormat::R32G32B32A32_Float) &&
                    preparePass(ShadowDistanceJumpState, ShadowDistanceSeeds[1], "PS_V2ShadowDistanceJump", TEXT("V2 Shadow Distance Seeds B"), PixelFormat::R32G32B32A32_Float) &&
                    preparePass(ShadowDistanceResolveState, ShadowDistance, "PS_V2ShadowDistanceResolve", TEXT("V2 Shadow Distance"), PixelFormat::R32_Float) &&
                    preparePass(ShadowDistanceResolveState, ShadowDistanceDebug, "PS_V2ShadowDistanceResolve", TEXT("V2 Shadow Distance Debug"), PixelFormat::R8G8B8A8_UNorm);
                resolve.DebugOptions.Y = distanceReady ? 1.0f : 0.0f;
                resolve.DebugOptions.W = settings ? settings->ShadowDistanceDisplayRange : 32.0f;
                const bool applyScreenFilter = distanceReady || (useConfiguredCascades ? needsScreenFilter : blurBands.HasItems() ? blurNeedsScreenFilter : filterRadius == 0 && screenSpaceFilter);
                if (applyScreenFilter &&
                    (useConfiguredCascades ? visibilityTexture != nullptr :
                        preparePass(ShadowMaskState, ShadowMask, "PS_V2ShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float)) &&
                    preparePass(ShadowFilterState, FilteredShadowMask, "PS_V2ShadowFilter", TEXT("V2 Filtered Shadow Mask"), PixelFormat::R16_Float))
                {
                    if (distanceReady)
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Distance Receiver Geometry");
                        ResolveConstants distanceResolve = resolve;
                        // Cascades changed data after the original resolve constants were built.
                        // Cross camera receivers against the actual full-resolution shared depth projection.
                        distanceResolve.LightViewProjection = data.LightViewProjection;
                        distanceResolve.ShadowTiles[0] = data.ShadowTiles[0];
                        distanceResolve.ShadowSplit.W = GetShadowDepthPrecisionBias(lightDepth->Format());
                        distanceResolve.ReductionFootprint = Float4::Zero;
                        distanceResolve.ShadowFilter.W = 0.0f; // Geometry only; core classification follows the completed filter.
                        context->ResetSR();
                        context->UpdateCB(resolveConstants, &distanceResolve);
                        context->BindCB(3, resolveConstants);
                        context->BindSR(2, depth);
                        context->BindSR(7, lightDepth);
                        context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                        context->SetRenderTarget(ShadowDistanceReceivers->View());
                        context->SetState(ShadowDistanceReceiverState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                        context->ResetSR();
                    }
                    context->UpdateCB(resolveConstants, &resolve);
                    context->BindCB(3, resolveConstants);
                    context->BindSR(2, depth);
                    context->BindSR(8, receiverNormals);
                    context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                    if (!useConfiguredCascades)
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Mask");
                        if (BlurBandSettings) context->BindSR(0, BlurBandSettings->View());
                        context->BindSR(7, lightDepth);
                        context->SetRenderTarget(ShadowMask->View());
                        context->SetState(ShadowMaskState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                    }
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Filter");
                        context->BindSR(12, ShadowMask);
                        context->SetRenderTarget(FilteredShadowMask->View());
                        context->SetState(ShadowFilterState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                    }
                    visibilityTexture = FilteredShadowMask;
                    if (distanceReady)
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Distance Field");
                        context->ResetSR();
                        {
                            PROFILE_GPU_CPU_NAMED("V2 Shadow Core Seeds");
                            resolve.ShadowSampling.Z = 1.0f;
                            context->UpdateCB(resolveConstants, &resolve);
                            context->BindCB(3, resolveConstants);
                            context->BindSR(2, depth);
                            context->BindSR(13, FilteredShadowMask);
                            context->BindSR(15, ShadowDistanceReceivers);
                            // Target 0 is scratch; target 1 stores seeds. Scratch is overwritten by JFA.
                            GPUTextureView* seedTargets[] = { ShadowDistanceSeeds[1]->View(), ShadowDistanceSeeds[0]->View() };
                            context->SetRenderTarget(nullptr, ToSpan(seedTargets, 2));
                            context->SetState(ShadowFilterState);
                            context->DrawFullscreenTriangle();
                            context->ResetRenderTarget();
                            context->ResetSR();
                            resolve.ShadowSampling.Z = 0.0f;
                        }
                        int32 step = 1;
                        while (step < Math::Max(depth->Width(), depth->Height()))
                            step *= 2;
                        int32 source = 0;
                        // One extra unit-step refinement after the logarithmic jump sequence (JFA+1).
                        for (step /= 2; step >= 1; step /= 2)
                        {
                            resolve.DebugOptions.Z = (float)step;
                            context->UpdateCB(resolveConstants, &resolve);
                            context->BindSR(14, ShadowDistanceSeeds[source]);
                            context->BindSR(2, depth);
                            context->BindSR(15, ShadowDistanceReceivers);
                            context->BindSR(13, FilteredShadowMask);
                            context->SetRenderTarget(ShadowDistanceSeeds[1 - source]->View());
                            context->SetState(ShadowDistanceJumpState);
                            context->DrawFullscreenTriangle();
                            context->ResetRenderTarget();
                            context->ResetSR();
                            source = 1 - source;
                        }
                        resolve.DebugOptions.Z = 1.0f;
                        context->UpdateCB(resolveConstants, &resolve);
                        context->BindSR(14, ShadowDistanceSeeds[source]);
                        context->BindSR(2, depth);
                        context->BindSR(15, ShadowDistanceReceivers);
                        context->BindSR(13, FilteredShadowMask);
                        context->SetRenderTarget(ShadowDistanceSeeds[1 - source]->View());
                        context->SetState(ShadowDistanceJumpState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                        context->ResetSR();
                        source = 1 - source;
                        context->BindSR(2, depth);
                        context->BindSR(12, ShadowMask);
                        context->BindSR(14, ShadowDistanceSeeds[source]);
                        context->BindSR(2, depth);
                        context->BindSR(15, ShadowDistanceReceivers);
                        context->BindSR(13, FilteredShadowMask);
                        GPUTextureView* targets[] = { ShadowDistance->View(), ShadowDistanceDebug->View() };
                        context->SetRenderTarget(nullptr, ToSpan(targets, 2));
                        context->SetState(ShadowDistanceResolveState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                        context->ResetSR();
                        resources.SetOutput(TEXT("Shadow.SignedDistance"), ShadowDistance);
                        resources.SetOutput(TEXT("Shadow.DistanceReceivers"), ShadowDistanceReceivers);
                        resources.SetOutput(TEXT("Shadow.DistanceDebug"), ShadowDistanceDebug);
                        distanceDebugReady = true;
                    }
                }

                if (!celRequested)
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Lighting Resolve");
                    drawLightingResolve(false);
                }
            }
        }
    }

    if (!useConfiguredCascades)
    {
        for (auto& map : BandDepthMaps)
            SAFE_DELETE_GPU_RESOURCE(map.Texture);
        BandDepthMaps.Clear();
    }

    if (publishedDepth)
    {
        resources.Set(TEXT("Shadow.Depth"), publishedDepth);
        resources.SetOutput(TEXT("Shadow.Depth"), publishedDepth);
    }

    if (distanceDebugReady && !celRequested)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Distance Debug");
        context->ResetRenderTarget();
        context->ResetSR();
        context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
        context->Draw(sceneColor, ShadowDistanceDebug);
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.DistanceDebug"), ShadowDistanceDebug);
    }
    else if (!distanceDebugRequested)
    {
        // Debug off has no extra GPU passes and releases the full-resolution work textures.
        for (auto& seed : ShadowDistanceSeeds)
            SAFE_DELETE_GPU_RESOURCE(seed);
        SAFE_DELETE_GPU_RESOURCE(ShadowDistance);
        SAFE_DELETE_GPU_RESOURCE(ShadowDistanceDebug);
        SAFE_DELETE_GPU_RESOURCE(ShadowDistanceReceivers);
    }

    if (celRequested)
    {
        PROFILE_GPU_CPU_NAMED("V2 Cel Shading Resolve");
        drawLightingResolve(true);
    }

    if (cascadeDebugReady)
    {
        context->ResetRenderTarget();
        context->ResetSR();
        context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
        context->Draw(sceneColor, CascadeDebug);
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(blurDebugRequested ? TEXT("Shadow.BlurBandsDebug") : blockerDebugRequested ? TEXT("Shadow.BlockerDistanceDebug") : TEXT("Shadow.CascadeDebug"), CascadeDebug);
        resources.SetOutput(TEXT("Shadow.DebugResource"), CascadeDebug);
    }

    if ((showSelectedCasters || showRejectedCasters) && sceneColor)
    {
        PROFILE_GPU_CPU_NAMED("V2 Expensive Caster Debug");
        if (!CasterBoundsDebugState)
            CasterBoundsDebugState = device->CreatePipelineState();
        auto* vertexShader = SelectionShader->GPU->GetVS("VS_ShadowCasterBoundsDebug");
        auto* pixelShader = SelectionShader->GPU->GetPS("PS_ShadowCasterBoundsDebug");
        if (!CasterBoundsDebugState || !vertexShader || !pixelShader)
            return StageResult::Failed;
        if (!CasterBoundsDebugState->IsValid())
        {
            auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            desc.VS = vertexShader;
            desc.PS = pixelShader;
            desc.PrimitiveTopology = PrimitiveTopologyType::Line;
            desc.DepthEnable = false;
            desc.DepthWriteEnable = false;
            if (CasterBoundsDebugState->Init(desc))
                return StageResult::Failed;
        }
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->UpdateCB(constants, &data);
        context->BindCB(1, constants);
        context->BindSR(3, BoundsBuffer->View());
        context->BindSR(18, SelectionBuffer->View());
        context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
        context->SetRenderTarget(sceneColor->View());
        context->SetState(CasterBoundsDebugState);
        context->DrawInstanced(24, count);
        context->ResetRenderTarget();
        context->ResetSR();
    }

    LastCasterCount = count;
    resources.Set(TEXT("Shadow.CasterSelection"), SelectionBuffer);
    resources.SetOutput(TEXT("Shadow.CasterSelection"), SelectionBuffer);
    resources.SetOutput(TEXT("Shadow.CasterBounds"), BoundsBuffer);
    return StageResult::Completed;
}

void ShadowBoxCasterStage::OnEndFrame(GPUContext* context)
{
    Casters.Clear();
}
