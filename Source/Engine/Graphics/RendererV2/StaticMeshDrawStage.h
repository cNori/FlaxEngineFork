// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"

class RenderBuffers;
class GPUBuffer;
class GPUTexture;
class RenderList;
class Shader;

/// <summary>Draws opaque static mesh materials and previews the GBuffer base color.</summary>
API_CLASS() class FLAXENGINE_API RenderPipelineDrawStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineDrawStage);
private:

public:
    ~RenderPipelineDrawStage();
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
