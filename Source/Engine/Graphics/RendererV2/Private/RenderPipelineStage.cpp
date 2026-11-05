// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderPipelineStage.h"
#include "../IVisualData.h"
#include "../ICamera.h"
#include "Engine/Graphics/GPUContext.h"

RenderPipelineStage::RenderPipelineStage(const SpawnParams& params)
    : ScriptingObject(params)
{
}

String RenderPipelineStage::GetNameTag() const
{ return NameTag; }

RenderPipelineStage::RenderPipelineStage(const String& InNameTag)
        : RenderPipelineStage()
{
    NameTag = InNameTag;
}

void RenderPipelineStage::OnBeginFrame(GPUContext* RefContext)
{}

StageResult RenderPipelineStage::Render(GPUContext* RefContext, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    return StageResult::Skipped;
}

void RenderPipelineStage::OnEndFrame(GPUContext* RefContext)
{}
