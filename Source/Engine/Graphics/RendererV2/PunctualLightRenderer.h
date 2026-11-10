// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "ILightData.h"
#include "Engine/Content/AssetReference.h"

class Shader;
class GPUTexture;
class GPUPipelineState;
class GPUBuffer;
class ShadowBoxCasterStage;

/// <summary>Native implementation shared by the independently scriptable point and spot stages.</summary>
class PunctualLightRenderer
{
private:
    AssetReference<Shader> LightingShader;
    GPUPipelineState* LightingState = nullptr;
    GPUBuffer* FilterBands = nullptr;
    GPUTexture* PointShadowMaps = nullptr;
    GPUBuffer* PointFaceMatrices = nullptr;
    ShadowBoxCasterStage* ShadowStage = nullptr;
    int32 LastLightCount = 0;
    int32 LastCasterCount = 0;
    void OnShaderReloading(Asset* asset);
public:
    ~PunctualLightRenderer();
    int32 GetLightCount() const { return LastLightCount; }
    int32 GetShadowCasterCount() const { return LastCasterCount; }
    void SetShadowStage(ShadowBoxCasterStage* stage) { ShadowStage = stage; }
    StageResult Render(RenderLightKind kind, GPUContext* context, Array<IVisualData*>& objects,
        ICamera*& viewpoint, RenderResourceTable& resources, bool selectionOnly = false);
};
