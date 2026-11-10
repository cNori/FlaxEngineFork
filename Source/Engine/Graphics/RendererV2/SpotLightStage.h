// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"

class PunctualLightRenderer;
class ShadowBoxCasterStage;

/// <summary>Adds spot lighting and perspective shadow maps to the current scene colour.</summary>
API_CLASS() class FLAXENGINE_API SpotLightStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(SpotLightStage);
private:
    PunctualLightRenderer* Renderer = nullptr;
    ShadowBoxCasterStage* ShadowStage = nullptr;
public:
    ~SpotLightStage();

    /// <summary>Number of spotlights submitted for the last view.</summary>
    API_PROPERTY() int32 GetSpotLightCount() const;

    /// <summary>Number of caster entries submitted across the last spotlight shadow maps.</summary>
    API_PROPERTY() int32 GetShadowCasterCount() const;

    /// <summary>Uses the same AABB selection and indirect shadow draws as directional and point lights.</summary>
    void SetShadowStage(ShadowBoxCasterStage* stage) { ShadowStage = stage; }

    /// <summary>Selects shadow casters before material evaluation, retaining GPU results for Render.</summary>
    StageResult PrepareShadows(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources);
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
