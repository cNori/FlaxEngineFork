// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Content/JsonAssetReference.h"
#include "RenderPipelineGraphicSettings.h"
#include "RenderResourceTable.h"
#include "StageResult.h"
#include "PipelineConnection.h"
#include "RenderOutputTarget.h"

class GPUContext;
class ICamera;
class RenderPipelineStage;

/// <summary>
/// A runtime pipeline instance with independent state per view.
/// </summary>
API_CLASS() class FLAXENGINE_API RenderPipeline : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE(RenderPipeline);
private:
    String NameTag;
public:
    /// <summary>
    /// The runtime routing name, derived from the pipeline asset filename without its extension.
    /// </summary>
    API_PROPERTY() String GetNameTag() const;

    /// <summary>
    /// Shared graphics configuration. Runtime stages treat this asset as read-only.
    /// </summary>
    API_FIELD() JsonAssetReference<RenderPipelineGraphicSettings> GraphicSettings;

protected:
    friend class RenderCompositor;
    /// <summary>Registers a stage owned and destroyed by this runtime pipeline.</summary>
    bool AddOwnedStage(RenderPipelineStage* stage);
    virtual void PrepareView(ICamera* viewpoint) {}
private:
    Array<RenderPipelineStage*> Stages;
    Array<ICamera*> Viewpoints;
    void ExecuteView(GPUContext* context, ICamera* viewpoint);
    Array<RenderPipelineStage*> OwnedStages;
    Array<PipelineConnection> Connections;
    Array<int32> ExecutionOrder;
    bool GraphDirty = true;
    bool BuildExecutionOrder();
    bool AddConnection(const PipelineConnection& connection);
    bool IsExecuting = false;
    RenderOutputTarget OutputTarget;
    RenderResourceTable Resources;
    StageResult LastResult = StageResult::Skipped;

public:

    ~RenderPipeline();

    /// <summary>Configures this execution's borrowed destination; does not bind it.</summary>
    void SetOutputTarget(const RenderOutputTarget& target);
    const RenderOutputTarget& GetOutputTarget() const;

    /// <summary>The result of the last pipeline execution.</summary>
    API_PROPERTY() StageResult GetLastResult() const;

    /// <summary>
    /// Gets a named output published by this pipeline, for example Color or Depth.
    /// Borrowed reference, valid only while its owner retains the resource. Null if unavailable.
    /// Output references are reset on the next execution; GPU completion is not implied.
    /// </summary>
    API_FUNCTION() GPUResource* GetOutput(const String& name) const;

    /// <summary>Execution-local resource references; populated again for each execution.</summary>
    RenderResourceTable& GetResources();

    /// <summary>
    /// Registers a borrowed stage. Stage names must be unique.
    /// </summary>
    bool AddStage(RenderPipelineStage* stage);

    /// <summary>
    /// Starts a new collection after producer updates and before submissions.
    /// Clears pointer lists only; does not delete objects, cameras or GPU resources.
    /// Collection is single-threaded and cannot change during execution.
    /// </summary>
    bool BeginCollection();

    /// <summary>
    /// Orders two stages and optionally forwards the source's filtered objects.
    /// </summary>
    bool ConnectStages(RenderPipelineStage* source, RenderPipelineStage* target, bool forwardObjects = false);

    /// <summary>
    /// Routes a resource published with Set to a named input on another stage.
    /// </summary>
    bool LinkResource(RenderPipelineStage* source, const String& output, RenderPipelineStage* target, const String& input, bool required = true);

    const Array<RenderPipelineStage*>& GetStages() const { return Stages; }
    const Array<PipelineConnection>& GetConnections() const { return Connections; }

    void Execute(GPUContext* context);

    /// <summary>Prepares each camera and renders its view through the stage graph.</summary>
    virtual void Render(GPUContext* context, const Array<ICamera*>& views);

private:

    /// <summary>
    /// Begins a frame and invokes the pipeline stages' frame-begin callbacks.
    /// Call before submitting data and executing the pipeline.
    /// </summary>
    virtual void BeginFrame(GPUContext* RefContext);

    /// <summary>
    /// Ends CPU-side frame processing and invokes the stages' frame-end callbacks.
    /// This does not imply that the GPU has finished using submitted resources.
    /// </summary>
    virtual void EndFrame(GPUContext* RefContext);
};
