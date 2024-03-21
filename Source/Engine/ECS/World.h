// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "WorldSector.h"

API_CLASS(Static) class FLAXENGINE_API World
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(World);

    inline static Dictionary<Int3, WorldSector*> WorldSectors = Dictionary<Int3, WorldSector*>();
public:
    API_FUNCTION() static Dictionary<Int3, WorldSector*> GetWorldSectors()
    {
        return WorldSectors;
    }
    static WorldSector* GetOrAddWorldSector(const Int3& InWorldCoordinate)
    {
        WorldSector* Sector = nullptr;
        auto result = WorldSectors.Find(InWorldCoordinate);
        if(result.IsEnd())
        {
            Sector = New<WorldSector>();
            WorldSectors.Add(InWorldCoordinate, Sector);
        }
        else
        {
            Sector = WorldSectors[result.Index()];
        }
        return Sector;
    }
    API_FUNCTION() static WorldEntity CreateWorldEntity(const Int3& AtWorldCoordinate)
    {
        WorldSector* Sector = GetOrAddWorldSector(AtWorldCoordinate);
        if(Sector)
        {
            return Sector->CreateEntity();
        }
        return WorldEntity();
    }
    API_FUNCTION() static uint64 GetEntityID(const WorldEntity& InEntity)
    {
        //the WorldEntity can be converted automatically in c++ side to the ID but the c# has no idea about the ID
        //but the structs are not suporting a funcions now so it is done this way...
        return InEntity;
    }
    API_FUNCTION() static WorldSector* GetEntityWorldSector(const WorldEntity& InEntity)
    {
        //structs are not suporting a funcions now so it is done this way...
        return InEntity.m_Sector;
    }

    API_FUNCTION() static bool HasComponent(ScriptingTypeHandle* type)
    {
        //[todo] do a look up based on MClass and call templated HasComponent<T> funcion for it return resoult as a output
    }
    template<typename IComponent>
    static void RegisterComponent()
    {
        //[todo] register component to static Dictionary<MClass typename,Function<bool>> at compile time
    }
};
