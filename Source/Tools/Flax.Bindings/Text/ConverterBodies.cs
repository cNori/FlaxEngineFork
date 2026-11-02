namespace VETools.Text;

// Names are supplied by the emitter; callbacks must not invent shared local names.
// Completed means the conversion expression was consumed successfully.
// CallSucceeded records native-call success separately from copyback/conversion success.
internal sealed record ConverterEmitContext(string Value, ParameterDirection? Direction,
    Func<string, string> Unique, string Completed, string CallSucceeded, string Support = "", Func<string, string>? RetainManaged = null);

internal sealed record ConverterBody(string Expression, string Declarations = "", string Setup = "", string Cleanup = "");

internal sealed class ConverterBodies
{
    // File-scope dependencies are collected once, separately from helper bodies.
    public NativeSupportRequirements NativeRequirements { get; init; } = new();
    // Optional helper declarations; {Support} is replaced by an emitter-owned unique name.
    public string NativeSupport { get; init; } = "";
    public string ManagedSupport { get; init; } = "";
    // Automatic value converters belong to their generated partial value type.
    // Custom/shared helpers without an owner are emitted in the Interop namespace.
    public string? ManagedSupportOwner { get; init; }
    public Func<string,string>? WrapManagedSupport { get; init; }
    string managedType = "";
    public required string ManagedType { get => managedType; init => managedType = value; }
    public required string ManagedAbiType { get; init; }
    // Explicit native/managed inline representation contract, separate from the
    // transport packet. Never infer native object size from ManagedAbiType.
    public (int Size, int Alignment)? NativeInlineStorage { get; init; }
    // Return a C# optional-parameter constant for a known native default, or null.
    // Runtime values (for example Quaternion::Identity) require overload lowering.
    public Func<string, string?>? ManagedOptionalDefault { get; init; }
    // Materialize a native reference return as a managed value rather than exposing a borrowed ref.
    bool copyReferenceReturns;
    public bool CopyReferenceReturns { get => copyReferenceReturns; init => copyReferenceReturns = value; }
    // Read-only reference to a native-owned inline collection. Elements become
    // engine identities; never instantiate, copy or destroy them in glue.
    public bool BorrowedObjectElements { get; init; }
    public bool BorrowedObjectViewReturn { get; init; }
    // Writable ref uses an independent native value and copies fields back on success.
    public bool CopyReferenceParameters { get; init; }
    // Explicit settings DTO: only selected fields cross; native field setters
    // preserve unselected runtime state. General ref copies are not allowed.
    public bool SelectedFieldsSnapshot { get; init; }
    public Func<string, string, string>? AssignNativeValue { get; init; }
    // Explicit opt-in: the scalar wire value contains the complete callback
    // payload, never a borrowed address/handle. Both directions must be pure
    // expressions without temporary storage, helper dependencies or cleanup.
    public bool InlineCallbackValue { get; init; }
    // Native owns the callback packet until return; managed decoding must produce an independent snapshot.
    public bool CopiedCallbackValue { get; init; }
    // Construct an independent native value and pass it as an rvalue input.
    public bool MoveInputs { get; init; }
    // Optional owning-packet release hooks for composition inside containers.
    public Func<ConverterEmitContext, string>? ReleaseManagedAbi { get; init; }
    public Func<ConverterEmitContext, string>? ReleaseNativeAbi { get; init; }
    Func<ConverterEmitContext, ConverterBody> managedToAbi = null!;
    public required Func<ConverterEmitContext, ConverterBody> ManagedToAbi { get => managedToAbi; init => managedToAbi = value; }
    Func<ConverterEmitContext, ConverterBody> abiToManaged = null!;
    public required Func<ConverterEmitContext, ConverterBody> AbiToManaged { get => abiToManaged; init => abiToManaged = value; }
    public ConverterBodies ProjectManagedValue(string target)
    {
        var result = (ConverterBodies)MemberwiseClone();
        result.managedType = target;
        result.copyReferenceReturns = true;
        result.managedToAbi = context => ManagedToAbi(context with { Value = "(" + ManagedType + ")(" + context.Value + ")" });
        result.abiToManaged = context => {
            var body = AbiToManaged(context);
            return body with { Expression = "(" + target + ")(" + body.Expression + ")" };
        };
        return result;
    }
    public required Func<ConverterEmitContext, ConverterBody> AbiToNative { get; init; }
    public required Func<ConverterEmitContext, ConverterBody> NativeToAbi { get; init; }
}

internal sealed class NativeSupportRequirements
{
    public IReadOnlyList<string> Includes { get; init; } = [];
    public IReadOnlyList<string> Libraries { get; init; } = [];
    public IReadOnlyList<string> Declarations { get; init; } = [];
    public IReadOnlyList<StructDescriptor> PrivateScalarLayouts { get; init; } = [];
    public IReadOnlyDictionary<string, string> EnumLayoutChecks { get; init; } = new Dictionary<string, string>();
    public static NativeSupportRequirements Merge(IEnumerable<NativeSupportRequirements> inputs)
    {
        var items = inputs.ToArray();
        var enumChecks = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var check in items.SelectMany(i => i.EnumLayoutChecks))
        {
            if (enumChecks.TryGetValue(check.Key, out string? previous) && previous != check.Value)
                throw new InvalidOperationException("Conflicting enum ABI guesses for " + check.Key);
            enumChecks[check.Key] = check.Value;
        }
        return new() {
            Includes = items.SelectMany(i => i.Includes).Distinct(StringComparer.Ordinal).ToArray(),
            Libraries = items.SelectMany(i => i.Libraries).Distinct(StringComparer.Ordinal).ToArray(),
            Declarations = items.SelectMany(i => i.Declarations).Distinct(StringComparer.Ordinal).ToArray(),
            EnumLayoutChecks = enumChecks,
            PrivateScalarLayouts = items.SelectMany(i => i.PrivateScalarLayouts).Distinct().ToArray()
        };
    }
}
