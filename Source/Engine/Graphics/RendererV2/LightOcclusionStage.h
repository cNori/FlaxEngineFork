// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"

class Shader;
class GPUTexture;

class ShadowBoxCasterStage;
class SpotLightStage;
class PointLightStage;

/// <summary>Selects directional, spot and point shadow casters using current-view receiver depth.</summary>
API_CLASS() class FLAXENGINE_API LightOcclusionStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(LightOcclusionStage);
private:
    AssetReference<Shader> SurfaceShader;
    GPUTexture* SurfaceNormals = nullptr;
    ShadowBoxCasterStage* Shadows = nullptr;
    SpotLightStage* Spots = nullptr;
    PointLightStage* Points = nullptr;
public:
    ~LightOcclusionStage();
    void SetLightStages(ShadowBoxCasterStage* shadows, SpotLightStage* spots, PointLightStage* points) { Shadows = shadows; Spots = spots; Points = points; }
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
