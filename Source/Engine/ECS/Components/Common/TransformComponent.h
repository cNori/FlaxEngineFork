
#pragma once
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/ECS/Components/Primitive/Component.h"


API_CLASS() class TransformComponent : public Component
{
    DECLARE_SCRIPTING_TYPE_WITH_CONSTRUCTOR_IMPL(TransformComponent, Component);

    void OnInitialized() override 
    {
        m_Flags = (ComponentFlags)(NetworkCanReplicate | Enabled);
    };
    void NetowrkTick(float NetworkDeltaTime) override 
    {

    };

    void Move(const Vector3& Direction)
    {
        m_WorldTransform.Add(Direction);
    }

private:
    Int3 WorldSectorCoordinates;
    Transform m_WorldTransform;
    TransformComponent* m_Parent;
    Array<TransformComponent> m_Children;
};
