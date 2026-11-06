// Copyright (c) Wojciech Figat. All rights reserved.

#include "../FrustumCullStage.h"
#include "../WorldWarpStage.h"
#include "Engine/Core/Math/BoundingFrustum.h"

FrustumCullStage::FrustumCullStage(const SpawnParams& params)
    : RenderPipelineStage(params)
{
}

StageResult FrustumCullStage::Render(GPUContext* context, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources)
{
    Rejected.Clear();
    const auto* view = dynamic_cast<const WarpedViewpoint*>(viewpoint);
    if (!view)
        return StageResult::Completed;

    const BoundingFrustum frustum(view->ViewProjection);
    int32 visibleCount = 0;
    for (auto* object : objects)
    {
        const auto* warped = dynamic_cast<const WarpedVisualData*>(object);
        if (warped && !frustum.Intersects(warped->Bounds))
            Rejected.Add(object);
        else
            objects[visibleCount++] = object;
    }
    objects.Resize(visibleCount);
    return StageResult::Completed;
}

void FrustumCullStage::OnEndFrame(GPUContext* context)
{
    Rejected.Clear();
}
