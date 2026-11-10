// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

class RenderPipelineDrawStage;

/// <summary>Builds opaque receiver depth before light selection and material evaluation.</summary>
API_CLASS() class FLAXENGINE_API DepthPrepassStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(DepthPrepassStage);
private:
    RenderPipelineDrawStage* DrawStage = nullptr;
public:
    void SetDrawStage(RenderPipelineDrawStage* stage) { DrawStage = stage; }
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
