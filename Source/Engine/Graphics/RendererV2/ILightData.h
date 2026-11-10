// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Kind of light supplied to the renderer.</summary>
API_ENUM() enum class RenderLightKind { Point = 0, Directional = 1, Spot = 2 };

/// <summary>Provides borrowed, scene-independent light data to the render pipeline.</summary>
API_INTERFACE() class FLAXENGINE_API ILightData
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(ILightData);
public:
    virtual ~ILightData();

    /// <summary>Returns the light kind.</summary>
    API_FUNCTION() virtual RenderLightKind GetILD_Type() const { return RenderLightKind::Point; }
    /// <summary>Returns the direction in which light travels.</summary>
    API_FUNCTION() virtual Vector3 GetILD_Direction() const { return Vector3::Forward; }
    /// <summary>Returns the linear light colour. Alpha scales intensity.</summary>
    API_FUNCTION() virtual Color GetILD_Color() const { return Color::White; }
    /// <summary>Returns the direct-light brightness before colour alpha.</summary>
    API_FUNCTION() virtual float GetILD_Intensity() const { return 0.0f; }

    /// <summary>Returns the spot cone's inner half-angle in degrees.</summary>
    API_FUNCTION() virtual float GetILD_InnerConeAngle() const { return 0.0f; }
    /// <summary>Returns the spot cone's outer half-angle in degrees.</summary>
    API_FUNCTION() virtual float GetILD_OuterConeAngle() const { return 0.0f; }
    /// <summary>Uses inverse-square attenuation in meters instead of the radial exponent.</summary>
    API_FUNCTION() virtual bool GetILD_InverseSquaredFalloff() const { return false; }
    /// <summary>Returns the radial attenuation exponent.</summary>
    API_FUNCTION() virtual float GetILD_FalloffExponent() const { return 8.0f; }
    /// <summary>Whether this light casts shadows in a dynamic view.</summary>
    API_FUNCTION() virtual bool GetILD_CastShadows() const { return true; }
    /// <summary>Returns shadow opacity. Zero disables shadow rendering.</summary>
    API_FUNCTION() virtual float GetILD_ShadowStrength() const { return 1.0f; }

    /// <summary>Returns the light position in world space.</summary>
    API_FUNCTION() virtual Vector3 GetILD_WorldPosition() const = 0;

    /// <summary>Returns the light influence radius in world units.</summary>
    API_FUNCTION() virtual float GetILD_Range() const = 0;
};
