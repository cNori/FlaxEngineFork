// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

class PunctualLightRenderer;
class ShadowBoxCasterStage;

/// <summary>Draws point lighting using the shared punctual-light renderer and six perspective shadow views.</summary>
API_CLASS() class FLAXENGINE_API PointLightStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(PointLightStage);
private:
    PunctualLightRenderer* Renderer = nullptr;
    ShadowBoxCasterStage* ShadowStage = nullptr;
public:
    ~PointLightStage();

    /// <summary>Number of point lights submitted for the last view.</summary>
    API_PROPERTY() int32 GetPointLightCount() const;

    /// <summary>Number of caster entries submitted across the last point-light shadow faces.</summary>
    API_PROPERTY() int32 GetShadowCasterCount() const;

    /// <summary>Uses the same AABB selection and indirect shadow draws as directional and spot lights.</summary>
    void SetShadowStage(ShadowBoxCasterStage* stage) { ShadowStage = stage; }

    /// <summary>Selects shadow casters before material evaluation, retaining GPU results for Render.</summary>
    StageResult PrepareShadows(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources);
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
