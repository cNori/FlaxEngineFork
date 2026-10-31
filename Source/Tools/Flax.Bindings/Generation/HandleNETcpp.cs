using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

// Flax-specific native object ABI. Object identity/lifetime remains with Flax;
// these wrappers neither box engine objects nor allocate/delete their wrappers.
internal static class HandleNETcpp
{
    public static readonly NativeSupportRequirements CoTaskMemorySupport = new() {
        Includes = ["<cstddef>", "<cstring>", "<stdexcept>", "<new>"],
        Libraries = ["ole32.lib"], Declarations = [CoTaskMemoryDeclarations]
    };
    public const string CoTaskMemoryDeclarations = """
        #ifndef VET_NET_COTASKMEM_DECLARED
        #define VET_NET_COTASKMEM_DECLARED
        #if !defined(_WIN64)
        #error "Native memory converters are configured for Windows x64."
        #endif
        extern "C" __declspec(dllimport) void* __stdcall CoTaskMemAlloc(std::size_t);
        extern "C" __declspec(dllimport) void __stdcall CoTaskMemFree(void*);
        #endif

        """;
    public sealed record ObjectUse(TypeDescriptor Type, bool Pointer, bool Slot, bool Reference);

    public static bool IsScriptingObject(TypeDescriptor type) => type is ClassDescriptor && type.SourceFile?.Converters.FlaxScriptingObjects == true && Root(type) != null;
    public static bool IsScriptingInterface(TypeDescriptor type) => type is InterfaceDescriptor &&
        type.SourceFile?.Converters.FlaxScriptingObjects == true &&
        type.Declarations.OfType<RawDeclarationDescriptor>().Any(d => d.Name.text == "DECLARE_SCRIPTING_TYPE_MINIMAL") &&
        type.SourceFile.TypeInfos?.Symbols.GetValueOrDefault("ScriptingObject")?.Definition is ClassDescriptor;
    public static bool IsRoot(TypeDescriptor type) => IsScriptingObject(type) && type.QualifiedName == "ScriptingObject";

    sealed record BaseNode(TypeDescriptor Type, IReadOnlyDictionary<string, TypeInfo> Arguments);
    static NamedTypeInfo? ResolveBase(TypeInfo? type, IReadOnlyDictionary<string, TypeInfo> arguments)
    {
        HashSet<string> seen = [];
        while (type is NamedTypeInfo named && arguments.TryGetValue(named.Name, out var actual))
        { if (!seen.Add(named.Name)) return null; type = actual; }
        return type as NamedTypeInfo;
    }
    static IEnumerable<BaseNode> Bases(BaseNode node, bool publicOnly)
    {
        foreach (var basis in node.Type.BaseTypes)
        {
            if (publicOnly && (basis.Access != "public" || basis.IsVirtual)) continue;
            var named = ResolveBase(basis.Type, node.Arguments);
            if (named == null) continue;
            var parent = named.Specialization?.Concrete ?? named.Symbol?.Definition as TypeDescriptor;
            if (parent == null) continue;
            Dictionary<string, TypeInfo> arguments = [];
            if (parent.TemplateParameters.Count != 0)
            {
                if (parent.TemplateParameters.Count != named.Arguments.Count || parent.TemplateParameters.Any(p => !p.IsType || p.IsPack)) continue;
                bool valid = true;
                for (int i = 0; i < parent.TemplateParameters.Count; ++i)
                {
                    var actual = named.Arguments[i].Type;
                    if (actual is NamedTypeInfo n && node.Arguments.TryGetValue(n.Name, out var resolved)) actual = resolved;
                    if (actual == null) { valid = false; break; }
                    arguments.Add(parent.TemplateParameters[i].Name.text, actual);
                }
                if (!valid) continue;
            }
            yield return new(parent, arguments);
        }
    }
    static string BaseKey(BaseNode node) => node.Type.QualifiedName + "<" + string.Join(",", node.Arguments.OrderBy(a => a.Key).Select(a => a.Key + "=" + a.Value.Display)) + ">";
    public static ClassDescriptor? Root(TypeDescriptor type)
    {
        if (IsScriptingInterface(type)) return type.SourceFile!.TypeInfos!.Symbols["ScriptingObject"].Definition as ClassDescriptor;
        HashSet<string> path = [];
        ClassDescriptor? Find(BaseNode node)
        {
            string key = BaseKey(node); if (path.Count >= 64 || !path.Add(key)) return null;
            try {
                if (node.Type is ClassDescriptor root && root.QualifiedName == "ScriptingObject") return root;
                foreach (var parent in Bases(node, false)) if (Find(parent) is { } result) return result;
                return null;
            } finally { path.Remove(key); }
        }
        return Find(new(type, new Dictionary<string, TypeInfo>()));
    }
    public static IEnumerable<ClassDescriptor> ManagedBases(TypeDescriptor type)
    {
        HashSet<string> path = [];
        IEnumerable<ClassDescriptor> Find(BaseNode node)
        {
            string key = BaseKey(node); if (path.Count >= 64 || !path.Add(key)) yield break;
            try {
                if (node.Type is ClassDescriptor cls && ExportAttributes.Read(cls).Export && IsScriptingObject(cls)) { yield return cls; yield break; }
                foreach (var parent in Bases(node, true)) foreach (var result in Find(parent)) yield return result;
            } finally { path.Remove(key); }
        }
        foreach (var parent in Bases(new(type, new Dictionary<string, TypeInfo>()), true)) foreach (var result in Find(parent)) yield return result;
    }

    public static ObjectUse? Use(TypeInfo? type)
    {
        bool reference = type is ReferenceTypeInfo;
        if (type is ReferenceTypeInfo r) type = r.Referent;
        bool pointer = type is PointerTypeInfo;
        if (type is PointerTypeInfo p) type = p.Pointee;
        return type is NamedTypeInfo { Symbol.Definition: TypeDescriptor cls } && (IsScriptingObject(cls) || IsScriptingInterface(cls))
            ? new(cls, pointer, reference && pointer, reference) : null;
    }

    public static bool Contains(TypeInfo? type) => Use(type) != null || type switch
    {
        PointerTypeInfo pointer => Contains(pointer.Pointee), ReferenceTypeInfo reference => Contains(reference.Referent), _ => false
    };
    public static bool SupportedUse(TypeInfo type) => Use(type) is { Type: InterfaceDescriptor }
        ? type is PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { EffectiveQualifiers: TypeQualifiers.None } }
        : type is not ReferenceTypeInfo { ReferenceKind: ReferenceKind.RValue } && Use(type) is { Pointer: true } or { Reference: true };
    public static bool CanCast(TypeDescriptor type)
    {
        if (type is InterfaceDescriptor) return IsScriptingInterface(type);
        HashSet<string> path = [];
        int Paths(BaseNode node)
        {
            string key = BaseKey(node); if (path.Count >= 64 || !path.Add(key)) return 0;
            try { if (node.Type.QualifiedName == "ScriptingObject") return 1; int total = 0; foreach (var basis in Bases(node, true)) { total += Paths(basis); if (total > 1) return 2; } return total; }
            finally { path.Remove(key); }
        }
        return Paths(new(type, new Dictionary<string, TypeInfo>())) == 1;
    }
    public static string RootName(TypeDescriptor type) => "::" + Root(type)!.NativeQualifiedName;

    public static string ReceiverParameter(ClassDescriptor type) => RootName(type) + "* self";
    public static string ReceiverSetup(ClassDescriptor type) => $"auto* nativeSelf = static_cast<::{type.NativeQualifiedName}*>(self);";

    public static string ParameterType(TypeInfo type) => RootName(Use(type)!.Type) + (Use(type)!.Slot ? "**" : "*");
    public static string ToNative(TypeInfo type, string expression)
    {
        var use = Use(type)!;
        if (use.Type is InterfaceDescriptor iface)
            return $"([&]() {{ auto* object = {expression}; auto* value = ::ScriptingObject::ToInterface<::{iface.NativeQualifiedName}>(object); if (object && !value) throw std::invalid_argument(\"Object does not implement the native interface\"); return value; }}())";
        TypeInfo target = type is ReferenceTypeInfo r ? r.Referent : type;
        string castType = TemplateInstantiationPass.Native(target);
        if (target is not PointerTypeInfo) castType += "*";
        string cast = $"static_cast<{castType}>({expression})";
        return use.Reference && !use.Pointer ? "*" + cast : cast;
    }
    public static string ToRoot(TypeDescriptor type, string expression) => type is InterfaceDescriptor
        ? $"([&]() {{ auto* value = {expression}; auto* object = ::ScriptingObject::FromInterface(value); if (value && !object) throw std::invalid_argument(\"Native interface has no scripting object identity\"); return object; }}())"
        : $"const_cast<{RootName(type)}*>(static_cast<const {RootName(type)}*>({expression}))";
    public static string ReturnType(TypeInfo type) => RootName(Use(type)!.Type) + "*";
    public static string ReturnValue(TypeInfo type, string call) => ToRoot(Use(type)!.Type, Use(type)!.Reference && !Use(type)!.Pointer ? "std::addressof(" + call + ")" : call);

    public static IEnumerable<string> DefinitionHeaders(EmittedBinding entry)
    {
        foreach (var type in new[] { entry.NativeType, entry.NativeFunction?.ReturnType }.Concat(entry.NativeFunction?.Parameters ?? []).OfType<TypeInfo>())
            if (Use(type)?.Type is { } definition)
            {
                if (definition.SourceFile?.Path is { } path) yield return path;
                if (definition is InterfaceDescriptor && Root(definition)?.SourceFile?.Path is { } rootHeader) yield return rootHeader;
            }
    }
}
