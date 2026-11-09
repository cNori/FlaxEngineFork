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

    /// <summary>Depth-map resolution sampled by this band. Zero uses the full shared depth map. Smaller maps are reduced from that one geometry render and share its projection; values above the shared depth resolution are clamped.</summary>
    API_FIELD(Attributes="EditorOrder(-10), Limit(0, 16384, 512)")
    int32 TextureSize = 0;

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

    /// <summary>Filter radius in shadow-map texels for Adaptive (0 to 32), or maximum blur radius in screen pixels for ScreenSpace (clamped to 4).</summary>
    API_FIELD(Attributes="Limit(0.0f, 32.0f, 0.1f)")
    float FilterRadius = 2.5f;

    /// <summary>Light angular radius in degrees for contact softness when Filter Mode is ScreenSpace.</summary>
    API_FIELD(Attributes="Limit(0.0f, 45.0f, 0.01f)")
    float AngularRadius = 0.27f;
};



/// <summary>Which side of the shadow edge contributes receiver-distance values.</summary>
API_ENUM() enum class ShadowDistanceSide
{
    /// <summary>Generates distance inside and outside the shadow.</summary>
    Both = 0,
    /// <summary>Retains only negative distances inside the shadow.</summary>
    InnerOnly = 1,
    /// <summary>Retains only positive distances outside the shadow.</summary>
    OuterOnly = 2,
};

/// <summary>Graphics configuration for the scriptable render pipeline.</summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Rendering/Pipeline Graphics Settings\")")
class FLAXENGINE_API RenderPipelineGraphicSettings : public ScriptingObject, public ISerializable
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineGraphicSettings);
    API_AUTO_SERIALIZATION();
public:
    /// <summary>Square resolution of the one directional-light depth map shared by all bands.</summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Shadows\", \"Depth Resolution\"), Limit(512, 16384, 512)")
    int32 ShadowDepthResolution = 2048;

    /// <summary>Radial shadow bands in increasing end-distance order. The final band covers the remaining view. Empty uses one unbanded map.</summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Shadows\", \"Bands\")")
    Array<ShadowCascadeSettings> ShadowCascades;

    /// <summary>Uses adaptive light-space sampling. Per-band filter overrides take precedence.</summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Shadow Filtering\", \"Adaptive Filter\")")
    bool UseAdaptiveShadowFilter = true;

    /// <summary>Adaptive sample radius in shadow-map texels.</summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Shadow Filtering\", \"Adaptive Radius\"), Limit(0.0f, 32.0f, 0.1f), VisibleIf(nameof(UseAdaptiveShadowFilter))")
    float AdaptiveShadowRadius = 2.5f;

    /// <summary>Uses the screen-space filter when adaptive filtering is disabled. Per-band overrides take precedence.</summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Shadow Filtering\", \"Screen Space Filter\"), VisibleIf(nameof(UseAdaptiveShadowFilter), true)")
    bool UseScreenSpaceShadowFilter = true;

    /// <summary>Light angular radius in degrees for screen-space contact softness.</summary>
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Shadow Filtering\", \"Angular Radius\"), Limit(0.0f, 45.0f, 0.01f), VisibleIf(nameof(UseAdaptiveShadowFilter), true), VisibleIf(nameof(UseScreenSpaceShadowFilter))")
    float ShadowAngularRadius = 0.27f;

    /// <summary>Enables camera depth occlusion on DirectX 11.</summary>
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Occlusion Culling\", \"Enabled\")")
    bool EnableOcclusionCulling = true;

    /// <summary>Maximum triangles per mesh contributing to the occluder depth prepass.</summary>
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"Occlusion Culling\", \"Occluder Triangle Limit\"), Limit(0), VisibleIf(nameof(EnableOcclusionCulling))")
    int32 OccluderTriangleLimit = 4096;

    /// <summary>Casters above this total triangle count use their bounds for GPU shadow selection.</summary>
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"Advanced\", \"Shadow Proxy Triangle Threshold\"), Limit(0)")
    int32 ShadowProxyTriangleThreshold = 4096;

    /// <summary>Consumes GPU shadow selection without blocking CPU readback. Disable only for comparison or troubleshooting.</summary>
    API_FIELD(Attributes="EditorOrder(90), EditorDisplay(\"Advanced\", \"Indirect Shadow Draws\")")
    bool UseIndirectShadowDraws = true;

    /// <summary>Consumes GPU camera visibility without blocking CPU readback. Disable only for comparison or troubleshooting.</summary>
    API_FIELD(Attributes="EditorOrder(100), EditorDisplay(\"Advanced\", \"Indirect Occlusion Draws\"), VisibleIf(nameof(EnableOcclusionCulling))")
    bool UseIndirectOcclusionDraws = true;

    /// <summary>Displays accepted proxy bounds in green through scene geometry.</summary>
    API_FIELD(Attributes="EditorOrder(110), EditorDisplay(\"Debug\", \"Proxy Boxes: Selected\")")
    bool ShowSelectedExpensiveShadowCasters = false;

    /// <summary>Displays rejected proxy bounds in red through scene geometry.</summary>
    API_FIELD(Attributes="EditorOrder(120), EditorDisplay(\"Debug\", \"Proxy Boxes: Rejected\")")
    bool ShowRejectedExpensiveShadowCasters = false;

    /// <summary>Colours radial shadow bands and their blend transitions. Takes priority over proxy-box overlays.</summary>
    API_FIELD(Attributes="EditorOrder(130), EditorDisplay(\"Debug\", \"Shadow Cascades\")")
    bool ShowShadowCascades = false;

    /// <summary>Displays white diffuse surfaces with scene lighting and shadows. Ignores material colour, emissive, AO, metalness and specular; preserves surface normals. Cascade debug takes priority.</summary>
    API_FIELD(Attributes="EditorOrder(140), EditorDisplay(\"Debug\", \"White Surface Lighting\")")
    bool ShowWhiteSurfaceLighting = false;

    /// <summary>Uses blue lit surfaces and light-blue shadows in quantized bands. Runs after distance-field generation; when enabled, the distance field supplies the bands. Takes priority over White Surface Lighting and the distance preview; cascade debug still takes priority.</summary>
    API_FIELD(Attributes="EditorOrder(145), EditorDisplay(\"Debug\", \"Cel Shading\")")
    bool ShowCelShading = false;

    /// <summary>Number of discrete colours between light-blue shadow and blue light. Two gives the original two-tone palette.</summary>
    API_FIELD(Attributes="EditorOrder(146), EditorDisplay(\"Debug\", \"Cel Bands\"), Limit(2, 16), VisibleIf(nameof(ShowCelShading))")
    int32 CelShadingBands = 2;

    /// <summary>Enables the randomized adaptive shadow sampling pattern for comparison. Off uses fixed filter taps. The SDF measures the fully shadowed core of the completed filtered result.</summary>
    API_FIELD(Attributes="EditorOrder(147), EditorDisplay(\"Debug\", \"Shadow Jitter\")")
    bool UseShadowJitter = true;

    /// <summary>Displays approximate receiver-surface distance to the fully shadowed core edge. Combines camera depth and light depth; negative inside, positive outside, in world centimeters. Only visible receiver surfaces are available. Debug only; does not modify lighting. Cascade debug takes priority.</summary>
    API_FIELD(Attributes="EditorOrder(150), EditorDisplay(\"Debug\", \"Shadow Distance Field\")")
    bool ShowShadowDistanceField = false;

    /// <summary>World distance in centimeters mapped to full debug colour. Does not change the stored distance field.</summary>
    API_FIELD(Attributes="EditorOrder(160), EditorDisplay(\"Debug\", \"Distance Display Range\"), Limit(0.01f), ValueCategory(Utils.ValueCategory.Distance), VisibleIf(nameof(ShowShadowDistanceField))")
    float ShadowDistanceDisplayRange = 32.0f;

    /// <summary>Retains the inner, outer or both distance ranges. The excluded side stays fully shadowed or lit in cel banding and is black in the distance preview.</summary>
    API_FIELD(Attributes="EditorOrder(161), EditorDisplay(\"Debug\", \"SDF Side\"), VisibleIf(nameof(ShowShadowDistanceField))")
    ShadowDistanceSide DistanceFieldSide = ShadowDistanceSide::Both;

    /// <summary>Cel preview fill at the jitter-to-core handoff. Zero keeps the original shadow. Higher values lift the handoff; the SDF fades this fill inward to the original shadow core over the distance range. The jitter curve keeps its shape and shares the handoff height.</summary>
    API_FIELD(Attributes="EditorOrder(162), EditorDisplay(\"Debug\", \"Shadow Fill Strength\"), Limit(0.0f, 1.0f, 0.01f), VisibleIf(nameof(ShowShadowDistanceField))")
    float ShadowFillStrength = 0.5f;

};
