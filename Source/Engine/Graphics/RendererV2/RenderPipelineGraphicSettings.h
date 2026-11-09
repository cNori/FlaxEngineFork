// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"

/// <summary>Master shadow rendering mode.</summary>
API_ENUM() enum class ShadowRenderingMode
{
    /// <summary>One point depth comparison; no filtering or distance blending.</summary>
    Pixel = 0,
    /// <summary>Filtered shadows with cascade and blocker-distance transitions.</summary>
    Smooth = 1,
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

    /// <summary>Receiver-to-blocker distance along the light ray in centimeters where this entry's filter blends into the next entry's filter. Independent of the map End Distance. Zero uses the numeric map End Distance as the blocker-distance threshold. Ignored on the final entry.</summary>
    API_FIELD(Attributes="Limit(0), ValueCategory(Utils.ValueCategory.Distance)")
    float BlurEndDistance = 0.0f;

    /// <summary>Fraction of the blocker-distance interval used to blend evaluated filter results. Independent of the map Blend Fraction.</summary>
    API_FIELD(Attributes="Limit(0.0f, 1.0f, 0.01f)")
    float BlurBlendFraction = 0.1f;

    /// <summary>Direct Smooth blur radius in this entry's Texture Size texels. Larger values soften this camera-distance band and the corresponding blocker-distance band. Pixel ignores it. Zero disables filtering for this entry.</summary>
    API_FIELD(Attributes="EditorDisplay(null, \"Filter Size\"), Limit(0.0f, 32.0f, 0.1f)")
    float FilterRadius = 2.5f;

    /// <summary>Extra depth comparison bias in shadow-map texels, added to the automatic precision correction.</summary>
    API_FIELD(Attributes="Limit(0.0f, 10.0f, 0.01f)")
    float DepthBias = 0.0f;

    /// <summary>Receiver normal offset in shadow-map texels, scaled by the angle to the light.</summary>
    API_FIELD(Attributes="Limit(0.0f, 10.0f, 0.01f)")
    float NormalBias = 0.0f;



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
    /// <summary>Pixel skips shadow filtering, contact correction, distance-field processing and all cascade/blur transition blending. Smooth uses distance-based filtering.</summary>
    API_FIELD(Attributes="EditorOrder(-10), EditorDisplay(\"Shadows\", \"Rendering\")")
    ShadowRenderingMode ShadowRendering = ShadowRenderingMode::Smooth;

    /// <summary>Square resolution of the one directional-light depth map shared by all bands.</summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Shadows\", \"Depth Resolution\"), Limit(512, 16384, 512)")
    int32 ShadowDepthResolution = 2048;

    /// <summary>Radial shadow bands in increasing end-distance order. The final band covers the remaining view. Empty uses one unbanded map.</summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Shadows\", \"Shadow Distance Settings\")")
    Array<ShadowCascadeSettings> ShadowCascades;

    /// <summary>Displays blur bands by receiver-to-blocker distance along the light ray, using the cascade palette. Blue-black means no blocker; magenta means outside light coverage. Enable Shadow Cascades too to overlay white cascade ends and dark cascade blend-start contours.</summary>
    API_FIELD(Attributes="EditorOrder(131), EditorDisplay(\"Debug\", \"Blur Bands\")")
    bool ShowShadowBlurBands = false;





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


    /// <summary>Displays receiver-to-blocker distance along the directional-light ray in centimeters. With cascades, evaluates each configured band map and blends its distance display using the existing radial ranges and Blend Fraction. Without cascades, uses the full light map. Black is contact, white reaches the display range, blue has no blocking sample, magenta is outside light coverage. Averages blocker depths over the local shadow-map search; does not include contact visibility or recover hidden depth layers.</summary>
    API_FIELD(Attributes="EditorOrder(148), EditorDisplay(\"Debug\", \"Blocker Distance\")")
    bool ShowBlockerDistance = false;

    /// <summary>Receiver-to-blocker distance mapped to white. Changes only the display, not the measured distance or shadows.</summary>
    API_FIELD(Attributes="EditorOrder(149), EditorDisplay(\"Debug\", \"Blocker Display Range\"), Limit(0.01f), ValueCategory(Utils.ValueCategory.Distance), VisibleIf(nameof(ShowBlockerDistance))")
    float BlockerDistanceDisplayRange = 100.0f;

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


    /// <summary>Combines short camera-depth contact visibility with mapped shadows using minimum visibility. Camera-hidden and off-screen geometry remains covered only by the shadow map.</summary>
    API_FIELD(Attributes="EditorOrder(51), EditorDisplay(\"Contact Shadows\", \"Enabled\")")
    bool UseContactShadows = true;

    /// <summary>Maximum contact ray length in world centimeters. This is a visibility correction, not a blocker-distance replacement.</summary>
    API_FIELD(Attributes="EditorOrder(52), EditorDisplay(\"Contact Shadows\", \"Length\"), Limit(0.0f, 200.0f, 0.1f), ValueCategory(Utils.ValueCategory.Distance), VisibleIf(nameof(UseContactShadows))")
    float ContactShadowLength = 50.0f;

    /// <summary>Camera-depth thickness accepted by the contact trace, in centimeters. Larger values tolerate coarse depth but may darken around silhouettes.</summary>
    API_FIELD(Attributes="EditorOrder(53), EditorDisplay(\"Contact Shadows\", \"Thickness\"), Limit(0.01f), ValueCategory(Utils.ValueCategory.Distance), VisibleIf(nameof(UseContactShadows))")
    float ContactShadowThickness = 1.0f;

    /// <summary>Camera distance where contact shadows finish fading out. The regular shadow map is unaffected.</summary>
    API_FIELD(Attributes="EditorOrder(54), EditorDisplay(\"Contact Shadows\", \"Fade Distance\"), Limit(0.01f), ValueCategory(Utils.ValueCategory.Distance), VisibleIf(nameof(UseContactShadows))")
    float ContactShadowFadeDistance = 2000.0f;

    /// <summary>Displays contact visibility alone: white is unblocked, black is contact shadow. Cascade and blocker-distance debug take priority.</summary>
    API_FIELD(Attributes="EditorOrder(149), EditorDisplay(\"Debug\", \"Contact Shadows\")")
    bool ShowContactShadows = false;

};
