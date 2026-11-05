// Copyright (c) Wojciech Figat. All rights reserved.

#include "../PrepareRenderTargetsStage.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/Textures/GPUTexture.h"

PrepareRenderTargetsStage::PrepareRenderTargetsStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

PrepareRenderTargetsStage::~PrepareRenderTargetsStage()
{
    SAFE_DELETE_GPU_RESOURCE(ColorTarget);
    SAFE_DELETE_GPU_RESOURCE(DepthTarget);
}

void PrepareRenderTargetsStage::SetSize(int32 width, int32 height)
{
    Width = width;
    Height = height;
}

void PrepareRenderTargetsStage::OnBeginFrame(GPUContext* context)
{
    Ready = false;
    if (Width <= 0 || Height <= 0)
        return;
    auto device = context->GetDevice();
    if (!ColorTarget)
        ColorTarget = device->CreateTexture(TEXT("V2 Scene Color"));
    if (!DepthTarget)
        DepthTarget = device->CreateTexture(TEXT("V2 Scene Depth"));
    if (!ColorTarget || !DepthTarget)
        return;
    if (ColorTarget->Width() != Width || ColorTarget->Height() != Height || !ColorTarget->GetMemoryUsage())
    {
        auto desc = GPUTextureDescription::New2D(Width, Height, PixelFormat::R8G8B8A8_UNorm);
        if (ColorTarget->Init(desc))
            return;
    }
    if (DepthTarget->Width() != Width || DepthTarget->Height() != Height || !DepthTarget->GetMemoryUsage())
    {
        auto desc = GPUTextureDescription::New2D(Width, Height, PixelFormat::D32_Float, GPUTextureFlags::DepthStencil | GPUTextureFlags::ShaderResource);
        if (DepthTarget->Init(desc))
            return;
    }
    Ready = true;
}

StageResult PrepareRenderTargetsStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (Width <= 0 || Height <= 0)
        return StageResult::Skipped;
    if (!Ready)
        return StageResult::Failed;
    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    context->SetViewportAndScissors((float)Width, (float)Height);
    context->SetRenderTarget(DepthTarget->View(), ColorTarget->View());
    context->Clear(ColorTarget->View(), ClearColor);
    context->ClearDepth(DepthTarget->View());
    resources.Set(TEXT("Scene.Color"), ColorTarget);
    resources.Set(TEXT("Scene.Depth"), DepthTarget);
    resources.SetOutput(TEXT("Color"), ColorTarget);
    resources.SetOutput(TEXT("Depth"), DepthTarget);
    return StageResult::Completed;
}
