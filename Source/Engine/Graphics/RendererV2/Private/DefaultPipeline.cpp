// Copyright (c) Wojciech Figat. All rights reserved.

#include "../DefaultPipeline.h"
#include "../WorldWarpStage.h"
#include "../FrustumCullStage.h"
#include "../OcclusionCullStage.h"
#include "../ShadowBoxCasterStage.h"
#include "../SpotLightStage.h"
#include "../PointLightStage.h"
#include "../PrepareRenderTargetsStage.h"
#include "../CopyPipelineOutputStage.h"
#include "../StaticMeshDrawStage.h"
#include "../DepthPrepassStage.h"
#include "../LightOcclusionStage.h"

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
    auto* spotLightStage = New<SpotLightStage>();
    spotLightStage->SetShadowStage(shadowBoxCasterStage);
    AddOwnedStage(spotLightStage);
    auto* pointLightStage = New<PointLightStage>();
    pointLightStage->SetShadowStage(shadowBoxCasterStage);
    AddOwnedStage(pointLightStage);
    auto* depthStage = New<DepthPrepassStage>();
    depthStage->SetDrawStage(DrawStage);
    auto* lightOcclusionStage = New<LightOcclusionStage>();
    lightOcclusionStage->SetLightStages(shadowBoxCasterStage, spotLightStage, pointLightStage);
    AddOwnedStage(depthStage);
    AddOwnedStage(lightOcclusionStage);
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
    // Visibility -> geometry-only receiver depth -> all-light selection -> materials.
    ConnectStages(occlusionCullStage, depthStage, true);
    LinkResource(PrepareStage, TEXT("Scene.Color"), depthStage, TEXT("Scene.Color"));
    LinkResource(occlusionCullStage, TEXT("Occlusion.Visibility"), depthStage, TEXT("Occlusion.Visibility"), false);
    ConnectStages(depthStage, lightOcclusionStage, true);
    LinkResource(depthStage, TEXT("Scene.Depth"), lightOcclusionStage, TEXT("Scene.Depth"));
    ConnectStages(lightOcclusionStage, DrawStage, true);
    LinkResource(occlusionCullStage, TEXT("Occlusion.Visibility"), DrawStage, TEXT("Occlusion.Visibility"), false);
    ConnectStages(DrawStage, CopyStage);
    ConnectStages(shadowBoxCasterStage, spotLightStage);
    ConnectStages(occlusionCullStage, spotLightStage, true);
    LinkResource(DrawStage, TEXT("Scene.Depth"), spotLightStage, TEXT("Spot.ReceiverDepth"));
    ConnectStages(spotLightStage, pointLightStage);
    ConnectStages(occlusionCullStage, pointLightStage, true);
    LinkResource(DrawStage, TEXT("Scene.Depth"), pointLightStage, TEXT("Spot.ReceiverDepth"));
    ConnectStages(pointLightStage, CopyStage);
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
