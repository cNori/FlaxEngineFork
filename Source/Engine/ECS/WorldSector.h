// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/ScriptingObject.h"
#include "WorldEntity.h"
#include "ThirdParty/entt/entt.hpp"
#include <Engine/Core/Log.h>

//entt::entity is just uint32_t
using EntityHandle = WorldEntity;
using EntityRegistry = entt::basic_registry<WorldEntity>;

API_CLASS() class FLAXENGINE_API WorldSector : public ScriptingObject 
{
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(WorldSector, ScriptingObject)
protected:
    friend class World;
    friend class Entity;

    //EnTT Implementacion
    EntityRegistry registry;
public:

    template<typename T>
    T& GetComponent(WorldEntity* Handle)
    {
        return registry.get<T>(*Handle);
    }
    template<typename T, typename... Args>
    T& GetOrAddComponent(WorldEntity* Handle)
    {
        return registry.get_or_emplace<T>(*Handle, std::forward<Args>(args)...);
    }

    template<typename T,typename... Args>
    T& AddComponent(WorldEntity* Handle, Args&&... args)
    {
        return registry.emplace<T>(*Handle, std::forward<Args>(args)...);
    }

    template<typename T>
    void DestroyComponent(WorldEntity* Handle)
    {
        registry.destroy(*Handle);
    }

    template<typename T>
    bool HasComponent(WorldEntity* Handle)
    {
        return registry.any_of<T>(*Handle);
    }

    WorldEntity CreateEntity()
    {
        auto& e = registry.create();
        e.m_Sector = this;
        LOG(Info, "Created entity with ID {0} At Sector {1}", e.m_ID,(uint64)e.m_Sector);
        return e;
    }
    void DestroyEntity(WorldEntity entity)
    {
        entity.m_Sector = nullptr;
        registry.destroy(entity);
    }
};
