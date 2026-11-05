// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Scripting/ScriptingType.h"


/// <summary>
/// Describes a viewpoint used to render a scene or other visual data.
/// Can represent a camera, a cubemap face, a probe view,
/// or another custom rendering viewpoint.
/// </summary>
API_INTERFACE() class FLAXENGINE_API ICamera
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ICamera);
public:
    virtual ~ICamera();
    /// <summary>
    /// Gets the tag used by pipeline stages to identify and filter camera data.
    /// </summary>
    /// <returns>A tag describing the camera data contract.</returns>
    /// <remarks>
    /// Use distinct tags for different data contracts.
    /// Distinct prefixes can allow comparisons to reject mismatches earlier,
    /// but do not guarantee negligible filtering cost.
    /// </remarks>
    API_FUNCTION() virtual uint64 GetRenderTypeId() = 0;
};
