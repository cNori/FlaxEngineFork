// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Collections/Dictionary.h"

class WorldSector;

API_CLASS() class FLAXENGINE_API Entity : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(Entity, ScriptingObject);
protected:
    friend class WorldSector;
    uint32_t Handle;
    WorldSector* Sector = nullptr;
public:
    API_FUNCTION() WorldSector* GetWorldSector()
    {
        return Sector;
    }
public:
    template<typename T>
    T GetComponent()
    {
        return Sector->GetComponent<T>(Handle);
    }
    template<typename T>
    T GetOrAddComponent()
    {
        return Sector->GetOrAddComponent<T>(Handle);
    }
    template<typename T>
    T HasComponent()
    {
        return Sector->HasComponent<T>(Handle);
    }
    template<typename T>
    T DestroyComponent()
    {
        return Sector->DestroyComponent<T>(Handle);
    }
};
