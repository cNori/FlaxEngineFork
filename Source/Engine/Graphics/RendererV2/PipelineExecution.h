// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Collections/Array.h"

class RenderPipeline;
class ICamera;

/// <summary>
/// One named compositor execution. Pipeline state is exclusive to this execution;
/// resources referenced by that pipeline can be shared with other executions.
/// Camera pointers are borrowed and must stay valid through execution.
/// </summary>
struct PipelineExecution
{
    String Name;
    RenderPipeline* Pipeline = nullptr;
    Array<ICamera*> Viewpoints;
};
