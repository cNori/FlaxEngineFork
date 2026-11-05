// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipeline.h"
class PrepareRenderTargetsStage;
class WorldWarpStage;
class CopyPipelineOutputStage;
class RenderPipelineDrawStage;

/// <summary>Initial V2 pipeline: prepares scene targets and copies color to its configured destination.</summary>
API_CLASS() class FLAXENGINE_API DefaultPipeline : public RenderPipeline
{
    DECLARE_SCRIPTING_TYPE(DefaultPipeline);
private:
    WorldWarpStage* WarpStage = nullptr;
    void PrepareView(ICamera* viewpoint) override;
    PrepareRenderTargetsStage* PrepareStage = nullptr;
    CopyPipelineOutputStage* CopyStage = nullptr;
    RenderPipelineDrawStage* DrawStage = nullptr;
    void BeginFrame(GPUContext* context) override;
    void EndFrame(GPUContext* context) override;
};
