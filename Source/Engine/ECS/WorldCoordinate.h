// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Math/Int3.h"

API_CLASS() class FLAXENGINE_API WorldCoordinate : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(WorldCoordinate, ScriptingObject);
public:
    API_FIELD() Int3 SectorCoordinate;
    API_FIELD() Transform Transformation;
};
