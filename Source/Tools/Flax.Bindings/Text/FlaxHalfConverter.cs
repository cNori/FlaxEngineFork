namespace VETools.Text;

// Explicit four-word representation. Preserve half-float bit patterns, including
// NaNs and signed zero; do not round-trip through float or copy native structs.
internal static class FlaxHalfConverter
{
    public static void Register(TypeConverterRegistry registry)
    {
        string[] axes = ["X", "Y", "Z", "W"];
        registry.RegisterBodyConverter("Half4", "unsigned long long", new()
        {
            ManagedType = "global::FlaxEngine.Half4", ManagedAbiType = "ulong", CopyReferenceReturns = true, CopyReferenceParameters = true, CopiedCallbackValue = true,
            NativeRequirements = new() { Includes = ["\"Engine/Core/Math/Half.h\""] },
            NativeSupport = "static_assert(sizeof(::Half) == 2, \"Half4 converter requires sixteen-bit component words\");",
            ManagedToAbi = c => new(string.Join(" | ", axes.Select((axis,i) => $"((ulong)({c.Value}).{axis}.RawValue << {i*16})"))),
            AbiToManaged = c => new("new global::FlaxEngine.Half4(" + string.Join(", ", axes.Select((_,i) => $"global::System.Runtime.CompilerServices.Unsafe.BitCast<ushort, global::FlaxEngine.Half>(unchecked((ushort)(({c.Value}) >> {i*16})))")) + ")"),
            NativeToAbi = c => new(string.Join(" | ", axes.Select((axis,i) => $"(static_cast<unsigned long long>(({c.Value}).{axis}) << {i*16})"))),
            AbiToNative = c => new("::Half4(" + string.Join(", ", axes.Select((_,i) => $"static_cast<::Half>(({c.Value}) >> {i*16})")) + ")"),
            ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => ""
        }, nativeCppType: "::Half4", nativeDefault: "::Half4{}", existingManagedType: true);
    }
}
