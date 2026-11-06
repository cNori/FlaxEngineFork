// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "Engine/Content/AssetReference.h"

class GPUBuffer;
class RenderBuffers;
class Shader;

/// <summary>Tests projected object bounds against an opaque camera depth prepass.</summary>
API_CLASS() class FLAXENGINE_API OcclusionCullStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(OcclusionCullStage);
private:
    Array<IVisualData*> Occluded;
    int32 LastOccludedCount = 0;
    AssetReference<Shader> ShaderAsset;
    RenderBuffers* DepthBuffers = nullptr;
    GPUBuffer* BoundsBuffer = nullptr;
    GPUBuffer* ResultsBuffer = nullptr;
    GPUBuffer* ReadbackBuffer = nullptr;

public:
    ~OcclusionCullStage();

    /// <summary>Enables depth occlusion with same-frame readback on DirectX 11.</summary>
    API_FIELD() bool Enabled = true;

    /// <summary>Maximum triangle count for a mesh to contribute to the depth prepass.</summary>
    API_FIELD() int32 OccluderTriangleLimit = 4096;

    /// <summary>Gets the number of objects occluded in the last view.</summary>
    API_PROPERTY() int32 GetOccludedCount() const { return LastOccludedCount; }

    /// <summary>Gets the borrowed objects hidden in the current view.</summary>
    const Array<IVisualData*>& GetOccludedObjects() const { return Occluded; }

    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
    void OnEndFrame(GPUContext* context) override;
};
