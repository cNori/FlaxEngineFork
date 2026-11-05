// Copyright (c) Wojciech Figat. All rights reserved.

#include "../RenderResourceTable.h"

void RenderResourceTable::Set(const String& name, GPUResource* resource)
{
    if (InStage)
        Pending[name] = resource;
    else if (resource)
        Resources[name] = resource;
    else
        Resources.Remove(name);
}

void RenderResourceTable::Remove(const String& name)
{
    Set(name, nullptr);
}

GPUResource* RenderResourceTable::Get(const String& name) const
{
    GPUResource* resource = nullptr;
    if (InStage && Pending.TryGet(name, resource))
        return resource;
    if (InStage && StageInputs.TryGet(name, resource))
        return resource;
    Resources.TryGet(name, resource);
    return resource;
}

void RenderResourceTable::BeginStage()
{
    Pending.Clear();
    PendingOutputs.Clear();
    InStage = true;
}

void RenderResourceTable::EndStage(bool commit)
{
    if (commit)
    {
        for (const auto& entry : Pending)
        {
            if (entry.Value)
                Resources[entry.Key] = entry.Value;
            else
                Resources.Remove(entry.Key);
        }
    }
    if (commit)
    {
        for (const auto& entry : PendingOutputs)
        {
            if (entry.Value)
                Outputs[entry.Key] = entry.Value;
            else
                Outputs.Remove(entry.Key);
        }
    }
    Pending.Clear();
    PendingOutputs.Clear();
    StageInputs.Clear();
    InStage = false;
}

void RenderResourceTable::Clear()
{
    ViewRect = Viewport();
    Pending.Clear();
    Resources.Clear();
    Outputs.Clear();
    PendingOutputs.Clear();
    StageInputs.Clear();
    InStage = false;
}

void RenderResourceTable::SetOutput(const String& name, GPUResource* resource)
{
    if (InStage)
        PendingOutputs[name] = resource;
    else if (resource)
        Outputs[name] = resource;
    else
        Outputs.Remove(name);
}

GPUResource* RenderResourceTable::GetOutput(const String& name) const
{
    GPUResource* resource = nullptr;
    if (InStage && PendingOutputs.TryGet(name, resource))
        return resource;
    Outputs.TryGet(name, resource);
    return resource;
}
