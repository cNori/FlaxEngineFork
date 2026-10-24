#pragma once
#include "Engine/V2/Entity.h"
#include "Engine/V2/Component.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/ChunkedArray.h"
#include <Engine/Core/Memory/Allocation.h>
#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Types/StringView.h"
#include "Engine/Core/Types/String.h"


#define DECLARE_COMPONENT(Type)                          \
    static StringView GetTypeName()                      \
    {                                                    \
        return StringView(TEXT(#Type));                  \
    }                                                    \
    static EntityComponentSystem::ComponentTypeId GetComponentTypeId()\
    {                                                    \
        static const EntityComponentSystem::ComponentTypeId id =      \
            EntityComponentSystem::RegisterComponent(sizeof(Type),alignof(Type),GetTypeName());  \
        return id;                                       \
    }\

//Component
struct Component
{
    DECLARE_COMPONENT(Type)
};
/// <summary>
/// ECS stuff
/// </summary>
static class EntityComponentSystem
{
public:
    using ComponentTypeId = uint32;
private:
    struct ComponentDescriptor
    {
    public:
        String TypeName;
        ComponentTypeId TypeID;
        uint32 Size;
        uint32 Alignment;
        // life cycle callbacks, stable UUID/name, etc.
    };
    struct ComponentLayout
    {
    public:
        ComponentTypeId Type; // descriptor index
        uint32 Offset;        // column start in this chunk
        uint32 Stride;        // usually sizeof(component)
    };
    struct Archetype
    {
        uint64 ID; //ID to the ComponentDescriptor
        Array<ComponentLayout> Components; // sorted by Type
        uint32 RowCapacity;                // derived from the layout
    };
    struct Chunk
    {
        uint64 alignas(sizeof(uint64)) Chunks[1024*2];
    };
    struct Entry
    {
        Archetype Archetype;
        ChunkedArray<Chunk, 16> Data;
    };
    Array<Entry> Registry;
    static Array<ComponentDescriptor> descriptors;
    static Dictionary<StringView, ComponentTypeId> Types;
public:
    static ComponentTypeId RegisterComponent(uint32 Size,uint32 Alignment,const StringView& TypeName)
    {
        auto it = Types.Find(TypeName);
        if (it.Index() == -1)
        {
            //missing component declaration
            auto componentTypeId = descriptors.Count();
            //construct Descriptor
            ComponentDescriptor descriptor;
            descriptor.Size = Size;
            descriptor.Alignment = Alignment;
            descriptor.TypeID = componentTypeId;
            descriptor.TypeName = TypeName;
            descriptors.Add(descriptor);
            Types.Add(TypeName, componentTypeId);
        }
        else
        {
            //we all ready have it
            //hard crash for now
            //TODO: handle it softly when using editor
            if (Platform::IsDebuggerPresent())
            {
                PLATFORM_DEBUG_BREAK;
            }
            Platform::Assert("Duplicate component registration", __FILE__, __LINE__);
        }
    }
};
