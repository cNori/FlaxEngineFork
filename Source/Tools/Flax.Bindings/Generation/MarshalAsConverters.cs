using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

// MarshalAs chooses a representation, not a native layout. The programmer supplies
// assignment/conversion operators in both languages; generated code uses them.
internal sealed class MarshalAsConverters
{
    readonly Dictionary<TypeDescriptor, NativeTypeConverter?> converters = [];
    readonly HashSet<TypeDescriptor> active = [];

    public static void Apply(FileDescriptor root)
    {
        if (root.TypeInfos!.MarshalAsConvertersReady) return;
        var files = root.ProjectFiles.Count == 0 ? new[] { root } : root.ProjectFiles.ToArray();
        var declarations = files.SelectMany(TemplateInstantiationPass.Declarations)
            .Concat(root.TypeInfos.TemplateInstances.Values.SelectMany(t => new[] { t }.Concat(TemplateInstantiationPass.Declarations(t)))).Distinct().ToArray();
        var pass = new MarshalAsConverters();
        foreach (var type in declarations.OfType<TypeDescriptor>()) type.MarshalAsError = null;
        foreach (var type in declarations.OfType<TypeDescriptor>().Where(t => !t.Api.Template && t.MarshalAsType != null)) pass.Build(type);
        foreach (var declaration in declarations) foreach (var type in TemplateInstantiationPass.Types(declaration)) pass.Attach(type);
        // Container element types can now use the newly composed converters.
        foreach (var file in files) { file.TypeInfos!.MarshalAsConvertersReady = true; file.TypeInfos.StandardConvertersReady = false; }
        StandardConverters.Apply(root);
    }

    static TypeDescriptor? Definition(NamedTypeInfo named) => named.Specialization?.Concrete ?? named.Symbol?.Definition as TypeDescriptor;
    void Attach(TypeInfo type)
    {
        if (type is ReferenceTypeInfo r) Attach(r.Referent);
        else if (type is PointerTypeInfo p) Attach(p.Pointee);
        else if (type is ArrayTypeInfo a) Attach(a.ElementType);
        else if (type is NamedTypeInfo named)
        {
            foreach (var argument in named.Arguments.Select(a => a.Type).OfType<TypeInfo>()) Attach(argument);
            if (named.Converter == null && Definition(named) is { Api.Template: false, MarshalAsType: not null } definition)
                named.Converter = Build(definition);
        }
    }

    NativeTypeConverter? Build(TypeDescriptor type)
    {
        if (active.Contains(type)) { type.MarshalAsError = "MarshalAs conversion cycle at '" + type.QualifiedName + "'."; return null; }
        if (converters.TryGetValue(type, out var cached)) return cached;
        converters[type] = null;
        if (type.MarshalAsType is not { } target) return null;
        if (active.Count >= 32) { type.MarshalAsError = "MarshalAs chain exceeds 32 types."; return null; }
        active.Add(type);
        try
        {
            if (target is ReferenceTypeInfo or ArrayTypeInfo or UnsupportedTypeInfo || target is NamedTypeInfo { Qualifiers: not TypeQualifiers.None } || target is PointerTypeInfo { Qualifiers: not TypeQualifiers.None })
            { type.MarshalAsError = $"MarshalAs target '{target.Display}' for '{type.QualifiedName}' must have an unqualified value converter (not a reference, C array or unsupported declarator)."; return null; }
            if (target is not PointerTypeInfo) Attach(target); // A handle does not require a value converter for its pointee.
            var converter = StandardConverters.ValueConverter(target);
            if (converter?.Bodies is not { } body)
            { type.MarshalAsError ??= $"MarshalAs target '{target.Display}' for '{type.QualifiedName}' has no usable converter. Register its converter before using this adapter."; return null; }
            string? managed = ExistingManagedName(type);
            if (managed == null) { type.MarshalAsError = "InBuild generic arguments need managed value converters for '" + type.NativeQualifiedName + "'."; return null; }
            string native = "::" + type.NativeQualifiedName.TrimStart(':');
            bool scalar = target is PointerTypeInfo || target is NamedTypeInfo { Resolution: TypeResolution.Builtin } || target is NamedTypeInfo { Converter.IsAlias: true } or NamedTypeInfo { Converter.EnumValue: true };
            NativeTypeConverter adapted = new()
            {
                NativeName = type.QualifiedName, NativeCppType = native, AbiCppType = converter.AbiCppType,
                RepresentationAdapter = true,
                NativeDefault = native + "{}", ToAbi = _ => throw new InvalidOperationException(), ToNative = _ => throw new InvalidOperationException(),
                DefinitionHeaders = converter.DefinitionHeaders.Concat(type.SourceFile?.Path is { } path ? new[] { path } : []).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(),
                ManagedDeclarations = converter.ManagedDeclarations,
                Bodies = new()
                {
                    ManagedType = managed, ManagedAbiType = body.ManagedAbiType, CopyReferenceReturns = true,
                    NativeRequirements = body.NativeRequirements, ManagedSupport = body.ManagedSupport, ManagedSupportOwner = body.ManagedSupportOwner, NativeSupport = body.NativeSupport,
                    ReleaseManagedAbi = body.ReleaseManagedAbi ?? (scalar ? _ => "" : null), ReleaseNativeAbi = body.ReleaseNativeAbi ?? (scalar ? _ => "" : null),
                    ManagedToAbi = c =>
                    {
                        string temp = c.Unique("__marshalAsInput");
                        var converted = body.ManagedToAbi(c with { Value = temp });
                        return converted with { Declarations = $"{body.ManagedType} {temp} = default;\n" + converted.Declarations,
                            Setup = $"{temp} = {c.Value};\n" + (c.RetainManaged?.Invoke(temp) ?? "") + "\n" + converted.Setup,
                            Cleanup = $"try {{\n{converted.Cleanup}\n}} finally {{ global::System.GC.KeepAlive({temp}); }}" };
                    },
                    AbiToManaged = c =>
                    {
                        var converted = body.AbiToManaged(c);
                        string temp = c.Unique("__marshalAsOutput");
                        // Assignment is deliberately separate from byte/packet conversion.
                        return converted with { Expression = temp, Setup = converted.Setup + $"\n{managed} {temp} = {converted.Expression};" };
                    },
                    AbiToNative = c =>
                    {
                        var converted = body.AbiToNative(c);
                        string temp = c.Unique("marshalAsOutput");
                        return converted with { Expression = temp, Setup = converted.Setup + $"\n{native} {temp}{{}};\n{temp} = {converted.Expression};" };
                    },
                    NativeToAbi = c =>
                    {
                        string temp = c.Unique("marshalAsInput");
                        var converted = body.NativeToAbi(c with { Value = temp });
                        return converted with { Setup = $"{converter.NativeCppType} {temp}{{}};\n{temp} = {c.Value};\n" + converted.Setup };
                    }
                }
            };
            converters[type] = adapted; return adapted;
        }
        finally { active.Remove(type); }
    }

    static string? ExistingManagedName(TypeDescriptor type)
    {
        if (type.ConcreteTemplate is { } specialization)
        {
            string? registered = type.SourceFile?.Converters.ManagedGenericType(specialization.Definition.NativeQualifiedName)
                ?? specialization.Definition.SourceFile?.Converters.ManagedGenericType(specialization.Definition.NativeQualifiedName);
            if (registered != null || type.Api.InBuild && specialization.Definition.Api.InBuild)
            {
                string? Argument(TypeInfo? argument)
                {
                    if (argument == null) return null;
                    if (StandardConverters.ValueConverter(argument)?.Bodies?.ManagedType is { } managed) return managed;
                    if (argument is NamedTypeInfo named && Definition(named) is { } declaration && HandleNETcpp.IsScriptingObject(declaration))
                        return HandleNetCS.ManagedName(declaration);
                    return null;
                }
                var arguments = specialization.Definition.TemplateParameters.Select(p => Argument(specialization.TypeArguments.GetValueOrDefault(p.Name.text))).ToArray();
                if (arguments.Any(a => a == null)) return null;
                return (registered ?? HandleNetCS.ManagedName(specialization.Definition)) + "<" + string.Join(", ", arguments) + ">";
            }
        }
        if (!type.Api.InBuild || type.Api.ManagedNamespace == null) return HandleNetCS.ManagedName(type);
        string prefix = type.Api.ManagedNamespace.Replace(".", "::");
        string nested = string.Concat(type.EnclosingTypeTokens.Select(t => t.text));
        string name = (nested.Length == 0 ? "" : nested + "::") + type.Name.text;
        return "global::" + CSharpGenerator.Qualified((prefix.Length == 0 ? "" : prefix + "::") + name);
    }
}
