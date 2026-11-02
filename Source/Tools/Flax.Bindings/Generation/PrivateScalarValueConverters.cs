using VETools.Text;

namespace VETools.Generation;

// Preserve private scalar state without publishing fields or guessing ownership.
// Compiler assertions validate the layout and trivial lifetime contract.
internal static class PrivateScalarValueConverters
{
    public static NativeTypeConverter? Create(TypeDescriptor candidate)
    {
        if (candidate is not StructDescriptor type || type.IsForwardDeclaration || type.Api.InBuild || type.Api.Template ||
            type.Api.MarshalAsTokens.Count != 0 || !ExportAttributes.Read(type).Export || !CppGenerator.HasFriendAccess(type) ||
            type.Functions.Any(f => f.TypeInfo is { IsConstructor: true } or { IsDestructor: true }) ||
            new NativeLayout().Struct(type) is not { } layout || layout.Storage.Alignment > IntPtr.Size || layout.Storage.Size > 65536) return null;
        for (DeclarationDescriptor child = type; child.Parent is TypeDescriptor parent; child = parent)
            if (child.Access != "public" || !ExportAttributes.Read(parent).Export) return null;
        var fields = type.Fields.Where(f => !NativeLayout.IsStatic(f)).ToArray();
        if (fields.Length == 0 || !fields.Any(f => f.Access != "public") || fields.Any(f => f.IsBitField ||
            f.TypeInfo is not NamedTypeInfo n || n.EffectiveQualifiers != TypeQualifiers.None || ScalarAbi.Resolve(n) == null)) return null;
        // Mixing public and private data is not standard-layout in C++.
        if (fields.Select(f => f.Access).Distinct().Count() != 1) return null;
        type.PrivateScalarStorage.Clear();
        foreach (var field in fields) type.PrivateScalarStorage.Add(field, ScalarAbi.Resolve(field.TypeInfo!)!.CS);
        string native = "::" + type.NativeQualifiedName, managed = HandleNetCS.ManagedName(type);
        int size = layout.Storage.Size;
        return new()
        {
            NativeName = type.QualifiedName, NativeCppType = native, AbiCppType = "unsigned long long", NativeDefault = native + "{}",
            AutomaticFields = true, SharedReference = true,
            DefinitionHeaders = type.SourceFile?.Path is { } path ? [path] : [],
            ToAbi = _ => throw new InvalidOperationException(), ToNative = _ => throw new InvalidOperationException(),
            Bodies = new()
            {
                ManagedType = managed, ManagedAbiType = "ulong", NativeInlineStorage = (size, layout.Storage.Alignment),
                CopyReferenceReturns = true, CopyReferenceParameters = true, CopiedCallbackValue = true,
                NativeRequirements = NativeSupportRequirements.Merge([HandleNETcpp.CoTaskMemorySupport,
                    new() { Includes = ["<cstring>", "<memory>", "<type_traits>", "<stdexcept>"], PrivateScalarLayouts = [type] }]),
                ReleaseManagedAbi = c => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));",
                ReleaseNativeAbi = c => $"::CoTaskMemFree(reinterpret_cast<void*>({c.Value}));",
                ManagedToAbi = c =>
                {
                    string buffer = c.Unique("__privateValue");
                    return new($"(ulong)(nuint){buffer}", $"nint {buffer}=0;",
                        $"{buffer}=global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem({size});\nglobal::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*){buffer}, {c.Value});",
                        $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
                },
                AbiToManaged = c => new($"global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<{managed}>((void*)(nint)(nuint)({c.Value}))",
                    Setup: $"if ({c.Value} == 0) throw new global::System.InvalidOperationException(\"Null private-value packet.\");",
                    Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({c.Value}));"),
                AbiToNative = c =>
                {
                    string value = c.Unique("privateValue");
                    return new(value, Setup: $"if (!{c.Value}) throw std::invalid_argument(\"Null private-value packet.\");\n{native} {value}{{}};\nstd::memcpy(std::addressof({value}), reinterpret_cast<const void*>({c.Value}), {size});");
                },
                NativeToAbi = c =>
                {
                    string buffer = c.Unique("privateValueBuffer");
                    return new(buffer, $"unsigned long long {buffer}=0;",
                        $"{buffer}=reinterpret_cast<unsigned long long>(::CoTaskMemAlloc({size}));\nif (!{buffer}) throw std::bad_alloc();\nstd::memcpy(reinterpret_cast<void*>({buffer}), std::addressof({c.Value}), {size});",
                        $"if (!{c.Completed}) ::CoTaskMemFree(reinterpret_cast<void*>({buffer}));");
                }
            }
        };
    }
}
