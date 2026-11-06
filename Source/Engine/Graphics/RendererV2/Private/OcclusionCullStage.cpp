// Copyright (c) Wojciech Figat. All rights reserved.

#include "../OcclusionCullStage.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Profiler/Profiler.h"
#include "Engine/Renderer/RenderList.h"

namespace
{
    struct OcclusionBounds
    {
        Float4 Minimum;
        Float4 Maximum;
    };

    GPU_CB_STRUCT(OcclusionConstants
    {
        Matrix ViewProjection;
        uint32 Width;
        uint32 Height;
        uint32 Count;
        uint32 GroupsPerRow;
    });
}

OcclusionCullStage::OcclusionCullStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

OcclusionCullStage::~OcclusionCullStage()
{
    if (DepthBuffers)
        Delete(DepthBuffers);
    SAFE_DELETE_GPU_RESOURCE(BoundsBuffer);
    SAFE_DELETE_GPU_RESOURCE(ResultsBuffer);
    SAFE_DELETE_GPU_RESOURCE(ReadbackBuffer);
}

StageResult OcclusionCullStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    Occluded.Clear();
    LastOccludedCount = 0;
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    auto* device = context->GetDevice();
    const auto& rect = resources.GetViewRect();
    if (!Enabled || !view || objects.IsEmpty() || rect.Width <= 0 || rect.Height <= 0 ||
        device->GetRendererType() != RendererType::DirectX11 || !device->Limits.HasCompute ||
        context != device->GetMainContext())
        return StageResult::Completed;
    const bool indirect = UseIndirectDraws && device->Limits.HasDrawIndirect;

    if (!ShaderAsset)
        ShaderAsset = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/Culling"));
    if (!ShaderAsset || !ShaderAsset->IsLoaded() || !ShaderAsset->GPU)
        return StageResult::Completed;
    auto* shader = ShaderAsset->GPU->GetCS("CS_V2DepthOcclusion");
    auto* cb = ShaderAsset->GPU->GetCB(2);
    if (!shader || !cb || cb->GetSize() != sizeof(OcclusionConstants))
        return StageResult::Completed;

    if (!DepthBuffers)
        DepthBuffers = New<RenderBuffers>();
    if (DepthBuffers->Init((int32)rect.Width, (int32)rect.Height))
    {
        Delete(DepthBuffers);
        DepthBuffers = nullptr;
        return StageResult::Completed;
    }

    RenderContext depthContext;
    depthContext.Buffers = DepthBuffers;
    depthContext.List = RenderList::GetFromPool();
    auto& depthView = depthContext.View;
    depthView.Pass = DrawPass::Depth;
    depthView.Flags = ViewFlags::None;
    depthView.SetUp(view->View, view->Projection);
    depthView.Position = Float3(view->LocalTransform.Translation);
    depthView.Direction = Float3(Vector3::Transform(Vector3::Forward, view->LocalTransform.Orientation));
    if (auto* source = viewpoint->GetIC_Source())
        if (const auto* pose = static_cast<const Transform*>(source->GetIC_WorldTransform()))
            depthView.Origin = pose->Translation - view->LocalTransform.Translation;
    depthView.PrevViewProjection = depthView.ViewProjection();
    depthView.PrepareCache(depthContext, rect.Width, rect.Height, Float2::Zero);
    depthContext.List->Init(depthContext);

    Array<OcclusionBounds> bounds;
    bounds.EnsureCapacity(objects.Count());
    for (auto* object : objects)
    {
        auto* warped = dynamic_cast<WarpedVisualData*>(object);
        if (!warped)
        {
            bounds.Add({ Float4::Zero, Float4::Zero });
            continue;
        }
        bounds.Add({ Float4(Float3(warped->Bounds.Minimum), 1.0f), Float4(Float3(warped->Bounds.Maximum), 0.0f) });

        auto* actor = dynamic_cast<StaticModel*>(warped->GetIVD_Source());
        auto* model = actor ? actor->Model.Get() : nullptr;
        if (!model || !model->IsLoaded())
            continue;
        const int32 lod = model->HighestResidentLODIndex();
        if (lod < 0 || lod >= model->LODs.Count())
            continue;

        const auto& entries = actor->GetEntries();
        for (const auto& mesh : model->LODs[lod].Meshes)
        {
            if (mesh.GetTriangleCount() > OccluderTriangleLimit)
                continue;
            const int32 slot = mesh.GetMaterialSlotIndex();
            if (slot < 0 || slot >= model->MaterialSlots.Count())
                continue;
            MaterialBase* material = nullptr;
            if (slot < entries.Count())
            {
                if (!entries[slot].Visible)
                    continue;
                material = entries[slot].Material.Get();
            }
            if (!material || !material->IsReady())
                material = model->MaterialSlots[slot].Material.Get();
            if (!material || !material->IsReady())
                material = device->GetDefaultMaterial();
            if (!material || !material->IsReady() ||
                !EnumHasAllFlags(material->GetDrawModes(), DrawPass::Depth | DrawPass::GBuffer))
                continue;
            mesh.Draw(depthContext, material, warped->WorldMatrix, actor->GetStaticFlags(),
                      false, DrawPass::Depth, actor->GetPerInstanceRandom());
        }
    }

    constexpr auto depthList = DrawCallsListType::Depth;
    if (depthContext.List->DrawCallsLists[(int32)depthList].IsEmpty())
    {
        RenderList::ReturnToPool(depthContext.List);
        return StageResult::Completed;
    }

    auto* depth = DepthBuffers->DepthBuffer;
    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    context->SetViewportAndScissors(DepthBuffers->GetViewport());
    context->SetRenderTarget(depth->View(), static_cast<GPUTextureView*>(nullptr));
    context->ClearDepth(depth->View());
    depthContext.List->SortDrawCalls(depthContext, false, depthList, DrawPass::Depth);
    depthContext.List->ExecuteDrawCalls(depthContext, depthList);
    context->ResetRenderTarget();
    context->ResetSR();
    RenderList::ReturnToPool(depthContext.List);

    if (!BoundsBuffer)
        BoundsBuffer = device->CreateBuffer(TEXT("V2 Occlusion Bounds"));
    if (!ResultsBuffer)
        ResultsBuffer = device->CreateBuffer(TEXT("V2 Occlusion Results"));
    if (!indirect && !ReadbackBuffer)
        ReadbackBuffer = device->CreateBuffer(TEXT("V2 Occlusion Readback"));
    if (!BoundsBuffer || !ResultsBuffer || (!indirect && !ReadbackBuffer))
        return StageResult::Completed;

    const int32 count = bounds.Count();
    const auto resultDesc = GPUBufferDescription::Structured(count, sizeof(uint32), true);
    if ((BoundsBuffer->GetElementsCount() < (uint32)count &&
         BoundsBuffer->Init(GPUBufferDescription::Structured(count, sizeof(OcclusionBounds)))) ||
        (ResultsBuffer->GetElementsCount() < (uint32)count && ResultsBuffer->Init(resultDesc)) ||
        (!indirect && ReadbackBuffer->GetSize() < resultDesc.Size &&
         ReadbackBuffer->Init(resultDesc.ToStagingReadback())))
        return StageResult::Completed;

    OcclusionConstants data;
    Matrix::Transpose(view->ViewProjection, data.ViewProjection);
    data.Width = depth->Width();
    data.Height = depth->Height();
    data.Count = count;
    data.GroupsPerRow = Math::Min(count, 1024);
    context->UpdateBuffer(BoundsBuffer, bounds.Get(), count * sizeof(OcclusionBounds));
    context->UpdateCB(cb, &data);
    context->BindCB(2, cb);
    context->BindSR(5, depth);
    context->BindSR(6, BoundsBuffer->View());
    context->BindUA(0, ResultsBuffer->View());
    context->Dispatch(shader, data.GroupsPerRow, Math::DivideAndRoundUp(data.Count, data.GroupsPerRow));
    context->ResetUA();
    context->ResetSR();

    if (indirect)
    {
        // Indices in this buffer refer to the forwarded objects in their original order.
        resources.Set(TEXT("Occlusion.Visibility"), ResultsBuffer);
        return StageResult::Completed;
    }

    context->CopyBuffer(ReadbackBuffer, ResultsBuffer, count * sizeof(uint32));

    int32 visibleCount = 0;
    {
        PROFILE_CPU_NAMED("V2 Occlusion Readback");
        const auto* visible = static_cast<const uint32*>(ReadbackBuffer->Map(GPUResourceMapMode::Read));
        if (!visible)
            return StageResult::Completed;
        for (int32 i = 0; i < count; i++)
        {
            if (visible[i])
                objects[visibleCount++] = objects[i];
            else
                Occluded.Add(objects[i]);
        }
        ReadbackBuffer->Unmap();
    }
    objects.Resize(visibleCount);
    LastOccludedCount = Occluded.Count();
    return StageResult::Completed;
}

void OcclusionCullStage::OnEndFrame(GPUContext* context)
{
    Occluded.Clear();
}
