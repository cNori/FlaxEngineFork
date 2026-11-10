// Copyright (c) Wojciech Figat. All rights reserved.

#include "../PointLightStage.h"
#include "../PunctualLightRenderer.h"

PointLightStage::PointLightStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

PointLightStage::~PointLightStage()
{
    if (Renderer)
        Delete(Renderer);
}

int32 PointLightStage::GetPointLightCount() const
{
    return Renderer ? Renderer->GetLightCount() : 0;
}

int32 PointLightStage::GetShadowCasterCount() const
{
    return Renderer ? Renderer->GetShadowCasterCount() : 0;
}

StageResult PointLightStage::Render(GPUContext* context, Array<IVisualData*>& objects,
    ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (!Renderer)
        Renderer = New<PunctualLightRenderer>();
    Renderer->SetShadowStage(ShadowStage);
    return Renderer->Render(RenderLightKind::Point, context, objects, viewpoint, resources);
}

StageResult PointLightStage::PrepareShadows(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (!Renderer) Renderer = New<PunctualLightRenderer>();
    Renderer->SetShadowStage(ShadowStage);
    return Renderer->Render(RenderLightKind::Point, context, objects, viewpoint, resources, true);
}
