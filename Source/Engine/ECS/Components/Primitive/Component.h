// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#pragma once
#include "Engine/Scripting/ScriptingObject.h"

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
    /// Override in derived classes, but always call the parent implementation.
    /// </summary>
    /// <param name="DeltaTime">The delta time.</param>
    API_FUNCTION() virtual void Tick(float DeltaTime) {};

    /// <summary>
    /// Called when a Component is no longer referenced causing the resource to be destroyed.
    /// Just like construct this event can be called multiple times.
    /// </summary>
    API_FUNCTION() virtual void Destruct() {};
};
