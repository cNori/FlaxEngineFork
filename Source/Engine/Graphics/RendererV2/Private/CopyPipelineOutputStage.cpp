// Copyright (c) Wojciech Figat. All rights reserved.

#include "../CopyPipelineOutputStage.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/Textures/GPUTexture.h"

CopyPipelineOutputStage::CopyPipelineOutputStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

void CopyPipelineOutputStage::SetTarget(const RenderOutputTarget& target)
{
    Target = target;
}

StageResult CopyPipelineOutputStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources)
{
    auto color = resources.Get<GPUTexture>(TEXT("Scene.Color"));
    if (!Target.Color || !color || Target.View.Width <= 0 || Target.View.Height <= 0)
        return StageResult::Skipped;
    context->ResetRenderTarget();
    context->ResetSR();
    context->ResetUA();
    context->SetViewportAndScissors(Target.View);
    context->SetRenderTarget(Target.Color);
    context->Draw(color);
    context->ResetSR();
    context->ResetRenderTarget();
    return StageResult::Completed;
}
