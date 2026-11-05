// Copyright (c) Wojciech Figat. All rights reserved.

#include "../StaticMeshDrawStage.h"
#include "../RenderCameraData.h"
#include "../WorldWarpStage.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUVertexLayout.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"

namespace
{
    // Matches the existing Shaders/Utils/DebugDraw constant buffer.
    GPU_CB_STRUCT(DrawConstants {
        Matrix ViewProjection;
        Float2 SceneDepthUVScale;
        float ClipPosZBias;
        uint32 EnableDepthTest;
    });
}

RenderPipelineDrawStage::RenderPipelineDrawStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

RenderPipelineDrawStage::~RenderPipelineDrawStage()
{
    if (ShaderAsset)
        ShaderAsset->Reloading.Unbind<RenderPipelineDrawStage, &RenderPipelineDrawStage::OnShaderReloading>(this);
    SAFE_DELETE_GPU_RESOURCE(State);
    SAFE_DELETE_GPU_RESOURCE(ColorBuffer);
}

void RenderPipelineDrawStage::OnShaderReloading(Asset* asset)
{
    Ready = false;
    if (State)
        State->ReleaseGPU();
}

void RenderPipelineDrawStage::OnBeginFrame(GPUContext* context)
{
    Ready = false;
    if (!ShaderAsset)
    {
        ShaderAsset = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/DebugDraw"));
        if (ShaderAsset)
            ShaderAsset->Reloading.Bind<RenderPipelineDrawStage, &RenderPipelineDrawStage::OnShaderReloading>(this);
    }
    if (!ShaderAsset || !ShaderAsset->IsLoaded() || !ShaderAsset->GPU)
        return;
    auto device = context->GetDevice();
    if (!State)
        State = device->CreatePipelineState();
    if (!State)
        return;
    if (!State->IsValid())
    {
        auto desc = GPUPipelineState::Description::Default;
        desc.VS = ShaderAsset->GPU->GetVS("VS");
        desc.PS = ShaderAsset->GPU->GetPS("PS", 1);
        desc.CullMode = CullMode::TwoSided;
        if (State->Init(desc))
            return;
    }
    if (!ColorBuffer)
    {
        // A single instance color, not a copy of the mesh's vertex data.
        const Float4 color(0.75f, 0.8f, 0.9f, 1.0f);
        Array<VertexElement, FixedAllocation<GPU_MAX_VS_ELEMENTS>> elements;
        VertexElement e = {};
        e.Type = VertexElement::Types::Color;
        e.Format = PixelFormat::R32G32B32A32_Float;
        e.PerInstance = 1;
        elements.Add(e);
        auto layout = GPUVertexLayout::Get(elements);
        ColorBuffer = device->CreateBuffer(TEXT("V2 Diagnostic Mesh Color"));
        if (!ColorBuffer)
            return;
        auto desc = GPUBufferDescription::Vertex(layout, sizeof(Float4), 1, &color);
        if (ColorBuffer->Init(desc))
        {
            SAFE_DELETE_GPU_RESOURCE(ColorBuffer);
            return;
        }
    }
    Ready = true;
}

StageResult RenderPipelineDrawStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources)
{
    auto color = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    auto depth = resources.Get<GPUTexture>(TEXT("Scene.Depth"));
    if (!Ready || !color || !depth)
        return StageResult::Skipped;

    auto camera = dynamic_cast<RenderCameraData*>(viewpoint);
    if (!camera)
        return StageResult::Skipped;
    auto cb = ShaderAsset->GPU->GetCB(0);
    if (!cb || cb->GetSize() != sizeof(DrawConstants))
        return StageResult::Failed;
    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    context->SetViewportAndScissors((float)color->Width(), (float)color->Height());
    context->SetRenderTarget(depth->View(), color->View());
    context->SetState(State);
    context->BindCB(0, cb);

    static uint64 StaticModelActorRenderTypeID = StringUtils::GetHashCode(TEXT("StaticModelActor"));

    for (auto object : objects)
    {
        const auto* warped = dynamic_cast<WarpedStaticMesh*>(object);
        if (warped)
            object = warped->Source;
        if (!object || object->GetRenderTypeId() != StaticModelActorRenderTypeID)
            continue;
        auto actor = dynamic_cast<StaticModel*>(object);
        if (!actor)
            continue;
        auto model = actor->Model.Get();
        if (!model || !model->IsLoaded())
            continue;
        const int32 lod = model->HighestResidentLODIndex();
        if (lod < 0 || lod >= model->LODs.Count())
            continue;

        const auto& transform = warped ? warped->LocalTransform : actor->GetTransform();
        Matrix world, worldViewProjection;
        const Float3 translation(warped ? transform.Translation : transform.Translation - camera->Origin);
        Matrix::Transformation(transform.Scale, transform.Orientation, translation, world);
        Matrix::Multiply(world, camera->ViewProjection, worldViewProjection);
        DrawConstants data = {};
        Matrix::Transpose(worldViewProjection, data.ViewProjection);
        data.SceneDepthUVScale = Float2::One;
        context->UpdateCB(cb, &data);

        // Bind only positions and a constant instance color for the diagnostic
        // shader. No mesh vertex data is copied or uploaded here.
        for (const auto& mesh : model->LODs[lod].Meshes)
        {
            auto positions = mesh.GetVertexBuffer(0);
            auto indices = mesh.GetIndexBuffer();
            if (!positions || !indices || !mesh.GetTriangleCount())
                continue;
            auto sourceLayout = positions->GetVertexLayout();
            if (!sourceLayout)
                continue;
            auto position = sourceLayout->FindElement(VertexElement::Types::Position);
            if (position.Type != VertexElement::Types::Position)
                continue;
            Array<VertexElement, FixedAllocation<GPU_MAX_VS_ELEMENTS>> elements;
            position.Slot = 0;
            elements.Add(position);
            VertexElement vertexColor = {};
            vertexColor.Type = VertexElement::Types::Color;
            vertexColor.Format = PixelFormat::R32G32B32A32_Float;
            vertexColor.Slot = 1;
            vertexColor.PerInstance = 1;
            elements.Add(vertexColor);
            auto layout = GPUVertexLayout::Get(elements, true);
            GPUBuffer* buffers[] = { positions, ColorBuffer };
            context->BindVB(ToSpan(buffers, 2), nullptr, layout);
            context->BindIB(indices);
            context->DrawIndexed(mesh.GetTriangleCount() * 3);
        }
    }
    return StageResult::Completed;
}
