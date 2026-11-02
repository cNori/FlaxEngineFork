// Copyright (c) Wojciech Figat. All rights reserved.
#include "NativeReflection.h"
#include <mutex>
#include "Engine/Core/Collections/Dictionary.h"
#include <cstring>

namespace NativeReflection
{
    namespace
    {
        struct Registry { std::mutex Mutex; Registration* Head = nullptr; };
        Registry& GetRegistry() { static Registry value; return value; }
    }
    Registration::Registration(const Type* type) : Descriptor(type)
    {
        auto& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        Next = registry.Head; registry.Head = this;
    }
    Registration::~Registration()
    {
        auto& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        auto** position = &registry.Head;
        while (*position && *position != this) position = &(*position)->Next;
        if (*position) *position = Next;
    }
    const Type* FindType(const char* name)
    {
        if (!name) return nullptr;
        auto& registry = GetRegistry();
        std::lock_guard<std::mutex> lock(registry.Mutex);
        const Type* result = nullptr;
        for (auto* entry = registry.Head; entry; entry = entry->Next)
        {
            if (std::strcmp(entry->Descriptor->Name, name) != 0) continue;
            // Distinct native/anonymous types can share a spelling. Never pick
            // a layout/accessor set arbitrarily for an untyped subtree address.
            if (result && result != entry->Descriptor) return nullptr;
            result = entry->Descriptor;
        }
        return result;
    }
    Status ReadSubtree(const void* instance, const char* typeName, Variant& output)
    {
        auto* type = FindType(typeName);
        if (!type) return Status::UnsupportedValue;
        if (!instance) return Status::NullInstance;
        static thread_local unsigned depth = 0;
        if (depth >= 32) return Status::OutOfRange;
        struct Depth { unsigned& Value; Depth(unsigned& value) : Value(value) { ++Value; } ~Depth() { --Value; } } guard(depth);
        auto fields = ReadFields(*type, instance);
        Array<Variant> records; records.Resize(static_cast<int>(fields.size()));
        for (int index = 0; index < records.Count(); ++index)
        {
            const auto& field = fields[index];
            Dictionary<Variant, Variant> record;
            record.Add(Variant("Variant"), field.Data);
            record.Add(Variant("TypeName"), Variant(field.TypeName));
            record.Add(Variant("Name"), Variant(field.Name));
            record.Add(Variant("ReadStatus"), Variant(static_cast<int32>(field.ReadStatus)));
            records[index] = Variant(std::move(record));
        }
        output = Variant(std::move(records)); return Status::Ok;
    }
    std::vector<Value> ReadFields(const Type& type, const void* instance)
    {
        std::vector<Value> result;
        result.reserve(type.FieldCount);
        for (unsigned i = 0; i < type.FieldCount; i++)
        {
            const auto& field = type.Fields[i];
            Value value{ Variant(), field.TypeName, field.Name, Status::Unavailable };
            if (field.Get) value.ReadStatus = field.Get(instance, value.Data);
            result.emplace_back(std::move(value));
        }
        return result;
    }
}
