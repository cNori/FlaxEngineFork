// Copyright (c) Wojciech Figat. All rights reserved.

#include "../ShadowBoxCasterStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../RenderPipelineGraphicSettings.h"
#include "../WorldWarpStage.h"
#include "../StaticMeshDrawStage.h"
#include "Engine/Render2D/Render2D.h"
#include "Engine/Render2D/FontAsset.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Content.h"
#include "Engine/Core/Math/BoundingFrustum.h"
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
        uint32 DebugCategory;
        uint32 CameraIndex;
        uint32 Padding[3] = {};
    };
    static_assert(sizeof(CasterBounds) == 48, "Caster bounds must match the shader layout.");

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
        Float4 MaskBlendParameters;
        uint32 MaskBlendInputVisibility;
        uint32 MaskBlendDebugMode;
        Float2 MaskBlendPadding;
    });

    GPU_CB_STRUCT(MaskBlendConstants
    {
        Float4 Parameters;
        uint32 InputVisibility;
        uint32 DebugMode;
        Float2 Padding;
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
        uint32 WriteDebugMask;
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
        float ReceiverBias;
        uint32 ReceiverTraceStep;
        uint32 ReceiverTraceCaster;
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
    SAFE_DELETE_GPU_RESOURCE(DebugState);
    SAFE_DELETE_GPU_RESOURCE(CasterBoundsDebugState);
    SAFE_DELETE_GPU_RESOURCE(ResourceDebugState);
    SAFE_DELETE_GPU_RESOURCE(LightDebugColor);
    SAFE_DELETE_GPU_RESOURCE(LightDebugShadedColor);
    SAFE_DELETE_GPU_RESOURCE(LightDebugShadingState);
    SAFE_DELETE_GPU_RESOURCE(ResourceDebugComposite);
    if (LightDebugDrawStage)
        Delete(LightDebugDrawStage);
    SAFE_DELETE_GPU_RESOURCE(ResolveState);
    SAFE_DELETE_GPU_RESOURCE(ShadowMaskState);
    SAFE_DELETE_GPU_RESOURCE(CascadeMaskState);
    SAFE_DELETE_GPU_RESOURCE(CascadeDebugState);
    SAFE_DELETE_GPU_RESOURCE(CascadeDebug);
    SAFE_DELETE_GPU_RESOURCE(ShadowFilterState);
    SAFE_DELETE_GPU_RESOURCE(ShadowMask);
    SAFE_DELETE_GPU_RESOURCE(FilteredShadowMask);
    SAFE_DELETE_GPU_RESOURCE(BlendReduceState);
    SAFE_DELETE_GPU_RESOURCE(BlendFilterState);
    SAFE_DELETE_GPU_RESOURCE(BlendCombineState);
    SAFE_DELETE_GPU_RESOURCE(BlendResult);
    for (auto*& texture : BlendPyramid)
        SAFE_DELETE_GPU_RESOURCE(texture);
    for (int32 i = 0; i < 2; i++)
    {
        SAFE_DELETE_GPU_RESOURCE(BlendBlurTemp[i]);
        SAFE_DELETE_GPU_RESOURCE(BlendFiltered[i]);
    }
    SAFE_DELETE_GPU_RESOURCE(SelectionReadback);
    SAFE_DELETE_GPU_RESOURCE(IndirectInputs);
    SAFE_DELETE_GPU_RESOURCE(IndirectOutput);
    SAFE_DELETE_GPU_RESOURCE(IndirectArguments);
    SAFE_DELETE_GPU_RESOURCE(DebugMask);
    SAFE_DELETE_GPU_RESOURCE(CandidateOccluderDepth);
    SAFE_DELETE_GPU_RESOURCE(LightVisibilityBuffer);
    SAFE_DELETE_GPU_RESOURCE(ProxyShadowDepth);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverMask);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverNear);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverFar);
    SAFE_DELETE_GPU_RESOURCE(CameraReceiverCasters);
    SAFE_DELETE_GPU_RESOURCE(SelectionStepDebug);
    SAFE_DELETE_GPU_RESOURCE(ReceiverTrace);
    SAFE_DELETE_GPU_RESOURCE(ReceiverTraceProbe);
    SAFE_DELETE_GPU_RESOURCE(ReceiverTraceReadback);
    SAFE_DELETE_GPU_RESOURCE(ReceiverTraceProbeState);
    SAFE_DELETE_GPU_RESOURCE(ProxyDepthState);
    SAFE_DELETE_GPU_RESOURCE(DepthCrossDebugState);
    for (auto& map : ShadowCascadeMaps)
    {
        SAFE_DELETE_GPU_RESOURCE(map.Depth);
    }
    if (LightDepthBuffers)
        Delete(LightDepthBuffers);
    SAFE_DELETE_GPU_RESOURCE(BoundsBuffer);
    SAFE_DELETE_GPU_RESOURCE(SelectionBuffer);
}

void ShadowBoxCasterStage::OnShaderReloading(Asset* asset)
{
    if (DebugState)
        DebugState->ReleaseGPU();
    if (CasterBoundsDebugState)
        CasterBoundsDebugState->ReleaseGPU();
    if (ResourceDebugState)
        ResourceDebugState->ReleaseGPU();
    if (LightDebugShadingState)
        LightDebugShadingState->ReleaseGPU();
    if (ProxyDepthState)
        ProxyDepthState->ReleaseGPU();
    if (DepthCrossDebugState)
        DepthCrossDebugState->ReleaseGPU();
    if (ReceiverTraceProbeState)
        ReceiverTraceProbeState->ReleaseGPU();
    if (ResolveState)
        ResolveState->ReleaseGPU();
    if (ShadowMaskState)
        ShadowMaskState->ReleaseGPU();
    if (CascadeMaskState)
        CascadeMaskState->ReleaseGPU();
    if (CascadeDebugState)
        CascadeDebugState->ReleaseGPU();
    if (ShadowFilterState)
        ShadowFilterState->ReleaseGPU();
    if (BlendReduceState)
        BlendReduceState->ReleaseGPU();
    if (BlendFilterState)
        BlendFilterState->ReleaseGPU();
    if (BlendCombineState)
        BlendCombineState->ReleaseGPU();
}

bool ShadowBoxCasterStage::BuildShadowMaskBlend(GPUContext* context, GPUTexture* visibility, const RenderPipelineGraphicSettings* settings)
{
    auto* device = context->GetDevice();
    auto* constants = SelectionShader->GPU->GetCB(3);
    if (!visibility || !settings || !constants || constants->GetSize() != sizeof(ResolveConstants))
        return false;
    const auto prepareState = [&](GPUPipelineState*& state, const char* entry)
    {
        auto* shader = SelectionShader->GPU->GetPS(entry);
        if (!shader)
            return false;
        if (!state)
            state = device->CreatePipelineState();
        if (!state)
            return false;
        if (!state->IsValid())
        {
            auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            desc.PS = shader;
            if (state->Init(desc))
                return false;
        }
        return true;
    };
    const auto prepareTexture = [&](GPUTexture*& texture, int32 width, int32 height, const Char* name)
    {
        if (!texture)
            texture = device->CreateTexture(name);
        return texture && ((texture->Width() == width && texture->Height() == height && texture->GetMemoryUsage()) ||
            !texture->Init(GPUTextureDescription::New2D(width, height, PixelFormat::R8_UNorm,
                GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget)));
    };
    if (!prepareState(BlendReduceState, "PS_V2ShadowBlendReduce") ||
        !prepareState(BlendFilterState, "PS_V2ShadowBlendFilter") ||
        !prepareState(BlendCombineState, "PS_V2ShadowBlendCombine"))
        return false;
    const auto draw = [&](GPUPipelineState* state, GPUTexture* input, GPUTexture* output, const MaskBlendConstants& data,
                          GPUTexture* low = nullptr, GPUTexture* medium = nullptr)
    {
        context->ResetRenderTarget();
        context->ResetSR();
        ResolveConstants upload = {};
        upload.MaskBlendParameters = data.Parameters;
        upload.MaskBlendInputVisibility = data.InputVisibility;
        upload.MaskBlendDebugMode = data.DebugMode;
        context->UpdateCB(constants, &upload);
        context->BindCB(3, constants);
        context->BindSR(14, input);
        if (low) context->BindSR(15, low);
        if (medium) context->BindSR(16, medium);
        context->SetViewportAndScissors((float)output->Width(), (float)output->Height());
        context->SetRenderTarget(output->View());
        context->SetState(state);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->ResetSR();
    };

    PROFILE_GPU_CPU_NAMED("V2 Multi-resolution Shadow Blend");
    int32 pyramidIndex = 0;
    const auto reduceTo = [&](GPUTexture* input, int32 targetEdge, bool invertVisibility, GPUTexture*& result)
    {
        result = input;
        int32 edge = Math::Max(input->Width(), input->Height());
        while (edge > targetEdge || invertVisibility)
        {
            const int32 nextEdge = Math::Max(targetEdge, (edge + 1) / 2);
            const int32 width = Math::Max(1, (int32)Math::Round((float)result->Width() * nextEdge / edge));
            const int32 height = Math::Max(1, (int32)Math::Round((float)result->Height() * nextEdge / edge));
            if (pyramidIndex >= BlendPyramid.Count())
                BlendPyramid.Add(nullptr);
            auto*& target = BlendPyramid[pyramidIndex++];
            if (!prepareTexture(target, width, height, TEXT("V2 Shadow Mask Reduction")))
                return false;
            MaskBlendConstants data = {};
            data.Parameters = Float4(1.0f / width, 1.0f / height, 0, 0);
            data.InputVisibility = invertVisibility;
            {
                PROFILE_GPU_CPU_NAMED("V2 Shadow Mask Reduce");
                draw(BlendReduceState, result, target, data);
            }
            invertVisibility = false;
            result = target;
            edge = nextEdge;
        }
        return true;
    };
    const int32 sourceEdge = Math::Max(visibility->Width(), visibility->Height());
    const int32 mediumEdge = Math::Min(sourceEdge, Math::Clamp(settings->ShadowBlendMediumResolution, 8, 2048));
    const int32 lowEdge = Math::Min(mediumEdge, Math::Clamp(settings->ShadowBlendLowResolution, 8, 2048));
    GPUTexture* medium;
    GPUTexture* low;
    if (!reduceTo(visibility, mediumEdge, true, medium) || !reduceTo(medium, lowEdge, false, low))
        return false;
    GPUTexture* inputs[2] = { low, medium };
    const float radii[2] = { settings->ShadowBlendLowFilterRadius, settings->ShadowBlendMediumFilterRadius };
    for (int32 i = 0; i < 2; i++)
    {
        auto* input = inputs[i];
        if (!prepareTexture(BlendBlurTemp[i], input->Width(), input->Height(), TEXT("V2 Shadow Mask Blur Temp")) ||
            !prepareTexture(BlendFiltered[i], input->Width(), input->Height(), TEXT("V2 Filtered Shadow Mask Scale")))
            return false;
        MaskBlendConstants data = {};
        data.Parameters = Float4(1.0f / input->Width(), 0, Math::Clamp(radii[i], 0.0f, 8.0f), 0);
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Mask Blur Horizontal");
            draw(BlendFilterState, input, BlendBlurTemp[i], data);
        }
        data.Parameters = Float4(0, 1.0f / input->Height(), data.Parameters.Z, 0);
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Mask Blur Vertical");
            draw(BlendFilterState, BlendBlurTemp[i], BlendFiltered[i], data);
        }
    }
    if (!prepareTexture(BlendResult, visibility->Width(), visibility->Height(), TEXT("V2 Combined Shadow Mask")))
        return false;
    MaskBlendConstants data = {};
    data.Parameters = Float4(1.0f / visibility->Width(), 1.0f / visibility->Height(), 0, 0);
    data.DebugMode = (uint32)settings->ShadowMaskBlendDebug;
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Mask Combine");
        draw(BlendCombineState, visibility, BlendResult, data, BlendFiltered[0], BlendFiltered[1]);
    }
    return true;
}

void ShadowBoxCasterStage::SetProjection(const Vector3& lightDirection, float distance)
{
    LightDirection = lightDirection;
    LightDirection.Normalize();
    ProjectionDistance = distance;
}

int32 ShadowBoxCasterStage::GetAllocatedShadowMapResolution() const
{
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

void ShadowBoxCasterStage::PrepareShadowCascadeMaps(GPUDevice* device, const RenderPipelineGraphicSettings* settings)
{
    const int32 count = settings ? settings->ShadowCascades.Count() : 0;
    for (int32 i = count; i < ShadowCascadeMaps.Count(); i++)
    {
        SAFE_DELETE_GPU_RESOURCE(ShadowCascadeMaps[i].Depth);
    }
    ShadowCascadeMaps.Resize(count);

    const int32 limit = Math::Min(MaximumShadowResolution, device->Limits.MaximumTexture2DSize);
    for (int32 i = 0; i < count; i++)
    {
        auto& map = ShadowCascadeMaps[i];
        const int32 resolution = Math::Min(Math::Max(settings->ShadowCascades[i].TextureSize, FarShadowResolution), limit);
        if (map.RequestedResolution == resolution && (!map.Depth || map.Depth->GetMemoryUsage()))
            continue;

        SAFE_DELETE_GPU_RESOURCE(map.Depth);
        map.RequestedResolution = resolution;
        map.Depth = device->CreateTexture(TEXT("V2 Shadow Cascade Depth"));
        if (map.Depth && !InitShadowDepthMap(device, map.Depth, resolution, 0))
        {
            SAFE_DELETE_GPU_RESOURCE(map.Depth);
        }
    }
}

StageResult ShadowBoxCasterStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    Casters.Clear();
    LastCasterCount = 0;
    LastRejectedProxyCount = 0;
    LastProxyCasterCount = 0;
    LastSelectionDispatchCount = 0;
    LastSelectionTierCount = 0;
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
    auto* shader = SelectionShader->GPU->GetCS("CS_ShadowCasterSelection");
    auto* constants = SelectionShader->GPU->GetCB(1);
    if (!shader || !constants || constants->GetSize() != sizeof(SelectionConstants))
        return StageResult::Skipped;

    const auto* settings = resources.GetGraphicsSettings();
    const int32 proxyTriangleThreshold = settings ? settings->ShadowProxyTriangleThreshold : ProxyTriangleThreshold;
    const float nearShadowDistance = settings ? settings->NearShadowDistance : NearShadowDistance;
    const int32 requestedShadowResolution = settings ? settings->ShadowMapResolution : ShadowMapResolution;
    const bool adaptiveFilter = settings ? settings->UseAdaptiveShadowFilter : UseAdaptiveShadowFilter;
    const float adaptiveRadius = settings ? settings->AdaptiveShadowRadius : AdaptiveShadowRadius;
    const bool screenSpaceFilter = settings ? settings->UseScreenSpaceShadowFilter : UseScreenSpaceShadowFilter;
    const float angularRadius = settings ? settings->ShadowAngularRadius : ShadowAngularRadius;
    const bool indirectShadowDraws = settings ? settings->UseIndirectShadowDraws : UseIndirectShadowDraws;
    const bool receiverTrace = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::ReceiverShaderSteps;
    const bool receiverMaskDebug = settings && (settings->ShadowResourceDebug == ShadowResourceDebugView::CameraReceiverCoverage ||
        settings->ShadowResourceDebug == ShadowResourceDebugView::CameraReceiverLightDepth ||
        settings->ShadowResourceDebug == ShadowResourceDebugView::CameraReceiverLightCross);
    const bool showCameraAndLight = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::CameraAndLight;
    const bool hiddenContributions = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::HiddenCasterContributions;
    const bool lightVisibilityDebug = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::LightCasterVisibility;
    const bool depthCrossDebug = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::ReceiverDepthCross;
    const bool colorFootprints = settings && settings->ShadowResourceDebug == ShadowResourceDebugView::CasterSelection;
    const bool showResourceDebug = settings && settings->ShadowResourceDebug != ShadowResourceDebugView::None && !colorFootprints && !hiddenContributions;
    const bool showReceiverHits = !receiverTrace && !showCameraAndLight && (colorFootprints || hiddenContributions || ShowReceiverHits || (settings && settings->ShowShadowReceiverHits));
    // The old per-camera-pixel/per-caster search is diagnostic only when the
    // shared light-space receiver mask is available. It must not run normally.
    const bool legacyReceiverDiagnostics = receiverTrace || showReceiverHits || depthCrossDebug;
    const bool needsProxyDepth = legacyReceiverDiagnostics ||
        (settings && settings->ShadowResourceDebug == ShadowResourceDebugView::ProxyShadowDepth);
    const bool filterDebugCaster = settings && settings->ShadowDebugCasterName.HasChars();
    const bool showSelectedCasters = !showCameraAndLight && (ShowSelectedExpensiveCasters || (settings && settings->ShowSelectedExpensiveShadowCasters));
    const bool showRejectedCasters = !showCameraAndLight && (ShowRejectedExpensiveCasters || (settings && settings->ShowRejectedExpensiveShadowCasters));

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
    const auto append = [&](const Array<IVisualData*>& candidates, uint32 debugCategory)
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
            entry.DebugCategory = debugCategory;
            entry.CameraIndex = debugCategory == 1 ? (uint32)cameraIndex : MAX_uint32;
            if (filterDebugCaster && actor->GetName() == settings->ShadowDebugCasterName)
                entry.DebugCategory |= 128u;
            if (colorFootprints)
                entry.DebugCategory |= 0x80000000u | ((GetHash(actor->GetID()) & 0x7fffffu) << 8);
            bounds.Add(entry);
            Casters.Add(object);
        }
    };
    append(objects, 1);
    if (FrustumStage)
        append(FrustumStage->GetRejectedObjects(), 4);
    if (OcclusionStage)
        append(OcclusionStage->GetOccludedObjects(), 2);
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
        if (settings && !settings->ShadowCascades.IsEmpty() && ShadowCascadeMaps.IsEmpty() && LightDepthBuffers)
            LightDepthBuffers->DepthBuffer->ReleaseGPU();
        PrepareShadowCascadeMaps(device, settings);
        for (const auto& map : ShadowCascadeMaps)
        {
            if (map.Depth)
            {
                lightDepth = map.Depth;
                smallestCascade = Math::Min(smallestCascade, map.Depth->Width());
            }
        }
        useConfiguredCascades = lightDepth != nullptr;
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
        ? Math::Clamp(adaptiveRadius, 0.0f, 4.0f) : 0.0f;
    float filterRadius = useConfiguredCascades ? 0.0f : inheritedRadius;
    struct CascadeFilter
    {
        float AdaptiveRadius;
        float ScreenRadius;
        float AngularTangent;
    };
    Array<CascadeFilter, InlinedAllocation<8>> cascadeFilters;
    float maxNormalBias = 0.0f;
    if (useConfiguredCascades)
    {
        for (const auto& cascade : settings->ShadowCascades)
        {
            const bool inherit = cascade.FilterMode == ShadowCascadeFilterMode::Inherit;
            const auto mode = inherit ?
                (inheritedRadius > 0 ? ShadowCascadeFilterMode::Adaptive :
                    (screenSpaceFilter ? ShadowCascadeFilterMode::ScreenSpace : ShadowCascadeFilterMode::Hard)) :
                cascade.FilterMode;
            const float radius = Math::Clamp(cascade.FilterRadius, 0.0f, 4.0f);
            CascadeFilter filter;
            filter.AdaptiveRadius = mode == ShadowCascadeFilterMode::Adaptive ? (inherit ? inheritedRadius : radius) : 0.0f;
            filter.ScreenRadius = mode == ShadowCascadeFilterMode::ScreenSpace ? (inherit ? 4.0f : radius) : 0.0f;
            const float angle = Math::Clamp(inherit ? angularRadius : cascade.AngularRadius, 0.0f, 45.0f);
            filter.AngularTangent = Math::Tan(angle * DegreesToRadians);
            cascadeFilters.Add(filter);
            filterRadius = Math::Max(filterRadius, filter.AdaptiveRadius);
            maxNormalBias = Math::Max(maxNormalBias, Math::Clamp(cascade.NormalBias, 0.0f, 10.0f));
        }
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

    Array<IVisualData*> lightDebugObjects;
    if (showCameraAndLight)
        lightDebugObjects = Casters;

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
    auto* debugColor = showReceiverHits ? sceneColor : nullptr;
    uint32 traceCaster = MAX_uint32;
    uint32 traceHash = MAX_uint32;
    String traceName = TEXT("No matching caster collected");
    if (receiverTrace)
    {
        for (int32 i = 0; i < count; i++)
        {
            auto* actor = static_cast<StaticModel*>(static_cast<WarpedVisualData*>(Casters[i])->GetIVD_Source());
            if (!bounds[i].UseProxy || (filterDebugCaster && actor->GetName() != settings->ShadowDebugCasterName))
                continue;
            const uint32 hash = GetHash(actor->GetID());
            if (traceCaster == MAX_uint32 || hash < traceHash)
            {
                traceCaster = i;
                traceHash = hash;
                traceName = actor->GetName();
            }
        }
        if (!ReceiverTrace)
            ReceiverTrace = device->CreateTexture(TEXT("V2 Receiver Kernel Trace"));
        if (!ReceiverTrace || ((ReceiverTrace->Width() != depth->Width() || ReceiverTrace->Height() != depth->Height() || !ReceiverTrace->GetMemoryUsage()) &&
            ReceiverTrace->Init(GPUTextureDescription::New2D(depth->Width(), depth->Height(), PixelFormat::R32G32B32A32_Float,
                GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess))))
            return StageResult::Failed;
        const Float4 clear = settings->ReceiverDebugStep == ShadowReceiverDebugStep::Decision ? Float4(1, 0, 0, 0) : Float4(0, 0, 0, -1);
        context->ClearUA(ReceiverTrace, clear);
        resources.SetOutput(TEXT("Shadow.ReceiverTrace"), ReceiverTrace);
    }
    if (debugColor)
    {
        auto* pixelShader = SelectionShader->GPU->GetPS("PS_ShadowReceiverHits");
        if (!DebugMask)
            DebugMask = device->CreateTexture(TEXT("V2 Shadow Receiver Hits"));
        if (!DebugState)
            DebugState = device->CreatePipelineState();
        if (!pixelShader || !DebugMask || !DebugState)
            return StageResult::Failed;
        if (DebugMask->Width() != depth->Width() || DebugMask->Height() != depth->Height() || !DebugMask->GetMemoryUsage())
        {
            const auto desc = GPUTextureDescription::New2D(depth->Width(), depth->Height(), PixelFormat::R32_UInt,
                GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
            if (DebugMask->Init(desc))
                return StageResult::Failed;
        }
        if (!DebugState->IsValid())
        {
            auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            desc.PS = pixelShader;
            desc.BlendMode = BlendingMode::AlphaBlend;
            if (DebugState->Init(desc))
                return StageResult::Failed;
        }
    }

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
    data.WriteDebugMask = debugColor ? 1 : 0;
    auto* receiverNormals = resources.Get<GPUTexture>(TEXT("Shadow.ReceiverNormals"));
    data.HasReceiverNormals = receiverNormals ? 1 : 0;
    data.FilterRadius = filterRadius;
    data.CameraPosition = Float4(Float3(view->LocalTransform.Translation), 0);
    data.ReceiverTraceStep = receiverTrace ? Math::Clamp((int32)settings->ReceiverDebugStep, 0, 11) : 0;
    data.ReceiverTraceCaster = traceCaster;
    data.ReceiverBias = settings ? Math::Clamp(settings->ShadowSelectionReceiverBias, 0.0f, 100.0f) : 0.1f;
    auto* cameraVisibility = resources.Get<GPUBuffer>(TEXT("Occlusion.Visibility"));
    data.CasterDebugFlags = (showSelectedCasters ? 1u : 0u) | (showRejectedCasters ? 2u : 0u) |
        (filterDebugCaster ? 4u : 0u) | (colorFootprints ? 8u : 0u) | (hiddenContributions ? 16u : 0u) | (cameraVisibility ? 64u : 0u) | (receiverTrace ? 128u : 0u);

    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Upload Caster Bounds");
        context->UpdateBuffer(BoundsBuffer, bounds.Get(), count * sizeof(CasterBounds));
    }
    constexpr int32 selectionMapResolution = 1024;
    // Build one fixed-size light-space occluder map. Cascades retain their own final depth maps.
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

    // Proxy depth is only needed to inspect the legacy receiver-ray tests.
    // Normal selection uses the camera-to-light mask and real cheap depth below.
    if (candidateData.HasLightDepth && needsProxyDepth)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow 2 Proxy Depth");
        if (!ProxyShadowDepth)
            ProxyShadowDepth = device->CreateTexture(TEXT("V2 Shadow Proxy Depth"));
        if (!ProxyDepthState)
            ProxyDepthState = device->CreatePipelineState();
        auto* proxyVS = SelectionShader->GPU->GetVS("VS_V2ShadowProxyDepth");
        if (!ProxyShadowDepth || !ProxyDepthState || !proxyVS ||
            ((!ProxyShadowDepth->GetMemoryUsage() || ProxyShadowDepth->Width() != selectionMapResolution) &&
                !InitShadowDepthMap(device, ProxyShadowDepth, selectionMapResolution, 0)))
            return StageResult::Failed;
        if (!ProxyDepthState->IsValid())
        {
            auto desc = GPUPipelineState::Description::Default;
            desc.VS = proxyVS;
            desc.PS = nullptr;
            desc.CullMode = CullMode::TwoSided;
            if (ProxyDepthState->Init(desc))
                return StageResult::Failed;
        }
        context->ResetRenderTarget();
        context->CopyResource(ProxyShadowDepth, CandidateOccluderDepth);
        context->UpdateCB(constants, &candidateData);
        context->BindCB(1, constants);
        context->BindSR(3, BoundsBuffer->View());
        context->BindSR(21, LightVisibilityBuffer->View());
        context->SetViewportAndScissors((float)selectionMapResolution, (float)selectionMapResolution);
        context->SetRenderTarget(ProxyShadowDepth->View(), static_cast<GPUTextureView*>(nullptr));
        context->SetState(ProxyDepthState);
        context->DrawInstanced(36, count);
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.ProxyDepth"), ProxyShadowDepth);
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
        auto* projectReceivers = SelectionShader->GPU->GetCS("CS_V2ProjectCameraReceivers");
        auto* intersectReceivers = SelectionShader->GPU->GetCS("CS_V2ReceiverMaskCasters");
        if (!projectReceivers || !intersectReceivers)
            return StageResult::Failed;
        const uint32 clear[4] = {};
        const uint32 emptyNear[4] = { MAX_uint32, MAX_uint32, MAX_uint32, MAX_uint32 };
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->ClearUA(CameraReceiverMask, clear);
        context->ClearUA(CameraReceiverNear, emptyNear);
        context->ClearUA(CameraReceiverFar, clear);
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
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Receiver Mask Caster Gate");
            LastSelectionDispatchCount++;
            LastSelectionTierCount++;
            context->BindSR(3, BoundsBuffer->View());
            context->BindSR(7, CandidateOccluderDepth);
            context->BindSR(21, LightVisibilityBuffer->View());
            context->BindSR(24, CameraReceiverMask);
            context->BindSR(25, CameraReceiverNear);
            context->BindSR(26, CameraReceiverFar);
            context->BindUA(0, CameraReceiverCasters->View());
            context->Dispatch(intersectReceivers, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
            context->ResetUA();
            context->ResetSR();
        }
        candidateData.CasterDebugFlags |= 131072u;
        resources.SetOutput(TEXT("Shadow.CameraReceiverCoverage"), CameraReceiverMask);
        resources.SetOutput(TEXT("Shadow.CameraReceiverNear"), CameraReceiverNear);
        resources.SetOutput(TEXT("Shadow.CameraReceiverFar"), CameraReceiverFar);
        resources.SetOutput(TEXT("Shadow.CameraReceiverCasters"), CameraReceiverCasters);
    }

    const auto selectCasters = [&]()
    {
        {
            PROFILE_CPU_NAMED("V2 Shadow Selection Bindings");
            context->UpdateCB(constants, &candidateData);
            context->BindCB(1, constants);
            context->BindSR(2, depth);
            context->BindSR(3, BoundsBuffer->View());
            context->BindSR(7, candidateData.HasLightDepth ? CandidateOccluderDepth : depth);
            context->BindSR(8, receiverNormals ? receiverNormals : depth);
            context->BindSR(21, LightVisibilityBuffer->View());
            context->BindSR(23, cameraVisibility ? cameraVisibility->View() : LightVisibilityBuffer->View());
            context->BindSR(22, candidateData.HasLightDepth ? ProxyShadowDepth : depth);
            context->BindSR(27, candidateData.HasLightDepth ? CameraReceiverCasters->View() : LightVisibilityBuffer->View());
            context->BindUA(1, SelectionBuffer->View());
            if (receiverTrace) context->BindUA(3, ReceiverTrace->View());
            if (debugColor)
                context->BindUA(2, DebugMask->View());
        }
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Legacy Receiver Diagnostics");
            LastSelectionDispatchCount++;
            LastSelectionTierCount++;
            ZoneValue(data.CasterCount);
            context->Dispatch(shader, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
        }
        context->ResetUA();
        context->ResetSR();
    };
    if (debugColor)
    {
        const uint32 clear[4] = { colorFootprints ? MAX_uint32 : 0u, 0, 0, 0 };
        context->ClearUA(DebugMask, clear);
    }

    // The light-space mask is the actual selection result, not another broad
    // phase followed by the same expensive camera search. Explicit diagnostics
    // may still trace that old search, but cannot change normal caster flags.
    if (!candidateData.HasLightDepth || legacyReceiverDiagnostics)
        selectCasters();
    if (candidateData.HasLightDepth)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Publish Receiver Mask Selection");
        context->CopyBuffer(SelectionBuffer, CameraReceiverCasters, count * sizeof(uint32));
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

    const bool showBlendDebug = settings && settings->ShadowMaskBlendDebug != ShadowMaskBlendDebugView::None;
    const bool useMaskBlend = settings && (settings->UseMultiResolutionShadowBlend || showBlendDebug);
    const bool showCascadeDebug = !showResourceDebug && useConfiguredCascades && settings->ShowShadowCascades && !showReceiverHits && !showBlendDebug;
    bool cascadeDebugReady = false;
    bool needsScreenFilter = false;
    GPUTexture* visibilityTexture = nullptr;
    GPUTexture* publishedDepth = useConfiguredCascades ? nullptr : lightDepth;
    GPUTexture* resourceCascadeDepth = useConfiguredCascades ? nullptr : lightDepth;
    if (useConfiguredCascades)
    {
        // Clear visibility to fully lit. Draw far to near so each map can fade into the one behind it.
        resolve.CascadeRange.W = 1.0f;
        const bool drawMask = sceneColor && receiverNormals && haveResolveConstants &&
            (showCascadeDebug ?
                preparePass(CascadeDebugState, CascadeDebug, "PS_V2CascadeShadowMask", TEXT("V2 Cascade Debug"), PixelFormat::R8G8B8A8_UNorm, true) :
                preparePass(CascadeMaskState, ShadowMask, "PS_V2CascadeShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float, true));
        if (drawMask)
        {
            if (showCascadeDebug)
            {
                context->Clear(CascadeDebug->View(), Color(0.08f, 0.08f, 0.08f, 1.0f));
                cascadeDebugReady = true;
            }
            else
            {
                context->Clear(ShadowMask->View(), Color(1.0f, 0.5f, 0.0f, 0.0f));
                visibilityTexture = ShadowMask;
            }
        }

        if (drawMask || debugColor)
        {
            struct CascadeSlice
            {
                GPUTexture* Depth;
                BoundingBox Bounds;
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

            int32 lastCascade = ShadowCascadeMaps.Count() - 1;
            while (lastCascade >= 0 && !ShadowCascadeMaps[lastCascade].Depth)
                lastCascade--;
            float previousEnd = 0.0f;
            float previousFade = previousEnd;
            for (int32 i = 0; i < ShadowCascadeMaps.Count(); i++)
            {
                const auto& map = ShadowCascadeMaps[i];
                const bool terminal = i == lastCascade;
                const float end = terminal ? visibleFar : settings->ShadowCascades[i].EndDistance;
                const float rangeStart = terminal ? previousFade : previousEnd;
                if (!map.Depth || !(end > rangeStart && end <= MAX_float))
                    continue;
                const float fitEnd = Math::Min(end, visibleFar);
                if (fitEnd <= previousFade)
                    break;

                CascadeSlice slice;
                const float blend = terminal ? 0.0f : Math::Clamp(settings->ShadowCascades[i].BlendFraction, 0.0f, 1.0f);
                const float fade = terminal ? end : Math::Lerp(previousEnd, end, 1.0f - blend);
                if (FitShadowSphere(resolve.CameraPosition, lightView, lightBounds,
                    fitEnd, filterRadius + maxNormalBias, map.Depth->Width(), slice.Bounds))
                {
                    slice.Depth = map.Depth;
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

            data.CameraPosition = Float4(resolve.CameraPosition, 0.0f);
            data.CameraForward = Float4(resolve.CameraForward, 0.0f);
            data.HasLightDepth = 1;
            LastCascadeCount = slices.Count();
            for (int32 i = slices.Count() - 1; i >= 0; i--)
            {
                PROFILE_GPU_CPU_NAMED("V2 Shadow Cascade");
                const auto& slice = slices[i];
                lightDepth = slice.Depth;
                publishedDepth = lightDepth;
                if (showResourceDebug && (int32)slice.Parameters.Z == settings->ShadowResourceCascadeIndex)
                    resourceCascadeDepth = lightDepth;
                nearBounds = slice.Bounds;
                const Float3 minimum(nearBounds.Minimum);
                const Float3 maximum(nearBounds.Maximum);
                Matrix lightProjection;
                Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, lightProjection);
                auto& lightContext = lightContexts[0];
                const float size = (float)lightDepth->Width();
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
                const float worldTexel = Math::Max(maximum.X - minimum.X, maximum.Y - minimum.Y) / size;
                data.ShadowSplit.W = GetShadowDepthPrecisionBias(lightDepth->Format()) +
                    slice.Parameters.X * worldTexel / (maximum.Z - minimum.Z);
                data.CameraPosition.W = slice.Parameters.Y * worldTexel;
                data.ReceiverRange = Float4(slice.Range.X, slice.Range.Z, 0.0f, 0.0f);
                data.FilterRadius = slice.Filter.AdaptiveRadius;
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Clear Cascade Depth");
                    context->ClearDepth(lightDepth->View());
                }
                // Shared selection already tested receivers and light occlusion.
                if (!drawMask)
                    continue;

                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Selected Casters");
                    drawSelectedDepth(1);
                }
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Resolve Cascade Mask");
                    ResolveConstants cascadeResolve = resolve;
                    cascadeResolve.LightViewProjection = data.LightViewProjection;
                    cascadeResolve.ShadowTiles[0] = data.ShadowTiles[0];
                    cascadeResolve.ShadowSplit = data.ShadowSplit;
                    cascadeResolve.ShadowFilter.X = maximum.Z - minimum.Z;
                    cascadeResolve.ShadowFilter.Z = slice.Filter.AngularTangent;
                    cascadeResolve.ShadowFilter.W = slice.Filter.AdaptiveRadius;
                    needsScreenFilter |= slice.Filter.ScreenRadius > 0.5f;
                    cascadeResolve.CascadeRange = slice.Range;
                    cascadeResolve.CascadeParameters = Float4(data.CameraPosition.W, slice.Parameters.Z,
                        showCascadeDebug ? 1.0f : 0.0f, slice.Filter.ScreenRadius);
                    context->UpdateCB(resolveConstants, &cascadeResolve);
                    context->BindCB(3, resolveConstants);
                    context->BindSR(2, depth);
                    context->BindSR(7, lightDepth);
                    context->BindSR(8, receiverNormals);
                    context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                    context->SetRenderTarget(showCascadeDebug ? CascadeDebug->View() : ShadowMask->View());
                    context->SetState(showCascadeDebug ? CascadeDebugState : CascadeMaskState);
                    context->DrawFullscreenTriangle();
                    context->ResetRenderTarget();
                    context->ResetSR();
                }
            }
        }
    }


    if (!showCascadeDebug && sceneColor && lightDepth)
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
                const bool applyScreenFilter = useConfiguredCascades ? needsScreenFilter : filterRadius == 0 && screenSpaceFilter;
                if (applyScreenFilter &&
                    (useConfiguredCascades ? visibilityTexture != nullptr :
                        preparePass(ShadowMaskState, ShadowMask, "PS_V2ShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float)) &&
                    preparePass(ShadowFilterState, FilteredShadowMask, "PS_V2ShadowFilter", TEXT("V2 Filtered Shadow Mask"), PixelFormat::R8_UNorm))
                {
                    context->UpdateCB(resolveConstants, &resolve);
                    context->BindCB(3, resolveConstants);
                    context->BindSR(2, depth);
                    context->BindSR(8, receiverNormals);
                    context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                    if (!useConfiguredCascades)
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Shadow Mask");
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
                }

                GPUTexture* blendDebugTexture = nullptr;
                if (useMaskBlend)
                {
                    // Cascades already publish a common receiver-space mask. Legacy
                    // adaptive views normally shade directly, so materialize A first.
                    if (!visibilityTexture && !useConfiguredCascades &&
                        preparePass(ShadowMaskState, ShadowMask, "PS_V2ShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float))
                    {
                        PROFILE_GPU_CPU_NAMED("V2 Detailed Shadow Mask");
                        context->ResetRenderTarget();
                        context->ResetSR();
                        context->UpdateCB(resolveConstants, &resolve);
                        context->BindCB(3, resolveConstants);
                        context->BindSR(2, depth);
                        context->BindSR(7, lightDepth);
                        context->BindSR(8, receiverNormals);
                        context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                        context->SetRenderTarget(ShadowMask->View());
                        context->SetState(ShadowMaskState);
                        context->DrawFullscreenTriangle();
                        context->ResetRenderTarget();
                        context->ResetSR();
                        visibilityTexture = ShadowMask;
                    }
                    if (visibilityTexture && BuildShadowMaskBlend(context, visibilityTexture, settings))
                    {
                        resources.SetOutput(TEXT("Shadow.MaskBlend.A"), visibilityTexture);
                        resources.SetOutput(TEXT("Shadow.MaskBlend.B"), BlendFiltered[0]);
                        resources.SetOutput(TEXT("Shadow.MaskBlend.C"), BlendFiltered[1]);
                        resources.SetOutput(TEXT("Shadow.MaskBlend.Result"), BlendResult);
                        if (showBlendDebug)
                            blendDebugTexture = BlendResult;
                        else
                            visibilityTexture = BlendResult;
                    }
                }

                if (blendDebugTexture)
                {
                    context->ResetRenderTarget();
                    context->ResetSR();
                    context->Draw(sceneColor, blendDebugTexture);
                    context->ResetRenderTarget();
                    context->ResetSR();
                }
                else
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Lighting Resolve");
                    context->ResetSR();
                    context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
                    context->SetRenderTarget(sceneColor->View());
                    resolve.UseFilteredShadow = visibilityTexture != nullptr;
                    context->UpdateCB(resolveConstants, &resolve);
                    context->BindCB(3, resolveConstants);
                    context->BindSR(2, depth);
                    context->BindSR(7, lightDepth);
                    context->BindSR(8, receiverNormals);
                    context->BindSR(9, baseColor);
                    context->BindSR(10, material);
                    context->BindSR(11, emissive);
                    if (visibilityTexture)
                    {
                        context->BindSR(13, visibilityTexture);
                        resources.SetOutput(TEXT("Shadow.Visibility"), visibilityTexture);
                    }
                    context->SetState(ResolveState);
                    context->DrawFullscreenTriangle();
                    context->ResetRenderTarget();
                    context->ResetSR();
                }
                if (publishedDepth)
                {
                    resources.Set(TEXT("Shadow.Depth"), publishedDepth);
                    resources.SetOutput(TEXT("Shadow.Depth"), publishedDepth);
                }
            }
        }
    }

    if (cascadeDebugReady)
    {
        context->ResetRenderTarget();
        context->ResetSR();
        context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
        context->Draw(sceneColor, CascadeDebug);
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.CascadeDebug"), CascadeDebug);
    }

    if (debugColor)
    {
        context->SetViewportAndScissors((float)debugColor->Width(), (float)debugColor->Height());
        context->SetRenderTarget(debugColor->View());
        context->BindSR(4, DebugMask);
        context->SetState(DebugState);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.ReceiverHits"), DebugMask);
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

    if ((colorFootprints || hiddenContributions) && sceneColor)
        resources.SetOutput(TEXT("Shadow.DebugResource"), sceneColor);

    if ((lightVisibilityDebug || depthCrossDebug) && candidateData.HasLightDepth)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Selection Step Debug");
        if (!SelectionStepDebug)
            SelectionStepDebug = device->CreateTexture(TEXT("V2 Shadow Selection Step Debug"));
        if (!SelectionStepDebug || ((!SelectionStepDebug->GetMemoryUsage() || SelectionStepDebug->Width() != FarShadowResolution) &&
            SelectionStepDebug->Init(GPUTextureDescription::New2D(FarShadowResolution, FarShadowResolution,
                PixelFormat::R8G8B8A8_UNorm, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget))))
            return StageResult::Failed;
        context->ResetRenderTarget();
        context->ResetSR();
        context->SetViewportAndScissors((float)FarShadowResolution, (float)FarShadowResolution);
        context->Clear(SelectionStepDebug->View(), Color(0.025f, 0.025f, 0.025f, 1));
        SelectionConstants debugData = candidateData;
        debugData.Width = FarShadowResolution;
        debugData.Height = FarShadowResolution;
        if (lightVisibilityDebug)
        {
            if (!CasterBoundsDebugState)
                CasterBoundsDebugState = device->CreatePipelineState();
            if (!CasterBoundsDebugState->IsValid())
            {
                auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
                desc.VS = SelectionShader->GPU->GetVS("VS_ShadowCasterBoundsDebug");
                desc.PS = SelectionShader->GPU->GetPS("PS_ShadowCasterBoundsDebug");
                desc.PrimitiveTopology = PrimitiveTopologyType::Line;
                if (!desc.VS || !desc.PS || CasterBoundsDebugState->Init(desc))
                    return StageResult::Failed;
            }
            debugData.ViewProjection = candidateData.LightViewProjection;
            debugData.CasterDebugFlags = 3u | 32u | (filterDebugCaster ? 4u : 0u);
            context->UpdateCB(constants, &debugData);
            context->BindCB(1, constants);
            context->BindSR(3, BoundsBuffer->View());
            context->BindSR(18, LightVisibilityBuffer->View());
            context->SetRenderTarget(SelectionStepDebug->View());
            context->SetState(CasterBoundsDebugState);
            context->DrawInstanced(24, count);
        }
        else
        {
            if (!DepthCrossDebugState)
                DepthCrossDebugState = device->CreatePipelineState();
            if (!DepthCrossDebugState->IsValid())
            {
                auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
                desc.PS = SelectionShader->GPU->GetPS("PS_V2ReceiverDepthCross");
                if (!desc.PS || DepthCrossDebugState->Init(desc))
                    return StageResult::Failed;
            }
            context->UpdateCB(constants, &debugData);
            context->BindCB(1, constants);
            context->BindSR(2, depth);
            context->BindSR(22, ProxyShadowDepth);
            context->BindSR(8, receiverNormals ? receiverNormals : depth);
            context->SetRenderTarget(SelectionStepDebug->View());
            context->SetState(DepthCrossDebugState);
            context->DrawFullscreenTriangle();
        }
        context->ResetRenderTarget();
        context->ResetSR();
        resources.SetOutput(TEXT("Shadow.SelectionStepDebug"), SelectionStepDebug);
    }

    bool lightDebugReady = false;
    if (showCameraAndLight && sceneColor && lightDepth && !lightDebugObjects.IsEmpty())
    {
        PROFILE_GPU_CPU_NAMED("V2 Light POV Material Debug");
        if (!LightDebugColor)
            LightDebugColor = device->CreateTexture(TEXT("V2 Light POV Material Colour"));
        if (!LightDebugDrawStage)
            LightDebugDrawStage = New<RenderPipelineDrawStage>();
        if (!LightDebugColor || !LightDebugDrawStage)
            return StageResult::Failed;
        if ((LightDebugColor->Width() != FarShadowResolution || !LightDebugColor->GetMemoryUsage()) &&
            LightDebugColor->Init(GPUTextureDescription::New2D(FarShadowResolution, FarShadowResolution, sceneColor->Format(),
                GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget)))
            return StageResult::Failed;
        WarpedViewpoint lightViewpoint;
        lightViewpoint.View = lightView;
        const Float3 minimum = Float3(lightBounds.Minimum) - Float3::One;
        const Float3 maximum = Float3(lightBounds.Maximum) + Float3::One;
        Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, lightViewpoint.Projection);
        Matrix::Multiply(lightViewpoint.View, lightViewpoint.Projection, lightViewpoint.ViewProjection);
        const Float3 up = Math::Abs(direction.Y) > 0.99f ? Float3::Right : Float3::Up;
        lightViewpoint.LocalTransform.Translation = Vector3::Zero;
        lightViewpoint.LocalTransform.Orientation = Quaternion::LookRotation(direction, up);
        RenderResourceTable lightResources;
        lightResources.Set(TEXT("Scene.Color"), LightDebugColor);
        ICamera* lightCamera = &lightViewpoint;
        lightDebugReady = LightDebugDrawStage->Render(context, lightDebugObjects, lightCamera, lightResources) == StageResult::Completed;
        if (lightDebugReady)
        {
            auto* normals = lightResources.Get<GPUTexture>(TEXT("Scene.Normals"));
            auto* lightViewDepth = lightResources.Get<GPUTexture>(TEXT("Scene.Depth"));
            auto* pixelShader = SelectionShader->GPU->GetPS("PS_V2LightPOVShaded");
            if (!LightDebugShadingState)
                LightDebugShadingState = device->CreatePipelineState();
            if (!LightDebugShadedColor)
                LightDebugShadedColor = device->CreateTexture(TEXT("V2 Light POV Shaded Material Colour"));
            if (!normals || !lightViewDepth || !pixelShader || !LightDebugShadingState || !LightDebugShadedColor)
                return StageResult::Failed;
            if ((LightDebugShadedColor->Width() != FarShadowResolution || !LightDebugShadedColor->GetMemoryUsage()) &&
                LightDebugShadedColor->Init(GPUTextureDescription::New2D(FarShadowResolution, FarShadowResolution, sceneColor->Format(),
                    GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget)))
                return StageResult::Failed;
            if (!LightDebugShadingState->IsValid())
            {
                auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
                desc.PS = pixelShader;
                if (LightDebugShadingState->Init(desc))
                    return StageResult::Failed;
            }
            context->ResetRenderTarget();
            context->ResetSR();
            context->UpdateCB(constants, &candidateData);
            context->BindCB(1, constants);
            context->BindSR(2, lightViewDepth);
            context->BindSR(8, normals);
            context->BindSR(9, LightDebugColor);
            context->SetViewportAndScissors((float)FarShadowResolution, (float)FarShadowResolution);
            context->SetRenderTarget(LightDebugShadedColor->View());
            context->SetState(LightDebugShadingState);
            context->DrawFullscreenTriangle();
            context->ResetRenderTarget();
            context->ResetSR();
            resources.SetOutput(TEXT("Shadow.LightPOVColor"), LightDebugShadedColor);
        }
    }

    if (showResourceDebug && sceneColor)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Resource Debug");
        GPUTexture* source = nullptr;
        switch (settings->ShadowResourceDebug)
        {
        case ShadowResourceDebugView::LightCasterVisibility:
        case ShadowResourceDebugView::ReceiverDepthCross: source = SelectionStepDebug; break;
        case ShadowResourceDebugView::ProxyShadowDepth: source = ProxyShadowDepth; break;
        case ShadowResourceDebugView::ReceiverShaderSteps: source = ReceiverTrace; break;
        case ShadowResourceDebugView::CameraReceiverCoverage:
        case ShadowResourceDebugView::CameraReceiverLightDepth:
        case ShadowResourceDebugView::CameraReceiverLightCross: source = CameraReceiverMask ? CandidateOccluderDepth : nullptr; break;
        case ShadowResourceDebugView::ReceiverDepth: source = depth; break;
        case ShadowResourceDebugView::ReceiverNormals: source = receiverNormals; break;
        case ShadowResourceDebugView::CandidateOccluderDepth: source = CandidateOccluderDepth; break;
        case ShadowResourceDebugView::CascadeDepth: source = resourceCascadeDepth; break;
        case ShadowResourceDebugView::ShadowVisibility: source = visibilityTexture; break;
        case ShadowResourceDebugView::CameraAndLight: source = lightDebugReady ? LightDebugShadedColor : nullptr; break;
        default: break;
        }
        auto* pixelShader = SelectionShader->GPU->GetPS("PS_V2ShadowResourceDebug");
        if (!ResourceDebugState)
            ResourceDebugState = device->CreatePipelineState();
        if (!pixelShader || !ResourceDebugState)
            return StageResult::Failed;
        if (!ResourceDebugState->IsValid())
        {
            auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            desc.PS = pixelShader;
            if (ResourceDebugState->Init(desc))
                return StageResult::Failed;
        }
        SelectionConstants debugData = data;
        const bool fullscreen = showCameraAndLight || settings->ShadowResourceDebugFullscreen;
        const int32 panelWidth = fullscreen ? sceneColor->Width() : Math::Max(1, sceneColor->Width() / 3);
        const int32 panelLeft = fullscreen ? 0 : sceneColor->Width() - panelWidth;
        debugData.Width = panelWidth;
        debugData.Height = sceneColor->Height();
        debugData.CameraPosition = Float4(Float3(view->LocalTransform.Translation), 0);
        debugData.ReceiverRange = Float4(Math::Max(settings->ShadowDebugDepthDistance, 1.0f), Math::Max(settings->ReceiverDebugRange, 0.001f), (float)panelLeft, 0);
        debugData.CasterDebugFlags = (uint32)settings->ShadowResourceDebug << 8;
        if (receiverMaskDebug)
        {
            debugData.ShadowSplit.W = candidateData.ShadowSplit.W;
            debugData.FilterRadius = candidateData.ReceiverRange.Z; // Shared light depth span, in cm.
        }
        if (source)
            debugData.CasterDebugFlags |= 65536u;
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->UpdateCB(constants, &debugData);
        context->BindCB(1, constants);
        context->BindSR(3, BoundsBuffer->View());
        context->BindSR(18, SelectionBuffer->View());
        context->BindSR(19, source ? source : depth);
        if (showCameraAndLight)
        {
            if (!ResourceDebugComposite)
                ResourceDebugComposite = device->CreateTexture(TEXT("V2 Camera And Light Composite"));
            if (!ResourceDebugComposite)
                return StageResult::Failed;
            if ((ResourceDebugComposite->Width() != sceneColor->Width() || ResourceDebugComposite->Height() != sceneColor->Height() ||
                ResourceDebugComposite->Format() != sceneColor->Format() || !ResourceDebugComposite->GetMemoryUsage()) &&
                ResourceDebugComposite->Init(GPUTextureDescription::New2D(sceneColor->Width(), sceneColor->Height(), sceneColor->Format(),
                    GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget)))
                return StageResult::Failed;
            context->BindSR(20, sceneColor);
        }
        context->SetViewportAndScissors(Viewport((float)panelLeft, 0, (float)panelWidth, (float)sceneColor->Height()));
        context->SetRenderTarget(showCameraAndLight ? ResourceDebugComposite->View() : sceneColor->View());
        if (receiverMaskDebug && source)
        {
            context->BindSR(24, CameraReceiverMask);
            context->BindSR(25, CameraReceiverNear);
            context->BindSR(26, CameraReceiverFar);
        }
        context->SetState(ResourceDebugState);
        context->DrawFullscreenTriangle();
        context->ResetRenderTarget();
        context->ResetSR();
        if (showCameraAndLight)
            context->CopyResource(sceneColor, ResourceDebugComposite);
        resources.SetOutput(TEXT("Shadow.DebugResource"), sceneColor);
        if (receiverMaskDebug)
        {
            if (!ReceiverTraceFont)
                ReceiverTraceFont = Content::LoadAsyncInternal<FontAsset>(TEXT("Editor/Fonts/Roboto-Regular"));
            if (ReceiverTraceFont && ReceiverTraceFont->IsLoaded() && !Render2D::IsRendering())
            {
                const Char* legend = settings->ShadowResourceDebug == ShadowResourceDebugView::CameraReceiverCoverage ?
                    TEXT("Camera receivers in LIGHT space\nGreen: can receive this light\nPurple: camera-visible back face\nBlack: no camera sample") :
                    settings->ShadowResourceDebug == ShadowResourceDebugView::CameraReceiverLightDepth ?
                    TEXT("Camera receiver LIGHT depth\nNearest eligible surface per texel\nGray scale: Shadow Debug Depth Distance\nPurple: back face; black: no sample") :
                    TEXT("Camera / cheap LIGHT depth cross\nGreen: matches or precedes light depth\nRed: receiver behind blocker\nYellow: mixed depths; blue: no blocker");
                const float top = Math::Max(0.0f, (float)sceneColor->Height() - 85.0f);
                Render2D::Begin(context, sceneColor);
                Render2D::FillRectangle(Rectangle((float)panelLeft, top, (float)panelWidth, 85.0f), Color(0.015f, 0.015f, 0.015f, 0.94f));
                Render2D::DrawText(ReceiverTraceFont->CreateFont(11), legend, Color::White, Float2((float)panelLeft + 6, top + 4));
                Render2D::End();
                context->ResetRenderTarget();
                context->ResetSR();
            }
        }
        if (receiverTrace)
        {
            const int32 probeX = settings->ReceiverDebugPixel.X < 0 ? depth->Width() / 2 : Math::Clamp((int32)settings->ReceiverDebugPixel.X, 0, depth->Width() - 1);
            const int32 probeY = settings->ReceiverDebugPixel.Y < 0 ? depth->Height() / 2 : Math::Clamp((int32)settings->ReceiverDebugPixel.Y, 0, depth->Height() - 1);
            if (settings->ReadReceiverDebugPixel && Platform::GetTimeSeconds() >= ReceiverTraceNextRead)
            {
                PROFILE_GPU_CPU_NAMED("V2 Receiver Trace Pixel Readback");
                ReceiverTraceNextRead = Platform::GetTimeSeconds() + 0.25;
                if (!ReceiverTraceProbe)
                    ReceiverTraceProbe = device->CreateTexture(TEXT("V2 Receiver Trace Probe"));
                if (!ReceiverTraceReadback)
                    ReceiverTraceReadback = device->CreateTexture(TEXT("V2 Receiver Trace Probe Readback"));
                if (!ReceiverTraceProbeState)
                    ReceiverTraceProbeState = device->CreatePipelineState();
                const auto probeDesc = GPUTextureDescription::New2D(1, 1, PixelFormat::R32G32B32A32_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget);
                if (!ReceiverTraceProbe || !ReceiverTraceReadback || !ReceiverTraceProbeState ||
                    (!ReceiverTraceProbe->GetMemoryUsage() && ReceiverTraceProbe->Init(probeDesc)) ||
                    (!ReceiverTraceReadback->GetMemoryUsage() && ReceiverTraceReadback->Init(probeDesc.ToStagingReadback())))
                    return StageResult::Failed;
                if (!ReceiverTraceProbeState->IsValid())
                {
                    auto desc = GPUPipelineState::Description::DefaultFullscreenTriangle;
                    desc.PS = SelectionShader->GPU->GetPS("PS_V2ReceiverTraceProbe");
                    if (!desc.PS || ReceiverTraceProbeState->Init(desc))
                        return StageResult::Failed;
                }
                auto probeData = candidateData;
                probeData.CameraForward = Float4((float)probeX, (float)probeY, 0, 0);
                context->UpdateCB(constants, &probeData);
                context->BindCB(1, constants);
                context->BindSR(19, ReceiverTrace);
                context->SetViewportAndScissors(1.0f, 1.0f);
                context->SetRenderTarget(ReceiverTraceProbe->View());
                context->SetState(ReceiverTraceProbeState);
                context->DrawFullscreenTriangle();
                context->ResetRenderTarget();
                context->ResetSR();
                context->CopyResource(ReceiverTraceReadback, ReceiverTraceProbe);
                TextureData pixels;
                if (!ReceiverTraceReadback->DownloadData(pixels))
                    ReceiverTraceValue = pixels.GetData(0, 0)->Get<Float4>(0, 0);
            }
            if (!ReceiverTraceFont)
                ReceiverTraceFont = Content::LoadAsyncInternal<FontAsset>(TEXT("Editor/Fonts/Roboto-Regular"));
            if (ReceiverTraceFont && ReceiverTraceFont->IsLoaded() && !Render2D::IsRendering())
            {
                static const Char* stepNames[] = { TEXT("Decision"), TEXT("Camera depth"), TEXT("World position"), TEXT("Surface normal"), TEXT("Receiver bias"), TEXT("Light projection"), TEXT("Proxy separation"), TEXT("Cheap separation"), TEXT("Comparison tolerance"), TEXT("Surface confidence"), TEXT("Cheap taps"), TEXT("Proxy taps") };
                static const Char* channels[] = { TEXT("R: reason code"), TEXT("R: raw depth  G: distance cm\nB: uncertainty cm"), TEXT("RGB: world XYZ in cm"), TEXT("RGB: normal  A: reliable plane"), TEXT("RGB: offset cm  A: bias cm"), TEXT("RG: UV  B: depth  A: valid"), TEXT("RG: min/max cm  B: tolerance\nA: valid-tap mask"), TEXT("RG: min/max cm  B: tolerance\nA: valid-tap mask"), TEXT("R: precision  G: uncertainty\nB: total tolerance (cm)"), TEXT("R: reliable  G: normal dot light\nB: uncertainty cm"), TEXT("R: valid  G: blocked tap mask\nB: tolerance cm"), TEXT("R: valid  G: potential tap mask\nB: tolerance cm") };
                const int32 step = (int32)data.ReceiverTraceStep;
                String text = String::Format(TEXT("Caster: {0}\nStep: {1}\n{2}\nPixel {3}, {4} / {5} x {6}\n"), traceName, stepNames[step], channels[step], probeX, probeY, depth->Width(), depth->Height());
                if (settings->ReadReceiverDebugPixel)
                    text += String::Format(TEXT("R {0:.6g}   G {1:.6g}\nB {2:.6g}   A {3:.6g}\n"), ReceiverTraceValue.X, ReceiverTraceValue.Y, ReceiverTraceValue.Z, ReceiverTraceValue.W);
                else text += TEXT("Enable Read Receiver Debug Pixel\nto inspect raw values.\n");
                text += TEXT("Legacy ray trace: diagnostic only\nMask controls actual caster selection\nGreen accepted / orange uncertain");
                const float legendTop = Math::Max(0.0f, (float)sceneColor->Height() - 190.0f);
                const Float2 sourceSize((float)depth->Width(), (float)depth->Height());
                const float scale = Math::Min((float)panelWidth / sourceSize.X, (float)sceneColor->Height() / sourceSize.Y);
                const Float2 cross = Float2((float)panelLeft + panelWidth * 0.5f, sceneColor->Height() * 0.5f) +
                    (Float2((float)probeX + 0.5f, (float)probeY + 0.5f) - sourceSize * 0.5f) * scale;
                Render2D::Begin(context, sceneColor);
                Render2D::FillRectangle(Rectangle((float)panelLeft, legendTop, (float)panelWidth, 190.0f), Color(0.015f, 0.015f, 0.015f, 0.94f));
                Render2D::DrawText(ReceiverTraceFont->CreateFont(11), text, Color::White, Float2((float)panelLeft + 6, legendTop + 4));
                Render2D::DrawLine(cross - Float2(5, 0), cross + Float2(5, 0), Color::Yellow);
                Render2D::DrawLine(cross - Float2(0, 5), cross + Float2(0, 5), Color::Yellow);
                Render2D::End();
                context->ResetRenderTarget();
                context->ResetSR();
            }
        }
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
