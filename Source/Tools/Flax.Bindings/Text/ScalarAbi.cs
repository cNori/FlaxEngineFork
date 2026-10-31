namespace VETools.Text;

// Windows-x64 wire identity and size come from one descriptor. Managed storage
// types must not be chosen independently from the native slot stride.
internal sealed record ScalarAbi(string Cpp, string CS, int Size)
{
    public static ScalarAbi? Resolve(TypeInfo type)
    {
        if (type is not NamedTypeInfo named) return null;
        if (named.Converter is { IsAlias: true } alias) return Resolve(alias.AbiType());
        return named.Resolution == TypeResolution.Builtin ? Resolve(named.Name) : null;
    }

    public static ScalarAbi? Resolve(string spelling)
    {
        if (!TypeInfoPass.IsBuiltin(spelling)) return null;
        var words = spelling.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        bool unsigned = words.Contains("unsigned"); int longs = words.Count(w => w == "long");
        if (words.Contains("void") || words.Contains("double") && longs != 0) return null;
        if (words.Contains("bool")) return new("bool", "bool", 1);
        if (words.Contains("float")) return new("float", "float", 4);
        if (words.Contains("double")) return new("double", "double", 8);
        if (words.Contains("char16_t") || words.Contains("wchar_t")) return new("unsigned short", "char", 2);
        if (words.Contains("char32_t")) return new("unsigned int", "uint", 4);
        if (words.Contains("char8_t")) return new("unsigned char", "byte", 1);
        if (words.Contains("char")) return new(unsigned ? "unsigned char" : "signed char", unsigned ? "byte" : "sbyte", 1);
        if (words.Contains("short")) return new(unsigned ? "unsigned short" : "short", unsigned ? "ushort" : "short", 2);
        return longs >= 2 ? new(unsigned ? "unsigned long long" : "long long", unsigned ? "ulong" : "long", 8) : new(unsigned ? "unsigned int" : "int", unsigned ? "uint" : "int", 4);
    }

    public static ScalarAbi Require(string nativeName, string nativeAbi, string managedAbi)
    {
        var wire = Resolve(nativeAbi) ?? throw new ArgumentException($"Converter for '{nativeName}' has unsupported native scalar ABI '{nativeAbi}'.");
        if (wire.CS != managedAbi)
            throw new ArgumentException($"Converter for '{nativeName}' declares managed ABI '{managedAbi}', but native ABI '{nativeAbi}' requires '{wire.CS}' ({wire.Size} bytes).");
        return wire;
    }

    public static ScalarAbi Require(NativeTypeConverter converter) => Require(converter.NativeName, converter.AbiCppType,
        converter.Bodies?.ManagedAbiType ?? throw new ArgumentException("A body converter is required."));
}
