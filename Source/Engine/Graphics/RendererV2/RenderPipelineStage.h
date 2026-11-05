// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Collections/Array.h"
#include "StageResult.h"
#include "RenderResourceTable.h"

class GPUContext;
class IVisualData;
class ICamera;

/// <summary>
/// Processes submitted visual data for a rendering stage.
/// </summary>
API_CLASS() class FLAXENGINE_API RenderPipelineStage : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineStage);
private:
    String NameTag;
public:
    /// <summary>
    /// The runtime routing name, derived from its implementation type name for asset-created stages.
    /// </summary>
    API_PROPERTY() String GetNameTag() const;
protected:
    RenderPipelineStage(const String& InNameTag);
private:
    Array<IVisualData*> Objects;
    friend class RenderCompositor;
    friend class RenderPipeline;
public:
    virtual ~RenderPipelineStage() = default;

    struct SubmitStatus
    {
        enum Result
        {
            /// <summary>
            /// nothing blown up :)
            /// </summary>
            Ok = 0,
            /// <summary>
            /// it fine return state on data discard
            /// in case data tag is undesired in this stage
            /// </summary>
            DataDiscard = 1,
            /// <summary>
            /// The fatal state time to panic when received we are on fire
            /// </summary>
            Fatal = 2,
        };
        Result Status = Result::Ok;
        String Message = String::Empty;
    };

    virtual void OnBeginFrame(GPUContext* RefContext);

    /// <summary>
    /// Renders the submitted objects using the supplied camera data.
    /// </summary>
    /// <param name="RefContext">The borrowed GPU command context.</param>
    /// <param name="objects">The collected visual objects.</param>
    /// <param name="viewpoint">The camera for this execution.</param>
    /// <param name="resources">Shared resources for this execution. Changes publish only on Completed.</param>
    /// <returns>Completed, Skipped, or Failed. Skipping rolls back table references, not GPU commands.</returns>
    virtual StageResult Render(GPUContext* RefContext, Array<IVisualData*>& objects, ICamera*& viewpoint, RenderResourceTable& resources);

    virtual void OnEndFrame(GPUContext* RefContext);
};
