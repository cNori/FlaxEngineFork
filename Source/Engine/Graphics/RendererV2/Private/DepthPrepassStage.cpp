// Copyright (c) Wojciech Figat. All rights reserved.

#include "../DepthPrepassStage.h"
#include "../StaticMeshDrawStage.h"

DepthPrepassStage::DepthPrepassStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

StageResult DepthPrepassStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    return DrawStage ? DrawStage->RenderDepth(context, objects, viewpoint, resources) : StageResult::Failed;
}
