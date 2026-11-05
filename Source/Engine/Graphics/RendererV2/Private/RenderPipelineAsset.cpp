// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderPipelineAsset.h"
#include "../RenderPipeline.h"
#include "../RenderPipelineStage.h"
#include "Engine/Scripting/Scripting.h"

bool RenderPipelineAsset::Validate(String& error) const
{
    error = String::Empty;
    const auto pipelineType = Scripting::FindScriptingType(PipelineType);
    if (!pipelineType || !RenderPipeline::TypeInitializer.IsAssignableFrom(pipelineType))
    {
        error = String::Format(TEXT("Broken pipeline implementation reference: {0}"), String(PipelineType));
        return false;
    }
    Array<StringAnsi> stageNames;
    for (int32 i = 0; i < Stages.Count(); i++)
    {
        const auto& reference = Stages[i];
        auto type = Scripting::FindScriptingType(reference);
        if (!type || !RenderPipelineStage::TypeInitializer.IsAssignableFrom(type))
        {
            error = String::Format(TEXT("Broken stage implementation reference at index {0}: {1}"), i, String(reference));
            return false;
        }
        const StringAnsi name(type.GetType().Fullname);
        if (stageNames.Contains(name))
        {
            error = String::Format(TEXT("Duplicate stage implementation at index {0}: {1}"), i, String(reference));
            return false;
        }
        stageNames.Add(name);
    }
    return true;
}

RenderPipelineAsset::RenderPipelineAsset(const SpawnParams& params)
    : ScriptingObject(params)
{
}
