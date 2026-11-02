// Copyright (c) Wojciech Figat. All rights reserved.
#pragma once
#include "Engine/Core/Types/Variant.h"
#include "Engine/Core/Collections/Array.h"
#include <type_traits>
#include <utility>
#include <limits>
#include <memory>
#include <vector>
#include <array>

template<typename T> struct VETReflectionAccess;

namespace NativeReflection
{
    enum class Status { Ok, NullInstance, Unavailable, UnsupportedValue, TypeMismatch, OutOfRange };
    enum class Access { Public, Protected, Private };
    enum class TypeKind { Class, Struct, Interface, Enum, Template };
    struct Type;
    struct Field
    {
        const char* TypeName;
        const char* Name;
        Access Visibility;
        bool IsStatic;
        bool IsReadOnly;
        bool IsUnionMember;
        Status (*Get)(const void*, Variant&);
        Status (*Set)(void*, const Variant&);
        const void* (*GetSubtreeInstance)(const void*);
        const Type* (*GetSubtreeType)();
        void* (*GetMutableSubtreeInstance)(void*);
        bool IsSerializable = true;
        bool IsSequence = false;
        Status (*GetValue)(const void*, Variant&) = nullptr;
    };
    struct Base
    {
        const char* TypeName;
        Access Visibility;
        const Type* (*GetType)();
        const void* (*GetInstance)(const void*);
        void* (*GetMutableInstance)(void*);
    };
    struct Type
    {
        const char* Name;
        const Field* Fields;
        unsigned FieldCount;
        const Type* const* Children;
        unsigned ChildCount;
        const Base* Bases = nullptr;
        unsigned BaseCount = 0;
        TypeKind Kind = TypeKind::Struct;
    };
    // A value owns its Variant; names refer to the generated descriptor's storage.
    struct Value
    {
        Variant Data;
        const char* TypeName;
        const char* Name;
        Status ReadStatus;
    };
    class FLAXENGINE_API Registration
    {
    public:
        explicit Registration(const Type* type);
        ~Registration();
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        const Type* Descriptor;
        Registration* Next = nullptr;
    };
    FLAXENGINE_API const Type* FindType(const char* name);
    FLAXENGINE_API std::vector<Value> ReadFields(const Type& type, const void* instance);
    FLAXENGINE_API Status ReadSubtree(const void* instance, const char* typeName, Variant& output);
    template<typename T> const Type* TypeOf() { return VETReflectionAccess<typename std::remove_cv<T>::type>::GetType(); }
    template<typename T> std::vector<Value> ReadFields(const T& instance) { return ReadFields(*TypeOf<T>(), std::addressof(instance)); }


    template<typename T> using ScalarStorage = typename std::conditional<std::is_same<T, bool>::value, bool,
        typename std::conditional<std::is_same<T, float>::value, float,
        typename std::conditional<std::is_floating_point<T>::value, double,
        typename std::conditional<sizeof(T) == 2, typename std::conditional<std::is_signed<T>::value, int16, uint16>::type,
        typename std::conditional<(sizeof(T) <= 4), typename std::conditional<std::is_signed<T>::value || sizeof(T) == 1, int32, uint32>::type,
        typename std::conditional<std::is_signed<T>::value, int64, uint64>::type>::type>::type>::type>::type>::type;
    template<typename T> struct DirectValue : std::integral_constant<bool,
        std::is_arithmetic<T>::value ||
        std::is_same<T, String>::value || std::is_same<T, StringAnsi>::value || std::is_same<T, StringView>::value || std::is_same<T, StringAnsiView>::value ||
        std::is_same<T, Guid>::value || std::is_same<T, Float2>::value || std::is_same<T, Float3>::value || std::is_same<T, Float4>::value ||
        std::is_same<T, Double2>::value || std::is_same<T, Double3>::value || std::is_same<T, Double4>::value ||
        std::is_same<T, Int2>::value || std::is_same<T, Int3>::value || std::is_same<T, Int4>::value ||
        std::is_same<T, Color>::value || std::is_same<T, Quaternion>::value || std::is_same<T, Transform>::value ||
        std::is_same<T, BoundingBox>::value || std::is_same<T, BoundingSphere>::value || std::is_same<T, Rectangle>::value ||
        std::is_same<T, Ray>::value || std::is_same<T, Matrix>::value || std::is_same<T, Variant>::value> {};
    template<typename...> using Void = void;
    template<typename T, typename = void> struct Conversion
    {
        static Status Read(const T&, Variant&) { return Status::UnsupportedValue; }
        static Status Write(T&, const Variant&) { return Status::UnsupportedValue; }
    };
    template<typename T> struct Conversion<T, Void<typename std::enable_if<DirectValue<T>::value>::type, decltype(Variant(std::declval<const T&>()))>>
    {
        static Status Read(const T& value, Variant& output) { output = Variant(value); return Status::Ok; }
        template<typename U> static auto Assign(U& target, const Variant& value, int) -> decltype(target = static_cast<U>(value), Status())
        {
            if (Variant(target).Type.Type != value.Type.Type) return Status::TypeMismatch;
            target = static_cast<U>(value); return Status::Ok;
        }
        template<typename U> static Status Assign(U&, const Variant&, long) { return Status::UnsupportedValue; }
        template<typename U> static Status SafeAssign(U& target, const Variant& value, std::true_type) { return Assign(target, value, 0); }
        template<typename U> static Status SafeAssign(U&, const Variant&, std::false_type) { return Status::UnsupportedValue; }
        static Status Write(T& target, const Variant& value)
        {
            // Variant string views borrow its storage; assigning them would leave
            // a dangling view when this call's temporary Variant is destroyed.
            return SafeAssign(target, value, std::integral_constant<bool, !std::is_pointer<T>::value && !std::is_same<T, StringView>::value && !std::is_same<T, StringAnsiView>::value>());
        }
    };
    template<> struct Conversion<Variant, void>
    {
        static Status Read(const Variant& value, Variant& output) { output = value; return Status::Ok; }
        static Status Write(Variant& target, const Variant& value) { target = value; return Status::Ok; }
    };
    template<> struct Conversion<StringAnsi, void>
    {
        static Status Read(const StringAnsi& value, Variant& output) { output = Variant(StringAnsiView(value)); return Status::Ok; }
        static Status Write(StringAnsi& target, const Variant& value)
        {
            if (value.Type.Type != VariantType::String) return Status::TypeMismatch;
            target = StringAnsi(static_cast<StringView>(value)); return Status::Ok;
        }
    };
    template<> struct Conversion<String, void>
    {
        static Status Read(const String& value, Variant& output) { output = Variant(StringView(value)); return Status::Ok; }
        static Status Write(String& target, const Variant& value)
        {
            if (value.Type.Type != VariantType::String) return Status::TypeMismatch;
            target = String(static_cast<StringView>(value)); return Status::Ok;
        }
    };
    template<typename T> Status ReadPointer(T* value, Variant& output, std::true_type) { output = Variant(value); return Status::Ok; }
    template<typename T> Status ReadPointer(T* value, Variant& output, std::false_type) { output = Variant(const_cast<void*>(static_cast<const volatile void*>(value))); return Status::Ok; }
    template<typename T> typename std::enable_if<!std::is_function<T>::value, Status>::type ReadValue(T* value, Variant& output)
    { return ReadPointer(value, output, std::is_convertible<T*, ScriptingObject*>()); }
    template<typename T> typename std::enable_if<std::is_function<T>::value, Status>::type ReadValue(T*, Variant&) { return Status::UnsupportedValue; }
    template<typename T> typename std::enable_if<std::is_arithmetic<T>::value, Status>::type ReadValue(const T& value, Variant& output)
    {
        if (std::is_same<T, long double>::value) return Status::UnsupportedValue;
        output = Variant(static_cast<ScalarStorage<T>>(value)); return Status::Ok;
    }
    template<typename T> typename std::enable_if<!std::is_enum<T>::value && !std::is_arithmetic<T>::value, Status>::type ReadValue(const T& value, Variant& output)
    { return Conversion<T>::Read(value, output); }
    template<typename T> typename std::enable_if<std::is_enum<T>::value, Status>::type ReadValue(const T& value, Variant& output)
    { return ReadValue(static_cast<typename std::underlying_type<T>::type>(value), output); }
    template<typename T> Status WriteNumber(T& target, const Variant& value, std::true_type)
    {
        if (std::is_signed<T>::value)
        {
            auto number = static_cast<int64>(value);
            if (number < static_cast<int64>((std::numeric_limits<T>::min)()) || number > static_cast<int64>((std::numeric_limits<T>::max)())) return Status::OutOfRange;
            target = static_cast<T>(number);
        }
        else
        {
            auto number = static_cast<uint64>(value);
            if (number > static_cast<uint64>((std::numeric_limits<T>::max)())) return Status::OutOfRange;
            target = static_cast<T>(number);
        }
        return Status::Ok;
    }
    template<typename T> Status WriteNumber(T& target, const Variant& value, std::false_type)
    { target = static_cast<T>(static_cast<double>(value)); return Status::Ok; }
    template<typename T> typename std::enable_if<std::is_arithmetic<T>::value && !std::is_const<T>::value, Status>::type WriteValue(T& target, const Variant& value)
    {
        if (std::is_same<T, long double>::value) return Status::UnsupportedValue;
        if (Variant(ScalarStorage<T>{}).Type.Type != value.Type.Type) return Status::TypeMismatch;
        return WriteNumber(target, value, std::is_integral<T>());
    }
    template<typename T> typename std::enable_if<!std::is_arithmetic<T>::value && !std::is_enum<T>::value && !std::is_const<T>::value, Status>::type WriteValue(T& target, const Variant& value)
    { return Conversion<T>::Write(target, value); }
    template<typename T> typename std::enable_if<std::is_enum<T>::value && !std::is_const<T>::value, Status>::type WriteValue(T& target, const Variant& value)
    {
        auto number = static_cast<typename std::underlying_type<T>::type>(target);
        auto status = WriteValue(number, value);
        if (status == Status::Ok) target = static_cast<T>(number);
        return status;
    }
    template<typename T> Status WriteValue(const T&, const Variant&) { return Status::Unavailable; }
    template<typename T> typename std::enable_if<!std::is_enum<T>::value, bool>::type FitsBits(T value, unsigned width)
    {
        if (!width) return false;
        if (width >= sizeof(T) * 8) return true;
        if (std::is_signed<T>::value)
        {
            auto maximum = (int64(1) << (width - 1)) - 1;
            return static_cast<int64>(value) >= -maximum - 1 && static_cast<int64>(value) <= maximum;
        }
        return static_cast<uint64>(value) <= (uint64(1) << width) - 1;
    }
    template<typename T> typename std::enable_if<std::is_enum<T>::value, bool>::type FitsBits(T value, unsigned width)
    { return FitsBits(static_cast<typename std::underlying_type<T>::type>(value), width); }
    template<typename Getter> Status ReadSequence(int count, Getter get, const char* elementType, Variant& output)
    {
        if (count < 0 || count > 65536) return Status::OutOfRange;
        Array<Variant> values; values.Resize(count);
        for (int index = 0; index < count; ++index)
        {
            auto status = ReadValue(get(index), values[index]);
            if (status == Status::UnsupportedValue) status = ReadSubtree(std::addressof(get(index)), elementType, values[index]);
            if (status != Status::Ok) return status;
        }
        output = Variant(std::move(values)); return Status::Ok;
    }
    template<typename T, size_t N> typename std::enable_if<DirectValue<T>::value || std::is_enum<T>::value, Status>::type WriteSequence(T (&target)[N], const Variant& value)
    {
        if (N > 65536) return Status::OutOfRange;
        if (value.Type.Type != VariantType::Array || value.AsArray().Count() != static_cast<int>(N)) return Status::TypeMismatch;
        std::unique_ptr<T[]> temporary(new T[N]);
        for (size_t i = 0; i < N; i++) { temporary[i] = target[i]; auto status = WriteValue(temporary[i], value.AsArray()[static_cast<int>(i)]); if (status != Status::Ok) return status; }
        for (size_t i = 0; i < N; i++) target[i] = std::move(temporary[i]);
        return Status::Ok;
    }
    template<typename T, size_t N> typename std::enable_if<!DirectValue<T>::value && !std::is_enum<T>::value, Status>::type WriteSequence(T (&)[N], const Variant&)
    { return Status::UnsupportedValue; }
    template<typename T, typename Allocator> typename std::enable_if<DirectValue<T>::value || std::is_enum<T>::value, Status>::type WriteSequence(Array<T, Allocator>& target, const Variant& value)
    {
        if (value.Type.Type != VariantType::Array) return Status::TypeMismatch;
        const auto& values = value.AsArray(); if (values.Count() < 0 || values.Count() > 65536) return Status::OutOfRange;
        Array<T, Allocator> temporary; temporary.Resize(values.Count());
        for (int i = 0; i < values.Count(); i++) { temporary[i] = T{}; auto status = WriteValue(temporary[i], values[i]); if (status != Status::Ok) return status; }
        target = std::move(temporary); return Status::Ok;
    }
    template<typename T, typename Allocator> typename std::enable_if<!DirectValue<T>::value && !std::is_enum<T>::value, Status>::type WriteSequence(Array<T, Allocator>&, const Variant&)
    { return Status::UnsupportedValue; }
    template<typename T, size_t N> Status ReadSequence(const T (&values)[N], const char* elementType, Variant& output)
    { return ReadSequence(static_cast<int>(N), [&](int i) -> const T& { return values[i]; }, elementType, output); }
    template<typename T, typename Allocator> Status ReadSequence(const Array<T, Allocator>& values, const char* elementType, Variant& output)
    { return ReadSequence(values.Count(), [&](int i) -> const T& { return values[i]; }, elementType, output); }

}
