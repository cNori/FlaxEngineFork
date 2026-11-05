// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingType.h"

class RenderCompositor;
class RenderPipeline;

API_STRUCT() struct FLAXENGINE_API RenderCollectionContext
{
    DECLARE_SCRIPTING_TYPE_STRUCTURE(RenderCollectionContext);
    RenderCompositor* Compositor;
    RenderPipeline* Pipeline;
};
