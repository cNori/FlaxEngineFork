// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RenderPipeline.h"
#include "Engine/Core/Memory/Memory.h"
#include <type_traits>

class GPUContext;
class IVisualData;
class ICamera;

/// <summary>
/// Coordinates rendering pipelines.
/// Controls which pipelines render each view and where the resulting images
/// are written or composited.
/// </summary>
API_CLASS() class FLAXENGINE_API RenderCompositor : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE(RenderCompositor);
private:
    Array<RenderPipeline*> PipeInstances;
    RenderPipeline* ActivePipe = nullptr;
    bool IsExecuting = false;
public:

    template<typename T>
    T* CreateRenderPipeline(const String& InNameTag)
    {
        static_assert(std::is_base_of<RenderPipeline, T>::value, "T must derive from RenderPipeline.");
        if (IsExecuting || GetRenderPipeline(String(InNameTag)))
            return nullptr;
        auto pipeline = New<T>();
        pipeline->NameTag = InNameTag;
        PipeInstances.Add(pipeline);
        return pipeline;
    }

    template<typename T>
    T* CreateRenderPipeline(const char* InNameTag)
    {
        return CreateRenderPipeline<T>(String(InNameTag));
    }

    /// <summary>
    /// Creates a per-view runtime pipeline and ordered stage instances from a pipeline asset.
    /// Returns null on failure. Call outside execution; loading can block.
    /// The returned instance is registered but is not selected automatically.
    /// </summary>
    API_FUNCTION() RenderPipeline* CreateRenderPipeline(JsonAsset* asset);

    /// <summary>
    /// Creates another instance of an asset with a unique runtime name, for another execution/view.
    /// </summary>
    /// <param name="asset"></param>
    /// <param name="instanceName"></param>
    /// <returns></returns>
    API_FUNCTION() RenderPipeline* CreateRenderPipelineInstance(JsonAsset* asset, const String& instanceName);

    /// <summary>
    /// Selects a registered pipeline for the owning view.
    /// </summary>
    API_FUNCTION() bool SetActivePipeline(RenderPipeline* pipeline);

    /// <summary>
    /// Returns the active pipeline for the owning view.
    /// </summary>
    API_PROPERTY() RenderPipeline* GetActivePipeline() const;

    void SetViewOutput(const RenderOutputTarget& target);
    bool BeginCollection();

    /// <summary>
    /// Gets the render pipeline.
    /// </summary>
    /// <param name="InName">Name of the Render Pipeline.</param>
    /// <returns></returns>
    RenderPipeline* GetRenderPipeline(const String& InName);

    /// <summary>
    /// Executes the active pipeline for the owning view.
    /// Output targets and presentation remain controlled by the stages and caller.
    /// </summary>
    void Execute(GPUContext* RefContext);

    /// <summary>
    /// Executes the supplied pipeline for off-screen rendering.
    /// This call runs synchronously.
    /// </summary>
    /// <param name="pipeline">The pipeline to execute.</param>
    void ExecuteOnRenderPipeline(GPUContext* RefContext, RenderPipeline* RefPipeline);
    
    /// <summary>
    /// Appends borrowed object pointers.
    /// </summary>
    /// <param name="RefPipeline">The reference pipeline.</param>
    /// <param name="objects">The objects.</param>
    void Submit(RenderPipeline* RefPipeline, const Array<IVisualData*>& objects);
    
    /// <summary>
    /// Appends borrowed camera pointers.
    /// </summary>
    /// <param name="RefPipeline">The reference pipeline.</param>
    /// <param name="Viewpoints">The viewpoints.</param>
    void Submit(RenderPipeline* RefPipeline, const Array<ICamera*>& Viewpoints);

    /// <summary>
    /// Appends borrowed object pointers.
    /// </summary>
    /// <param name="RefPipeline">The reference pipeline.</param>
    /// <param name="objects">The objects.</param>
    void Submit(RenderPipeline* RefPipeline, IVisualData* object);

    /// <summary>
    /// Appends borrowed camera pointers.
    /// </summary>
    /// <param name="RefPipeline">The reference pipeline.</param>
    /// <param name="Viewpoints">The viewpoints.</param>
    void Submit(RenderPipeline* RefPipeline, ICamera* Viewpoint);
};
