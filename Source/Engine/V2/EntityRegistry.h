#pragma once
#include "Engine/V2/Entity.h"
#include "Engine/V2/Component.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/ChunkedArray.h"
#include <Engine/Core/Memory/Allocation.h>
#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Types/StringView.h"
#include "Engine/Core/Types/String.h"
#include <Engine/Core/Memory/Memory.h>
#include <Engine/Core/Templates.h>


#define DECLARE_COMPONENT(Type)                                                                                        \
    static StringView GetTypeName()                                                                                    \
    {                                                                                                                  \
        return StringView(TEXT(#Type));                                                                                \
    }                                                                                                                  \
    static EntityComponentSystem::ComponentTypeId GetComponentTypeId()                                                 \
    {                                                                                                                  \
        static_assert(std::is_default_constructible<Type>::value,"ECS component "#Type"must be default constructible");\
        static const EntityComponentSystem::ComponentTypeId id =                                                       \
            EntityComponentSystem::RegisterComponent<Type>(                                                            \
            sizeof(Type),                                                                                              \
            alignof(Type),                                                                                             \
            GetTypeName(),                                                                                             \
                [](void* memory) { new (memory) Type(); },                                                             \
                [](void* memory) { static_cast<Type*>(memory)->~Type(); }                                              \
                );                                                                                                     \
        return id;                                                                                                     \
    }                                                                                                                  \


struct Entity
{
private:
    //for now this is not shared and potentially unsafe access can happen
    
    //ID corresponds to the Entry index in the registry
    uint32 ID;
    //Generation corresponds to the index in the Entry[ID].Data[archetype + Generation]
    //by doing ID then Generation with find the data block where Components data is living
    //fast
    uint32 Generation;
private:
    friend class EntityComponentSystem::Registry;
public:
    //template<typename T>
    //entity.HasComponent<T>();
    //entity.GetComponent<T>(instanceIndex);
    //entity.GetComponents<T>();
    //entity.AddComponent<T>(component);
    //entity.DestroyComponent<T>(component.ID);
};

/// <summary>
/// default empty component
/// </summary>
struct IComponent
{
    DECLARE_COMPONENT(IComponent)
private:
    uint32 ID;
    uint32 Owner;
private:
    friend class EntityComponentSystem::Registry;
};

/// <summary>
/// ECS stuff
/// </summary>
static class EntityComponentSystem
{
public:
    using ComponentTypeId = uint32;
    struct ComponentDescriptor
    {
    public:
        String TypeName;
        ComponentTypeId TypeID;
        uint32 Size;
        uint32 Alignment;
        void (*Construct)(void* memory);
        void (*Destroy)(void* memory);
        // life cycle callbacks, stable UUID/name, etc.
    };

    class Registry
    {
    public:
        enum Result
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
        struct Entry
        {
        public:
            typedef Array<EntityComponentSystem::ComponentDescriptor> Archetypes;
        private:
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

            Result Init(Archetypes Types)
            {
                if (Data != nullptr)
                    return Result::DoubleInitCall;
                if (Types.IsEmpty())
                    return Result::EmptyArchetype;

                //validate the Types is not over the size limit
                //compute RowStride
                uint64 size = 0;
                for (size_t i = 0; i < Types.Count(); i++)
                {
                    size += Types[i].Size;
                }
                if (size > MaxRowStrideSize)
                    return Result::ArchetypeGroupToBig;

                //set the stride
                RowStride = size;
                //we know the RowStride now the compute count of the number of columns
                //if we are near the MaxRowStrideSize allocation just 1 Chunk 
                //might be to low we will need to double it to save on allocations later
                uint32 columnCount = 0;
                uint32 columnSize = 0;
                const uint32 chunkSize = sizeof(Chunk);
                while (columnSize <= chunkSize - RowStride)
                {
                    columnSize += RowStride;
                    columnCount++;
                }
                Padding = chunkSize - columnSize;

                //cNori note:
                //Depending on the RowStride we need to pre allocate X chunks
                //it will be pointless to allocate 1 chunk where stride is 512 bytes
                //it will fill up with 32 component sets fast assumption
                //is made base on my experience with making large games

                //linear interpolate to the min and max to compute pre-allocation amount
                const uint32 t = RowStride - 1; // 0..511
                const uint32 slots = 10000 - (t * 9000) / 511;

                //number of allocations depends on the number of
                //slots requested and sizeof(Chunk) and its RowStride
                RowsPerChunk = sizeof(Chunk) / RowStride;
                const uint32 chunkCount = (slots + RowsPerChunk - 1) / RowsPerChunk;

                //we know size, init Data
                const uint64 allocationSize = sizeof(Chunk) * chunkCount;
                auto* data = static_cast<Chunk*>(Allocator::Allocate(allocationSize, alignof(Chunk)));

                //TODO: Probably general log in ECS Entry got created
                //will allow for better memory debug

                if (data == nullptr)
                {
                    //OutOfMemory ? probably yes there is no other case where Allocate can fail
                    //also flax do be handling it but at the same time keep it here for rare case
                    //something crap it self
                    return Result::OutOfMemory;
                }

                Data = data;
                DataCount = chunkCount;
                DataCapacity = chunkCount;
                //copy Archetypes
                this->Types = Types;
                return Result::Ok;
            }
            /// <summary>
            /// Ensures the slots.
            /// Expands Chunks
            /// </summary>
            Result EnsureSlots(uint64 count)
            {
                //compute current slot count
                const uint64 currentSlots = static_cast<uint64>(DataCapacity) * RowsPerChunk;
                if (count <= currentSlots)
                    return Result::Ok;

                //compute next slot count
                const uint64 newChunkCount = (count + RowsPerChunk - 1) / RowsPerChunk;
                if (newChunkCount > MAX_uint32)
                    return Result::CapacityOverflow;

                //compere
                const uint64 oldSize = static_cast<uint64>(DataCapacity) * sizeof(Chunk);
                const uint64 newSize = newChunkCount * sizeof(Chunk);

                //this shit has wrong implementation (Allocate + MemoryCopy + Free)
                //crossing system-user boundary 3 times on windows
                //it cant be used even has wrong sig
                //void* newData = AllocatorExt::ReallocAligned(Data, oldSize, newSize, alignof(Chunk));

                //manual emulation
                //will be crap because of crossing user->system->user boundary 3 times on windows
                if (oldSize == newSize)
                    return Result::Ok;

                //TODO: Probably general log in ECS Entry got expanded
                //will allow for better memory debug
                auto* newData = static_cast<Chunk*>(Allocator::Allocate(newSize, alignof(Chunk)));
                if (!newData)
                    return Result::OutOfMemory;

                if (Data)
                {
                    Platform::MemoryCopy(newData, Data, oldSize);
                    Allocator::Free(Data);
                }

                Data = newData;
                DataCapacity = static_cast<uint32>(newChunkCount);
                return Result::Ok;
            }
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

            void ConstructComponents(uint32 rowIndex)
            {
                //cNori Node:
                //umm this is where magic happens
                //just in case leaving this comment and comment below for future reference

                //It is constructing objects pre allocated memory
                //this place controls how components are created
                //also see: memory comment layout above Data
                //so just of it [A][B][C]....
                //it is constructed as a group
                //wen we iterate over the components CPU will fetch blocks of memory
                //in case of L1 case on CPU it is 64 bytes L2 and L3 are larger
                //but we care about the L1 so if A is fetched for example vector3 of 32bits
                //CPU might load a 12 bytes (16 allied) and pre-load next component data at the same time
                //speeding up operations
                //so layout has been picked for next or last component access
                //if it will result in extreme slowness to the
                //[A][A][A][A]...
                //[B][B][B][B]...
                //[C][C][C][C]...
                //layout might be worth investigating.
                //it has draw back of the memory jumps but iterating over same component type is fast


                //TODO: reuse objects marked as deleted and correct the GetNextGeneration
                //and cheek logic for handling so entities so we don't have dangling IDs
                //but for now
                //we will do ActiveRowCount so when we are do advance constructors are called 
                auto gen = ActiveRowCount - 1;
                const uint32 chunkIndex = rowIndex / RowsPerChunk;
                const uint32 rowInChunk = rowIndex % RowsPerChunk;

                auto* chunkBytes = reinterpret_cast<uint8*>(&Data[chunkIndex]);
                auto* row = chunkBytes + static_cast<uint64>(rowInChunk) * RowStride;

                uint32 offset = 0;
                for (int32 i = 0; i < Types.Count(); i++)
                {
                    const auto& type = Types[i];
                    type.Construct(row + offset);
                    offset += type.Size;
                }
            }

            bool IsRowAlive(uint32 rowIndex)
            {
                //TODO: implemented it when objects marked as deleted is implemented
                return true;
            }

            void MarkRowDead(uint32 rowIndex)
            {
                //TODO: implemented it when objects marked as deleted is implemented
            }

            void DestroyComponents(uint32 rowIndex)
            {
                const uint32 chunkIndex = rowIndex / RowsPerChunk;
                const uint32 rowInChunk = rowIndex % RowsPerChunk;
                auto* chunkBytes = reinterpret_cast<uint8*>(&Data[chunkIndex]);
                auto* row = chunkBytes + static_cast<uint64>(rowInChunk) * RowStride;
                uint32 offset = 0;
                for (int32 i = 0; i < Types.Count(); i++)
                {
                    const auto& type = Types[i];
                    type.Destroy(row + offset);
                    offset += type.Size;
                }
            }


            void* GetComponentData(uint32 generation, uint32 where)
            {
                if (!Data || RowsPerChunk == 0 ||
                    generation >= ActiveRowCount ||
                    where >= Types.Count())
                {
                    return nullptr;
                }

                const uint32 chunkIndex = generation / RowsPerChunk;
                const uint32 rowInChunk = generation % RowsPerChunk;

                if (chunkIndex >= DataCapacity)
                    return nullptr;

                auto* chunkBytes = reinterpret_cast<uint8*>(&Data[chunkIndex]);
                auto* row = chunkBytes + static_cast<uint64>(rowInChunk) * RowStride;

                uint64 componentOffset = 0;
                for (uint32 i = 0; i < where; ++i)
                    componentOffset += Types[i].Size;

                return row + componentOffset;
            }

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
                    component.Size = Types[i].Size;
                    components.Add(component);

                    offset += Types[i].Size;
                }
            }

            friend class EntityComponentSystem;
        };
    private:
        Array<Entry> Registry;
    private:
        Result CreateEntity(const Entry::Archetypes& Archetypes, Entity& OutEntity)
        {
            // 1. Find matching Entry and input-to-entry order remap.
            Array<Entry::IndexRemapper> Map = {};
            int EntryID;
            FindArchetypeMatch(Archetypes, EntryID, Map);
            if (EntryID == -1)
            {
                // 2. If absent, create and initialize an Entry.
                const int32 entryIndex = Registry.Count();

                Entry entry;
                Result result = entry.Init(Archetypes);
                if (result != Result::Ok)
                    return result;

                OutEntity.Generation = entry.GetNextGeneration();
                OutEntity.ID = EntryID;
                Registry.Add(MoveTemp(entry));
            }
            else
            {
                // 2. If not absent, set the Generation and ID.
                OutEntity.Generation = Registry[EntryID].GetNextGeneration();
                OutEntity.ID = EntryID;
            }

            // 3. Construct each component in the Entry for Entity.
            Entry& entry = Registry[OutEntity.ID];
            auto result = entry.EnsureSlots(entry.ActiveRowCount + 1);
            if (result == Result::Ok)
                entry.ConstructComponents(OutEntity.Generation);
            return result;
        }
        Result DestroyEntity(Entity& entity)
        {
            if (entity.ID >= Registry.Count() || entity.Generation == MAX_uint32)
                return Result::InvalidEntity;

            Entry& entry = Registry[entity.ID];
            const uint32 rowIndex = entity.Generation;

            if (!entry.IsRowAlive(rowIndex))
                return Result::InvalidEntity;

            entry.DestroyComponents(rowIndex); // reverse component order
            entry.MarkRowDead(rowIndex);

            entity.ID = MAX_uint32;
            entity.Generation = 0;
            return Result::Ok;
        }
        bool   HasComponentsOfType(Entity& entity, ComponentDescriptor descriptor)
        {
            Entry& entry = Registry[entity.ID];
            //component type fetch
            for (size_t i = 0; i < entry.Types.Count(); i++)
            {
                if (descriptor.TypeID == entry.Types[i].TypeID)
                {
                    return true;
                }
            }
            return false;
        }
        Result GetComponentsOfType(Entity& entity, ComponentDescriptor descriptor, Array<IComponent*>& components)
        {
            Array<Entry::ComponentData> ComponentData;
            void* dataBlock;
            uint32 dataSize;
            uint8* chunkBytes;
            Entry& entry = Registry[entity.ID];
            auto result = entry.GetComponentBlockInfo(entity.Generation, chunkBytes, dataBlock, dataSize);
            if (result != Result::Ok)
                return result;
            entry.SplitToComponents(chunkBytes, dataBlock, ComponentData);
            Entry& entry = Registry[entity.ID];
            //component type fetch
            for (size_t i = 0; i < entry.Types.Count(); i++)
            {
                if (descriptor.TypeID == entry.Types[i].TypeID)
                {
                    components.Add((IComponent*)ComponentData[i].Data);
                }
            }
            return Result::Ok;
        }
        Result AddComponentOfType(Entity& entity, ComponentDescriptor descriptor, const IComponent* component)
        {
            //find new Archetype set
            Entry::Archetypes ar = Registry[entity.ID].Types;
            ar.Add(descriptor);
            int32 entryID;
            Array<Entry::IndexRemapper> orderRemap;
            FindArchetypeMatch(ar, entryID, orderRemap);
            if (entryID == -1)
            {
                // 1. If absent, create and initialize an Entry.
                const int32 entryIndex = Registry.Count();

                Entry entry;
                Result result = entry.Init(ar);
                if (result != Result::Ok)
                    return result;

                auto oldGeneration = entity.Generation;
                Entry& oldEntry = Registry[entity.ID];

                entity.Generation = entry.GetNextGeneration();
                entity.ID = Registry.Count();
                Registry.Add(MoveTemp(entry));

                // 2. Construct each component in the Entry for Entity.
                Entry& entry = Registry[entity.ID];
                auto result = entry.EnsureSlots(entry.ActiveRowCount + 1);
                if (result == Result::Ok)
                {
                    //cNori note:
                    //we don't need to construct components it is simple memory move
                    //but we need whole chunk of group of Archetypes {[A][B][C]} needs a copy
                    //not 1 by one it will be stupid way to do it
                    //also we don't care about the IndexRemapper it is new group of Archetypes

                    //3. get component data and new place data
                    void* oldDataBlock = nullptr;
                    uint32 oldDataSize;
                    uint8* _ = nullptr;
                    result = oldEntry.GetComponentBlockInfo(oldGeneration, _, oldDataBlock, oldDataSize);
                    if (result != Ok)
                        return result;
                    void* dataBlock = nullptr;
                    uint32 dataSize;
                    result = entry.GetComponentBlockInfo(entity.Generation, _, dataBlock, dataSize);
                    if (result != Ok)
                        return result;

                    //wtf happen case just in case something will go horribly wrong
                    if (oldDataSize != dataSize)
                        return Result::BufferMismatch;

                    //3. Copy component data to new place
                    Platform::MemoryCopy(dataBlock, oldDataBlock, dataSize);

                    //4. Append new data
                    //time to pray the descriptor corresponds to the component
                    Platform::MemoryCopy(static_cast<uint8*>(dataBlock) + dataSize, component, descriptor.Size);

                    //5. clear old chunk and mark it dead for GC
                    oldEntry.MarkRowDead(oldGeneration);
                    Platform::MemoryClear(oldDataBlock, dataSize);
                }
            }
            else
            {
                //1. Get current existing registry data
                Array<Entry::ComponentData> currentComponentData;
                auto& curentEntry = Registry[entity.ID];
                void* oldDataBlock;
                uint32 oldDataSize;
                uint8* oldChunkBytes;
                auto result = curentEntry.GetComponentBlockInfo(entity.Generation, oldChunkBytes, oldDataBlock, oldDataSize);
                curentEntry.SplitToComponents(oldChunkBytes, oldDataBlock, currentComponentData);
                curentEntry.MarkRowDead(entity.Generation);
                Platform::MemoryClear(oldDataBlock, oldDataSize);

                //2. Get existing registry data and remap entity to new entry
                Array<Entry::ComponentData> componentData;
                auto& entry = Registry[entryID];
                void* dataBlock;
                uint32 dataSize;
                uint8* chunkBytes;
                auto result = entry.EnsureSlots(entry.ActiveRowCount + 1);
                if (result != Result::Ok)
                    return result;
                entity.Generation = entry.GetNextGeneration();
                entity.ID = entryID;
                auto result = entry.GetComponentBlockInfo(entity.Generation, chunkBytes, dataBlock, dataSize);
                entry.SplitToComponents(chunkBytes, dataBlock, componentData);
                //3. copy components based on IndexRemapper info
                const uint32 newInputIndex = currentComponentData.Count();
                uint32 newComponentSlot = MAX_uint32;

                for (int32 i = 0; i < orderRemap.Count(); ++i)
                {
                    const auto& remap = orderRemap[i];

                    if (remap.From == newInputIndex)
                    {
                        newComponentSlot = remap.To;
                        continue;
                    }

                    //wtf happen case just in case something will go horribly wrong
                    if (componentData[remap.To].Size != currentComponentData[remap.From].Size)
                        return Result::BufferMismatch;

                    Platform::MemoryCopy(componentData[remap.To].Data, currentComponentData[remap.From].Data, componentData[remap.From].Size);
                }

                //4. Append new data
                //time to pray the descriptor corresponds to the component
                Platform::MemoryCopy(componentData[newComponentSlot].Data, component, descriptor.Size);
            }
            return Result::Ok;
        }
        Result DestroyComponentOfType(Entity& entity, const IComponent* component)
        {
            //find new Archetype set
            Entry::Archetypes ar = Registry[entity.ID].Types;
            ar.RemoveAt(component->ID);
            int32 entryID;
            Array<Entry::IndexRemapper> orderRemap;
            bool useIndexRemapper = orderRemap.Count() != 0;
            FindArchetypeMatch(ar, entryID, orderRemap);

            //cNori note:
            //small amount of shenanigans here with memory here
            //we need to copy upper and lower parts of data
            //in case we pop a {[A][B][C]} and the [B] is pop-ed
            //result is {[A][C]}
            //- if is first component upper part dose not need copy
            //- if is last component lower part dose not need copy
            //- if we are at the middle A and B needs a copy
            //and if the component->ID == 0 or Registry[entity.ID].Types.Count();
            //skip 2 pass copy

            //1. Get current existing data
            Array<Entry::ComponentData> oldComponentData;
            auto& oldEntry = Registry[entity.ID];
            auto oldGeneration = entity.Generation;
            void* oldDataBlock;
            uint32 oldDataSize;
            uint8* oldChunkBytes;
            auto result = oldEntry.GetComponentBlockInfo(entity.Generation, oldChunkBytes, oldDataBlock, oldDataSize);
            oldEntry.SplitToComponents(oldChunkBytes, oldDataBlock, oldComponentData);

            //2. Get new entry
            if (entryID == -1)
            {
                //absent, create and initialize an Entry.
                Entry e;
                Result result = e.Init(ar);
                if (result != Result::Ok)
                    return result;
                Registry.Add(MoveTemp(e));
                entryID = Registry.Count();
            }
            Entry& entry = Registry[entryID];
            auto result = entry.EnsureSlots(entry.ActiveRowCount + 1);
            if (result != Result::Ok)
                return result;

            Array<Entry::ComponentData> newComponentDataNest;
            {
                void* newDataBlock;
                uint32 newDataSize;
                uint8* newChunkBytes;
                auto result = entry.GetComponentBlockInfo(entity.Generation, newChunkBytes, newDataBlock, newDataSize);
                entry.SplitToComponents(newChunkBytes, newDataBlock, newComponentDataNest);
            }

            if (useIndexRemapper)
            {
                for (int32 i = 0; i < orderRemap.Count(); ++i)
                {
                    const auto& remap = orderRemap[i];

                    //wtf happen case just in case something will go horribly wrong
                    if (newComponentDataNest[remap.To].Size != oldComponentData[remap.From].Size)
                        return Result::BufferMismatch;

                    Platform::MemoryCopy(newComponentDataNest[remap.To].Data, oldComponentData[remap.From].Data, oldComponentData[remap.From].Size);
                }
            }
            else
            {
                //cNori note:
                //this is slow way to do it there is faster way but lazy me works who cares (probably future cNori screw u for bad code in the past)

                //straight up copy 1 to 1, - 1 component
                int32 j = 0;
                for (int32 i = 0; i < oldComponentData.Count(); i++)
                {
                    if (component->ID == i)
                        continue;

                    //wtf happen case just in case something will go horribly wrong
                    if (newComponentDataNest[i].Size != oldComponentData[i].Size)
                        return Result::BufferMismatch;

                    Platform::MemoryCopy(newComponentDataNest[i].Data, oldComponentData[i].Data, oldComponentData[i].Size);
                }
            }
            oldEntry.MarkRowDead(oldGeneration);
            Platform::MemoryClear(oldDataBlock, oldDataSize);
            entity.ID = entryID;
            entity.Generation = entry.GetNextGeneration();
            return Result::Ok;
        }
        void FindArchetypeMatch(const Entry::Archetypes& Archetypes, int32& OutEntryID, Array<Entry::IndexRemapper>& OutOrderRemap)
        {
            OutOrderRemap.Resize(Archetypes.Count(), false);
            OutEntryID = -1;
            int32 hitFound = Archetypes.Count();
            int32 hitCount = 0;

            //scan Registry for Archetypes
            for (int32 i = 0; i < Registry.Count(); i++)
            {
                const auto& rtypes = Registry[i].Types;
                if (rtypes.Count() != Archetypes.Count())
                    continue;

                for (int32 j = 0; j < rtypes.Count(); j++)
                {
                    for (int32 k = 0; k < Archetypes.Count(); k++)
                    {
                        //primary filter
                        if (Archetypes[k].Size == rtypes[j].Size &&
                            Archetypes[k].Alignment == rtypes[j].Alignment)
                        {
                            //deep filter
                            if (Archetypes[k].TypeName == rtypes[j].TypeName)
                            {
                                //duplicate check

                                for (int32 l = 0; l < hitCount; l++)
                                {
                                    if (k == OutOrderRemap[l].From)
                                    {
                                        goto Next;
                                    }
                                }
                                OutOrderRemap[hitCount].From = k;
                                OutOrderRemap[hitCount].To = j;
                                hitCount++;
                                if (hitCount == hitFound)
                                {
                                    //done found
                                    OutEntryID = i;
                                    return;
                                }
                                else
                                {
                                    break;
                                }
                            }
                        }
                    Next:
                    }
                }
                hitCount = 0;
            }
            //done we are done this is end of func
            OutOrderRemap.Clear();
        }
        friend class EntityComponentSystem;
    };
private:
    static Array<ComponentDescriptor> descriptors;
    static Dictionary<StringView, ComponentTypeId> Types;
public:
    //internal and bindings use only
    static ComponentTypeId RegisterComponent(uint32 Size, uint32 Alignment, const StringView& TypeName, void (*Construct)(void* memory), void (*Destroy)(void* memory))
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
            descriptor.Destroy = Destroy;
            descriptor.Construct = Construct;
            descriptors.Add(descriptor);
            Types.Add(TypeName, componentTypeId);

            return componentTypeId;
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

            //return -1;
        }
    }
public:
    //Exposed Low level calls don't use it unless u know what are u doing
    //also: API here is volatile can change at anytime between versions of the engine use High Level-API instead
    static class LowLevelCalls
    {
        [[nodiscard]] FORCE_INLINE EntityComponentSystem::Registry::Result CreateEntity(Registry* reg, const EntityComponentSystem::Registry::Entry::Archetypes& Archetypes, Entity& OutEntity)
        {
            return reg->CreateEntity(Archetypes, OutEntity);
        }
        [[nodiscard]] FORCE_INLINE EntityComponentSystem::Registry::Result DestroyEntity(Registry* reg, Entity& OutEntity)
        {
            return reg->DestroyEntity(OutEntity);
        }
        [[nodiscard]] FORCE_INLINE bool HasComponentsOfType(Registry* reg, Entity& entity, ComponentDescriptor descriptor)
        {
            return reg->HasComponentsOfType(entity, descriptor);
        }
        [[nodiscard]] FORCE_INLINE EntityComponentSystem::Registry::Result GetComponentsOfType(Registry* reg, Entity& entity, ComponentDescriptor descriptor, Array<IComponent*>& components)
        {
            return reg->GetComponentsOfType(entity, descriptor, components);
        }
        [[nodiscard]] FORCE_INLINE EntityComponentSystem::Registry::Result AddComponentOfType(Registry* reg, Entity& entity, ComponentDescriptor descriptor, const IComponent* component)
        {
            return reg->AddComponentOfType(entity, descriptor, component);
        }
        [[nodiscard]] FORCE_INLINE EntityComponentSystem::Registry::Result DestroyComponentOfType(Registry* reg, Entity& entity, const IComponent* component)
        {
            return reg->DestroyComponentOfType(entity, component);
        }
    };
};
