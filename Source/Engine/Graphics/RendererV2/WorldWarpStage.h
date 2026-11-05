// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipelineStage.h"
#include "IVisualData.h"
#include "ICamera.h"
#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/BoundingSphere.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Math/Matrix.h"
#include "StageResult.h"
#include "Engine/Core/Collections/Array.h"

/// <summary>
/// Camera-relative static mesh data valid for the current pipeline view.
/// </summary>
struct WarpedVisualData : IVisualData
{
    IVisualData* Source = nullptr;
    Transform LocalTransform;
    Matrix WorldMatrix;
    BoundingBox Bounds;
    BoundingSphere Sphere;

    IVisualData* GetIVD_Source() override { return Source->GetIVD_Source(); }
    uint64 GetIVD_RenderTypeId() override;
    void* GetIVD_WorldTransform() override { return &LocalTransform; }
    void* GetIVD_Bounds() override { return &Bounds; }
    void* GetIVD_BoundsSphere() override { return &Sphere; }
};

/// <summary>A view-projection matrix operating on float positions relative to Origin.</summary>
struct WarpedViewpoint : ICamera
{
    // Borrowed source camera, when this view was prepared from a camera actor.
    ICamera* Source = nullptr;
    ICamera* GetIC_Source() override { return Source ? Source->GetIC_Source() : this; }
    Transform LocalTransform;
    Matrix Projection     = Matrix::Identity;
    Matrix View           = Matrix::Identity;
    Matrix ViewProjection = Matrix::Identity;
    Matrix GetIC_Projection() override { return Projection; }
    void* GetIC_WorldTransform() override { return &LocalTransform; }
    uint64 GetIC_RenderTypeId() override;
};


/// <summary>
/// Prepares camera-relative transforms and bounds before downstream stages.
/// </summary>
API_CLASS() class FLAXENGINE_API WorldWarpStage : public RenderPipelineStage
{
    DECLARE_SCRIPTING_TYPE(WorldWarpStage);
private:
    WarpedViewpoint WarpedView;
    Array<WarpedVisualData> WarpedObjects;
    Vector3 Origin = Vector3::Zero;
public:
    void OnEndFrame(GPUContext* context) override;
    StageResult Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources) override;
public:
    /// <summary>
    /// Overrides the shared render origin in the default pipeline until cleared.
    /// </summary>
    API_FUNCTION() void SetOrigin(const Vector3& origin)
    {
        Origin = origin;
    }
};
