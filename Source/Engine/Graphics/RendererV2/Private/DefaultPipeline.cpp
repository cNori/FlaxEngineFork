// Copyright (c) Wojciech Figat. All rights reserved.

#include "../DefaultPipeline.h"
#include "../WorldWarpStage.h"
#include "../RenderCameraData.h"
#include "../PrepareRenderTargetsStage.h"
#include "../CopyPipelineOutputStage.h"
#include "../StaticMeshDrawStage.h"

DefaultPipeline::DefaultPipeline(const SpawnParams& params)
    : RenderPipeline(params)
{
    WarpStage = New<WorldWarpStage>();
    PrepareStage = New<PrepareRenderTargetsStage>();
    CopyStage = New<CopyPipelineOutputStage>();
    DrawStage = New<RenderPipelineDrawStage>();
    AddOwnedStage(WarpStage);
    AddOwnedStage(PrepareStage);
    AddOwnedStage(DrawStage);
    AddOwnedStage(CopyStage);
    LinkResource(PrepareStage, TEXT("Scene.Color"), DrawStage, TEXT("Scene.Color"));
    LinkResource(PrepareStage, TEXT("Scene.Depth"), DrawStage, TEXT("Scene.Depth"));
    LinkResource(PrepareStage, TEXT("Scene.Color"), CopyStage, TEXT("Scene.Color"));
    ConnectStages(WarpStage, DrawStage, true);
    ConnectStages(DrawStage, CopyStage);
}

void DefaultPipeline::PrepareView(ICamera* viewpoint)
{
    auto* camera = dynamic_cast<RenderCameraData*>(viewpoint);
    WarpStage->SetOrigin(camera ? camera->Origin : Vector3::Zero);
}

void DefaultPipeline::BeginFrame(GPUContext* context)
{
    const auto& target = GetOutputTarget();
    PrepareStage->SetSize((int32)target.View.Width, (int32)target.View.Height);
    CopyStage->SetTarget(target);
}

void DefaultPipeline::EndFrame(GPUContext* context)
{
}
