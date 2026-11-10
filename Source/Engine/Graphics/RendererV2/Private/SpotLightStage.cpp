// Copyright (c) Wojciech Figat. All rights reserved.

#include "../SpotLightStage.h"
#include "../PunctualLightRenderer.h"

SpotLightStage::SpotLightStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

SpotLightStage::~SpotLightStage()
{
    if (Renderer)
        Delete(Renderer);
}

int32 SpotLightStage::GetSpotLightCount() const
{
    return Renderer ? Renderer->GetLightCount() : 0;
}

int32 SpotLightStage::GetShadowCasterCount() const
{
    return Renderer ? Renderer->GetShadowCasterCount() : 0;
}

StageResult SpotLightStage::Render(GPUContext* context, Array<IVisualData*>& objects,
    ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (!Renderer)
        Renderer = New<PunctualLightRenderer>();
    Renderer->SetShadowStage(ShadowStage);
    return Renderer->Render(RenderLightKind::Spot, context, objects, viewpoint, resources);
}

StageResult SpotLightStage::PrepareShadows(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    if (!Renderer) Renderer = New<PunctualLightRenderer>();
    Renderer->SetShadowStage(ShadowStage);
    return Renderer->Render(RenderLightKind::Spot, context, objects, viewpoint, resources, true);
}
