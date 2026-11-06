// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"


/// <summary>
/// Serializable graphics configuration shared by pipeline definitions.
/// Derived types define the settings required by their rendering implementation.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Rendering/Pipeline Graphics Settings\")")
class FLAXENGINE_API RenderPipelineGraphicSettings : public ScriptingObject, public ISerializable
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineGraphicSettings);
    API_AUTO_SERIALIZATION();
public:
    /// <summary>Requested directional shadow map resolution, limited by the GPU and atlas layout.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(512, 16384, 512)")
    int32 ShadowMapResolution = 2048;

    /// <summary>View-space distance covered by the high-detail shadow tile. Zero uses one map.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0), ValueCategory(Utils.ValueCategory.Distance)")
    float NearShadowDistance = 2000.0f;

    /// <summary>Uses adaptive light-space sampling for directional shadows.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool UseAdaptiveShadowFilter = true;

    /// <summary>Adaptive filter radius in shadow-map texels.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0.0f, 4.0f, 0.1f)")
    float AdaptiveShadowRadius = 2.5f;

    /// <summary>Filters the shadow mask in screen space when adaptive filtering is disabled.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool UseScreenSpaceShadowFilter = true;

    /// <summary>Directional light angular radius in degrees for the screen-space filter.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0.0f, 45.0f, 0.01f)")
    float ShadowAngularRadius = 0.27f;

    /// <summary>Casters above this triangle count use a box for GPU receiver testing.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0)")
    int32 ShadowProxyTriangleThreshold = 4096;

    /// <summary>Uses GPU selection to submit expensive shadow draws without a CPU readback.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool UseIndirectShadowDraws = true;

    /// <summary>Enables camera depth occlusion on DirectX 11.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Occlusion Culling\")")
    bool EnableOcclusionCulling = true;

    /// <summary>Maximum triangle count for a mesh to contribute to the occluder depth prepass.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Occlusion Culling\"), Limit(0)")
    int32 OccluderTriangleLimit = 4096;

    /// <summary>Uses GPU visibility flags for indirect scene draws.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Occlusion Culling\")")
    bool UseIndirectOcclusionDraws = true;
};
