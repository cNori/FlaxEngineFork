// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

struct VirtualShadowMapData;
class GPUTexture;
class GPUPipelineState;

/// <summary>Selects and sizes shadow pages using camera-visible receivers.</summary>
API_CLASS() class FLAXENGINE_API VirtualShadowMapStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(VirtualShadowMapStage);
    friend class VirtualShadowMapDrawStage;
    VirtualShadowMapData* Data = nullptr;
public:
    ~VirtualShadowMapStage();
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
    void OnEndFrame(GPUContext* context) override;
};

/// <summary>Renders shadow pages and resolves direct lighting.</summary>
API_CLASS() class FLAXENGINE_API VirtualShadowMapDrawStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(VirtualShadowMapDrawStage);
    VirtualShadowMapStage* PageStage = nullptr;

    GPUPipelineState* AmbientState = nullptr;
    GPUPipelineState* LightState = nullptr;
    GPUPipelineState* ShadowPagesState = nullptr;
public:
    void SetPageStage(VirtualShadowMapStage* stage) { PageStage = stage; }
    ~VirtualShadowMapDrawStage();
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
