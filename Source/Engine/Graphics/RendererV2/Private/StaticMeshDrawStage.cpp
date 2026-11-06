// Copyright (c) Wojciech Figat. All rights reserved.

#include "../StaticMeshDrawStage.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Assets/MaterialBase.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Level/Actors/Camera.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Profiler/Profiler.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Renderer/RenderList.h"

RenderPipelineDrawStage::RenderPipelineDrawStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

RenderPipelineDrawStage::~RenderPipelineDrawStage()
{
    SAFE_DELETE_GPU_RESOURCE(EmissiveBuffer);
    SAFE_DELETE_GPU_RESOURCE(IndirectInputs);
    SAFE_DELETE_GPU_RESOURCE(IndirectOutput);
    SAFE_DELETE_GPU_RESOURCE(IndirectArguments);
    if (MaterialBuffers)
        Delete(MaterialBuffers);
}

void RenderPipelineDrawStage::BuildIndirectDraws(GPUContext* context, GPUBuffer* visibility, RenderList* list, const Array<uint32>& objectIndices)
{
    const int32 count = objectIndices.Count();
    if (count == 0 || count > 65535 * 64 || count != list->DrawCalls.Count())
        return;
    PROFILE_GPU_CPU_NAMED("V2 Camera Indirect Arguments");
    if (!CullingShader)
        CullingShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/Culling"));
    auto* shader = CullingShader && CullingShader->IsLoaded() && CullingShader->GPU
        ? CullingShader->GPU->GetCS("CS_V2ShadowIndirectArgs") : nullptr;
    if (!shader)
        return;

    auto* device = context->GetDevice();
    if (!IndirectInputs)
        IndirectInputs = device->CreateBuffer(TEXT("V2 Camera Indirect Inputs"));
    if (!IndirectOutput)
        IndirectOutput = device->CreateBuffer(TEXT("V2 Camera Indirect Output"));
    if (!IndirectArguments)
        IndirectArguments = device->CreateBuffer(TEXT("V2 Camera Indirect Arguments"));

    // Reuse the shadow argument kernel. The header carries the live draw count.
    struct IndirectInput
    {
        uint32 ObjectIndex;
        uint32 IndexCount;
    };
    const uint32 bytes = count * sizeof(GPUDrawIndexedIndirectArgs);
    if (!IndirectInputs || !IndirectOutput || !IndirectArguments ||
        (IndirectInputs->GetElementsCount() < (uint32)(count + 1) &&
         IndirectInputs->Init(GPUBufferDescription::Structured(count + 1, sizeof(IndirectInput)))) ||
        (IndirectOutput->GetSize() < bytes &&
         IndirectOutput->Init(GPUBufferDescription::Typed(bytes / sizeof(uint32), PixelFormat::R32_UInt, true))) ||
        (IndirectArguments->GetSize() < bytes &&
         IndirectArguments->Init(GPUBufferDescription::Argument(bytes))))
        return;

    auto* draws = list->DrawCalls.Get();
    Array<IndirectInput> inputs;
    inputs.Resize(count + 1);
    inputs[0] = { (uint32)count, 0 };
    for (int32 i = 0; i < count; i++)
        inputs[i + 1] = { objectIndices[i], (uint32)draws[i].Draw.IndicesCount };

    context->UpdateBuffer(IndirectInputs, inputs.Get(), inputs.Count() * sizeof(IndirectInput));
    context->BindSR(0, visibility->View());
    context->BindSR(1, IndirectInputs->View());
    context->BindUA(0, IndirectOutput->View());
    context->Dispatch(shader, Math::DivideAndRoundUp(count, 64));
    context->ResetUA();
    context->ResetSR();
    context->CopyBuffer(IndirectArguments, IndirectOutput, bytes);

    for (int32 i = 0; i < count; i++)
    {
        draws[i].InstanceCount = 0;
        draws[i].Draw.IndirectArgsBuffer = IndirectArguments;
        draws[i].Draw.IndirectArgsOffset = i * sizeof(GPUDrawIndexedIndirectArgs);
    }
}

StageResult RenderPipelineDrawStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    auto* color = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    const auto* warpedView = dynamic_cast<WarpedViewpoint*>(viewpoint);
    if (!color || !warpedView)
        return StageResult::Skipped;

    // The existing GBuffer and draw-list executors record on the main context.
    auto* device = context->GetDevice();
    if (context != device->GetMainContext())
        return StageResult::Failed;

    if (!MaterialBuffers)
        MaterialBuffers = New<RenderBuffers>();
    if (MaterialBuffers->Init(color->Width(), color->Height()))
    {
        Delete(MaterialBuffers);
        MaterialBuffers = nullptr;
        return StageResult::Failed;
    }

    if (!EmissiveBuffer)
        EmissiveBuffer = device->CreateTexture(TEXT("V2 Material Emissive"));
    if (!EmissiveBuffer)
        return StageResult::Failed;
    if (EmissiveBuffer->Width() != color->Width() || EmissiveBuffer->Height() != color->Height() || !EmissiveBuffer->GetMemoryUsage())
    {
        const auto desc = GPUTextureDescription::New2D(color->Width(), color->Height(),
            PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::RenderTarget);
        if (EmissiveBuffer->Init(desc))
            return StageResult::Failed;
    }

    RenderContext materialContext;
    materialContext.Buffers = MaterialBuffers;
    materialContext.List = RenderList::GetFromPool();
    auto& view = materialContext.View;
    view.Pass = DrawPass::GBuffer;
    view.Flags = ViewFlags::None;
    view.SetUp(warpedView->View, warpedView->Projection);
    view.Position = Float3(warpedView->LocalTransform.Translation);
    view.Direction = Float3(Vector3::Transform(Vector3::Forward, warpedView->LocalTransform.Orientation));
    auto* sourceView = viewpoint->GetIC_Source();
    if (const auto* pose = static_cast<const Transform*>(sourceView->GetIC_WorldTransform()))
        view.Origin = pose->Translation - warpedView->LocalTransform.Translation;
    if (const auto* camera = dynamic_cast<Camera*>(sourceView))
    {
        view.Near = camera->GetNearPlane();
        view.Far = camera->GetFarPlane();
    }
    view.PrevViewProjection = view.ViewProjection();
    view.PrepareCache(materialContext, (float)color->Width(), (float)color->Height(), Float2::Zero);
    materialContext.List->Init(materialContext);

    auto* visibility = resources.Get<GPUBuffer>(TEXT("Occlusion.Visibility"));
    if (visibility && visibility->GetElementsCount() < (uint32)objects.Count())
        visibility = nullptr;
    Array<uint32> objectIndices;
    for (int32 objectIndex = 0; objectIndex < objects.Count(); objectIndex++)
    {
        auto* warped = dynamic_cast<WarpedVisualData*>(objects[objectIndex]);
        auto* actor = warped ? dynamic_cast<StaticModel*>(warped->GetIVD_Source()) : nullptr;
        auto* model = actor ? actor->Model.Get() : nullptr;
        if (!model || !model->IsLoaded())
            continue;
        const int32 lod = model->HighestResidentLODIndex();
        if (lod < 0 || lod >= model->LODs.Count())
            continue;

        const auto& entries = actor->GetEntries();
        for (const auto& mesh : model->LODs[lod].Meshes)
        {
            const int32 slotIndex = mesh.GetMaterialSlotIndex();
            if (slotIndex < 0 || slotIndex >= model->MaterialSlots.Count())
                continue;
            MaterialBase* material = nullptr;
            if (slotIndex < entries.Count())
            {
                const auto& entry = entries[slotIndex];
                if (!entry.Visible)
                    continue;
                material = entry.Material.Get();
            }
            if (!material || !material->IsReady())
                material = model->MaterialSlots[slotIndex].Material.Get();
            if (!material || !material->IsReady())
                material = device->GetDefaultMaterial();
            if (!material || !material->IsReady())
                continue;

            const int32 firstDraw = materialContext.List->DrawCalls.Count();
            mesh.Draw(materialContext, material, warped->WorldMatrix,
                      actor->GetStaticFlags(), false, DrawPass::GBuffer, actor->GetPerInstanceRandom());
            if (visibility)
            {
                const int32 endDraw = materialContext.List->DrawCalls.Count();
                for (int32 i = firstDraw; i < endDraw; i++)
                    objectIndices.Add((uint32)objectIndex);
            }
        }
    }

    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    if (visibility)
        BuildIndirectDraws(context, visibility, materialContext.List, objectIndices);
    constexpr auto listType = DrawCallsListType::GBufferNoDecals;
    if (!materialContext.List->DrawCallsLists[(int32)listType].IsEmpty())
        materialContext.List->SortDrawCalls(materialContext, false, listType, DrawPass::GBuffer);

    GBufferPass::Instance()->Fill(materialContext, EmissiveBuffer);
    RenderList::ReturnToPool(materialContext.List);

    // Preview the material base color until the pipeline has a lighting pass.
    context->ResetSR();
    context->Draw(color, MaterialBuffers->GBuffer0);
    context->ResetSR();
    resources.Set(TEXT("Scene.Depth"), MaterialBuffers->DepthBuffer);
    resources.Set(TEXT("Scene.Normals"), MaterialBuffers->GBuffer1);
    resources.Set(TEXT("Scene.BaseColor"), MaterialBuffers->GBuffer0);
    resources.Set(TEXT("Scene.Material"), MaterialBuffers->GBuffer2);
    resources.Set(TEXT("Scene.Emissive"), EmissiveBuffer);
    resources.SetOutput(TEXT("Depth"), MaterialBuffers->DepthBuffer);
    return StageResult::Completed;
}
