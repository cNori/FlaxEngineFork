#pragma once
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Types/StringView.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Memory/Memory.h"
#include <Engine/Scripting/ScriptingObjectReference.h>
#include <Engine/Core/Types/Variant.h>


#define DECLARE_COMPONENT(Type)                                                                                        \
    static StringView GetTypeName()                                                                                    \
    {                                                                                                                  \
        return StringView(TEXT(#Type));                                                                                \
    }                                                                                                                  \
    static uint32 GetComponentTypeId()                                                                                 \
    {                                                                                                                  \
        static_assert(std::is_default_constructible<Type>::value,"ECS component "#Type"must be default constructible");\
        static const uint32 id =                                                                                       \
            EntityComponentSystem::RegisterComponent(                                                            \
            sizeof(Type),                                                                                              \
            alignof(Type),                                                                                             \
            GetTypeName(),                                                                                             \
                [](void* memory) { new (memory) Type(); },                                                             \
                [](void* memory) { static_cast<Type*>(memory)->~Type(); }                                              \
                );                                                                                                     \
        return id;                                                                                                     \
    }                                                                                                                  \

struct Entity;
struct Component;

/// <summary>
/// ECS stuff
/// </summary>
API_CLASS(Static) class EntityComponentSystem
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(EntityComponentSystem)
public:
    class LowLevelCalls;
    API_CLASS() class ComponentDescriptor : public ScriptingObject
    {
        DECLARE_SCRIPTING_TYPE(ComponentDescriptor);
    public:
        String TypeName;
        uint32 TypeID;
        uint32 Size;
        uint32 Alignment;
        void (*Construct)(void* memory);
        void (*Destroy)(void* memory);
        // life cycle callbacks, stable UUID/name, etc.
    };

    API_CLASS() class Registry : public ScriptingObject
    {
        DECLARE_SCRIPTING_TYPE(Registry);
    public:
        API_ENUM() enum Result
        {
            Ok,
            //errors below
            DoubleInitCall,
            ArchetypeGroupToBig,
            InvalidEntity,
            EmptyArchetype,
            //memory management
            BufferMismatch,
            OutOfMemory,
            CapacityOverflow,
            //miscellaneous stuff
            NotFound,
        };
    public:
        struct Entry
        {
        public:
            //TODO:
            //for this class
            //1. FIX THE FUCKING BINDINGS GENERATOR
            //it is some strate up bullshit it is ECS and component descryptors are ComponentDescriptor**
            //lets jump araund in memory like some rabit becuse why not
            //insted of having packed array of ComponentDescriptor (Archetypes) we have some strate up bullshit to get it to C#
            //when this is done swap it back to paced array and const ComponentDescriptor&
            typedef Array<EntityComponentSystem::ComponentDescriptor*> Archetypes;
        public:
            struct IndexRemapper
            {
                int32 From;
                int32 To;
            };

            struct Chunk
            {
                uint64 alignas(sizeof(uint64)) Chunks[1024 * 2];
            };
            //Archetype<A>,Archetype<B>, Archetype<C>
            //Archetype<C>,Archetype<A>, Archetype<B>
            //sequence is the same so it can be sorted
            //there is no point of holding ABC and CAB in 2 Entry's
            Archetypes Types;

            //Archetype:
            //       [A        ][B        ][C        ]
            //Data Layout:
            //       Rows
            //Column [RowStride][RowStride][RowStride]...
            //       [.........][.........][.........]...
            //       [.........][.........][.........]...
            //       [.........][.........][.........]...
            //
            //Memory View:      
            //       [A][B][C][A][B][C][A][B][C][A][B][C]
            //Memory View Matrix:
            //       [A][B][C]
            //       [A][B][C]
            //       [A][B][C]
            //       [A][B][C]
            //
            // now rules
            // we like to keep us at page size () for maximum component size
            // if user likes more he needs to use ptr and suffer the cost and memory management
            // when moving component across Registry's

            Chunk* Data = nullptr;
            //number of chunks currently containing active rows
            uint32 DataCount;
            //number of currently allocated chunks
            uint32 DataCapacity;
            // The active row count
            uint32 ActiveRowCount;
            //max StrideSize per Column is limited to 512 bytes, if u need more WTF are u doing
            const static uint32 MaxRowStrideSize = 512;
            //size of packed components
            uint32 RowStride = sizeof(uint64);
            uint32 Padding = 0;
            uint32 RowsPerChunk = 0;

            EntityComponentSystem::Registry::Result Init(Archetypes Types);

            /// <summary>
            /// Ensures the slots.
            /// Expands Chunks
            /// </summary>
            EntityComponentSystem::Registry::Result EnsureSlots(uint64 count);

            ~Entry()
            {
                if (Data != nullptr)
                {
                    //TODO: Probably general log in ECS system Entry got pop-ed
                    //will allow for better memory debug
                    Allocator::Free(Data);

                }
            }

            uint32 GetNextGeneration()
            {
                auto r = ActiveRowCount;
                ActiveRowCount++;
                return r;
            }

            void ConstructComponents(uint32 rowIndex);

            bool IsRowAlive(uint32 rowIndex)
            {
                //TODO: implemented it when objects marked as deleted is implemented
                return true;
            }

            void MarkRowDead(uint32 rowIndex)
            {
                //TODO: implemented it when objects marked as deleted is implemented
            }

            void DestroyComponents(uint32 rowIndex);

            Result GetComponentBlockInfo(
                uint32 rowIndex,
                uint8*& outChunkBytes,
                void*& outBlockData,
                uint32& outBlockSize)
            {
                outChunkBytes = nullptr;
                outBlockData = nullptr;
                outBlockSize = 0;

                if (!Data || RowsPerChunk == 0 || rowIndex >= ActiveRowCount)
                    return Result::InvalidEntity;

                const uint32 chunkIndex = rowIndex / RowsPerChunk;
                const uint32 rowInChunk = rowIndex % RowsPerChunk;
                if (chunkIndex >= DataCapacity)
                    return Result::InvalidEntity;

                outChunkBytes = reinterpret_cast<uint8*>(&Data[chunkIndex]);
                outBlockData = outChunkBytes + static_cast<uint64>(rowInChunk) * RowStride;
                outBlockSize = RowStride;
                return Result::Ok;
            }

            struct ComponentData
            {
                void* Data;
                uint32 Size;
            };
            void SplitToComponents(uint8* chunkBytes, void* blockData, Array<ComponentData>& components)
            {
                components.Clear();
                uint32 offset = 0;
                for (int32 i = 0; i < Types.Count(); ++i)
                {
                    ComponentData component;
                    component.Data = static_cast<uint8*>(blockData) + offset;
                    component.Size = Types[i]->Size;
                    components.Add(component);

                    offset += Types[i]->Size;
                }
            }
        };
        Array<EntityComponentSystem::Registry::Entry> registry;
    private:
        EntityComponentSystem::Registry::Result CreateEntity(const Entry::Archetypes& Archetypes, Entity& OutEntity);
        EntityComponentSystem::Registry::Result DestroyEntity(Entity& entity);
        bool HasComponentsOfType(Entity& entity, const ComponentDescriptor& descriptor);
        EntityComponentSystem::Registry::Result GetComponentsOfType(Entity& entity, const ComponentDescriptor& descriptor, Array<Component*>& components);
        EntityComponentSystem::Registry::Result AddComponentOfType(Entity& entity, const ComponentDescriptor& descriptor, const Component* component);
        EntityComponentSystem::Registry::Result DestroyComponentOfType(Entity& entity, const Component* component);
        void FindArchetypeMatch(const Entry::Archetypes& Archetypes, int32& OutEntryID, Array<Entry::IndexRemapper>& OutOrderRemap);
        friend class EntityComponentSystem::LowLevelCalls;
    public:
        ~Registry() = default;
    };
private:
    static Array<ComponentDescriptor> descriptors;
    static Dictionary<StringView, uint32> Types;
public:
    //internal and bindings use only
    static uint32 RegisterComponent(uint32 Size, uint32 Alignment, const StringView& TypeName, void (*Construct)(void* memory), void (*Destroy)(void* memory));
public:
    //Exposed Low level calls don't use it unless u know what are u doing
    //also: API here is volatile can change at anytime between versions of the engine use High Level-API instead
    API_CLASS(Static) class LowLevelCalls
    {
        DECLARE_SCRIPTING_TYPE_MINIMAL(LowLevelCalls)
    public:
        API_FUNCTION() static EntityComponentSystem::Registry::Result CreateEntity(EntityComponentSystem::Registry& reg, const Array<EntityComponentSystem::ComponentDescriptor*>& Archetypes, API_PARAM(Out) Entity& OutEntity)
        {
            return reg.CreateEntity(Archetypes, OutEntity);
        }
        API_FUNCTION() static EntityComponentSystem::Registry::Result DestroyEntity(EntityComponentSystem::Registry& reg, API_PARAM(Ref) Entity& OutEntity)
        {
            return reg.DestroyEntity(OutEntity);
        }
        API_FUNCTION() static bool HasComponentsOfType(EntityComponentSystem::Registry& reg, API_PARAM(Ref) Entity& entity, EntityComponentSystem::ComponentDescriptor& descriptor)
        {
            return reg.HasComponentsOfType(entity, descriptor);
        }
        API_FUNCTION() static EntityComponentSystem::Registry::Result GetComponentsOfType(EntityComponentSystem::Registry& reg, API_PARAM(Ref) Entity& entity, EntityComponentSystem::ComponentDescriptor& descriptor,API_PARAM(Out) Array<Variant>& components);
        API_FUNCTION() static EntityComponentSystem::Registry::Result AddComponentOfType(EntityComponentSystem::Registry& reg, API_PARAM(Ref) Entity& entity, EntityComponentSystem::ComponentDescriptor& descriptor, const Component* component)
        {
            return reg.AddComponentOfType(entity, descriptor, component);
        }
        API_FUNCTION() static EntityComponentSystem::Registry::Result DestroyComponentOfType(EntityComponentSystem::Registry& reg, API_PARAM(Ref) Entity& entity, const Component* component)
        {
            return reg.DestroyComponentOfType(entity, component);
        }
        API_FUNCTION() static EntityComponentSystem::Registry::Result GetComponentDescriptor(const StringView& componentTypeName, API_PARAM(Out) EntityComponentSystem::ComponentDescriptor*& OutComponentDescriptor);
    };
};


/*
hit another limit API_INTERFACE generation
so fields can be generated
property's throw out of scope stuff lost the IComponent for who knows why or how
API_FUNCTION() uint32 GetID() be overlooked by binding generator C++ glue exists C# nope
plopping it as a struct not ideal for the base
*/


/// <summary>
/// base component
/// </summary>
API_STRUCT() struct FLAXENGINE_API Component
{
    DECLARE_SCRIPTING_TYPE_STRUCTURE(Component)
    DECLARE_COMPONENT(Component)
private:
    uint32 ID;
    uint32 Owner;
private:
    friend EntityComponentSystem::Registry;
public:
    //Exposed Low level calls don't use it unless u know what are u doing
    //also: API here is volatile can change at anytime between versions of the engine use High Level-API instead
    API_PROPERTY() uint32 GetLowLevelID() { return ID; };
    //Exposed Low level calls don't use it unless u know what are u doing
    //also: API here is volatile can change at anytime between versions of the engine use High Level-API instead
    API_PROPERTY() uint32 GetLowLevelOwner() { return Owner; };
};




API_STRUCT() struct FLAXENGINE_API Entity
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(Entity)
private:
    //for now this is not shared and potentially unsafe access can happen

    //ID corresponds to the Entry index in the registry
    uint32 ID;
    //Generation corresponds to the index in the Entry[ID].Data[archetype + Generation]
    //by doing ID then Generation with find the data block where Components data is living
    //fast
    uint32 Generation;
private:
    friend EntityComponentSystem::Registry;
public:
    API_PROPERTY() uint64 GetID() const { return ID; }
    API_PROPERTY() uint64 GetGeneration() const { return ID; }
    //template<typename T>
    //entity.HasComponent<T>();
    //entity.GetComponent<T>(instanceIndex);
    //entity.GetComponents<T>();
    //entity.AddComponent<T>(component);
    //entity.DestroyComponent<T>(component.ID);
};
