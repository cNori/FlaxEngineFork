// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

/// <summary>Filters camera-relative bounds against the current view frustum.</summary>
API_CLASS() class FLAXENGINE_API FrustumCullStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(FrustumCullStage);
private:
    Array<IVisualData*> Rejected;
public:
    /// <summary>Borrowed objects outside the camera frustum, available during stage execution.</summary>
    const Array<IVisualData*>& GetRejectedObjects() const { return Rejected; }

    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
    void OnEndFrame(GPUContext* context) override;
};
