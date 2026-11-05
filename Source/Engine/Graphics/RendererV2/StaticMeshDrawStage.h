// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"
class Shader;
class GPUShader;
class GPUPipelineState;
class GPUBuffer;

/// <summary>
/// Initial static-mesh diagnostic draw. Uses resident position/index buffers directly,
/// with depth testing and a debug shader; material shading, lighting and culling are not implemented.
/// </summary>
API_CLASS() class FLAXENGINE_API RenderPipelineDrawStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineDrawStage);
private:
    AssetReference<Shader> ShaderAsset;
    GPUPipelineState* State = nullptr;
    GPUBuffer* ColorBuffer = nullptr;
    bool Ready = false;
    void OnShaderReloading(Asset* asset);
public:
    ~RenderPipelineDrawStage();
    void OnBeginFrame(GPUContext* context) override;
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources) override;
};
