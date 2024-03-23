// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"
#include "Engine/ECS/WorldEntity.h"

//#pragma pack(push, 4)
API_CLASS() class FLAXENGINE_API Component : public ScriptingObject
{
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(Component, ScriptingObject);
public:
    /// <summary>
    /// This is called only once.
    /// If you have one-time things to establish up-front (like binding callbacks or events), do so here.
    /// </summary>
    API_FUNCTION() virtual void OnInitialized() {};

    /// <summary>
    /// Called after the Component is constructed.
    /// Depending on how the object is used this may be called multiple times due to adding and removing.
    /// If you need a true called once, use OnInitialized.
    /// </summary>
    API_FUNCTION() virtual void Construct() {};

    /// <summary>
    /// Ticks this Component.
    /// </summary>
    /// <param name="DeltaTime">The delta time.</param>
    API_FUNCTION() virtual void Tick(float DeltaTime) {};

    /// <summary>
    /// Physics engine Ticks this Component.
    /// Executed by physics engine thread it might not be thread safe, it's depending on physics backend.
    /// if u have any physics object u like to modify transform of do it so here
    /// </summary>
    /// <param name="DeltaTime">The delta time.</param>
    API_FUNCTION() virtual void PhysicTick(float PhysicDeltaTime) {};

    /// <summary>
    /// Network menager Ticks this Component.
    /// only active when component can NetworkCanReplicate flag is set
    /// </summary>
    /// <param name="DeltaTime">The delta time.</param>
    API_FUNCTION() virtual void NetowrkTick(float NetworkDeltaTime) {};

    /// <summary>
    /// Called when a Component is no longer referenced causing the resource to be destroyed.
    /// Just like construct this event can be called multiple times.
    /// </summary>
    API_FUNCTION() virtual void Destruct() {};

    /// <summary>
    /// note it can return null if component is deleted or invalid 
    /// </summary>
    /// <returns>the owner for this component</returns>
    API_PROPERTY() WorldEntity GetOwner() 
    {
        return *Owner;
    }
protected:
    WorldEntity* Owner;
    enum ComponentFlags : byte
    {
        None = 0,
        Enabled = 1,
        CanTick = 2,
        CustomTickRate = 4,
        PhysicCanTick = 8,
        NetworkCanReplicate = 16,
    };
    ComponentFlags m_Flags = Enabled;
    // Updates 60 times per secend by default
    byte TickRate = 60;
    // Updates 10 times per secend by default
    byte NetworkTickRate = 10;
};
//#pragma pack(pop)
