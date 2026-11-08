// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"


/// <summary>Filtering applied by a directional shadow cascade.</summary>
API_ENUM() enum class ShadowCascadeFilterMode
{
    /// <summary>Uses the pipeline-wide shadow filter settings.</summary>
    Inherit = 0,
    /// <summary>Uses the central bilinear depth comparison.</summary>
    Hard = 1,
    /// <summary>Uses adaptive jittered PCF sampling in shadow-map space.</summary>
    Adaptive = 2,
    /// <summary>Uses a depth-aware screen-space blur.</summary>
    ScreenSpace = 3,
};

/// <summary>
/// Resolution and end distance of one directional shadow cascade.
/// </summary>
API_STRUCT() struct FLAXENGINE_API ShadowCascadeSettings : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(ShadowCascadeSettings);

    /// <summary>Requested width and height of the square shadow map in texels.</summary>
    API_FIELD(Attributes="Limit(512, 16384, 512)")
    int32 TextureSize = 2048;

    /// <summary>Radial distance from the camera in centimeters where the next cascade takes over. Ignored for the final entry, which covers the remaining view.</summary>
    API_FIELD(Attributes="Limit(0), ValueCategory(Utils.ValueCategory.Distance)")
    float EndDistance = 2000.0f;

    /// <summary>
    /// Fraction of this cascade's distance interval used to blend into the next cascade.
    /// Zero gives a hard split, one blends across the whole interval. Ignored for the final entry.
    /// </summary>
    API_FIELD(Attributes="Limit(0.0f, 1.0f, 0.01f)")
    float BlendFraction = 0.1f;

    /// <summary>Extra depth comparison bias in shadow-map texels, added to the automatic precision correction.</summary>
    API_FIELD(Attributes="Limit(0.0f, 10.0f, 0.01f)")
    float DepthBias = 0.0f;

    /// <summary>Receiver normal offset in shadow-map texels, scaled by the angle to the light.</summary>
    API_FIELD(Attributes="Limit(0.0f, 10.0f, 0.01f)")
    float NormalBias = 0.0f;

    /// <summary>Shadow filter used by this band. Inherit uses the pipeline-wide settings.</summary>
    API_FIELD()
    ShadowCascadeFilterMode FilterMode = ShadowCascadeFilterMode::Inherit;

    /// <summary>Filter radius in shadow-map texels for Adaptive, or maximum blur radius in screen pixels for ScreenSpace.</summary>
    API_FIELD(Attributes="Limit(0.0f, 4.0f, 0.1f)")
    float FilterRadius = 2.5f;

    /// <summary>Light angular radius in degrees for contact softness when Filter Mode is ScreenSpace.</summary>
    API_FIELD(Attributes="Limit(0.0f, 45.0f, 0.01f)")
    float AngularRadius = 0.27f;
};


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
    /// <summary>
    /// Directional shadow cascade configuration in increasing end-distance order.
    /// The final entry continues over the remaining view without fading out. An empty list uses the near and far atlas.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    Array<ShadowCascadeSettings> ShadowCascades;

    /// <summary>Colors the active shadow cascade at each receiver pixel, including the blend between ranges.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool ShowShadowCascades = false;

    /// <summary>Requested directional shadow map resolution, limited by the GPU and atlas layout.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(512, 16384, 512)")
    int32 ShadowMapResolution = 2048;

    /// <summary>High-detail shadow distance from the camera, in centimeters. Beyond it, shadows use the 512-texel far tile. Zero uses one map at the requested resolution.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0), ValueCategory(Utils.ValueCategory.Distance)")
    float NearShadowDistance = 2000.0f;

    /// <summary>Uses adaptive light-space sampling for directional shadows.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool UseAdaptiveShadowFilter = true;

    /// <summary>Maximum adaptive filter radius in shadow-map texels.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0.0f, 4.0f, 0.1f)")
    float AdaptiveShadowRadius = 2.5f;

    /// <summary>Filters the shadow mask in screen space when adaptive filtering is disabled.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    bool UseScreenSpaceShadowFilter = true;

    /// <summary>Directional light angular radius in degrees for contact softness in the screen-space filter.</summary>
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
