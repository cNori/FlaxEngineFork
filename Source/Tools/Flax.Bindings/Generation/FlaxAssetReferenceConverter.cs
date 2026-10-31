using VETools.Text;

namespace VETools.Generation;

// Asset references project to their object, matching Flax's managed asset API.
// SoftAssetReference resolves on native reads; SoftObjectReference preserves IDs.
// Construct/assign native wrappers instead of copying their storage/subscriptions.
internal static class FlaxAssetReferenceConverter
{
    public static NativeTypeConverter? Create(NamedTypeInfo named)
    {
        string? referenceName = named.Symbol?.QualifiedName;
        if (referenceName is not ("AssetReference" or "SoftAssetReference" or "ScriptingObjectReference" or "SoftObjectReference") || named.Arguments.Count != 1 ||
            named.Symbol?.Definition?.SourceFile?.Converters.FlaxScriptingObjects != true ||
            named.Arguments[0].Type is not NamedTypeInfo { Symbol.Definition: ClassDescriptor asset } argument ||
            argument.EffectiveQualifiers != TypeQualifiers.None || !HandleNETcpp.IsScriptingObject(asset) ||
            !HandleNETcpp.CanCast(asset) || referenceName is ("AssetReference" or "SoftAssetReference") && !IsAsset(asset, [])) return null;
        if(referenceName == "SoftObjectReference") return Soft(named,asset);
        string native = "::" + referenceName + "<::" + asset.NativeQualifiedName.TrimStart(':') + ">";
        string pointer = "::" + asset.NativeQualifiedName.TrimStart(':') + "*";
        string root = HandleNETcpp.RootName(asset);
        string managed = HandleNetCS.ManagedName(asset);
        return new()
        {
            NativeName = named.Display, NativeCppType = native, AbiCppType = "unsigned long long",
            ExistingManagedType = true, NativeDefault = native + "{}",
            DefinitionHeaders = new[] { named.Symbol.Definition.SourceFile?.Path, asset.SourceFile?.Path }
                .OfType<string>().Distinct(StringComparer.OrdinalIgnoreCase).ToArray(),
            ToAbi = _ => throw new InvalidOperationException("Asset references require body emission."),
            ToNative = _ => throw new InvalidOperationException("Asset references require body emission."),
            Bodies = new()
            {
                ManagedType = managed, ManagedAbiType = "ulong", CopyReferenceReturns = true,
                ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => "",
                ManagedToAbi = c => new($"(ulong)(nuint){HandleNetCS.ToNative(c.Value)}",
                    Setup: c.RetainManaged?.Invoke(c.Value) ?? ""),
                AbiToManaged = c => new($"({managed})global::FlaxEngine.Object.FromUnmanagedPtr((nint)(nuint)({c.Value}))"),
                AbiToNative = c => new($"{native}(static_cast<{pointer}>(reinterpret_cast<{root}*>({c.Value})))"),
                NativeToAbi = c => new($"reinterpret_cast<unsigned long long>({HandleNETcpp.ToRoot(asset, $"static_cast<{pointer}>({c.Value})")})")
            }
        };
    }

    static NativeTypeConverter? Soft(NamedTypeInfo named, ClassDescriptor type)
    {
        if(named.Symbol!.Definition!.SourceFile?.Converters.TryGet("Guid",out var guid) != true || guid.Bodies is not { } body) return null;
        string native="::SoftObjectReference<::"+type.NativeQualifiedName.TrimStart(':')+">";
        return new()
        {
            NativeName=named.Display,NativeCppType=native,AbiCppType=guid.AbiCppType,NativeDefault=native+"{}",ExistingManagedType=true,
            DefinitionHeaders=new[]{named.Symbol.Definition.SourceFile?.Path,type.SourceFile?.Path}.OfType<string>().ToArray(),
            ToAbi=_=>throw new InvalidOperationException(),ToNative=_=>throw new InvalidOperationException(),
            Bodies=new()
            {
                ManagedType="global::FlaxEngine.SoftObjectReference",ManagedAbiType=body.ManagedAbiType,CopyReferenceReturns=true,
                ManagedSupport=body.ManagedSupport,NativeSupport=body.NativeSupport,NativeRequirements=body.NativeRequirements,
                ReleaseManagedAbi=body.ReleaseManagedAbi,ReleaseNativeAbi=body.ReleaseNativeAbi,
                ManagedToAbi=c=>body.ManagedToAbi(c with {Value=$"({c.Value}).ID"}),
                AbiToManaged=c=> {var value=body.AbiToManaged(c);return value with {Expression=$"new global::FlaxEngine.SoftObjectReference {{ ID={value.Expression} }}"};},
                AbiToNative=c=> {var value=body.AbiToNative(c);string temp=c.Unique("softReference");return value with {Setup=value.Setup+$"\n{native} {temp}; {temp}={value.Expression};",Expression=temp};},
                NativeToAbi=c=>body.NativeToAbi(c with {Value=$"({c.Value}).GetID()"})
            }
        };
    }

    static bool IsAsset(TypeDescriptor type, HashSet<TypeDescriptor> seen)
    {
        if (!seen.Add(type)) return false;
        return type.QualifiedName == "Asset" || type.BaseTypes.Any(b =>
            b.Type is NamedTypeInfo { Symbol.Definition: TypeDescriptor parent } && IsAsset(parent, seen));
    }
}
