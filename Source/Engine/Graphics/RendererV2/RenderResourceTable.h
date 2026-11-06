// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Viewport.h"
#include "Engine/Graphics/GPUResource.h"

class ILightData;
class RenderPipelineGraphicSettings;

/// <summary>
/// Named, borrowed GPU resources shared within one pipeline execution.
/// Does not allocate, copy or destroy GPU resources, insert barriers or wait for GPU work.
/// Owners must keep resources alive until all GPU consumers complete.
/// Single-threaded; keys and pointers are copied, resource contents are not.
/// </summary>
class FLAXENGINE_API RenderResourceTable
{
private:
    Dictionary<String, GPUResource*> Resources;
    Dictionary<String, GPUResource*> Pending;
    Dictionary<String, GPUResource*> StageInputs;
    Dictionary<String, GPUResource*> Outputs;
    Dictionary<String, GPUResource*> PendingOutputs;
    Viewport ViewRect;
    Span<ILightData*> Lights;
    const RenderPipelineGraphicSettings* GraphicsSettings = nullptr;
    bool InStage = false;
    friend class RenderPipeline;

    void BeginStage();
    void EndStage(bool commit);
    void Clear();
public:
    const Viewport& GetViewRect() const { return ViewRect; }

    /// <summary>Returns the borrowed local lights collected for this view.</summary>
    Span<ILightData*> GetLights() const { return Lights; }

    /// <summary>Returns the borrowed, read-only settings for the current view.</summary>
    const RenderPipelineGraphicSettings* GetGraphicsSettings() const { return GraphicsSettings; }

    /// <summary>Publishes or replaces a borrowed resource. Null removes the entry.</summary>
    void Set(const String& name, GPUResource* resource);

    /// <summary>
    /// Publishes a dedicated pipeline output for composition or post-processing.
    /// Separate from intermediate resources; null withdraws an output.
    /// Stage changes publish only on Completed. No GPU copy or ownership transfer occurs.
    /// </summary>
    void SetOutput(const String& name, GPUResource* resource);

    /// <summary>Returns a dedicated output, or null if not published in this execution.</summary>
    GPUResource* GetOutput(const String& name) const;

    template<typename T>
    T* GetOutput(const String& name) const
    {
        return dynamic_cast<T*>(GetOutput(name));
    }

    /// <summary>Removes a named reference without releasing the GPU resource.</summary>
    void Remove(const String& name);

    /// <summary>Returns a named resource, or null if unavailable.</summary>
    GPUResource* Get(const String& name) const;

    /// <summary>Returns null if the resource is missing or has a different native type.</summary>
    template<typename T>
    T* Get(const String& name) const
    {
        return dynamic_cast<T*>(Get(name));
    }
};
