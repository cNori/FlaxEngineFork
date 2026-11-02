// Copyright (c) Wojciech Figat. All rights reserved.
#include "NativeReflectionSerialization.h"
#include "Engine/Core/Collections/Dictionary.h"
#include <set>
#include <cstring>

namespace NativeReflection
{
    namespace
    {
        constexpr unsigned MaxDepth = 32;
        constexpr unsigned MaxNodes = 65536;
        const Variant* Member(const Variant& object, const char* key)
        {
            if (object.Type.Type != VariantType::Dictionary || !object.AsDictionary || object.AsDictionary->Count() > 16) return nullptr;
            const Variant name(key);
            const Variant* result = nullptr;
            for (const auto& pair : *object.AsDictionary)
                if (pair.Key == name) { if (result) return nullptr; result = &pair.Value; }
            return result;
        }
        bool Text(const Variant* value, const char* expected)
        { return value && value->Type.Type == VariantType::String && *value == Variant(expected); }
        std::string Text(const Variant& value)
        {
            auto text = static_cast<StringView>(value);
            std::string result;
            if (text.Length() < 1 || text.Length() > 4096) return result;
            // Native declaration names are ASCII; reject, rather than truncate, other spellings.
            for (int i = 0; i < text.Length(); ++i)
            {
                if (text.Get()[i] == 0 || text.Get()[i] > 127) return {};
                result.push_back(static_cast<char>(text.Get()[i]));
            }
            return result;
        }
        bool SafeValue(const Variant& value, unsigned depth, unsigned& budget)
        {
            if (depth >= MaxDepth || budget == 0) return false;
            --budget;
            switch (value.Type.Type)
            {
            case VariantType::Null: case VariantType::Bool:
            case VariantType::Int16: case VariantType::Uint16:
            case VariantType::Int: case VariantType::Uint:
            case VariantType::Int64: case VariantType::Uint64:
            case VariantType::Float: case VariantType::Double:
            case VariantType::String: case VariantType::Blob: case VariantType::Guid:
            case VariantType::Float2: case VariantType::Float3: case VariantType::Float4:
            case VariantType::Double2: case VariantType::Double3: case VariantType::Double4:
            case VariantType::Int2: case VariantType::Int3: case VariantType::Int4:
            case VariantType::Color: case VariantType::Quaternion: case VariantType::Transform:
            case VariantType::BoundingBox: case VariantType::BoundingSphere:
            case VariantType::Rectangle: case VariantType::Ray: case VariantType::Matrix:
                return true;
            case VariantType::Array:
                if (value.AsArray().Count() < 0 || static_cast<unsigned>(value.AsArray().Count()) > budget) return false;
                for (int i = 0; i < value.AsArray().Count(); ++i)
                    if (!SafeValue(value.AsArray()[i], depth + 1, budget)) return false;
                return true;
            case VariantType::Dictionary:
                if (!value.AsDictionary || value.AsDictionary->Count() < 0 || static_cast<unsigned>(value.AsDictionary->Count()) > budget / 2) return false;
                for (const auto& pair : *value.AsDictionary)
                    if (!SafeValue(pair.Key, depth + 1, budget) || !SafeValue(pair.Value, depth + 1, budget)) return false;
                return true;
            default: return false;
            }
        }
        bool Eligible(const Field& field)
        {
            return field.Visibility == Access::Public && !field.IsStatic && !field.IsReadOnly &&
                !field.IsUnionMember && field.IsSerializable && field.Get && field.Set;
        }
        void Issue(SnapshotResult& result, const std::string& path, Status status)
        { result.Issues.push_back({path, status}); }
        Variant Record(const char* name, const char* type, int kind, const Variant& value)
        {
            Dictionary<Variant, Variant> record;
            record.Add(Variant("Name"), Variant(name));
            record.Add(Variant("TypeName"), Variant(type));
            record.Add(Variant("Kind"), Variant(static_cast<int32>(kind)));
            record.Add(Variant("Value"), value);
            return Variant(std::move(record));
        }
        void CaptureObject(const Type& type, const void* instance, Variant& output,
            SnapshotResult& result, const std::string& path, unsigned depth, unsigned& budget)
        {
            if (!instance) { Issue(result, path, Status::NullInstance); return; }
            if (depth >= MaxDepth || budget == 0) { Issue(result, path, Status::OutOfRange); return; }
            --budget;
            std::vector<Variant> records;
            for (unsigned i = 0; i < type.FieldCount; ++i)
            {
                const auto& field = type.Fields[i];
                if (!Eligible(field)) { ++result.Skipped; continue; }
                auto childPath = path + "." + field.Name;
                if (budget == 0) { Issue(result, childPath, Status::OutOfRange); break; }
                --budget;
                Variant value;
                int kind = 0;
                const Type* subtree = field.GetSubtreeType ? field.GetSubtreeType() : nullptr;
                // Sequence address is not an element address. Arrays use their copying converter.
                bool sequence = field.IsSequence;
                auto status = field.GetValue ? field.GetValue(instance, value) : field.Get(instance, value);
                if (status == Status::UnsupportedValue && subtree && subtree->Kind != TypeKind::Enum && !sequence && field.GetMutableSubtreeInstance && field.GetSubtreeInstance)
                {
                    kind = 1;
                    CaptureObject(*subtree, field.GetSubtreeInstance(instance), value, result, childPath, depth + 1, budget);
                }
                else
                {
                    if (status != Status::Ok) { Issue(result, childPath, status); continue; }
                    if (!SafeValue(value, depth + 1, budget)) { Issue(result, childPath, Status::UnsupportedValue); continue; }
                    ++result.Applied;
                }
                records.emplace_back(Record(field.Name, field.TypeName, kind, value));
            }
            for (unsigned i = 0; i < type.BaseCount; ++i)
            {
                const auto& base = type.Bases[i];
                if (base.Visibility != Access::Public || !base.GetType || !base.GetInstance || !base.GetMutableInstance) { ++result.Skipped; continue; }
                const auto* baseType = base.GetType();
                if (!baseType) { Issue(result, path + "::" + base.TypeName, Status::Unavailable); continue; }
                if (budget == 0) { Issue(result, path, Status::OutOfRange); break; }
                --budget;
                Variant value;
                CaptureObject(*baseType, base.GetInstance(instance), value, result, path + "::" + base.TypeName, depth + 1, budget);
                records.emplace_back(Record(base.TypeName, base.TypeName, 2, value));
            }
            Array<Variant> fields; fields.Resize(static_cast<int32>(records.size()));
            for (int i = 0; i < fields.Count(); ++i) fields[i] = std::move(records[i]);
            Dictionary<Variant, Variant> object;
            object.Add(Variant("Format"), Variant(int32(1)));
            object.Add(Variant("TypeName"), Variant(type.Name));
            object.Add(Variant("Fields"), Variant(std::move(fields)));
            output = Variant(std::move(object));
        }
        bool Validate(const Variant& input, unsigned depth, unsigned& budget)
        {
            if (depth >= MaxDepth || budget == 0) return false;
            --budget;
            const auto* format = Member(input, "Format");
            const auto* type = Member(input, "TypeName");
            const auto* fields = Member(input, "Fields");
            if (!format || format->Type.Type != VariantType::Int || format->AsInt != 1 ||
                !type || type->Type.Type != VariantType::String || Text(*type).empty() ||
                !fields || fields->Type.Type != VariantType::Array || fields->AsArray().Count() < 0 ||
                static_cast<unsigned>(fields->AsArray().Count()) > budget) return false;
            std::set<std::pair<int, std::string>> names;
            for (int i = 0; i < fields->AsArray().Count(); ++i)
            {
                if (budget == 0) return false;
                --budget;
                const auto& record = fields->AsArray()[i];
                const auto* name = Member(record, "Name");
                const auto* fieldType = Member(record, "TypeName");
                const auto* kind = Member(record, "Kind");
                const auto* value = Member(record, "Value");
                if (!name || name->Type.Type != VariantType::String || Text(*name).empty() ||
                    !fieldType || fieldType->Type.Type != VariantType::String || Text(*fieldType).empty() ||
                    !kind || kind->Type.Type != VariantType::Int || kind->AsInt < 0 || kind->AsInt > 2 || !value ||
                    !names.emplace(kind->AsInt == 2 ? 2 : 0, Text(*name)).second) return false;
                if (kind->AsInt == 0 ? !SafeValue(*value, depth + 1, budget) : !Validate(*value, depth + 1, budget)) return false;
            }
            return true;
        }
        void RestoreObject(const Type& type, void* instance, const Variant& input,
            SnapshotResult& result, const std::string& path)
        {
            if (!instance) { Issue(result, path, Status::NullInstance); return; }
            if (!Text(Member(input, "TypeName"), type.Name)) { Issue(result, path, Status::TypeMismatch); return; }
            const auto& records = Member(input, "Fields")->AsArray();
            for (int i = 0; i < records.Count(); ++i)
            {
                const auto& record = records[i];
                auto name = Text(*Member(record, "Name"));
                auto* value = Member(record, "Value");
                int kind = Member(record, "Kind")->AsInt;
                if (kind == 2)
                {
                    const Base* base = nullptr;
                    for (unsigned j = 0; j < type.BaseCount; ++j) if (name == type.Bases[j].TypeName) { base = &type.Bases[j]; break; }
                    if (!base || base->Visibility != Access::Public || !base->GetMutableInstance || !base->GetType) { ++result.Skipped; continue; }
                    const Type* baseType = base->GetType();
                    if (!Text(Member(record, "TypeName"), base->TypeName)) { Issue(result, path + "::" + name, Status::TypeMismatch); continue; }
                    if (!baseType) { Issue(result, path + "::" + name, Status::Unavailable); continue; }
                    RestoreObject(*baseType, base->GetMutableInstance(instance), *value, result, path + "::" + name);
                    continue;
                }
                const Field* field = nullptr;
                for (unsigned j = 0; j < type.FieldCount; ++j) if (name == type.Fields[j].Name) { field = &type.Fields[j]; break; }
                if (!field || !Eligible(*field)) { ++result.Skipped; continue; }
                auto childPath = path + "." + name;
                if (!Text(Member(record, "TypeName"), field->TypeName)) { Issue(result, childPath, Status::TypeMismatch); continue; }
                if (kind == 1)
                {
                    const Type* nested = field->GetSubtreeType ? field->GetSubtreeType() : nullptr;
                    if (!nested || !field->GetMutableSubtreeInstance) { Issue(result, childPath, Status::UnsupportedValue); continue; }
                    bool sequence = field->IsSequence;
                    if (sequence || nested->Kind == TypeKind::Enum) { Issue(result, childPath, Status::TypeMismatch); continue; }
                    RestoreObject(*nested, field->GetMutableSubtreeInstance(instance), *value, result, childPath);
                }
                else
                {
                    auto status = field->Set(instance, *value);
                    if (status == Status::Ok) ++result.Applied;
                    else Issue(result, childPath, status);
                }
            }
        }
    }
    SnapshotResult Capture(const Type& type, const void* instance, Variant& output)
    {
        SnapshotResult result;
        unsigned budget = MaxNodes;
        Variant document;
        CaptureObject(type, instance, document, result, type.Name, 0, budget);
        if (result.Succeeded()) output = std::move(document);
        return result;
    }
    SnapshotResult Restore(const Type& type, void* instance, const Variant& input)
    {
        SnapshotResult result;
        unsigned budget = MaxNodes;
        if (!Validate(input, 0, budget)) { Issue(result, type.Name, Status::TypeMismatch); return result; }
        RestoreObject(type, instance, input, result, type.Name);
        return result;
    }
}
