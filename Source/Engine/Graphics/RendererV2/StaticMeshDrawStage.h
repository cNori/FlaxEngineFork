// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"

class RenderBuffers;
class GPUBuffer;
class GPUTexture;
class RenderList;
class Shader;

/// <summary>Draws opaque static mesh materials and previews the GBuffer base color.</summary>
API_CLASS() class FLAXENGINE_API RenderPipelineDrawStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineDrawStage);
private:
    RenderBuffers* MaterialBuffers = nullptr;
    RenderBuffers* ReceiverBuffers = nullptr;
    GPUTexture* EmissiveBuffer = nullptr;
    StageResult RenderGeometry(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources, bool depthOnly);
    AssetReference<Shader> CullingShader;
    GPUBuffer* IndirectInputs = nullptr;
    GPUBuffer* IndirectOutput = nullptr;
    GPUBuffer* IndirectArguments = nullptr;

    void BuildIndirectDraws(GPUContext* context, GPUBuffer* visibility, RenderList* list, const Array<uint32>& objectIndices);
public:
    ~RenderPipelineDrawStage();
    /// <summary>Builds receiver depth without evaluating opaque material colours. Masked/deformed materials retain their depth shader.</summary>
    StageResult RenderDepth(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources);

    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
};
