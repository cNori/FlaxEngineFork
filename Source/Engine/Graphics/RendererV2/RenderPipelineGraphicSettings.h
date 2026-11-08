// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Math/Vector2.h"


/// <summary>Displays a shadow-stage GPU resource beside the scene, or coloured caster footprints over the scene.</summary>
API_ENUM() enum class ShadowResourceDebugView
{
    /// <summary>Displays normal pipeline output.</summary>
    None = 0,
    /// <summary>Camera depth reconstructed as distance. White is near; black reaches the debug depth distance.</summary>
    ReceiverDepth = 1,
    /// <summary>Raw encoded receiver normals from the GBuffer.</summary>
    ReceiverNormals = 2,
    /// <summary>The shared cheap-occluder light-depth map.</summary>
    CandidateOccluderDepth = 3,
    /// <summary>The depth map of the selected cascade, or the atlas for the legacy path.</summary>
    CascadeDepth = 4,
    /// <summary>The resolved shadow visibility mask. White is lit; black is shadow.</summary>
    ShadowVisibility = 5,
    /// <summary>Colours projected receiver footprints over the scene. Each expensive caster has a stable colour derived from its identifier.</summary>
    CasterSelection = 6,
    /// <summary>Shows the main camera output and a material-colour scene render from the directional light side by side. Disables visibility-colour overlays in this view.</summary>
    CameraAndLight = 7,
    /// <summary>Step 1: light-space wire boxes. Green remains potentially light-visible; red is fully blocked by opaque cheap geometry.</summary>
    LightCasterVisibility = 8,
    /// <summary>Step 2: shared light depth containing cheap meshes and light-visible expensive caster boxes.</summary>
    ProxyShadowDepth = 9,
    /// <summary>Step 3: camera-visible receivers crossed with proxy light depth. Orange can receive a proxy shadow; blue has no proxy blocker; dark gray faces away from the light; purple has no surface.</summary>
    ReceiverDepthCross = 10,
    /// <summary>Step 4: receiver footprints from selected casters hidden from the camera. Cyan is camera-occluded; magenta is outside its frustum.</summary>
    HiddenCasterContributions = 11,
    /// <summary>Traces one caster through the legacy receiver diagnostic shader. Uses Receiver Debug Step and the actor-name filter; final selection uses the shared camera/light mask.</summary>
    ReceiverShaderSteps = 12,
    /// <summary>Camera-visible surfaces reprojected into the shared light map. Green can receive light; purple faces away; black has no camera sample.</summary>
    CameraReceiverCoverage = 13,
    /// <summary>Nearest eligible camera-visible receiver depth in light space. Scaled by Shadow Debug Depth Distance, measured from the shared light near plane.</summary>
    CameraReceiverLightDepth = 14,
    /// <summary>Camera-visible receiver depths crossed with cheap light depth: green matches/precedes it, red is behind, yellow contains mixed depths, blue has no cheap blocker.</summary>
    CameraReceiverLightCross = 15,
    /// <summary>Colours radial shadow bands and their transitions. Takes priority over other debug overlays; all bands share the same light-depth map.</summary>
    CascadeBands = 16,
};

/// <summary>Intermediate data written by the actual receiver-selection kernel for one caster.</summary>
API_ENUM() enum class ShadowReceiverDebugStep
{
    /// <summary>R is decision code: 1 outside sweep, 2 no camera surface, 3 light-blocked, 4 ray miss, 5 back-facing, 7 proxy miss, 8 cheap-blocked, 9 accepted, 10 accepted with uncertainty.</summary>
    Decision = 0,
    /// <summary>R raw camera depth; G distance from camera in cm; B camera-depth uncertainty in cm; A one.</summary>
    CameraDepth = 1,
    /// <summary>RGB reconstructed world position in cm; A one.</summary>
    WorldPosition = 2,
    /// <summary>RGB geometric normal (or fallback shading normal); A one for a reliable plane, zero for fallback.</summary>
    SurfaceNormal = 3,
    /// <summary>RGB displacement of the selection probe in cm; A configured receiver bias in cm.</summary>
    ReceiverBias = 4,
    /// <summary>R/G light UV, B receiver light-space depth, A one for valid coordinates.</summary>
    LightProjection = 5,
    /// <summary>R/G minimum/maximum proxy depth separation in cm; B tolerance in cm; A valid-tap bitmask.</summary>
    ProxyDepthDifference = 6,
    /// <summary>R/G minimum/maximum cheap-occluder depth separation in cm; B tolerance in cm; A valid-tap bitmask.</summary>
    CheapDepthDifference = 7,
    /// <summary>R precision tolerance in cm; G camera-depth uncertainty in cm; B combined tolerance in cm; A one.</summary>
    ComparisonTolerance = 8,
    /// <summary>R reliable-plane flag; G normal dot light direction; B camera-depth uncertainty in cm; A one.</summary>
    SurfaceConfidence = 9,
    /// <summary>R valid cheap taps, G blocking cheap taps (bitmasks), B tolerance in cm; A one.</summary>
    CheapTapCoverage = 10,
    /// <summary>R valid proxy taps, G potential proxy taps (bitmasks), B tolerance in cm; A one.</summary>
    ProxyTapCoverage = 11,
};

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

    /// <summary>Requested shadow resolution. The largest band value selects the shared depth size when Shadow Depth Resolution is zero. Bands share one projection and depth draw.</summary>
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


/// <summary>Debug output for the experimental multi-resolution shadow mask blend. White means shadow, black means lit.</summary>
API_ENUM() enum class ShadowMaskBlendDebugView
{
    /// <summary>Shows the lit scene.</summary>
    None = 0,
    /// <summary>Shows (A + B*A + C*A) / 3.</summary>
    Combined = 1,
    /// <summary>Shows the detailed shadow mask A.</summary>
    Detailed = 2,
    /// <summary>Shows the filtered low-resolution shadow mask B.</summary>
    Low = 3,
    /// <summary>Shows the filtered medium-resolution shadow mask C.</summary>
    Medium = 4,
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
    /// Directional shadow band configuration in increasing end-distance order. All bands resolve a single light-depth render.
    /// The final entry continues over the remaining view without fading out. An empty list uses the near and far atlas.
    /// </summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\")")
    Array<ShadowCascadeSettings> ShadowCascades;

    /// <summary>Colours radial shadow bands and their blend transitions. Takes priority over resource, receiver-hit and mask-blend debug views.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ShowShadowCascades = false;

    /// <summary>Resolution of the single directional-light depth render shared by all shadow bands. Zero uses the largest Texture Size in Shadow Cascades. A common projection covers all bands, so increasing coverage reduces world-space texel density.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0, 16384, 512)")
    int32 ShadowDepthResolution = 0;

    /// <summary>Overlays receiver pixels hit by expensive caster bounds. Orange: camera-visible caster; cyan: occluded caster; magenta: outside the camera frustum. Enables exhaustive diagnostic scanning.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ShowShadowReceiverHits = false;

    /// <summary>Offset opposite the surface normal in centimeters used only by shadow caster selection. Keeps receiver tests off the occluder surface. Does not change final shadow-map depth or normal bias. Excessive values can discard small contact shadows.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadows\"), Limit(0.0f, 100.0f, 0.01f), ValueCategory(Utils.ValueCategory.Distance)")
    float ShadowSelectionReceiverBias = 0.1f;

    /// <summary>Displays a GPU resource beside the scene. Caster Selection instead overlays per-caster colours on receiver surfaces. Purple checkerboard means a resource is unavailable.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    ShadowResourceDebugView ShadowResourceDebug = ShadowResourceDebugView::None;

    /// <summary>Uses the whole viewport for resource textures. When disabled, textures appear in a right-side panel and the scene remains visible.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ShadowResourceDebugFullscreen = false;

    /// <summary>Exact source actor name to inspect. Empty includes all casters. Filters debug footprints and wire boxes only, without changing rendering selection.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    String ShadowDebugCasterName;

    /// <summary>Intermediate receiver-kernel data to inspect in Receiver Shader Steps. A negative A value means the pixel exited at an earlier decision; its magnitude is the reason code.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    ShadowReceiverDebugStep ReceiverDebugStep = ShadowReceiverDebugStep::Decision;

    /// <summary>Display range in cm for signed depth separation and spatial grid size. Does not change selection tolerance.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\"), Limit(0.001f), ValueCategory(Utils.ValueCategory.Distance)")
    float ReceiverDebugRange = 1.0f;

    /// <summary>Camera pixel to inspect. Negative coordinates select the center. A crosshair marks the sampled pixel in the side panel.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    Float2 ReceiverDebugPixel = Float2(-1, -1);

    /// <summary>Reads the probe's four raw values every quarter second while tracing. Debug only: the readback can synchronize the CPU and GPU.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ReadReceiverDebugPixel = false;

    /// <summary>Legacy cascade-array debug index. Cascade Depth displays the common light-depth map for every band.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\"), Limit(0)")
    int32 ShadowResourceCascadeIndex = 0;

    /// <summary>Distance in centimeters mapped to black by the Receiver Depth viewer. This changes display only.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\"), Limit(1), ValueCategory(Utils.ValueCategory.Distance)")
    float ShadowDebugDepthDistance = 2000.0f;

    /// <summary>Draws green wire boxes for expensive casters accepted by the view/light-wide potential-receiver test.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ShowSelectedExpensiveShadowCasters = false;

    /// <summary>Draws red wire boxes for expensive casters rejected by the potential-receiver test. Debug boxes remain visible through geometry.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Debug\")")
    bool ShowRejectedExpensiveShadowCasters = false;

    /// <summary>Builds two filtered lower-resolution masks from the completed cascade visibility and applies (A + B*A + C*A) / 3. No additional geometry draws.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\")")
    bool UseMultiResolutionShadowBlend = false;

    /// <summary>Inspects the input masks or combined shadow amount without lighting. A debug selection previews the blend even when it is disabled.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\")")
    ShadowMaskBlendDebugView ShadowMaskBlendDebug = ShadowMaskBlendDebugView::None;

    /// <summary>Longest edge of shadow mask B in pixels. Aspect ratio is preserved; limited to mask C's size.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\"), Limit(8, 2048, 8)")
    int32 ShadowBlendLowResolution = 32;

    /// <summary>Longest edge of shadow mask C in pixels. Aspect ratio is preserved; limited to the detailed mask's size.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\"), Limit(8, 2048, 8)")
    int32 ShadowBlendMediumResolution = 64;

    /// <summary>Separable Gaussian filter radius in mask B texels. Zero disables its blur.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\"), Limit(0.0f, 8.0f, 0.1f)")
    float ShadowBlendLowFilterRadius = 2.0f;

    /// <summary>Separable Gaussian filter radius in mask C texels. Zero disables its blur.</summary>
    API_FIELD(Attributes="EditorDisplay(\"Shadow Mask Blend\"), Limit(0.0f, 8.0f, 0.1f)")
    float ShadowBlendMediumFilterRadius = 2.0f;

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
