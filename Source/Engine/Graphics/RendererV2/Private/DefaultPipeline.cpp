// Copyright (c) Wojciech Figat. All rights reserved.

#include "../DefaultPipeline.h"
#include "../WorldWarpStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../ShadowBoxCasterStage.h"
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
    auto* frustumCullStage = New<FrustumCullStage>();
    AddOwnedStage(WarpStage);
    AddOwnedStage(frustumCullStage);
    auto* occlusionCullStage = New<OcclusionCullStage>();
    AddOwnedStage(occlusionCullStage);
    auto* shadowBoxCasterStage = New<ShadowBoxCasterStage>();
    shadowBoxCasterStage->SetFrustumStage(frustumCullStage);
    shadowBoxCasterStage->SetOcclusionStage(occlusionCullStage);
    AddOwnedStage(shadowBoxCasterStage);
    AddOwnedStage(PrepareStage);
    AddOwnedStage(DrawStage);
    AddOwnedStage(CopyStage);
    LinkResource(PrepareStage, TEXT("Scene.Color"), DrawStage, TEXT("Scene.Color"));
    LinkResource(PrepareStage, TEXT("Scene.Depth"), DrawStage, TEXT("Scene.Depth"));
    LinkResource(PrepareStage, TEXT("Scene.Color"), CopyStage, TEXT("Scene.Color"));
    ConnectStages(WarpStage, frustumCullStage, true);
    ConnectStages(frustumCullStage, occlusionCullStage, true);
    ConnectStages(occlusionCullStage, shadowBoxCasterStage, true);
    LinkResource(DrawStage, TEXT("Scene.Depth"), shadowBoxCasterStage, TEXT("Shadow.ReceiverDepth"));
    LinkResource(DrawStage, TEXT("Scene.Normals"), shadowBoxCasterStage, TEXT("Shadow.ReceiverNormals"), false);
    ConnectStages(occlusionCullStage, DrawStage, true);
    LinkResource(occlusionCullStage, TEXT("Occlusion.Visibility"), DrawStage, TEXT("Occlusion.Visibility"), false);
    ConnectStages(DrawStage, CopyStage);
    ConnectStages(shadowBoxCasterStage, CopyStage);
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
