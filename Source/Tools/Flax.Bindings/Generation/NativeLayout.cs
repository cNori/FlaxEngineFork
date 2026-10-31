using VETools.Text;
using static VETools.Tokenizer;

namespace VETools.Generation;

internal sealed record NativeStorage(int Size, int Alignment);
internal sealed class NativeStructLayout
{
    public required NativeStorage Storage;
    public Dictionary<FieldDescriptor, int> Offsets = [];
}

// Deliberately limited Windows-x64/default-packing layout calculator. It does
// not infer vtables, bases, bitfields, aliases or layouts of managed classes.
internal sealed class NativeLayout
{
    readonly HashSet<StructDescriptor> active = [];
    readonly Dictionary<StructDescriptor, NativeStructLayout?> cache = [];
    public bool DefaultPackingKnown = true;

    public NativeStructLayout? Struct(StructDescriptor type)
    {
        if (type.NativeFieldView || type.RequiresNativeFieldAccess || type.Api.Template || type.Api.NoPod) return null;
        if (type.ConcreteTemplate?.Definition.SourceFile is { } prototype &&
            (prototype.Errors.Any(entry => entry.IsError) || System.Text.RegularExpressions.Regex.IsMatch(prototype.SourceText, @"(?m)^\s*#\s*pragma\s+pack\b"))) return null;
        if (!DefaultPackingKnown) return null;
        if (type.SourceFile is { } source && (source.Errors.Any(entry => entry.IsError) || System.Text.RegularExpressions.Regex.IsMatch(source.SourceText, @"(?m)^\s*#\s*pragma\s+pack\b"))) return null;
        if (cache.TryGetValue(type, out var cached)) return cached;
        if (!active.Add(type)) return null;
        NativeStructLayout? value;
        try { value = Compute(type); }
        catch (OverflowException) { value = null; }
        active.Remove(type);
        cache[type] = value;
        return value;
    }

    NativeStructLayout? Compute(StructDescriptor type)
    {
        if (type.BaseTypes.Count > 0 || type.Functions.Any(f => f.TypeInfo?.ReturnType?.SpecifierTokens.Any(t => t == "virtual") == true)) return null;
        if (type.Declarations.OfType<RawDeclarationDescriptor>().Any(raw => !LayoutNeutralMacro(raw, type.SourceFile))) return null;
        int size = 0, alignment = 1;
        NativeStructLayout value = new() { Storage = new(1, 1) };
        foreach (var field in type.Fields)
        {
            if (IsStatic(field)) continue;
            if (field.IsBitField) return null;
            var storage = Storage(field.TypeInfo);
            if (storage == null || !Alignment(field.HeaderTokens, storage.Alignment, out int effective)) return null;
            size = RoundUp(size, effective);
            value.Offsets.Add(field, size);
            size = checked(size + storage.Size);
            alignment = Math.Max(alignment, effective);
        }
        if (!Alignment(type.HeaderTokens, alignment, out alignment)) return null;
        value.Storage = new(RoundUp(Math.Max(1, size), alignment), alignment);
        return value;
    }

    public NativeStorage? Storage(Text.TypeInfo? type)
    {
        if (type is PointerTypeInfo or FunctionPointerTypeInfo) return new(8, 8);
        if (type is ArrayTypeInfo array)
        {
            var element = Storage(array.ElementType);
            if (element == null || !int.TryParse(string.Concat(array.EffectiveExtentTokens.Select(t => t.text)), out int count) || count <= 0) return null;
            try { return new(checked(element.Size * count), element.Alignment); }
            catch (OverflowException) { return null; }
        }
        if (type is not NamedTypeInfo named) return null;
        if (named.Specialization?.Concrete is StructDescriptor concrete) return Struct(concrete)?.Storage;
        if (named.Arguments.Count != 0) return null;
        if (named.Converter is { AutomaticFields: true } && named.Symbol?.Definition is StructDescriptor automatic) return Struct(automatic)?.Storage;
        if (named.Converter is { EnumValue: true } enumerationConverter) return Storage(enumerationConverter.AbiType());
        if (named.Converter is { } converter)
        {
            if (converter.IsAlias) return Storage(converter.AbiType());
            if (converter.Bodies?.NativeInlineStorage is { } inline && inline.Size > 0 && inline.Alignment > 0 &&
                inline.Alignment <= 128 && (inline.Alignment & (inline.Alignment - 1)) == 0 && inline.Size % inline.Alignment == 0)
                return new(inline.Size, inline.Alignment);
            return null;
        }
        if (named.Resolution == TypeResolution.Builtin)
        {
            var words = named.Name.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (words.Contains("void") || (words.Contains("long") && words.Contains("double"))) return null;
            if (words.Any(w => w is "bool" or "char" or "char8_t")) return new(1, 1);
            if (words.Any(w => w is "short" or "char16_t" or "wchar_t")) return new(2, 2);
            if (words.Contains("double") || words.Count(w => w == "long") == 2) return new(8, 8);
            return new(4, 4);
        }
        if (named.Resolution != TypeResolution.Resolved) return null;
        if (named.Symbol?.Definition is StructDescriptor structure) return Struct(structure)?.Storage;
        if (named.Symbol?.Definition is EnumDescriptor enumeration)
            return enumeration.UnderlyingType != null ? Storage(enumeration.UnderlyingType) : enumeration.IsScoped ? new(4, 4) : null;
        return null;
    }

    public static bool IsStatic(FieldDescriptor field) => field.TypeInfo?.SpecifierTokens.Any(t => t.text is "static" or "constexpr" or "consteval") == true;
    // A stateless interface may add a vtable, so it still has no shared-layout
    // contract. It contributes no data that a field-by-field value copy can lose.
    public static bool HasStatelessInterfaceBases(TypeDescriptor type)
    {
        HashSet<TypeDescriptor> active = [];
        return CheckBases(type);
        bool CheckBases(TypeDescriptor current)
        {
            if (active.Count >= 32 || !active.Add(current)) return false;
            try
            {
                foreach (var basis in current.BaseTypes)
                {
                    if (basis.Access != "public" || basis.IsVirtual || basis.Type is not NamedTypeInfo { Symbol.Definition: InterfaceDescriptor contract } ||
                        contract.IsForwardDeclaration || contract.Fields.Any(f => !IsStatic(f)) ||
                        contract.Declarations.OfType<RawDeclarationDescriptor>().Any(raw => !LayoutNeutralMacro(raw, contract.SourceFile)) ||
                        !CheckBases(contract)) return false;
                }
                return true;
            }
            finally { active.Remove(current); }
        }
    }
    static bool LayoutNeutralMacro(RawDeclarationDescriptor raw, FileDescriptor? source)
    {
        if (source == null || !source.Converters.LayoutNeutralDeclarationMacros.Contains(raw.Name.text ?? "")) return false;
        var tokens = raw.HeaderTokens.Where(t => !t.IsTrivia).ToArray();
        if (tokens.Length < 3 || tokens[0].text != raw.Name.text || tokens[1] != "(") return false;
        int depth = 0;
        for (int i = 1; i < tokens.Length; i++)
        {
            if (tokens[i] == "(") depth++;
            else if (tokens[i] == ")" && --depth == 0) return tokens.Skip(i + 1).All(t => t == ";");
        }
        return false;
    }
    public static int RoundUp(int size, int alignment) => checked(checked(size + alignment - 1) / alignment * alignment);
    public static bool Alignment(IEnumerable<Token> source, int natural, out int value)
    {
        value = natural;
        int computed = natural;
        var tokens = source.Where(t => !t.IsTrivia).ToList();
        for (int i = 0; i < tokens.Count; i++)
        {
            // GNU layout attributes require their own handler; never silently infer through them.
            if (tokens[i].text is "__attribute__" or "__attribute") return false;
            if (tokens[i] == "__declspec" && i + 1 < tokens.Count && tokens[i + 1] == "(")
            {
                int depth = 1;
                for (int j = i + 2; j < tokens.Count && depth > 0; j++)
                {
                    if (tokens[j] == "align")
                    {
                        if (j + 3 >= tokens.Count || tokens[j + 1] != "(" || tokens[j + 3] != ")" ||
                            !int.TryParse(tokens[j + 2].text, out int declspecAlignment) || !Apply(declspecAlignment)) return false;
                    }
                    if (tokens[j] == "(") depth++;
                    if (tokens[j] == ")") depth--;
                }
                if (depth != 0) return false;
            }
            if (tokens[i] != "alignas") continue;
            if (i + 3 >= tokens.Count || tokens[i + 1] != "(" || tokens[i + 3] != ")" || !int.TryParse(tokens[i + 2].text, out int declared)) return false;
            if (!Apply(declared)) return false;
        }
        value = computed;
        return true;
        bool Apply(int declared)
        {
            if (declared == 0) return true;
            if (declared < natural || (declared & (declared - 1)) != 0 || declared > 128) return false;
            computed = Math.Max(computed, declared);
            return true;
        }
    }
}
