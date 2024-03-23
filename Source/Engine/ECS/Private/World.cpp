
#include "../World.h"
#include "Engine/Core/Log.h"
#include "Engine/ECS/Components/Primitive/Component.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include <Engine/ECS/Components/Common/TransformComponent.h>
#include "Engine/ECS/WorldEntity.h"

Dictionary<Int3, WorldSector*> World::GetWorldSectors()
{
    return WorldSectors;
}
WorldSector* World::GetOrAddWorldSector(const Int3& InWorldCoordinate)
{
    WorldSector* Sector = nullptr;
    auto result = WorldSectors.Find(InWorldCoordinate);
    if (result.IsEnd())
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
WorldEntity World::CreateWorldEntity(const Int3& AtWorldCoordinate)
{
    WorldSector* Sector = GetOrAddWorldSector(AtWorldCoordinate);
    if (Sector)
    {
        return Sector->CreateEntity();
    }
    return WorldEntity();
}
void World::Initialize()
{
    if (IsInitialized)
        return;
    IsInitialized = true;
    RegisterComponent<TransformComponent>();
}

 bool World::IsEntityValid(WorldEntity Entity)
{
     return Entity.IsValid();
}


bool World::HasComponent(WorldEntity Entity, MClass* type)
{
    EnTTCSharpGlue out{};
    if (reg.TryGet(GetComponentName(type), out))
    {
        return out.HasComponent(Entity);
    }
    return false;
}

Component* World::GetComponent(WorldEntity Entity, MClass* type)
{
    EnTTCSharpGlue out{};
    if (reg.TryGet(GetComponentName(type), out))
    {
        return &out.GetComponent(Entity);
    }
    return nullptr;
}

Component* World::GetOrAddComponent(WorldEntity Entity, MClass* type)
{
    EnTTCSharpGlue out{};
    if (reg.TryGet(GetComponentName(type), out))
    {
        return &out.GetOrAddComponent(Entity);
    }
    return nullptr;
}

Component* World::AddComponent(WorldEntity Entity, MClass* type)
{
    EnTTCSharpGlue out{};
    if (reg.TryGet(GetComponentName(type), out))
    {
        return &out.AddComponent(Entity);
    }
    return nullptr;
}

void World::DestoryComponent(WorldEntity Entity, MClass* type)
{
    EnTTCSharpGlue out{};
    if (reg.TryGet(GetComponentName(type), out))
    {
        out.DestoryComponent(Entity);
    }
}

String World::GetComponentName(MClass* type)
{
    return type->GetFullName().ToString();
}
