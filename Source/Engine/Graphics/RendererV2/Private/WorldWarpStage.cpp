// Copyright (c) Wojciech Figat. All rights reserved.

#include "../WorldWarpStage.h"
#include "Engine/Level/Actors/Camera.h"
#include "Engine/Platform/StringUtils.h"

WorldWarpStage::WorldWarpStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

uint64 WarpedVisualData::GetIVD_RenderTypeId()
{
    return Source->GetIVD_RenderTypeId();
}

void LookAt(const Vector3& eye, const Vector3& direction, const Vector3& up, Matrix& result)
{
    Float3 xaxis, yaxis, zaxis = direction;
    zaxis.Normalize();
    Float3::Cross(up, zaxis, xaxis);
    xaxis.Normalize();
    Float3::Cross(zaxis, xaxis, yaxis);

    result.M11 = xaxis.X;
    result.M21 = xaxis.Y;
    result.M31 = xaxis.Z;

    result.M12 = yaxis.X;
    result.M22 = yaxis.Y;
    result.M32 = yaxis.Z;

    result.M13 = zaxis.X;
    result.M23 = zaxis.Y;
    result.M33 = zaxis.Z;

    result.M14 = 0.0f;
    result.M24 = 0.0f;
    result.M34 = 0.0f;

    result.M41 = -Float3::Dot(xaxis, eye);
    result.M42 = -Float3::Dot(yaxis, eye);
    result.M43 = -Float3::Dot(zaxis, eye);
    result.M44 = 1.0f;
}

StageResult WorldWarpStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    const auto* pose = static_cast<const Transform*>(viewpoint->GetIC_WorldTransform());
    if (!pose)
        return StageResult::Failed;

    WarpedView.Source = viewpoint;
    WarpedView.LocalTransform = *pose;
    WarpedView.LocalTransform.Translation -= Origin;
    WarpedView.Projection = viewpoint->GetIC_Projection();
    //prep view
    const auto forward = Vector3::Transform(Vector3::Forward, WarpedView.LocalTransform.Orientation);
    const auto up = Vector3::Transform(Vector3::Up, WarpedView.LocalTransform.Orientation);
    LookAt(WarpedView.LocalTransform.Translation, forward, up, WarpedView.View);
    Matrix::Multiply(WarpedView.View, WarpedView.Projection, WarpedView.ViewProjection);

    viewpoint = &WarpedView;

    WarpedObjects.EnsureCapacity(objects.Count());
    for (size_t i = 0; i < objects.Count(); i++)
    {
        WarpedVisualData warped;
        warped.Source = objects[i];
        warped.LocalTransform = *(Transform*)warped.Source->GetIVD_WorldTransform();
        warped.LocalTransform.Translation -= Origin;
        const auto& transform = warped.LocalTransform;
        Matrix::Transformation(transform.Scale,transform.Orientation,Float3(transform.Translation),warped.WorldMatrix);
        const auto& bounds = *(BoundingBox*)warped.Source->GetIVD_Bounds();
        warped.Bounds = BoundingBox(bounds.Minimum - Origin, bounds.Maximum - Origin);
        const auto& sphere = *(BoundingSphere*)warped.Source->GetIVD_BoundsSphere();
        warped.Sphere = BoundingSphere(sphere.Center - Origin, sphere.Radius);
        WarpedObjects.Add(warped);
        objects[i] = &WarpedObjects.Last();
    }
    return StageResult::Completed;
}

void WorldWarpStage::OnEndFrame(GPUContext* context)
{
    WarpedObjects.Clear();
    WarpedView.Source = nullptr;
}
