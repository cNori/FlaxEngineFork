// Copyright (c) Wojciech Figat. All rights reserved.

#include "../ShadowBoxCasterStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUPipelineState.h"
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
    struct CasterBounds
    {
        Float3 Minimum;
        uint32 UseProxy;
        Float3 Maximum;
        uint32 DebugCategory;
    };
    static_assert(sizeof(CasterBounds) == 32, "Caster bounds must match the shader layout.");

    GPU_CB_STRUCT(ResolveConstants
    {
        Matrix InverseViewProjection;
        Matrix LightViewProjection;
        Float3 DirectionToLight;
        float AmbientIntensity;
        Float3 CameraPosition;
        uint32 IsOrthographic;
        Float3 CameraForward;
        float Padding;
        Float4 LightRadiance;
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
        uint32 Padding;
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
    SAFE_DELETE_GPU_RESOURCE(SelectionReadback);
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
}

void ShadowBoxCasterStage::SetProjection(const Vector3& lightDirection, float distance)
{
    LightDirection = lightDirection;
    LightDirection.Normalize();
    ProjectionDistance = distance;
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
            entry.UseProxy = triangles > ProxyTriangleThreshold ? 1 : 0;
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
    GPUTexture* lightDepth = nullptr;
    RenderContext lightContext;
    const auto drawShadowDepth = [&](bool proxies, const uint32* selected)
    {
        PROFILE_GPU_CPU_NAMED("V2 Shadow Depth");
        lightContext.List = RenderList::GetFromPool();
        lightContext.List->Init(lightContext);
        for (int32 i = 0; i < count; i++)
        {
            if ((bounds[i].UseProxy != 0) != proxies || (selected && !selected[i]))
                continue;
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
                mesh.Draw(lightContext, material, warped->WorldMatrix, actor->GetStaticFlags(),
                          false, DrawPass::Depth, actor->GetPerInstanceRandom());
            }
        }

        constexpr auto depthList = DrawCallsListType::Depth;
        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->SetViewportAndScissors(LightDepthBuffers->GetViewport());
        context->SetRenderTarget(lightDepth->View(), static_cast<GPUTextureView*>(nullptr));
        if (!proxies)
            context->ClearDepth(lightDepth->View());
        if (!lightContext.List->DrawCallsLists[(int32)depthList].IsEmpty())
        {
            lightContext.List->SortDrawCalls(lightContext, false, depthList, DrawPass::Depth);
            lightContext.List->ExecuteDrawCalls(lightContext, depthList);
        }
        context->ResetRenderTarget();
        RenderList::ReturnToPool(lightContext.List);
        lightContext.List = nullptr;
    };

    if (context == device->GetMainContext())
    {
        Matrix lightView;
        const Float3 direction(LightDirection);
        const Float3 up = Math::Abs(direction.Y) > 0.99f ? Float3::Right : Float3::Up;
        Matrix::LookAt(Float3::Zero, direction, up, lightView);

        BoundingBox lightBounds;
        for (int32 i = 0; i < count; i++)
        {
            const BoundingBox box(Vector3(bounds[i].Minimum), Vector3(bounds[i].Maximum));
            const BoundingBox transformed = BoundingBox::Transform(box, lightView);
            if (i == 0)
                lightBounds = transformed;
            else
                BoundingBox::Merge(lightBounds, transformed, lightBounds);
        }
        const Float3 minimum = Float3(lightBounds.Minimum) - Float3::One;
        const Float3 maximum = Float3(lightBounds.Maximum) + Float3::One;
        Matrix lightProjection;
        Matrix::OrthoOffCenter(minimum.X, maximum.X, minimum.Y, maximum.Y, minimum.Z, maximum.Z, lightProjection);

        constexpr int32 resolution = 2048;
        if (!LightDepthBuffers)
            LightDepthBuffers = New<RenderBuffers>();
        if (LightDepthBuffers->Init(resolution, resolution))
        {
            Delete(LightDepthBuffers);
            LightDepthBuffers = nullptr;
        }
        if (LightDepthBuffers)
        {
            lightContext.Buffers = LightDepthBuffers;
            lightContext.View.Pass = DrawPass::Depth;
            lightContext.View.Flags = ViewFlags::None;
            lightContext.View.Direction = direction;
            lightContext.View.SetUp(lightView, lightProjection);
            lightContext.View.PrevViewProjection = lightContext.View.ViewProjection();
            lightContext.View.Near = minimum.Z;
            lightContext.View.Far = maximum.Z;
            lightContext.View.PrepareCache(lightContext, resolution, resolution, Float2::Zero);

            lightDepth = LightDepthBuffers->DepthBuffer;
            Matrix::Transpose(lightContext.View.ViewProjection(), data.LightViewProjection);
            data.HasLightDepth = 1;
            drawShadowDepth(false, nullptr);
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
    context->Dispatch(shader, data.GroupsPerRow, Math::DivideAndRoundUp(data.CasterCount, data.GroupsPerRow));
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
        if (device->GetRendererType() == RendererType::DirectX11)
        {
            if (!SelectionReadback)
                SelectionReadback = device->CreateBuffer(TEXT("V2 Shadow Selection Readback"));
            const auto bytes = (uint32)count * sizeof(uint32);
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

        // Explicit APIs need a GPU-driven draw path or a synchronized readback.
        // Draw all expensive casters when a same-frame CPU result is unavailable.
        LastUsedSelection = selected.Count() == count;
        if (LastUsedSelection)
        {
            for (int32 i = 0; i < count; i++)
                if (bounds[i].UseProxy && selected[i] == 0)
                    LastRejectedProxyCount++;
        }
        drawShadowDepth(true, LastUsedSelection ? selected.Get() : nullptr);

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
                context->ResetSR();
                context->SetViewportAndScissors((float)sceneColor->Width(), (float)sceneColor->Height());
                context->SetRenderTarget(sceneColor->View());
                context->UpdateCB(resolveConstants, &resolve);
                context->BindCB(3, resolveConstants);
                context->BindSR(2, depth);
                context->BindSR(7, lightDepth);
                context->BindSR(8, receiverNormals);
                context->BindSR(9, baseColor);
                context->BindSR(10, material);
                context->BindSR(11, emissive);
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
