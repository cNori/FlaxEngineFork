// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Core/Math/Int3.h"
#include <entt/entt.hpp>

API_STRUCT(NoPod)
struct FLAXENGINE_API WorldEntity final
{
    DECLARE_SCRIPTING_TYPE_STRUCTURE(WorldEntity)
public:
    using entity_type = uint64;
    static constexpr auto null = entt::null;
    constexpr WorldEntity(entity_type value = null) noexcept : m_ID{ value } {}
    constexpr WorldEntity(const WorldEntity& other) = default;
    constexpr WorldEntity(WorldEntity&& other) noexcept = default;
    ~WorldEntity() noexcept = default;

    constexpr WorldEntity& operator=(const WorldEntity& other) = default;
    constexpr WorldEntity& operator=(WorldEntity&& other) noexcept = default;

    constexpr operator entity_type() const noexcept {
        return m_ID;
    }
protected:
    friend class WorldSector;
    friend class World;

    //Data size is allined to 32 bytes Don't add any more data to it !
    //struct data is allined with is 1024
    //next is 64 bytes with is to large

    String m_Name;
    entity_type m_ID;
    class WorldSector* m_Sector = nullptr;
};
