// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Content/JsonAssetReference.h"
#include "RenderPipelineGraphicSettings.h"


/// <summary>
/// Saved pipeline configuration. Implementation types create independent runtime objects per view.
/// Reorder Stages to change execution order; no separate stage assets are required.
/// </summary>
API_CLASS(Attributes="ContentContextMenu(\"New/Rendering/Render Pipeline\")")
class FLAXENGINE_API RenderPipelineAsset : public ScriptingObject, public ISerializable
{
    DECLARE_SCRIPTING_TYPE(RenderPipelineAsset);
    API_AUTO_SERIALIZATION();
public:
    /// <summary>The concrete runtime pipeline implementation.</summary>
    API_FIELD(Attributes="TypeReference(typeof(RenderPipeline)), CustomEditorAlias(\"FlaxEditor.CustomEditors.Editors.TypeNameEditor\")")
    StringAnsi PipelineType = "FlaxEngine.DefaultPipeline";

    /// <summary>The graphics configuration shared by runtime instances.</summary>
    API_FIELD() JsonAssetReference<RenderPipelineGraphicSettings> GraphicSettings;

    /// <summary>The stage implementation types in execution order.</summary>
    API_FIELD(Attributes="TypeReference(typeof(RenderPipelineStage)), Collection(CanReorderItems=true, OverrideEditorTypeName=\"FlaxEditor.CustomEditors.Editors.TypeNameEditor\")")
    Array<StringAnsi> Stages;

    /// <summary>Checks implementation references without creating runtime objects.</summary>
    /// <param name="error">Description of the first broken reference, or empty on success.</param>
    /// <returns>True if the definition is valid.</returns>
    API_FUNCTION() bool Validate(API_PARAM(Out) String& error) const;
};
