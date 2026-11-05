// Copyright (c) Wojciech Figat. All rights reserved.

#include "../WorldWarpStage.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Platform/StringUtils.h"

WorldWarpStage::WorldWarpStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

uint64 WarpedStaticMesh::GetRenderTypeId()
{
    static const uint64 typeId = StringUtils::GetHashCode(TEXT("WarpedStaticMesh"));
    return typeId;
}

StageResult WorldWarpStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera* viewpoint, RenderResourceTable& resources)
{
    Inputs = &objects;
    WarpedObjects.Clear();
    WarpedObjects.EnsureCapacity(objects.Count());
    for (auto& object : objects)
    {
        auto* model = dynamic_cast<StaticModel*>(object);
        if (!model)
            continue;

        WarpedStaticMesh warped;
        warped.Source = object;
        warped.LocalTransform = model->GetTransform();
        warped.LocalTransform.Translation -= Origin;
        const auto& bounds = model->GetBoundingBox();
        warped.Bounds = BoundingBox(bounds.Minimum - Origin, bounds.Maximum - Origin);
        const auto& sphere = model->GetBoundingSphere();
        warped.Sphere = BoundingSphere(sphere.Center - Origin, sphere.Radius);
        WarpedObjects.Add(warped);
        object = &WarpedObjects.Last();
    }
    return StageResult::Completed;
}

void WorldWarpStage::OnEndFrame(GPUContext* context)
{
    if (!Inputs)
        return;
    for (auto& object : *Inputs)
        if (auto* warped = dynamic_cast<WarpedStaticMesh*>(object))
            object = warped->Source;
    Inputs = nullptr;
}
