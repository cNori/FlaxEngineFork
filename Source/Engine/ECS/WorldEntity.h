// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include <entt/entt.hpp>

class WorldSector;

API_STRUCT(NoPod)
struct FLAXENGINE_API WorldEntity final
{
    DECLARE_SCRIPTING_TYPE_STRUCTURE(WorldEntity)
public:
    template<typename T>
    T& GetComponent(){ return GetSector()->GetComponent<T>(this); }
    template<typename T, typename... Args>
    T& GetOrAddComponent() { return GetSector()->GetOrAddComponent<T>(this, std::forward<Args>(args)...); }
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) { return GetSector()->AddComponent<T>(this, std::forward<Args>(args)...); }
    template<typename T>
    void DestroyComponent() { GetSector()->DestroyComponent<T>(this); }
    template<typename T>
    bool HasComponent() { return GetSector()->HasComponent<T>(this); }
public:
    using entity_type = uint64;
    static constexpr auto null = entt::null;
    constexpr WorldEntity(entity_type value = null) noexcept : m_ID{ value } {}
    constexpr WorldEntity(const WorldEntity& other) = default;
    constexpr WorldEntity(WorldEntity&& other) noexcept = default;
    ~WorldEntity() noexcept = default;

    constexpr WorldEntity& operator=(const WorldEntity& other) = default;
    constexpr WorldEntity& operator=(WorldEntity&& other) noexcept = default;

    constexpr operator entity_type() const noexcept { return m_ID; }
    operator bool() const noexcept { return IsValid(); }
    bool IsValid() const noexcept
    {
        return !(m_ID == entt::tombstone || m_ID == null);
    }

    WorldSector* GetSector() 
    {
        return (WorldSector*)m_Sector;
    }
protected:
    friend WorldSector;
    friend class World;
    //Data size is allined to 32 bytes Don't add any more data to it !
    //struct data is allined with is 1024
    //next is 64 bytes with is to large

public:
    //TODO: make this filed's protected
    //bindigs generation s not allowing for it right now

    /// <summary>
    /// For internal use only
    /// </summary>
    API_FIELD(internal) uint64 m_ID;

    /// <summary>
    /// For internal use only
    /// </summary>
    API_FIELD(internal) void* m_Sector = nullptr;
};
