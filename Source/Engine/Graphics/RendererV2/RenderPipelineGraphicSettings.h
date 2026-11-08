// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/ISerializable.h"

/// <summary>Graphics configuration for the scriptable render pipeline.</summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Rendering/Pipeline Graphics Settings\")")
class FLAXENGINE_API RenderPipelineGraphicSettings : public ScriptingObject, public ISerializable
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineGraphicSettings);
    API_AUTO_SERIALIZATION();
public:
    /// <summary>Enables camera depth occlusion on DirectX 11.</summary>
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Occlusion Culling\", \"Enabled\")")
    bool EnableOcclusionCulling = true;

    /// <summary>Maximum triangles per mesh contributing to the occluder depth prepass.</summary>
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"Occlusion Culling\", \"Occluder Triangle Limit\"), Limit(0), VisibleIf(nameof(EnableOcclusionCulling))")
    int32 OccluderTriangleLimit = 4096;

    /// <summary>Consumes GPU camera visibility without blocking CPU readback. Disable only for comparison or troubleshooting.</summary>
    API_FIELD(Attributes="EditorOrder(100), EditorDisplay(\"Occlusion Culling\", \"Indirect Draws\"), VisibleIf(nameof(EnableOcclusionCulling))")
    bool UseIndirectOcclusionDraws = true;

    /// <summary>Maximum number of texels along each axis of a shadow page.</summary>
    API_FIELD(Attributes="EditorOrder(105), EditorDisplay(\"World Shadow Grid\", \"Resolution\"), CustomEditorAlias(\"FlaxEditor.CustomEditors.Editors.VirtualShadowResolutionEditor\")")
    int32 VirtualShadowMapResolution = 512;

    /// <summary>Shows lit receivers in green and shadowed receivers in orange for the selected light.</summary>
    API_FIELD(Attributes="EditorOrder(120), EditorDisplay(\"World Shadow Grid\", \"Show Visibility\")")
    bool ShowVirtualShadowPages = false;

    /// <summary>Selects a shadow-casting light in collection order for the shadow debug overlays.</summary>
    API_FIELD(Attributes="EditorOrder(130), EditorDisplay(\"World Shadow Grid\", \"Debug Light Index\"), Limit(0)")
    int32 VirtualShadowDebugLight = 0;

    /// <summary>Shows the selected light's actual shadow texels and page sizes on camera-visible surfaces.</summary>
    API_FIELD(Attributes="EditorOrder(140), EditorDisplay(\"World Shadow Grid\", \"Show Grid\")")
    bool ShowVirtualShadowWorldGrid = false;

    /// <summary>World width of a shadow page for all lights. Span divided by Resolution is the finest world-space texel size.</summary>
    API_FIELD(Attributes="EditorOrder(110), EditorDisplay(\"World Shadow Grid\", \"Span\"), Limit(100), ValueCategory(Utils.ValueCategory.Distance)")
    float VirtualShadowWorldGridSpan = 10000.0f;
};
