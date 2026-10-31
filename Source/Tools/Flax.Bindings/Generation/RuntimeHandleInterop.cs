using VETools.Text;

namespace VETools.Generation;

// Host-managed handles, not native object addresses. Ownership follows Flax's
// existing cached-type and pooled/owned-array marshallers.
internal static class RuntimeHandleInterop
{
    internal enum HandleKind { Array, Type }
    public static HandleKind? Kind(TypeInfo? type)
    {
        if (type is not PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo named } ||
            (named.EffectiveQualifiers & TypeQualifiers.Volatile) != 0) return null;
        return NameKind(named);
    }
    public static bool Contains(TypeInfo? type) => type switch
    {
        NamedTypeInfo named => NameKind(named) != null,
        PointerTypeInfo pointer => Contains(pointer.Pointee),
        ReferenceTypeInfo reference => Contains(reference.Referent),
        ArrayTypeInfo array => Contains(array.ElementType),
        _ => false
    };
    static HandleKind? NameKind(NamedTypeInfo named)
    {
        if (named.Arguments.Count != 0) return null;
        return (named.Symbol?.QualifiedName ?? named.Name).TrimStart(':') switch
        {
            "MArray" or "DotNetArray" => HandleKind.Array,
            "MType" or "DotNetType" => HandleKind.Type,
            _ => null
        };
    }
    public static bool IsMapped(TypeInfo? type, string managed) => Kind(type) is { } kind && managed == Managed(kind);
    public static string Managed(HandleKind kind) => kind == HandleKind.Array ? "global::System.Array" : "global::System.Type";
    public static bool Parameter(EmittedParameter parameter, Func<string, string> unique, List<string> init,
        List<string> setup, List<string> cleanup, List<string> imports, List<string> args)
    {
        if (Kind(parameter.NativeType) is not { } kind || parameter.ManagedType != Managed(kind)) return false;
        string wire = unique("__managedHandle"); init.Add("nint " + wire + " = 0;");
        if (kind == HandleKind.Array)
        {
            string state = unique("__arrayMarshaller");
            init.Add("global::FlaxEngine.Interop.SystemArrayMarshaller.ManagedToNative " + state + " = default;");
            setup.Add(state + ".FromManaged(" + parameter.ManagedName + ");");
            setup.Add(wire + " = " + state + ".ToUnmanaged();");
            cleanup.Add(state + ".Free();");
        }
        else setup.Add(wire + " = global::FlaxEngine.Interop.SystemTypeMarshaller.ConvertToUnmanaged(" + parameter.ManagedName + ");");
        cleanup.Add("global::System.GC.KeepAlive(" + parameter.ManagedName + ");");
        imports.Add("nint " + unique("__arg")); args.Add(wire); return true;
    }
    public static string Return(HandleKind kind, string value, Func<string, string> unique,
        List<string> init, List<string> setup, List<string> cleanup)
    {
        if (kind == HandleKind.Type) return "global::FlaxEngine.Interop.SystemTypeMarshaller.ConvertToManaged(" + value + ")";
        string state = unique("__arrayResult");
        init.Add("global::FlaxEngine.Interop.SystemArrayMarshaller.NativeToManaged " + state + " = default;");
        setup.Add(state + ".FromUnmanaged(" + value + ");"); cleanup.Add(state + ".Free();");
        return state + ".ToManaged()";
    }
}
