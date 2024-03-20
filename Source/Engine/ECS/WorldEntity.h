// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Core/Math/Int3.h"
#include <entt/entt.hpp>

class WorldSector;

API_STRUCT()
struct FLAXENGINE_API WorldEntity final
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(WorldEntity)
public:
    constexpr WorldEntity(unsigned long long value = ULLONG_MAX) noexcept : m_ID{ value } {}
    constexpr WorldEntity(const WorldEntity& other) = default;
    constexpr WorldEntity(WorldEntity&& other) noexcept = default;
    ~WorldEntity() noexcept = default;

    constexpr WorldEntity& operator=(const WorldEntity& other) = default;
    constexpr WorldEntity& operator=(WorldEntity&& other) noexcept = default;
    constexpr operator unsigned long long() const noexcept {
        return m_ID;
    }

    API_PROPERTY();
    WorldSector* GetWorldSector()
    {
        return m_Sector.get();
    }
protected:
    friend WorldSector;
    API_FIELD(internal);
    uint64_t m_ID;
    std::shared_ptr<WorldSector> m_Sector;
};
