// Copyright (c) Wojciech Figat. All rights reserved.

#include "../DefaultPipeline.h"
#include "../WorldWarpStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../PrepareRenderTargetsStage.h"
#include "../StaticMeshDrawStage.h"
#include "../VirtualShadowMapStage.h"
#include "../CopyPipelineOutputStage.h"

DefaultPipeline::DefaultPipeline(const SpawnParams& params)
    : RenderPipeline(params)
{
    auto* warpStage = New<WorldWarpStage>();
    auto* frustumCullStage = New<FrustumCullStage>();
    auto* occlusionCullStage = New<OcclusionCullStage>();
    Targets = New<PrepareRenderTargetsStage>();
    auto* draw = New<RenderPipelineDrawStage>();
    auto* receivers = New<VirtualShadowMapStage>();
    auto* shadowDraw = New<VirtualShadowMapDrawStage>();
    CopyOutput = New<CopyPipelineOutputStage>();
    shadowDraw->SetPageStage(receivers);
    AddOwnedStage(warpStage);
    AddOwnedStage(frustumCullStage);
    AddOwnedStage(occlusionCullStage);
    AddOwnedStage(Targets);
    AddOwnedStage(draw);
    AddOwnedStage(receivers, TEXT("World Shadow Grid Receivers"));
    AddOwnedStage(shadowDraw, TEXT("World Shadow Grid HD"));
    AddOwnedStage(CopyOutput);

    ConnectStages(warpStage, frustumCullStage, true);
    ConnectStages(frustumCullStage, occlusionCullStage, true);
    ConnectStages(occlusionCullStage, Targets);
    ConnectStages(Targets, draw);
    ConnectStages(occlusionCullStage, draw, true);
    ConnectStages(draw, receivers);
    // The shadow pass needs off-camera casters. Only the receiver depth is camera-visible.
    ConnectStages(warpStage, receivers, true);
    ConnectStages(receivers, shadowDraw);
    ConnectStages(shadowDraw, CopyOutput);
}

void DefaultPipeline::BeginFrame(GPUContext* context)
{
    const auto& target = GetOutputTarget();
    Targets->SetSize((int32)target.View.Width, (int32)target.View.Height);
    CopyOutput->SetTarget(target);
}
