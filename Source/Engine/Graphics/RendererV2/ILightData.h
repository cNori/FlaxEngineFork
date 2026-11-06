// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Provides a local light to the render pipeline.</summary>
API_INTERFACE() class FLAXENGINE_API ILightData
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ILightData);
public:
    virtual ~ILightData();

    /// <summary>Returns the light position in world space.</summary>
    API_FUNCTION() virtual Vector3 GetILD_WorldPosition() const = 0;

    /// <summary>Returns the light influence radius in world units.</summary>
    API_FUNCTION() virtual float GetILD_Range() const = 0;
};
