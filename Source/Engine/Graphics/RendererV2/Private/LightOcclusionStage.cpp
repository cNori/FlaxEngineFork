// Copyright (c) Wojciech Figat. All rights reserved.

#include "../LightOcclusionStage.h"
#include "../ShadowBoxCasterStage.h"
#include "../SpotLightStage.h"
#include "../PointLightStage.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "../WorldWarpStage.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Profiler/Profiler.h"

LightOcclusionStage::LightOcclusionStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

LightOcclusionStage::~LightOcclusionStage()
{
    SAFE_DELETE_GPU_RESOURCE(SurfaceNormals);
}

StageResult LightOcclusionStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    auto* depth = resources.Get<GPUTexture>(TEXT("Scene.Depth"));
    if (!depth) return StageResult::Failed;
    resources.Set(TEXT("Shadow.ReceiverDepth"), depth);
    resources.Set(TEXT("Spot.ReceiverDepth"), depth);
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    if (!view) return StageResult::Failed;
    if (!SurfaceShader) SurfaceShader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Utils/ReceiverSurface"));
    if (!SurfaceShader || !SurfaceShader->IsLoaded() || !SurfaceShader->GPU) return StageResult::Skipped;
    auto* shader = SurfaceShader->GPU->GetCS("CS_V2CameraReceiverSurface");
    auto* cb = SurfaceShader->GPU->GetCB(0);
    struct SurfaceData
    {
        Matrix InverseViewProjection;
        Float4 CameraPosition;
        uint32 Width, Height, Padding[2];
    };
    static_assert(sizeof(SurfaceData) == 96, "Camera surface shader layout");
    if (!shader || !cb || cb->GetSize() != sizeof(SurfaceData)) return StageResult::Failed;
    if (!SurfaceNormals) SurfaceNormals = context->GetDevice()->CreateTexture(TEXT("V2 Camera Receiver Surface"));
    if (!SurfaceNormals || ((SurfaceNormals->Width() != depth->Width() || SurfaceNormals->Height() != depth->Height() || !SurfaceNormals->GetMemoryUsage()) &&
        SurfaceNormals->Init(GPUTextureDescription::New2D(depth->Width(), depth->Height(), PixelFormat::R16G16B16A16_Float,
            GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess)))) return StageResult::Failed;
    {
        PROFILE_GPU_CPU_NAMED("V2 Camera Receiver Surface");
        SurfaceData data = {};
        Matrix inverse;
        Matrix::Invert(view->ViewProjection, inverse);
        Matrix::Transpose(inverse, data.InverseViewProjection);
        data.CameraPosition = Float4(Float3(view->LocalTransform.Translation), 0);
        data.Width = depth->Width(); data.Height = depth->Height();
        context->ResetRenderTarget(); context->ResetSR(); context->ResetUA();
        context->UpdateCB(cb, &data); context->BindCB(0, cb);
        context->BindSR(0, depth); context->BindUA(0, SurfaceNormals->View());
        context->Dispatch(shader, Math::DivideAndRoundUp(data.Width, 8u), Math::DivideAndRoundUp(data.Height, 8u));
        context->ResetUA(); context->ResetSR();
    }
    resources.Set(TEXT("Shadow.ReceiverNormals"), SurfaceNormals);
    resources.SetOutput(TEXT("Shadow.ReceiverSurface"), SurfaceNormals);
    if (Shadows && Shadows->PrepareSelections(context, objects, viewpoint, resources) == StageResult::Failed) return StageResult::Failed;
    if (Spots && Spots->PrepareShadows(context, objects, viewpoint, resources) == StageResult::Failed) return StageResult::Failed;
    if (Points && Points->PrepareShadows(context, objects, viewpoint, resources) == StageResult::Failed) return StageResult::Failed;
    // A view without lights still forwards its objects and receiver depth.
    return StageResult::Completed;
}
