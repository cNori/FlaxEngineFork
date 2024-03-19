// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "WorldSector.h"
#include "Entity.h"

API_CLASS(Static) class FLAXENGINE_API World
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(World);

    inline static Dictionary<Int3, WorldSector*> WorldSectors = Dictionary<Int3, WorldSector*>();
public:
    API_FUNCTION() static Dictionary<Int3, WorldSector*> GetWorldSectors()
    {
        return WorldSectors;
    }

    API_FUNCTION() static Entity* CreateEntity(const WorldCoordinate& worldcords)
    {
        auto Sector = WorldSectors[worldcords.SectorCoordinate];
        if(Sector == nullptr)
        {
            Sector = New<WorldSector>();
            WorldSectors.Add(worldcords.SectorCoordinate, Sector);
        }
        return Sector->CreateEntity();
    }
};
