// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderCompositor.h"
#include "../RenderPipelineAsset.h"
#include "../RenderPipelineStage.h"
#include "../IVisualData.h"
#include "../ICamera.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Core/Log.h"
#include "Engine/Profiler/Profiler.h"

RenderPipeline* RenderCompositor::CreateRenderPipeline(JsonAsset* asset)
{
    if (!asset)
        return nullptr;
    return CreateRenderPipelineInstance(asset, StringUtils::GetFileNameWithoutExtension(asset->GetPath()));
}

RenderPipeline* RenderCompositor::CreateRenderPipelineInstance(JsonAsset* asset, const String& instanceName)
{
    if (!asset || IsExecuting || asset->WaitForLoaded())
        return nullptr;
    const String nameTag = instanceName;
    if (nameTag.IsEmpty() || GetRenderPipeline(nameTag))
        return nullptr;
    auto definition = asset->GetInstance<RenderPipelineAsset>();
    if (!definition)
        return nullptr;
    String error;
    if (!definition->Validate(error))
    {
        LOG(Error, "Cannot create render pipeline from {0}: {1}", asset->GetPath(), error);
        return nullptr;
    }
    auto instance = ScriptingObject::NewObject<RenderPipeline>(Scripting::FindScriptingType(definition->PipelineType));
    if (!instance)
        return nullptr;
    instance->NameTag = nameTag;
    instance->GraphicSettings = definition->GraphicSettings;
    if (instance->GraphicSettings && (instance->GraphicSettings->WaitForLoaded() || !instance->GraphicSettings.GetInstance()))
    {
        Delete(instance);
        return nullptr;
    }
    for (const auto& reference : definition->Stages)
    {
        auto type = Scripting::FindScriptingType(reference);
        auto stage = ScriptingObject::NewObject<RenderPipelineStage>(type);
        if (!stage)
        {
            Delete(instance);
            return nullptr;
        }
        // Full type names avoid collisions between implementations in different namespaces.
        stage->NameTag = String(type.GetType().Fullname);
        if (!instance->AddStage(stage))
        {
            Delete(stage);
            Delete(instance);
            return nullptr;
        }
        instance->OwnedStages.Add(stage);
    }
    PipeInstances.Add(instance);
    return instance;
}

void RenderCompositor::ExecuteOnRenderPipeline(GPUContext* context, RenderPipeline* pipeline)
{
    if (!context || !pipeline || IsExecuting)
        return;
    IsExecuting = true;
    pipeline->Execute(context);
    IsExecuting = false;
}

void RenderCompositor::Execute(GPUContext* context)
{
    PROFILE_GPU_CPU_NAMED("RenderCompositor");
    ExecuteOnRenderPipeline(context, ActivePipe);
}

RenderPipeline* RenderCompositor::GetRenderPipeline(const String& InName)
{
    for (int32 i = 0; i < PipeInstances.Count(); i++)
    {
        if (PipeInstances[i]->NameTag == InName)
        {
            return PipeInstances[i];
        }
    }
    return nullptr;
}

RenderCompositor::RenderCompositor(const SpawnParams& params) : ScriptingObject(params)
{

}

void RenderCompositor::Submit(RenderPipeline* RefPipeline, const Array<IVisualData*>& objects)
{
    if (!RefPipeline || RefPipeline->IsExecuting || IsExecuting)
        return;
    for (auto stage : RefPipeline->Stages)
    {
        stage->Objects.Add(objects);
    }
}

void RenderCompositor::Submit(RenderPipeline* RefPipeline, const Array<ICamera*>& Viewpoints)
{
    if (!RefPipeline || RefPipeline->IsExecuting || IsExecuting)
        return;
    for (auto viewpoint : Viewpoints)
        Submit(RefPipeline, viewpoint);
}

void RenderCompositor::Submit(RenderPipeline* RefPipeline, IVisualData* object)
{
    if (!RefPipeline || RefPipeline->IsExecuting || IsExecuting)
        return;
    for (auto stage : RefPipeline->Stages)
    {
        stage->Objects.Add(object);
    }
}

void RenderCompositor::Submit(RenderPipeline* RefPipeline, ICamera* Viewpoint)
{
    if (!RefPipeline || RefPipeline->IsExecuting || IsExecuting)
        return;
    if (Viewpoint && !RefPipeline->Viewpoints.Contains(Viewpoint))
        RefPipeline->Viewpoints.Add(Viewpoint);
}


bool RenderCompositor::SetActivePipeline(RenderPipeline* pipeline)
{
    if (IsExecuting || (pipeline && !PipeInstances.Contains(pipeline)))
        return false;
    ActivePipe = pipeline;
    return true;
}

RenderPipeline* RenderCompositor::GetActivePipeline() const
{
    return ActivePipe;
}

void RenderCompositor::SetViewOutput(const RenderOutputTarget& target)
{
    if (ActivePipe && !IsExecuting)
        ActivePipe->SetOutputTarget(target);
}

void RenderCompositor::SubmitLight(RenderPipeline* pipeline, ILightData* light)
{
    if (pipeline && light && !IsExecuting && !pipeline->IsExecuting)
        pipeline->Lights.Add(light);
}

bool RenderCompositor::BeginCollection()
{
    if (IsExecuting || !ActivePipe || !ActivePipe->BeginCollection())
        return false;
    return true;
}
