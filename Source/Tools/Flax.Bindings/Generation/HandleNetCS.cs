using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

// Matches Flax's existing Object bridge; native addresses are ScriptingObject*
// addresses, not DotNetObject*/GC handles or independently owned native pointers.
internal static class HandleNetCS
{
    public static string? ObjectType(TypeInfo? type) => HandleNETcpp.Use(type) is { } use
        ? ManagedName(use.Type.Api.Export ? use.Type : HandleNETcpp.ManagedBases(use.Type).FirstOrDefault() ?? use.Type) : null;
    public static string ManagedName(DeclarationDescriptor declaration) => declaration is TypeDescriptor root && (HandleNETcpp.IsRoot(root) || root.QualifiedName is "ManagedScriptingObject" or "PersistentScriptingObject" && root.SourceFile?.Converters.FlaxScriptingObjects == true)
        ? "global::FlaxEngine.Object" : "global::" + CSharpGenerator.Qualified(string.Join("::", new[] {
            NamespaceKey(declaration), EnclosingNames(declaration), SimpleName(declaration) }.Where(s=>s.Length!=0)));

    public static string SimpleName(DeclarationDescriptor declaration) => declaration.Api.ManagedName ?? declaration.Name.text;
    public static string EnclosingNames(DeclarationDescriptor declaration) => declaration.Parent is TypeDescriptor parent
        ? (EnclosingNames(parent) is { Length: > 0 } enclosing ? enclosing + "::" : "") + SimpleName(parent)
        : string.Concat(declaration.EnclosingTypeTokens.Select(t => t.text));
    static string QualifiedName(DeclarationDescriptor declaration) => string.Join("::", new[] {
        string.Concat(declaration.NamespaceTokens.Select(t => t.text)), EnclosingNames(declaration), SimpleName(declaration) }.Where(s => s.Length != 0));

    public static bool EngineNamespace(DeclarationDescriptor declaration)
    {
        bool Contains(TypeDescriptor type) => HandleNETcpp.IsScriptingObject(type) || type.Declarations.OfType<TypeDescriptor>().Any(Contains);
        for (var current = declaration; current != null && current is not FileDescriptor; current = current.Parent)
            if (current is TypeDescriptor type && Contains(type)) return true;
        if (declaration is AliasDescriptor { TypeInfo: NamedTypeInfo { Specialization.Concrete: { } concrete } }) return HandleNETcpp.IsScriptingObject(concrete);
        return false;
    }
    public static string NamespaceKey(DeclarationDescriptor declaration)
    {
        for (DeclarationDescriptor? current=declaration; current is not null and not FileDescriptor; current=current.Parent)
            if (current.Api.ManagedNamespace is { } configured) return configured.Replace(".", "::");
        string native = string.Concat(declaration.NamespaceTokens.Select(t => t.text));
        string? prefix = declaration.SourceFile?.Converters.DefaultManagedNamespace;
        if (prefix != null) return prefix.Replace(".", "::") + (native.Length == 0 || prefix.Length == 0 ? "" : "::") + native;
        return EngineNamespace(declaration) ? "FlaxEngine" + (native.Length == 0 ? "" : "::" + native) : native;
    }
    public static IReadOnlyList<string> Bases(TypeDescriptor type)
    {
        var bases = HandleNETcpp.ManagedBases(type).ToArray();
        return bases.Length == 0 ? ["global::FlaxEngine.Object"] : bases.Select(ManagedName).Distinct().ToArray();
    }
    public static string? Parameter(FieldDescriptor parameter)
    {
        if (parameter.TypeInfo == null || !HandleNETcpp.SupportedUse(parameter.TypeInfo) || HandleNETcpp.Use(parameter.TypeInfo) is not { } use) return null;
        string type = ObjectType(parameter.TypeInfo)!;
        return use.Slot ? (parameter.Binding?.ReadOnlyReference == true ? "ref readonly " : parameter.Binding?.Direction switch { ParameterDirection.Out => "out ", ParameterDirection.In => "in ", _ => "ref " }) + type : type;
    }
    public static string ToNative(string expression) => $"global::FlaxEngine.Object.GetUnmanagedPtr((global::FlaxEngine.Object)(object)({expression}))";
    public static string FromNative(TypeInfo type, string expression) => $"({ObjectType(type)})global::FlaxEngine.Object.FromUnmanagedPtr({expression})";

    public static bool EmitParameter(EmittedParameter parameter, Func<string, string> unique, List<string> init, List<string> setup,
        List<string> imports, List<string> args, List<string> copyback, List<string> keepAlive)
    {
        if (!HandleNETcpp.SupportedUse(parameter.NativeType)) return false;
        var use = HandleNETcpp.Use(parameter.NativeType)!;
        string local = unique("__scriptObject");
        init.Add("nint " + local + " = 0;");
        if (use.Slot && parameter.Direction == ParameterDirection.Out) init.Add(parameter.ManagedName + " = default;");
        else
        {
            string retained = unique("__scriptLifetime");
            init.Add("var " + retained + " = " + parameter.ManagedName + ";");
            setup.Add(local + " = " + ToNative(retained) + ";");
            keepAlive.Add("global::System.GC.KeepAlive(" + retained + ");");
        }
        imports.Add("nint" + (use.Slot ? "* " : " ") + unique("__objectArg"));
        args.Add((use.Slot ? "&" : "") + local);
        if (use.Slot && parameter.Direction != ParameterDirection.In) copyback.Add(parameter.ManagedName + " = " + FromNative(parameter.NativeType, local) + ";");
        return true;
    }
}
