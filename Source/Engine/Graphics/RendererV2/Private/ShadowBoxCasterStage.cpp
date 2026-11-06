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
        Float4 ShadowSplit;
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
        Float4 ShadowSplit;
        Float4 CameraPosition;
        Float4 CameraForward;
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
    SAFE_DELETE_GPU_RESOURCE(ShadowFilterState);
    SAFE_DELETE_GPU_RESOURCE(ShadowMask);
    SAFE_DELETE_GPU_RESOURCE(FilteredShadowMask);
    SAFE_DELETE_GPU_RESOURCE(SelectionReadback);
    SAFE_DELETE_GPU_RESOURCE(IndirectInputs);
    SAFE_DELETE_GPU_RESOURCE(IndirectOutput);
    SAFE_DELETE_GPU_RESOURCE(IndirectArguments);
    SAFE_DELETE_GPU_RESOURCE(DebugMask);
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
    if (ShadowFilterState)
        ShadowFilterState->ReleaseGPU();
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

    // The light views only write depth. Keep the RenderBuffers object for material binding.
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
            {
                RequestedAtlasWidth = width;
                RequestedAtlasHeight = resolution;
                return size;
            }
        }
        if (size == minResolution)
            return 0;
    }
}

StageResult ShadowBoxCasterStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    Casters.Clear();
    LastCasterCount = 0;
    LastRejectedProxyCount = 0;
    LastUsedSelection = false;
    if (!(ProjectionDistance > 0.0f) || LightDirection.IsZero() || !context->GetDevice()->Limits.HasCompute)
        return StageResult::Skipped;

    auto* depth = resources.Get<GPUTexture>(TEXT("Shadow.ReceiverDepth"));
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    if (!depth || !view)
        return StageResult::Skipped;

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
    if (context == device->GetMainContext())
    {
        resolution = PrepareShadowAtlas(device, resolution, farTileWidth);
        if (resolution == 0)
            return StageResult::Completed;
        lightDepth = LightDepthBuffers->DepthBuffer;
    }
    const int32 farResolution = useTiers ? FarShadowResolution : resolution;
    const float filterRadius = adaptiveFilter && !ShowReceiverHits
        ? Math::Clamp(adaptiveRadius, 0.0f, 4.0f) : 0.0f;
    float sweepPadding = 0.0f;
    if (filterRadius > 0)
    {
        // The world diagonal bounds both fitted light-space axes before caster rejection.
        Float3 minimum = bounds[0].Minimum;
        Float3 maximum = bounds[0].Maximum;
        for (const auto& box : bounds)
        {
            minimum = Float3::Min(minimum, box.Minimum);
            maximum = Float3::Max(maximum, box.Maximum);
        }
        sweepPadding = (filterRadius + 1.0f) * ((maximum - minimum).Length() + 2.0f) / farResolution * 1.415f;
    }
    int32 keptCount = 0;
    for (int32 i = 0; i < bounds.Count(); i++)
    {
        if (bounds[i].UseProxy && !CanReachView(bounds[i], sweepPlanes, sweepPadding))
            continue;
        bounds[keptCount] = bounds[i];
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
    const Viewport tileViewports[2] = {
        Viewport(0.0f, 0.0f, (float)farResolution, (float)farResolution),
        Viewport((float)farResolution, 0.0f, (float)resolution, (float)resolution),
    };
    Array<BoundingBox> lightCasterBounds;
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
        for (int32 i = 0; i < count; i++)
        {
            if ((bounds[i].UseProxy != 0) != proxies || (selected && (selected[i] & tierBit) == 0))
                continue;
            if (tier == 1)
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
            lightContext.List->SortDrawCalls(lightContext, false, depthList, DrawPass::Depth);
            lightContext.List->ExecuteDrawCalls(lightContext, depthList);
        }
        context->ResetRenderTarget();
        RenderList::ReturnToPool(lightContext.List);
        lightContext.List = nullptr;
    };

    if (lightDepth)
    {
        Matrix lightView;
        const Float3 direction(LightDirection);
        const Float3 up = Math::Abs(direction.Y) > 0.99f ? Float3::Right : Float3::Up;
        Matrix::LookAt(Float3::Zero, direction, up, lightView);

        BoundingBox lightBounds;
        lightCasterBounds.EnsureCapacity(count);
        for (int32 i = 0; i < count; i++)
        {
            const BoundingBox box(Vector3(bounds[i].Minimum), Vector3(bounds[i].Maximum));
            const BoundingBox transformed = BoundingBox::Transform(box, lightView);
            lightCasterBounds.Add(transformed);
            if (i == 0)
                lightBounds = transformed;
            else
                BoundingBox::Merge(lightBounds, transformed, lightBounds);
        }
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
            Matrix inverseProjection, inverseView, cameraToLight;
            Matrix::Invert(view->Projection, inverseProjection);
            Matrix::Invert(view->View, inverseView);
            Matrix::Multiply(inverseView, lightView, cameraToLight);
            Float3 corners[8];
            for (int32 i = 0; i < 4; i++)
            {
                const Float3 clip(i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, GPU_DEPTH_RANGE_MIN);
                Float3 point;
                Float3::TransformCoordinate(clip, inverseProjection, point);
                Float3::TransformCoordinate(point, cameraToLight, corners[i]);
                const float splitDepth = Math::Max(nearShadowDistance, point.Z + 1.0f);
                if (view->Projection.M34 != 0.0f)
                    point *= splitDepth / point.Z;
                else
                    point.Z = splitDepth;
                Float3::TransformCoordinate(point, cameraToLight, corners[i + 4]);
            }
            Float3 nearMin = corners[0];
            Float3 nearMax = corners[0];
            for (const auto& corner : corners)
            {
                nearMin = Float3::Min(nearMin, corner);
                nearMax = Float3::Max(nearMax, corner);
            }
            // Leave room for filtering and for snapping the center to whole texels.
            const Float2 span(nearMax.X - nearMin.X, nearMax.Y - nearMin.Y);
            const Float2 padding = Float2::Max(span * ((filterRadius + 2.0f) / resolution), Float2::One);
            nearMin -= Float3(padding.X, padding.Y, 0);
            nearMax += Float3(padding.X, padding.Y, 0);
            nearMin = Float3::Max(nearMin, minimum);
            nearMax = Float3::Min(nearMax, maximum);
            if (nearMax.X > nearMin.X && nearMax.Y > nearMin.Y)
            {
                const Float2 texel((nearMax.X - nearMin.X) / resolution, (nearMax.Y - nearMin.Y) / resolution);
                const Float2 center((nearMax.X + nearMin.X) * 0.5f, (nearMax.Y + nearMin.Y) * 0.5f);
                const Float2 offset = Float2(Math::Round(center.X / texel.X), Math::Round(center.Y / texel.Y)) * texel - center;
                nearMin += Float3(offset.X, offset.Y, 0);
                nearMax += Float3(offset.X, offset.Y, 0);
                nearMin.Z = minimum.Z;
                nearMax.Z = maximum.Z;
                nearBounds = BoundingBox(Vector3(nearMin), Vector3(nearMax));
                tierCount = 2;
                data.ShadowSplit = Float4(nearShadowDistance * 0.85f, nearShadowDistance, 1.0f, 0.0f);
            }
        }

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

    if (filterRadius > 0 && lightDepth)
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
    context->UpdateBuffer(BoundsBuffer, bounds.Get(), count * sizeof(CasterBounds));
    context->UpdateCB(constants, &data);
    context->BindCB(1, constants);
    context->BindSR(2, depth);
    context->BindSR(3, BoundsBuffer->View());
    context->BindSR(7, lightDepth ? lightDepth : depth);
    context->BindSR(8, receiverNormals ? receiverNormals : depth);
    context->BindUA(1, SelectionBuffer->View());
    if (debugColor)
    {
        const uint32 clear[4] = {};
        context->ClearUA(DebugMask, clear);
        context->BindUA(2, DebugMask->View());
    }
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Receiver Selection");
        context->Dispatch(shader, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
    }
    context->ResetUA();
    context->ResetSR();

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

    if (!ShowReceiverHits && sceneColor && lightDepth)
    {
        Array<uint32> selected;
        const bool indirect = indirectShadowDraws && device->Limits.HasDrawIndirect;
        if (!indirect && device->GetRendererType() == RendererType::DirectX11)
        {
            if (!SelectionReadback)
                SelectionReadback = device->CreateBuffer(TEXT("V2 Shadow Selection Readback"));
            const auto bytes = (uint32)count * sizeof(uint32);
            if (SelectionReadback && (SelectionReadback->GetSize() >= bytes ||
                !SelectionReadback->Init(SelectionBuffer->GetDescription().ToStagingReadback())))
            {
                PROFILE_CPU_NAMED("V2 Shadow Selection Readback");
                context->CopyBuffer(SelectionReadback, SelectionBuffer, (uint32)bytes);
                if (const auto* flags = static_cast<const uint32*>(SelectionReadback->Map(GPUResourceMapMode::Read)))
                {
                    selected.Add(flags, count);
                    SelectionReadback->Unmap();
                }
            }
        }

        // Explicit APIs need a GPU-driven draw path or a synchronized readback.
        // Draw all expensive casters when a same-frame CPU result is unavailable.
        LastUsedSelection = selected.Count() == count;
        if (LastUsedSelection)
        {
            for (int32 i = 0; i < count; i++)
                if (bounds[i].UseProxy && selected[i] == 0)
                    LastRejectedProxyCount++;
        }
        for (int32 tier = 0; tier < tierCount; tier++)
            drawShadowDepth(tier, true, selected.Count() == count ? selected.Get() : nullptr, indirect);

        auto* baseColor = resources.Get<GPUTexture>(TEXT("Scene.BaseColor"));
        auto* material = resources.Get<GPUTexture>(TEXT("Scene.Material"));
        auto* emissive = resources.Get<GPUTexture>(TEXT("Scene.Emissive"));
        auto* pixelShader = SelectionShader->GPU->GetPS("PS_V2DirectionalLighting");
        auto* resolveConstants = SelectionShader->GPU->GetCB(3);
        if (baseColor && receiverNormals && material && emissive && pixelShader && resolveConstants && resolveConstants->GetSize() == sizeof(ResolveConstants))
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

                if (filterRadius == 0 && screenSpaceFilter)
                {
                    const auto preparePass = [&](GPUPipelineState*& state, GPUTexture*& target, const char* entry, const Char* name, PixelFormat format)
                    {
                        auto* shader = SelectionShader->GPU->GetPS(entry);
                        if (!shader)
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
                            desc.PS = shader;
                            if (state->Init(desc))
                                return false;
                        }
                        return (target->Width() == depth->Width() && target->Height() == depth->Height() && target->GetMemoryUsage()) ||
                            !target->Init(GPUTextureDescription::New2D(depth->Width(), depth->Height(), format,
                                GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget));
                    };
                    if (preparePass(ShadowMaskState, ShadowMask, "PS_V2ShadowMask", TEXT("V2 Shadow Mask"), PixelFormat::R16G16_Float) &&
                        preparePass(ShadowFilterState, FilteredShadowMask, "PS_V2ShadowFilter", TEXT("V2 Filtered Shadow Mask"), PixelFormat::R8_UNorm))
                    {
                        resolve.UseFilteredShadow = 1;
                        context->ResetSR();
                        context->UpdateCB(resolveConstants, &resolve);
                        context->BindCB(3, resolveConstants);
                        context->BindSR(2, depth);
                        context->BindSR(7, lightDepth);
                        context->BindSR(8, receiverNormals);
                        context->SetViewportAndScissors((float)depth->Width(), (float)depth->Height());
                        {
                            PROFILE_GPU_CPU_NAMED("V2 Shadow Mask");
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
                        resources.SetOutput(TEXT("Shadow.Visibility"), FilteredShadowMask);
                    }
                }

                context->ResetSR();
                context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
                context->SetRenderTarget(sceneColor->View());
                if (!resolve.UseFilteredShadow)
                {
                    context->UpdateCB(resolveConstants, &resolve);
                    context->BindCB(3, resolveConstants);
                }
                context->BindSR(2, depth);
                context->BindSR(7, lightDepth);
                context->BindSR(8, receiverNormals);
                context->BindSR(9, baseColor);
                context->BindSR(10, material);
                context->BindSR(11, emissive);
                if (resolve.UseFilteredShadow)
                    context->BindSR(13, FilteredShadowMask);
                context->SetState(ResolveState);
                context->DrawFullscreenTriangle();
                context->ResetRenderTarget();
                context->ResetSR();
                resources.Set(TEXT("Shadow.Depth"), lightDepth);
                resources.SetOutput(TEXT("Shadow.Depth"), lightDepth);
            }
        }
    }

    LastCasterCount = count;
    resources.Set(TEXT("Shadow.CasterSelection"), SelectionBuffer);
    resources.SetOutput(TEXT("Shadow.CasterSelection"), SelectionBuffer);
    return StageResult::Completed;
}

void ShadowBoxCasterStage::OnEndFrame(GPUContext* context)
{
    Casters.Clear();
}
