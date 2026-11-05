// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Scripting/ScriptingType.h"


/// <summary>
/// Provides render-able data to pipeline stages, including GPU buffers,
/// meshes, procedural geometry, or other stage-defined data.
/// Does not transfer ownership of the supplied data or resources.
/// </summary>
API_INTERFACE() class FLAXENGINE_API IVisualData
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(IVisualData);
public:
    virtual ~IVisualData();
    /// <summary>
    /// Gets the tag used by pipeline stages to identify and filter visual data.
    /// </summary>
    /// <returns>A tag describing the visual data contract.</returns>
    /// <remarks>
    /// The tag identifies the data format, not an individual object.
    /// Stages must understand that format before interpreting opaque pointers.
    /// Distinct prefixes can allow comparisons to reject mismatches earlier.
    /// </remarks>
    API_FUNCTION() virtual uint64 GetRenderTypeId() = 0;

    /// <summary>
    /// Gets the object's bounds using the format defined by its data contract.
    /// </summary>
    /// <returns>
    /// A borrowed opaque pointer to the object's bounds.
    /// <para>
    /// WARNING: The owning object and its bounds storage must remain valid
    /// until all pipeline stages have finished using them.
    /// </para>
    /// <para>The render pipeline uses zero-copy access by default.</para>
    /// </returns>
    API_FUNCTION() virtual void* GetBounds() = 0;

    /// <summary>
    /// Gets the object's axis-aligned bounding box.
    /// </summary>
    /// <returns>
    /// A borrowed opaque pointer to the object's AABB, using the format
    /// and coordinate space defined by its data contract.
    /// <para>
    /// WARNING: The owning object and its bounds storage must remain valid
    /// until all pipeline stages have finished using them.
    /// </para>
    /// <para>The render pipeline uses zero-copy access by default.</para>
    /// </returns>
    API_FUNCTION() virtual void* GetBoundsSphere() = 0;
};
