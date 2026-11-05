// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "RenderOutputTarget.h"

/// <summary>Copies Scene.Color into an explicit view output without presenting a swap chain.</summary>
API_CLASS() class FLAXENGINE_API CopyPipelineOutputStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(CopyPipelineOutputStage);
private:
    RenderOutputTarget Target;
public:
    void SetTarget(const RenderOutputTarget& target);
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources) override;
};
