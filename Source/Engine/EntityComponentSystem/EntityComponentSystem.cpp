#include "EntityComponentSystem.h"
#include <Engine/Core/Types/BaseTypes.h>
#include <Engine/Core/Collections/Dictionary.h>
#include <Engine/Core/Collections/Array.h>
#include <Engine/Core/Types/StringView.h>
#include <Engine/Core/Templates.h>


Array<EntityComponentSystem::ComponentDescriptor> EntityComponentSystem::descriptors;
Dictionary<StringView, uint32> EntityComponentSystem::Types;

//internal and bindings use only
uint32 EntityComponentSystem::RegisterComponent(uint32 Size, uint32 Alignment, const StringView& TypeName, void(*Construct)(void* memory), void(*Destroy)(void* memory))
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

#pragma region EntityComponentSystemRegistry

EntityComponentSystem::Registry::Registry(const SpawnParams& params)
    : ScriptingObject(params)
{
}

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::CreateEntity(
    const Entry::Archetypes& Archetypes, Entity& OutEntity)
{
    // 1. Find matching Entry and input-to-entry order remap.
    Array<Entry::IndexRemapper> Map = {};
    int EntryID;
    FindArchetypeMatch(Archetypes, EntryID, Map);
    if (EntryID == -1)
    {
        // 2. If absent, create and initialize an Entry.
        const int32 entryIndex = registry.Count();

        Entry entry;
        Result result = entry.Init(Archetypes);
        if (result != Result::Ok)
            return result;

        OutEntity.Generation = entry.GetNextGeneration();
        OutEntity.ID = EntryID;
        registry.Add(MoveTemp(entry));
    }
    else
    {
        // 2. If not absent, set the Generation and ID.
        OutEntity.Generation = registry[EntryID].GetNextGeneration();
        OutEntity.ID = EntryID;
    }

    // 3. Construct each component in the Entry for Entity.
    Entry& entry = registry[OutEntity.ID];
    auto result = entry.EnsureSlots(entry.ActiveRowCount + 1);
    if (result == Result::Ok)
        entry.ConstructComponents(OutEntity.Generation);
    return result;
}

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::DestroyEntity(Entity& entity)
{
    if (registry.IsValidIndex(entity.ID) || entity.Generation == MAX_uint32)
        return EntityComponentSystem::Registry::Result::InvalidEntity;

    EntityComponentSystem::Registry::Entry& entry = registry[entity.ID];
    const uint32 rowIndex = entity.Generation;

    if (!entry.IsRowAlive(rowIndex))
        return EntityComponentSystem::Registry::Result::InvalidEntity;

    entry.DestroyComponents(rowIndex); // reverse component order
    entry.MarkRowDead(rowIndex);

    entity.ID = MAX_uint32;
    entity.Generation = 0;
    return EntityComponentSystem::Registry::Result::Ok;
}

bool EntityComponentSystem::Registry::HasComponentsOfType(Entity& entity, const ComponentDescriptor& descriptor)
{
    Entry& entry = registry[entity.ID];
    //component type fetch
    for (int32 i = 0; i < entry.Types.Count(); i++)
    {
        if (descriptor.TypeID == entry.Types[i].TypeID)
        {
            return true;
        }
    }
    return false;
}

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::GetComponentsOfType(
    Entity& entity, const ComponentDescriptor& descriptor, Array<Component*>& components)
{
    Array<Entry::ComponentData> ComponentData;
    void* dataBlock;
    uint32 dataSize;
    uint8* chunkBytes;
    Entry& entry = registry[entity.ID];
    auto result = entry.GetComponentBlockInfo(entity.Generation, chunkBytes, dataBlock, dataSize);
    if (result != EntityComponentSystem::Registry::Result::Ok)
        return result;
    entry.SplitToComponents(chunkBytes, dataBlock, ComponentData);
    //component type fetch
    for (int32 i = 0; i < entry.Types.Count(); i++)
    {
        if (descriptor.TypeID == entry.Types[i].TypeID)
        {
            components.Add((Component*)ComponentData[i].Data);
        }
    }
    return EntityComponentSystem::Registry::Result::Ok;
}

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::AddComponentOfType(
    Entity& entity, 
    const ComponentDescriptor& descriptor, 
    const Component* component
)
{
    //find new Archetype set
    Entry::Archetypes ar = registry[entity.ID].Types;
    ar.Add(descriptor);

    int32 entryID;
    Array<Entry::IndexRemapper> orderRemap;
    FindArchetypeMatch(ar, entryID, orderRemap);
    if (entryID == -1)
    {
        // 1. If absent, create and initialize an Entry.
        const int32 entryIndex = registry.Count();

        Entry e;
        Result result = e.Init(ar);
        if (result != Result::Ok)
            return result;

        auto oldGeneration = entity.Generation;
        Entry& oldEntry = registry[entity.ID];

        entity.Generation = e.GetNextGeneration();
        entity.ID = registry.Count();
        registry.Add(MoveTemp(e));

        // 2. Construct each component in the Entry for Entity.
        Entry& entry = registry[entity.ID];
        result = entry.EnsureSlots(entry.ActiveRowCount + 1);
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
        auto& curentEntry = registry[entity.ID];
        void* oldDataBlock;
        uint32 oldDataSize;
        uint8* oldChunkBytes;
        auto result = curentEntry.GetComponentBlockInfo(entity.Generation, oldChunkBytes, oldDataBlock, oldDataSize);
        curentEntry.SplitToComponents(oldChunkBytes, oldDataBlock, currentComponentData);
        curentEntry.MarkRowDead(entity.Generation);
        Platform::MemoryClear(oldDataBlock, oldDataSize);

        //2. Get existing registry data and remap entity to new entry
        Array<Entry::ComponentData> componentData;
        auto& entry = registry[entryID];
        void* dataBlock;
        uint32 dataSize;
        uint8* chunkBytes;
        result = entry.EnsureSlots(entry.ActiveRowCount + 1);
        if (result != Result::Ok)
            return result;
        entity.Generation = entry.GetNextGeneration();
        entity.ID = entryID;
        result = entry.GetComponentBlockInfo(entity.Generation, chunkBytes, dataBlock, dataSize);
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

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::DestroyComponentOfType(Entity& entity, const Component* component)
{
    //find new Archetype set
    Entry::Archetypes ar = registry[entity.ID].Types;
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
    auto& oldEntry = registry[entity.ID];
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
        registry.Add(MoveTemp(e));
        entryID = registry.Count();
    }
    Entry& entry = registry[entryID];
    result = entry.EnsureSlots(entry.ActiveRowCount + 1);
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
void EntityComponentSystem::Registry::FindArchetypeMatch(
    const Entry::Archetypes& Archetypes,
    int32& OutEntryID,
    Array<Entry::IndexRemapper>& OutOrderRemap)
{
    OutOrderRemap.Resize(Archetypes.Count(), false);
    OutEntryID = -1;
    int32 hitFound = Archetypes.Count();
    int32 hitCount = 0;

    //scan Registry for Archetypes
    for (int32 i = 0; i < registry.Count(); i++)
    {
        const auto& rtypes = registry[i].Types;
        if (rtypes.Count() != Archetypes.Count())
            continue;

        Next:
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
            }
        }
        hitCount = 0;
    }
    //done we are done this is end of func
    OutOrderRemap.Clear();
}
#pragma endregion

#pragma region EntityComponentSystemRegistryEntry
EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::Entry::Init(Archetypes Types)
{
    if (Data != nullptr)
        return Result::DoubleInitCall;
    if (Types.IsEmpty())
        return Result::EmptyArchetype;

    //validate the Types is not over the size limit
    //compute RowStride
    uint32 size = 0;
    for (int32 i = 0; i < Types.Count(); i++)
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

EntityComponentSystem::Registry::Result EntityComponentSystem::Registry::Entry::EnsureSlots(uint64 count)
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

void EntityComponentSystem::Registry::Entry::ConstructComponents(uint32 rowIndex)
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

void EntityComponentSystem::Registry::Entry::DestroyComponents(uint32 rowIndex)
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
#pragma endregion

EntityComponentSystem::Registry::Result EntityComponentSystem::LowLevelCalls::GetComponentsOfType(
    EntityComponentSystem::Registry& reg,
    Entity& entity,
    void* descriptor,
    Array<void*>& components)
{
    Array<Component*> ptrdata;
    ptrdata.Resize(components.Count(), false);
    for (int32 i = 0; i < components.Count(); i++)
    {
        ptrdata[i] = (Component*)components[i];
    }
    return reg.GetComponentsOfType(entity, *(EntityComponentSystem::ComponentDescriptor*)descriptor, ptrdata);
}

//EntityComponentSystem::Registry::Result EntityComponentSystem::LowLevelCalls::GetComponentDescriptor(const StringView& componentTypeName,void*& OutComponentDescriptor)
//{
//    if (uint32* index = EntityComponentSystem::Types.TryGet(componentTypeName))
//    {
//        //this is unsafe af binding bs
//        OutComponentDescriptor = &descriptors[*index];
//        return EntityComponentSystem::Registry::Result::Ok;
//    }
//    OutComponentDescriptor = nullptr;
//    return EntityComponentSystem::Registry::Result::NotFound;
//}

EntityComponentSystem::Registry::Result
EntityComponentSystem::LowLevelCalls::CreateEntity
(
    EntityComponentSystem::Registry& reg,
    const Array<void*>& Archetypes,
    Entity& OutEntity)
{
    Array<ComponentDescriptor> ptrdata;
    ptrdata.Resize(Archetypes.Count(), false);
    for (int32 i = 0; i < Archetypes.Count(); i++)
    {
        ptrdata[i] = *(ComponentDescriptor*)Archetypes[i];
    }
    return reg.CreateEntity(ptrdata, OutEntity);
}
EntityComponentSystem::Registry::Result
EntityComponentSystem::LowLevelCalls::DestroyEntity
(
    EntityComponentSystem::Registry& reg,
    Entity& OutEntity
)
{
    return reg.DestroyEntity(OutEntity);
}
bool
EntityComponentSystem::LowLevelCalls::HasComponentsOfType
(
    EntityComponentSystem::Registry& reg,
    API_PARAM(Ref)Entity& entity,
    void* descriptor
)
{
    //if crashes here u don't know what are u doing descriptor must be not null and the ComponentDescriptor* not garbage
    return reg.HasComponentsOfType(entity, *(EntityComponentSystem::ComponentDescriptor*)descriptor);
}
EntityComponentSystem::Registry::Result
EntityComponentSystem::LowLevelCalls::AddComponentOfType
(
    EntityComponentSystem::Registry& reg,
    API_PARAM(Ref)Entity& entity,
    void* descriptor,
    void* component)
{
    //if crashes here u don't know what are u doing descriptor must be not null and the ComponentDescriptor* not garbage
    return reg.AddComponentOfType(entity, *(EntityComponentSystem::ComponentDescriptor*)descriptor, (Component*) component);
}
EntityComponentSystem::Registry::Result
EntityComponentSystem::LowLevelCalls::DestroyComponentOfType(
    EntityComponentSystem::Registry& reg,
    Entity& entity,
    void* component)
{
    return reg.DestroyComponentOfType(entity, (Component*)component);
}
