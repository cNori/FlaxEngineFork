using System.Text.RegularExpressions;
using static VETools.Tokenizer;

namespace VETools.Text;

internal enum DeclarationMacroKind { Opaque, Deprecated, PublicDeclarations }

internal sealed class NativeTypeConverter
{
    public required string NativeName { get; init; }
    public required string NativeCppType { get; init; }
    public required string AbiCppType { get; init; }
    public bool IsAlias { get; init; }
    public bool AutomaticFields { get; init; }
    public bool EnumValue { get; init; }
    public bool SharedReference { get; init; }
    public bool ExistingManagedType { get; init; }
    public bool RepresentationAdapter { get; init; }
    public bool FunctionValue { get; init; }
    public string? NativeSignatureIdentity { get; init; }
    public IReadOnlyList<string> ManagedDeclarations { get; init; } = [];
    public IReadOnlyList<string> DefinitionHeaders { get; init; } = [];
    public TypeQualifiers NativeQualifiers { get; init; }
    public required Func<string, string> ToAbi { get; init; }
    public required Func<string, string> ToNative { get; init; }
    public string? NativeDefault { get; init; }
    public ConverterBodies? Bodies { get; init; }
    public NamedTypeInfo AbiType(TypeQualifiers qualifiers = TypeQualifiers.None) => new() { Name = AbiCppType, Resolution = TypeResolution.Builtin, Qualifiers = qualifiers };
}

// Explicit declarations only. No typedef discovery, includes or macro expansion.
internal sealed class TypeConverterRegistry
{
    public bool FlaxScriptingObjects { get; set; }
    public string? DefaultManagedNamespace { get; set; }
    public bool CanonicalValueRecords { get; set; }
    readonly Dictionary<string, string> managedGenericTypes = new(StringComparer.Ordinal);
    public void RegisterManagedGenericType(string nativeName, string managedName) => managedGenericTypes.Add(nativeName.TrimStart(':'), managedName);
    public string? ManagedGenericType(string nativeName) => managedGenericTypes.GetValueOrDefault(nativeName.TrimStart(':'));
    public HashSet<string> ManagedUsings { get; } = new(StringComparer.Ordinal);
    readonly Dictionary<string,string> managedValuePartials = new(StringComparer.Ordinal);
    public string? ManagedValuePartial(string name) => managedValuePartials.GetValueOrDefault(name.TrimStart(':'));
    readonly HashSet<string> existingValueFields = new(StringComparer.Ordinal);
    public bool ManagedValueHasExistingFields(string name) => existingValueFields.Contains(name.TrimStart(':'));
    public void RegisterManagedValuePartial(string nativeName, string managedName, bool generateFields = true)
    {
        managedValuePartials.Add(nativeName.TrimStart(':'), managedName);
        if (!generateFields) existingValueFields.Add(nativeName.TrimStart(':'));
    }
    public bool FlaxDelegateLifetimes { get; set; }
    public bool FlaxContainers { get; set; }
    public bool FlaxFunctions { get; set; }
    readonly HashSet<string> exportedSettingsSnapshots = new(StringComparer.Ordinal);
    public bool IsExportedSettingsSnapshot(string name) => exportedSettingsSnapshots.Contains(name.TrimStart(':'));
    public void RegisterExportedSettingsSnapshot(string name)
    {
        if (!Regex.IsMatch(name, @"^(::)?[A-Za-z_]\w*(::[A-Za-z_]\w*)*$")) throw new ArgumentException("Expected a qualified settings type name.");
        if (!exportedSettingsSnapshots.Add(name.TrimStart(':'))) throw new ArgumentException("Duplicate settings snapshot contract.");
    }
    public bool RetainedLegacyNetworkRpc { get; set; }
    readonly HashSet<string> opaqueHandleProperties = new(StringComparer.Ordinal);
    public bool IsOpaqueHandleProperty(string qualifiedName) => opaqueHandleProperties.Contains(qualifiedName.TrimStart(':'));
    public void RegisterOpaqueHandleProperty(string qualifiedName)
    {
        if (!Regex.IsMatch(qualifiedName, @"^(::)?[A-Za-z_]\w*(::[A-Za-z_]\w*)*$")) throw new ArgumentException("Expected a qualified handle property name.");
        if (!opaqueHandleProperties.Add(qualifiedName.TrimStart(':'))) throw new ArgumentException("Duplicate handle property contract.");
    }
    readonly HashSet<string> synchronousEvents = new(StringComparer.Ordinal);
    public bool IsSynchronousEvent(string qualifiedName) => synchronousEvents.Contains(qualifiedName.TrimStart(':'));
    readonly Dictionary<string, string> synchronousFailures = new(StringComparer.Ordinal);
    public string SynchronousEventFailure(string qualifiedName) => synchronousFailures.GetValueOrDefault(qualifiedName.TrimStart(':')) ?? "";
    // The native failure statement is an explicit host policy, never inferred
    // from a payload's field names or the meaning of a validation callback.
    readonly Dictionary<string, NativeSupportRequirements> synchronousRequirements = new(StringComparer.Ordinal);
    public NativeSupportRequirements SynchronousEventRequirements(string name) => synchronousRequirements.GetValueOrDefault(name.TrimStart(':')) ?? new();
    readonly HashSet<(string Event, int Index)> borrowedEventPointers = [];
    public bool IsBorrowedEventPointer(string name, int index) => borrowedEventPointers.Contains((name.TrimStart(':'), index));
    public void RegisterBorrowedEventPointer(string name, int index)
    {
        if (!IsSynchronousEvent(name) || index < 0) throw new ArgumentException("Borrowed event pointers require a synchronous event and nonnegative argument index.");
        if (!borrowedEventPointers.Add((name.TrimStart(':'), index))) throw new ArgumentException("Duplicate borrowed event pointer contract.");
    }
    public void RegisterSynchronousEvent(string qualifiedName, string nativeFailure = "", NativeSupportRequirements? nativeRequirements = null)
    {
        if (!Regex.IsMatch(qualifiedName, @"^(::)?[A-Za-z_]\w*(::[A-Za-z_]\w*)*$")) throw new ArgumentException("Expected a qualified event name.");
        if (!synchronousEvents.Add(qualifiedName.TrimStart(':'))) throw new ArgumentException("Duplicate synchronous event contract.");
        synchronousFailures.Add(qualifiedName.TrimStart(':'), nativeFailure);
        synchronousRequirements.Add(qualifiedName.TrimStart(':'), nativeRequirements ?? new());
    }
    readonly Dictionary<(string Function, string Parameter), string> inputPointerArrays = [];
    public string? InputPointerArrayCount(string function, string parameter) => inputPointerArrays.GetValueOrDefault((function.TrimStart(':'), parameter));
    // Explicit call-scoped input buffer. Native slot changes are not copied back.
    public void RegisterInputPointerArray(string function, string parameter, string countParameter)
    {
        if (!Regex.IsMatch(function, @"^(::)?[A-Za-z_]\w*(::[A-Za-z_]\w*)*$") ||
            !Regex.IsMatch(parameter, @"^[A-Za-z_]\w*$") || !Regex.IsMatch(countParameter, @"^[A-Za-z_]\w*$"))
            throw new ArgumentException("Invalid input pointer-array contract.");
        if (!inputPointerArrays.TryAdd((function.TrimStart(':'), parameter), countParameter)) throw new ArgumentException("Duplicate input pointer-array contract.");
    }
    readonly HashSet<string> containerAllocators = new(StringComparer.Ordinal);
    public IReadOnlySet<string> ContainerAllocators => containerAllocators;
    // Opt in to standard Flax Array construction/Resize/destruction through this
    // allocator. No native allocator object layout is inspected or copied.
    public void RegisterContainerAllocator(string nativeName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(nativeName);
        nativeName = nativeName.TrimStart(':');
        if (!Regex.IsMatch(nativeName, @"^[A-Za-z_]\w*(::[A-Za-z_]\w*)*$")) throw new ArgumentException("Expected a qualified allocator type name: " + nativeName);
        if (!containerAllocators.Add(nativeName)) throw new ArgumentException("Allocator is already registered: " + nativeName);
    }
    public TypeConverterRegistry(Action<TypeConverterRegistry>? configure = null) => configure?.Invoke(this);
    readonly Dictionary<string, NativeTypeConverter> entries = new(StringComparer.Ordinal);
    readonly Dictionary<string, DeclarationMacroKind> declarationMacros = new(StringComparer.Ordinal);
    readonly HashSet<string> layoutNeutralDeclarationMacros = new(StringComparer.Ordinal);
    public IReadOnlySet<string> LayoutNeutralDeclarationMacros => layoutNeutralDeclarationMacros;
    public IReadOnlyDictionary<string, DeclarationMacroKind> DeclarationMacros => declarationMacros;
    public void RegisterDeclarationMacro(string name, DeclarationMacroKind kind = DeclarationMacroKind.Opaque, bool preservesInstanceLayout = false)
    {
        if (!Regex.IsMatch(name, @"^[A-Za-z_]\w*$") || name.StartsWith("API_", StringComparison.Ordinal)) throw new ArgumentException("Invalid or reserved declaration macro name: " + name);
        if (FunctionTypeMacros.Contains(name) || entries.Values.Any(entry => entry.NativeName == name)) throw new ArgumentException("A type converter already uses declaration macro name: " + name);
        if (!declarationMacros.TryAdd(name, kind)) throw new ArgumentException("Declaration macro is already registered: " + name);
        if (preservesInstanceLayout) layoutNeutralDeclarationMacros.Add(name);
    }
    public IReadOnlyCollection<NativeTypeConverter> Entries => entries.Values;

    public void RegisterAlias(string nativeType, string canonicalCppType, string? nativeCppType = null)
    {
        var (abi, qualifiers) = Scalar(canonicalCppType);
        Add(new() { NativeName = nativeType, NativeCppType = nativeCppType ?? NativeSpelling(nativeType), AbiCppType = abi,
            IsAlias = true, NativeQualifiers = qualifiers, ToAbi = expression => expression, ToNative = expression => expression });
    }

    public void RegisterConverter(string nativeType, string abiCppType, Func<string, string> toAbi, Func<string, string> toNative,
        string? nativeCppType = null, TypeQualifiers nativeQualifiers = TypeQualifiers.None, string? nativeDefault = null)
    {
        ArgumentNullException.ThrowIfNull(toAbi); ArgumentNullException.ThrowIfNull(toNative);
        var (abi, qualifiers) = Scalar(abiCppType);
        if (abi == "void" || qualifiers != TypeQualifiers.None) throw new ArgumentException("Value converters require a non-void, unqualified scalar wire type.");
        Add(new() { NativeName = nativeType, NativeCppType = nativeCppType ?? NativeSpelling(nativeType), AbiCppType = abi,
            ToAbi = toAbi, ToNative = toNative, NativeQualifiers = nativeQualifiers, NativeDefault = nativeDefault });
    }

    public void RegisterBodyConverter(string nativeType, string abiCppType, ConverterBodies bodies,
        string? nativeCppType = null, string? nativeDefault = null, bool existingManagedType = false)
    {
        ArgumentNullException.ThrowIfNull(bodies);
        if (string.IsNullOrWhiteSpace(bodies.ManagedType) || string.IsNullOrWhiteSpace(bodies.ManagedAbiType)) throw new ArgumentException("Body converters require managed and ABI type names.");
        ArgumentNullException.ThrowIfNull(bodies.ManagedToAbi); ArgumentNullException.ThrowIfNull(bodies.AbiToManaged);
        ArgumentNullException.ThrowIfNull(bodies.AbiToNative); ArgumentNullException.ThrowIfNull(bodies.NativeToAbi);
        var (abi, cv) = Scalar(abiCppType);
        if (abi == "void" || cv != TypeQualifiers.None) throw new ArgumentException("Body converters require an unqualified non-void scalar wire type.");
        ScalarAbi.Require(nativeType, abi, bodies.ManagedAbiType);
        Add(new() { NativeName = nativeType, NativeCppType = nativeCppType ?? NativeSpelling(nativeType), AbiCppType = abi,
            Bodies = bodies, NativeDefault = nativeDefault, ExistingManagedType = existingManagedType,
            ToAbi = _ => throw new InvalidOperationException("Body converter needs native emission context."),
            ToNative = _ => throw new InvalidOperationException("Body converter needs native emission context.") });
    }

    void Add(NativeTypeConverter converter)
    {
        string head = Tokenizer.Tokenize(converter.NativeName).FirstOrDefault(t => !t.IsTrivia).text ?? "";
        if (declarationMacros.ContainsKey(head)) throw new ArgumentException("Type converter conflicts with registered declaration macro: " + head);
        if (string.IsNullOrWhiteSpace(converter.NativeName) || string.IsNullOrWhiteSpace(converter.NativeCppType)) throw new ArgumentException("Converter native type cannot be empty.");
        if (TypeInfoPass.IsBuiltin(converter.NativeName)) throw new ArgumentException("Built-in converters cannot be replaced: " + converter.NativeName);
        if (!entries.TryAdd(Key(converter.NativeName), converter)) throw new ArgumentException("A converter is already registered for '" + converter.NativeName + "'.");
    }
    public bool TryGet(string nativeName, out NativeTypeConverter converter) => entries.TryGetValue(Key(nativeName), out converter!);

    public bool TryMatchOpaque(IReadOnlyList<Token> tokens, int start, out NativeTypeConverter? converter, out int length)
    {
        converter = null; length = 0;
        foreach (var candidate in entries.Values)
        {
            var spelling = Tokenizer.Tokenize(candidate.NativeName).Where(t => !t.IsTrivia && t.kind != Token.Kind.EndOfFile).ToArray();
            // Ordinary names use the existing lexical symbol/registration lookup.
            if (!spelling.Any(t => t.text is "(" or "<") && spelling.Count(t => t.kind == Token.Kind.Identifier) < 2) continue;
            if (spelling.Length <= length || start + spelling.Length > tokens.Count) continue;
            if (!spelling.Select(t => t.text).SequenceEqual(tokens.Skip(start).Take(spelling.Length).Select(t => t.text))) continue;
            converter = candidate; length = spelling.Length;
        }
        return converter != null;
    }
    public IEnumerable<string> FunctionTypeMacros => entries.Values.Select(e => Tokenizer.Tokenize(e.NativeName).Where(t => !t.IsTrivia).ToArray())
        .Where(t => t.Length > 1 && t[0].kind == Token.Kind.Identifier && t[1] == "(").Select(t => t[0].text);

    static (string Type, TypeQualifiers Qualifiers) Scalar(string type)
    {
        var words = type.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        TypeQualifiers qualifiers = (words.Contains("const") ? TypeQualifiers.Const : TypeQualifiers.None) | (words.Contains("volatile") ? TypeQualifiers.Volatile : TypeQualifiers.None);
        string abi = string.Join(" ", words.Where(w => w is not ("const" or "volatile")));
        if (!TypeInfoPass.IsBuiltin(abi) || abi is "long double" or "wchar_t" or "char16_t" or "char32_t")
            throw new ArgumentException("Unsupported scalar ABI type '" + type + "'. Register a converter with a supported primitive wire type.");
        return (abi, qualifiers);
    }
    static string Key(string text) => string.Join("|", Tokenizer.Tokenize(text.TrimStart(':')).Where(t => !t.IsTrivia && t.kind != Token.Kind.EndOfFile).Select(t => t.text.Length + ":" + t.text));
    static string NativeSpelling(string text) => Regex.IsMatch(text, @"^(?:::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)+$") ? "::" + text.TrimStart(':') : text;

}
