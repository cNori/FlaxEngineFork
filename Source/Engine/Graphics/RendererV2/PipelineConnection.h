// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

class RenderPipelineStage;

/// <summary>A stage dependency with optional object or named resource transfer.</summary>
struct PipelineConnection
{
    RenderPipelineStage* Source = nullptr;
    RenderPipelineStage* Target = nullptr;
    String Output;
    String Input;
    bool Required = true;
    bool ForwardObjects = false;
};
