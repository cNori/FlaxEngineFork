// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipeline.h"

class CopyPipelineOutputStage;
class PrepareRenderTargetsStage;

/// <summary>Renders camera-visible static meshes and resolves their virtual shadow maps.</summary>
API_CLASS() class FLAXENGINE_API DefaultPipeline : public RenderPipeline
{
    DECLARE_SCRIPTING_TYPE(DefaultPipeline);
    CopyPipelineOutputStage* CopyOutput = nullptr;
    PrepareRenderTargetsStage* Targets = nullptr;
    void BeginFrame(GPUContext* context) override;
};
