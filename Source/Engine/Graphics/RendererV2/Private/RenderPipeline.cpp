// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderPipeline.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "../RenderPipelineStage.h"
#include "../ICamera.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Profiler/Profiler.h"

namespace
{
    // Dynamic names require the scoped profiler API rather than TEXT(literal) macros.
    struct V2ProfileScope
    {
#if COMPILE_WITH_PROFILER
        ScopeProfileBlockCPU CPU;
        ScopeProfileBlockGPU GPU;
        explicit V2ProfileScope(const String& name)
            : CPU(name.HasChars() ? name.Get() : TEXT("Unnamed V2 scope"))
            , GPU(name.HasChars() ? name.Get() : TEXT("Unnamed V2 scope"))
        {
        }
#else
        explicit V2ProfileScope(const String&) {}
#endif
    };
}



RenderPipeline::~RenderPipeline()
{
    // Only asset-created stage instances are owned; AddStage remains non-owning.
    for (auto stage : OwnedStages)
        Delete(stage);
}

RenderPipeline::RenderPipeline(const SpawnParams& params)
    : ScriptingObject(params)
{
}

String RenderPipeline::GetNameTag() const
{ return NameTag; }

bool RenderPipeline::AddStage(RenderPipelineStage* stage)
{
    if (!stage || IsExecuting)
        return false;
    for (auto existing : Stages)
        if (existing == stage || existing->NameTag == stage->NameTag)
            return false;
    Stages.Add(stage);
    GraphDirty = true;
    return true;
}

RenderPipelineStage* RenderPipeline::GetStage(const MClass* type) const
{
    if (!type)
        return nullptr;
    for (auto* stage : Stages)
    {
        const auto* stageClass = stage->GetClass();
        if (stageClass && (stageClass == type || stageClass->IsSubClassOf(type)))
            return stage;
    }
    return nullptr;
}

bool RenderPipeline::BeginCollection()
{
    if (IsExecuting)
        return false;
    Viewpoints.Clear();
    for (auto stage : Stages)
        stage->Objects.Clear();
    return true;
}

void RenderPipeline::BeginFrame(GPUContext* RefContext)
{

}

void RenderPipeline::EndFrame(GPUContext* RefContext)
{

}

StageResult RenderPipeline::GetLastResult() const
{
    return LastResult;
}

RenderResourceTable& RenderPipeline::GetResources()
{
    return Resources;
}

GPUResource* RenderPipeline::GetOutput(const String& name) const
{
    return Resources.GetOutput(name);
}

bool RenderPipeline::AddOwnedStage(RenderPipelineStage* stage)
{
    if (!stage)
        return false;
    if (stage->NameTag.IsEmpty())
        stage->NameTag = String(stage->GetType().Fullname);
    if (!AddStage(stage))
        return false;
    OwnedStages.Add(stage);
    return true;
}

void RenderPipeline::SetOutputTarget(const RenderOutputTarget& target)
{
    if (!IsExecuting)
        OutputTarget = target;
}

const RenderOutputTarget& RenderPipeline::GetOutputTarget() const
{
    return OutputTarget;
}

void RenderPipeline::Execute(GPUContext* context)
{
    Render(context, Viewpoints);
}

void RenderPipeline::Render(GPUContext* RefContext, const Array<ICamera*>& views)
{
    if (!RefContext || IsExecuting)
        return;

    struct ExecutionScope
    {
        bool& Executing;
        explicit ExecutionScope(bool& executing) : Executing(executing) { Executing = true; }
        ~ExecutionScope() { Executing = false; }
    } execution(IsExecuting);

    Resources.Clear();
    if (!BuildExecutionOrder())
    {
        LastResult = StageResult::Failed;
        return;
    }

    LastResult = StageResult::Skipped;
    V2ProfileScope pipelineProfile(NameTag);
    if (views.IsEmpty())
        return;

    // Restore collected inputs after each view, including temporary stage substitutions.
    Array<Array<IVisualData*>> collected;
    collected.Resize(Stages.Count());
    for (int32 i = 0; i < Stages.Count(); i++)
        collected[i] = Stages[i]->Objects;
    for (auto viewpoint : views)
    {
        if (!viewpoint)
            continue;
        ExecuteView(RefContext, viewpoint);
        for (int32 i = 0; i < Stages.Count(); i++)
            Stages[i]->Objects = collected[i];
        if (LastResult == StageResult::Failed)
            break;
    }
}

void RenderPipeline::ExecuteView(GPUContext* RefContext, ICamera* viewpoint)
{
    if (!viewpoint)
        return;

    Resources.Clear();
    Resources.ViewRect = OutputTarget.View;

    Array<GPUResource*, InlinedAllocation<16>> transfers;
    transfers.Resize(Connections.Count());
    for (auto& resource : transfers)
        resource = nullptr;
    Array<StageResult, InlinedAllocation<16>> results;
    results.Resize(Stages.Count());
    for (auto& result : results)
        result = StageResult::Skipped;

    LastResult = StageResult::Skipped;
    {
        PROFILE_GPU_CPU_NAMED("Pipeline BeginFrame");
        BeginFrame(RefContext);
    }
    for (int32 i : ExecutionOrder)
    {
        PROFILE_GPU_CPU_NAMED("Stage BeginFrame");
        auto stage = Stages[i];
        V2ProfileScope stageProfile(stage->NameTag);
        stage->OnBeginFrame(RefContext);
    }
    for (int32 i : ExecutionOrder)
    {
        auto stage = Stages[i];
        V2ProfileScope stageProfile(stage->NameTag);
        bool ready = true;
        bool routedObjects = false;
        for (int32 edge = 0; edge < Connections.Count(); edge++)
        {
            const auto& link = Connections[edge];
            if (link.Target != stage)
                continue;
            if (link.Required && link.Output.HasChars() && !transfers[edge])
                ready = false;
            if (link.ForwardObjects)
            {
                if (!routedObjects)
                    stage->Objects.Clear();
                routedObjects = true;
                if (results[Stages.Find(link.Source)] == StageResult::Completed)
                    stage->Objects.Add(link.Source->Objects);
            }
        }
        if (!ready)
            continue;

        Resources.BeginStage();
        for (int32 edge = 0; edge < Connections.Count(); edge++)
        {
            const auto& link = Connections[edge];
            if (link.Target == stage && link.Input.HasChars())
                Resources.StageInputs[link.Input] = transfers[edge];
        }
        auto* stageViewpoint = viewpoint;
        const auto result = stage->Render(RefContext, stage->Objects, stageViewpoint, Resources);
        if (result == StageResult::Completed)
            viewpoint = stageViewpoint;
        results[i] = result;
        if (result == StageResult::Completed)
        {
            for (int32 edge = 0; edge < Connections.Count(); edge++)
            {
                const auto& link = Connections[edge];
                GPUResource* resource = nullptr;
                if (link.Source == stage && link.Output.HasChars() && Resources.Pending.TryGet(link.Output, resource))
                    transfers[edge] = resource;
            }
        }
        Resources.EndStage(result == StageResult::Completed);
        if (result == StageResult::Failed)
        {
            LastResult = StageResult::Failed;
            LOG(Error, "Render stage {0} failed in pipeline {1}.", stage->NameTag, NameTag);
            break;
        }
        if (result == StageResult::Completed)
            LastResult = StageResult::Completed;
    }
    for (int32 i : ExecutionOrder)
    {
        PROFILE_GPU_CPU_NAMED("Stage EndFrame");
        auto stage = Stages[i];
        V2ProfileScope stageProfile(stage->NameTag);
        stage->OnEndFrame(RefContext);
    }
    {
        PROFILE_GPU_CPU_NAMED("Pipeline EndFrame");
        EndFrame(RefContext);
    }
}


bool RenderPipeline::ConnectStages(RenderPipelineStage* source, RenderPipelineStage* target, bool forwardObjects)
{
    PipelineConnection connection;
    connection.Source = source;
    connection.Target = target;
    connection.ForwardObjects = forwardObjects;
    return AddConnection(connection);
}

bool RenderPipeline::LinkResource(RenderPipelineStage* source, const String& output, RenderPipelineStage* target, const String& input, bool required)
{
    if (output.IsEmpty() || input.IsEmpty())
        return false;
    PipelineConnection connection;
    connection.Source = source;
    connection.Target = target;
    connection.Output = output;
    connection.Input = input;
    connection.Required = required;
    return AddConnection(connection);
}

bool RenderPipeline::AddConnection(const PipelineConnection& connection)
{
    if (IsExecuting || connection.Source == connection.Target ||
        !Stages.Contains(connection.Source) || !Stages.Contains(connection.Target))
        return false;
    for (const auto& existing : Connections)
    {
        if (existing.Target == connection.Target && existing.Input == connection.Input &&
            (connection.Input.HasChars() || existing.Source == connection.Source))
            return false;
    }
    Connections.Add(connection);
    GraphDirty = true;
    if (BuildExecutionOrder())
        return true;
    Connections.RemoveAt(Connections.Count() - 1);
    GraphDirty = true;
    return false;
}

bool RenderPipeline::BuildExecutionOrder()
{
    if (!GraphDirty)
        return true;
    ExecutionOrder.Clear();
    Array<int32, InlinedAllocation<16>> incoming;
    incoming.Resize(Stages.Count());
    for (auto& count : incoming)
        count = 0;
    for (const auto& connection : Connections)
        incoming[Stages.Find(connection.Target)]++;
    while (ExecutionOrder.Count() < Stages.Count())
    {
        const int32 next = incoming.Find(0);
        if (next == -1)
            return false;
        ExecutionOrder.Add(next);
        incoming[next] = -1;
        for (const auto& connection : Connections)
            if (connection.Source == Stages[next])
                incoming[Stages.Find(connection.Target)]--;
    }
    GraphDirty = false;
    return true;
}
