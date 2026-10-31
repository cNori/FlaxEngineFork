using VETools.Text;

namespace VETools.Generation;

internal enum EmittedBindingKind { Type, Enum, OpaqueType, Field, Property, Function, Destructor, Event }

// References only emitted declarations, not their enclosing source scopes.
// TypeInfo retains the native qualifiers that cannot be recovered from C# text.
internal sealed record EmittedParameter(string ManagedDeclaration, TypeInfo NativeType, ParameterDirection Direction, ParameterPassing Passing, string ManagedType = "", string ManagedName = "", PointerArrayInterop.Description? InputPointerArray = null)
{
    public bool CopyInOutReference => Direction == ParameterDirection.Ref &&
        NativeType is ReferenceTypeInfo { Referent: NamedTypeInfo { Converter: { SharedReference: false, Bodies.CopyReferenceParameters: true } } };
    // Only explicit readonly aliases with an established shared ABI bypass conversion.
    public bool DirectReadOnlyReference => ManagedType.StartsWith("ref readonly ", StringComparison.Ordinal) &&
        NativeType is ReferenceTypeInfo { Referent: NamedTypeInfo named } &&
        (named.Converter?.SharedReference == true || named.Converter == null && named.Resolution == TypeResolution.Builtin);
}
internal sealed record EmittedBinding(
    EmittedBindingKind Kind,
    DeclarationDescriptor Native,
    string ManagedDeclaration,
    TypeInfo? NativeType = null,
    IReadOnlyList<EmittedParameter>? Parameters = null,
    bool IsAccessorDependency = false,
    PropertyProjection? Property = null,
    FunctionTypeInfo? NativeFunction = null,
    int ManagedSourceStart = -1,
    int ManagedSourceLength = 0,
    string ManagedReturnType = "",
    NativeStructLayout? StorageLayout = null,
    ClassDescriptor? ClassBase = null,
    string ManagedTypeHeader = "",
    int ManagedTypeHeaderStart = -1,
    int ManagedTypeHeaderLength = 0,
    bool NativeAccessView = false,
    bool CopyReferenceReturn = false);

internal sealed class EmittedBindingManifest
{
    readonly List<EmittedBinding> entries = [];
    public IReadOnlyList<EmittedBinding> Entries => entries.AsReadOnly();
    internal void Add(EmittedBinding entry) => entries.Add(entry);
    internal void Clear() => entries.Clear();
}

// Stage two deliberately accepts no FileDescriptor or ExportPlan. Export and
// property decisions have already been made by C# emission.
internal static class NativeBindingPass
{
    public static IReadOnlyList<EmittedBinding> Prepare(CSharpGenerationResult managed)
    {
        if (!managed.Success) throw new InvalidOperationException("Native generation requires successful C# emission.");
        // Interfaces carry managed declarations. Calls are lowered only on a
        // concrete native receiver that provides the public interface base.
        return managed.Bindings.Entries.Where(e => !(e.Kind == EmittedBindingKind.Function && e.Native.Parent is InterfaceDescriptor)).ToArray();
    }
}
