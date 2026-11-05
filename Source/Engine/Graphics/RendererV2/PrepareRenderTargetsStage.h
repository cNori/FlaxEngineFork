// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Core/Math/Color.h"
class GPUTexture;

/// <summary>Allocates reusable per-view color/depth targets and publishes their cleared contents.</summary>
API_CLASS() class FLAXENGINE_API PrepareRenderTargetsStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(PrepareRenderTargetsStage);
private:
    GPUTexture* ColorTarget = nullptr;
    GPUTexture* DepthTarget = nullptr;
    int32 Width = 0;
    int32 Height = 0;
    bool Ready = false;
public:
    /// <summary>Diagnostic background color used before scene drawing is implemented.</summary>
    API_FIELD() Color ClearColor = Color(0.025f, 0.055f, 0.09f, 1.0f);

    ~PrepareRenderTargetsStage();
    void SetSize(int32 width, int32 height);
    void OnBeginFrame(GPUContext* context) override;
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources) override;
};
