# Generated native reflection

The V2 build emits reflection by default. Use `-nativeReflection=false` to disable it. Headers are mirrored beneath each module's `BindingsV2/Reflection` cache directory as `Name.gen.h`. Companion registration sources have a path-derived identifier in their filename to avoid object-file collisions between headers with the same name.

Standalone generation:

```text
VETools <header-or-folder> --emit-reflection <output-folder> --include-dir <source-root> --no-pause
```

Include the generated header when using `NativeReflection::TypeOf<T>()` or `NativeReflection::ReadFields(instance)`. Compile its companion source to register descriptors for lookup through `NativeReflection::FindType("Namespace::Type")`.

Each type describes its declared fields, lexical nested types (`Children`), and base types (`Bases`). Fields carry their native type spelling, name, visibility, read/write callbacks, and subtree callbacks. Base callbacks perform C++ pointer adjustment. Field values own their `Variant`; check `ReadStatus` before using a value. Inheritance traversal is explicit rather than flattened.

All declared fields are indexed independently of export annotations, except `API_EVENT`. Scripting declaration macros grant the generated friend access automatically. Other C++ types can declare `friend struct ::VETReflectionAccess<MyType>;`. Inaccessible fields remain in the metadata without callable getters. Private and protected fields expose no setter or mutable subtree pointer. Public writable fields use checked converters; unsupported values report a status rather than assuming a storage layout.

Pointers are opaque values, not automatically followed. Union members retain metadata without guessing the active member. View types have no default setter, avoiding references into temporary Variant storage. Static template declarations without a definition/initializer retain metadata without creating linker dependencies. Empty concrete templates retain metadata without instantiating unrelated argument headers. Template prototypes have metadata only; concrete instances discovered by type analysis receive accessors where supported.

This supplies the native inspection/runtime layer. Editor debug UI is not wired up yet. Callers must keep native objects alive and synchronize access while invoking callbacks. Engine persistence uses the separate typed auto-serialization generator described below.

## Native snapshots

Include `Engine/Scripting/NativeReflectionSerialization.h` and the generated header for your type:

```cpp
Variant document;
auto saved = NativeReflection::Capture(object, document);
if (saved.Succeeded())
{
    auto restored = NativeReflection::Restore(object, document);
    // Check restored.Succeeded() and restored.Issues before accepting the result.
}
```

The owning document is a Variant dictionary: `Format = 1`, `TypeName`, and `Fields`. Each field record contains `Name`, `TypeName`, `Kind`, and `Value`. Kind 0 holds a copied value, kind 1 a nested object document, and kind 2 a base-subobject document. Lexical nested declarations alone are not instance data. Bases remain separate to preserve hidden field names and native pointer adjustment.

Capture includes writable public instance fields. It excludes private/protected, readonly, static, union, and `NoSerialize` fields. These remain available for reflection/debug inspection. Raw addresses, runtime object handles, and unsupported Variant types cannot be persisted. Collection copying uses the existing checked converters; aggregate-element collection restoration still requires a converter. Snapshots are not a replacement for lifetime-aware asset/object reference serialization.

Restore checks document structure, duplicate records, format version, bounded depth/node count, and unsafe Variant payloads before writes. Names and native type spellings must match. Missing or unknown fields are left alone, allowing added/removed fields. Private/readonly/NoSerialize members cannot be enabled by a fabricated document. Converter errors include a full field/base path. Writes are checked per field; restore is not a transaction across the entire object, and an earlier successful field write is not undone by a later conversion failure.

Capture only publishes its output when successful. `Applied` counts copied leaf values, `Skipped` counts excluded/unmatched members, and `Issues` records failures. The native caller is responsible for object lifetime and synchronization. This is the in-memory snapshot document layer; it does not replace the engine persistence format or manual serializers.


## Typed API_AUTO_SERIALIZATION generation

V2 emits `Serialize`, `Deserialize`, and `ShouldSerialize` definitions into each header's native `BindingsV2/<Name>.<id>.g.cpp`, alongside the binding glue. `API_AUTO_SERIALIZATION()` retains the declarations in the original type. Legacy mode retains its original generator; V2 disables the retained legacy copies to prevent duplicate definitions.

The methods call typed `Serialization::Serialize`, `Serialization::Deserialize`, and `Serialization::ShouldSerialize` overloads, retaining manual serializers and the original JSON member names. They do not convert engine persistence through the Variant snapshot schema. Supported base serializers run first, with the original scripting-root exclusions.

API fields are public by default or explicitly opted in with the `Serialize` attribute. Static/constexpr, `NoSerialize`, `NonSerialized`, and interface-valued members are excluded. Classes also serialize API properties with getter/setter pairs. Obsolete properties are accepted on input but omitted on output. Private opted-in fields remain accessible inside the generated member definitions; this does not expose a public reflection setter.

The original pre/post callbacks receive `CallbackContext` with a null modifier during serialization and the supplied modifier during deserialization. Invalid callback signatures are generation errors. Differential writes compare against `otherObj`. Object-pointer and bitfield helper macros retain Flax's existing behavior; bitfield change detection compares values without taking an illegal bitfield address. Open templates require an explicit concrete serialization handler.
