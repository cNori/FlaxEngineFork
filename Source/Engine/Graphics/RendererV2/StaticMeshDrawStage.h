// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

class RenderBuffers;
class GPUTexture;

/// <summary>Draws opaque static mesh materials and previews the GBuffer base color.</summary>
API_CLASS() class FLAXENGINE_API RenderPipelineDrawStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineDrawStage);
private:
    RenderBuffers* MaterialBuffers = nullptr;
    GPUTexture* EmissiveBuffer = nullptr;
public:
    ~RenderPipelineDrawStage();
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
