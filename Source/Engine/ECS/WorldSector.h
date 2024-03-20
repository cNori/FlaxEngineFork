// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "WorldCoordinate.h"
#include "WorldEntity.h"
#include "ThirdParty/entt/entt.hpp"

//entt::entity is just uint32_t
using EntityHandle = WorldEntity;
using EntityRegistry = entt::basic_registry<WorldEntity>;

API_CLASS() class FLAXENGINE_API WorldSector
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(WorldSector)
protected:
    friend class World;
    friend class Entity;

    //EnTT Implementacion
    EntityRegistry registry;

    template<typename T>
    T GetComponent(EntityHandle Handle)
    {
        return registry.get<T>(Handle);
    }
    template<typename T>
    T GetOrAddComponent(EntityHandle Handle)
    {
        return registry.get_or_emplace<T>(Handle);
    }

    template<typename T>
    T AddComponent(EntityHandle Handle)
    {
        return registry.emplace<T>(Handle);
    }

    template<typename T>
    T DestroyComponent(EntityHandle Handle)
    {
        return registry.destroy<T>(Handle);
    }

    template<typename T>
    T HasComponent(EntityHandle Handle)
    {
        return registry.any_of<T>(Handle);
    }

    Entity* CreateEntity()
    {
        auto e = New<Entity>();
        e->Handle = static_cast<uint32_t>(registry.create());
        e->Sector = this;
        return e;
    }
    void DestroyEntity(Entity* entity)
    {
        entity->Sector = nullptr;
        registry.destroy(entity);
        Delete(entity);
        entity = nullptr;//clear the pointer
    }
};
