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
        Float4 ReceiverRange; // Inner and outer radial receiver distances.
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
        for (auto* object : candidates)
        {
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
    const float inheritedRadius = adaptiveFilter && !ShowReceiverHits
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
    auto* debugColor = ShowReceiverHits ? sceneColor : nullptr;
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
            drawShadowDepth(tier, false, nullptr, false);
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

    if ((filterRadius > 0 || maxNormalBias > 0) && lightDepth)
    {
        // Expand only the receiver test. Keep the actual depth geometry and its projection intact.
        const Float3 padding(sweepPadding);
        for (auto& box : bounds)
        {
            if (box.UseProxy)
            {
                box.Minimum -= padding;
                box.Maximum += padding;
            }
        }
    }

    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Upload Caster Bounds");
        context->UpdateBuffer(BoundsBuffer, bounds.Get(), count * sizeof(CasterBounds));
    }
    const auto selectCasters = [&]()
    {
        {
            PROFILE_CPU_NAMED("V2 Shadow Selection Bindings");
            context->UpdateCB(constants, &data);
            context->BindCB(1, constants);
            context->BindSR(2, depth);
            context->BindSR(3, BoundsBuffer->View());
            context->BindSR(7, lightDepth ? lightDepth : depth);
            context->BindSR(8, receiverNormals ? receiverNormals : depth);
            context->BindUA(1, SelectionBuffer->View());
            if (debugColor)
                context->BindUA(2, DebugMask->View());
        }
        {
            PROFILE_GPU_CPU_NAMED("V2 Shadow Receiver Selection");
            LastSelectionDispatchCount++;
            LastSelectionTierCount += data.ShadowSplit.Z != 0.0f ? 2 : 1;
            ZoneValue(data.CasterCount);
            context->Dispatch(shader, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
        }
        context->ResetUA();
        context->ResetSR();
    };
    if (debugColor)
    {
        const uint32 clear[4] = {};
        context->ClearUA(DebugMask, clear);
    }

    if (!useConfiguredCascades)
        selectCasters();

    const auto drawSelectedDepth = [&](int32 tiers)
    {
        Array<uint32> selected;
        const bool indirect = indirectShadowDraws && device->Limits.HasDrawIndirect;
        if (!indirect && device->GetRendererType() == RendererType::DirectX11)
        {
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
        for (int32 tier = 0; tier < tiers; tier++)
            drawShadowDepth(tier, true, haveSelection ? selected.Get() : nullptr, indirect);
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
    const bool showCascadeDebug = useConfiguredCascades && settings->ShowShadowCascades && !ShowReceiverHits && !showBlendDebug;
    bool cascadeDebugReady = false;
    bool needsScreenFilter = false;
    GPUTexture* visibilityTexture = nullptr;
    GPUTexture* publishedDepth = useConfiguredCascades ? nullptr : lightDepth;
    if (useConfiguredCascades)
    {
        // Clear visibility to fully lit. Draw far to near so each map can fade into the one behind it.
        resolve.CascadeRange.W = 1.0f;
        const bool drawMask = !ShowReceiverHits && sceneColor && receiverNormals && haveResolveConstants &&
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
                {
                    PROFILE_GPU_CPU_NAMED("V2 Shadow Occluder Prepass");
                    drawShadowDepth(0, false, nullptr, false);
                }
                selectCasters();
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

    if (!ShowReceiverHits && !showCascadeDebug && sceneColor && lightDepth)
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

    LastCasterCount = count;
    // Configured cascades reuse this buffer for each individual depth draw.
    if (!useConfiguredCascades)
    {
        resources.Set(TEXT("Shadow.CasterSelection"), SelectionBuffer);
        resources.SetOutput(TEXT("Shadow.CasterSelection"), SelectionBuffer);
    }
    return StageResult::Completed;
}

void ShadowBoxCasterStage::OnEndFrame(GPUContext* context)
{
    Casters.Clear();
}
