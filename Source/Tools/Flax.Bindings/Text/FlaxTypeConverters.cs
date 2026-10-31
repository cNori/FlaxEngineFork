namespace VETools.Text;

internal static class FlaxTypeConverters
{
    // Explicit map from Flax BaseTypes.h. No include traversal/preprocessor inference.
    // Pointer-sized aliases below assume the selected Windows x64 target.
    public static void RegisterWindowsX64Primitives(TypeConverterRegistry registry)
    {
        foreach (var (native, canonical) in new (string, string)[]
        {
            ("byte", "unsigned char"),
            ("uint8", "unsigned char"), ("uint16", "unsigned short"),
            ("uint32", "unsigned int"), ("uint64", "unsigned long long"),
            ("int8", "signed char"), ("int16", "signed short"),
            ("int32", "signed int"), ("int64", "long long"),
            ("uintptr", "unsigned long long"), ("intptr", "long long")
        }) registry.RegisterAlias(native, canonical);
        // Char is platform-dependent and needs a UTF-16 ABI handler.
        // Engine aggregates, strings and containers need dedicated value/lifetime converters.
    }
}
