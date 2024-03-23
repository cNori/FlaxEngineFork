// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "WorldSector.h"
#include "Engine/Core/Math/BoundingBox.h"
class Component;

API_CLASS(Static) class FLAXENGINE_API World
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(World);

    inline static Dictionary<Int3, WorldSector*> WorldSectors = Dictionary<Int3, WorldSector*>();
private:

    //0 just a sector with Player (PlayerComponent,EditorPlayerComponent) stands in
    //1 sector with player stands in and and sectors araund it
    //2 or more are unsuported if WorldSectorsBoundingBox is at default
    inline static byte SimulationDistance = 1;

    // default 50k units from sector orgin (6 digit of predation)
    // ak the size is 1km cubled before the WorldEnity will need to swap a sector
    inline static BoundingBox WorldSectorsBoundingBox = BoundingBox(Vector3(-50000),Vector3(50000));

    inline static bool IsInitialized = false;
public:
    API_FUNCTION() static Dictionary<Int3, WorldSector*> GetWorldSectors();
    static WorldSector* GetOrAddWorldSector(const Int3& InWorldCoordinate);
    API_FUNCTION() static WorldEntity CreateWorldEntity(const Int3& AtWorldCoordinate);
    API_FUNCTION() static void Initialize();
#pragma region CSharpGlue
protected:
    API_FUNCTION(internal) static WorldSector* GetWorldSectorFromPtr(void* InWorldSectorPtr)
    {
        return (WorldSector*)InWorldSectorPtr;
    }

    API_FUNCTION(internal) static bool IsEntityValid(WorldEntity Entity);

    API_FUNCTION(internal) static bool HasComponent(WorldEntity Entity, MClass* type);
    API_FUNCTION(internal) static Component* GetComponent(WorldEntity Entity, MClass* type);
    API_FUNCTION(internal) static Component* GetOrAddComponent(WorldEntity Entity, MClass* type);
    API_FUNCTION(internal) static Component* AddComponent(WorldEntity Entity, MClass* type);
    API_FUNCTION(internal) static void DestoryComponent(WorldEntity Entity, MClass* type);

    struct EnTTCSharpGlue
    {
    public:
        Function<bool       (WorldEntity)> HasComponent;
        Function<Component  (WorldEntity)> GetComponent;
        Function<Component  (WorldEntity)> GetOrAddComponent;
        Function<Component  (WorldEntity)> AddComponent;
        Function<void       (WorldEntity)> DestoryComponent;
    };
    inline static Dictionary<String, EnTTCSharpGlue> reg = Dictionary<String, EnTTCSharpGlue>();
    static String GetComponentName(MClass* type);
public:
    template<typename TComponent>
    static void RegisterComponent()
    {
        EnTTCSharpGlue glue{};
        glue.HasComponent           = [](WorldEntity e) { return e.HasComponent<TComponent>();     };
        glue.GetComponent           = [](WorldEntity e) { return e.GetComponent<TComponent>();     };
        glue.GetOrAddComponent      = [](WorldEntity e) { return e.GetOrAddComponent<TComponent>();};
        glue.AddComponent           = [](WorldEntity e) { return e.AddComponent<TComponent>();     };
        glue.DestoryComponent       = [](WorldEntity e) { return e.DestroyComponent<TComponent>(); };
        reg.Add(GetComponentName(TComponent::GetStaticClass()), glue);
    }
#pragma endregion
};
