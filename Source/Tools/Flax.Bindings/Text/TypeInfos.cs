using System.Diagnostics;
using static VETools.Tokenizer;

namespace VETools.Text;

[Flags]
internal enum TypeQualifiers { None = 0, Const = 1, Volatile = 2 }
internal enum ReferenceKind { LValue, RValue }
internal enum TypeResolution { Unresolved, Builtin, Resolved, Ambiguous, Deduced, Dependent }

// Syntax and resolution are separate. No ABI sizes or C# mappings are guessed here.
[DebuggerDisplay("{Display,nq}")]
internal abstract class TypeInfo
{
    // Alias spelling for native signatures; semantic analysis uses the resolved shape.
    public string? NativeAliasSpelling;
    public List<Token> SourceTokens = [];
    public List<Token> SpecifierTokens = [];
    public string OriginalText => string.Concat(SourceTokens.Select(t => t.text));
    public abstract string Display { get; }
    public abstract bool IsSupported { get; }
    protected static string Cv(TypeQualifiers value) =>
        ((value & TypeQualifiers.Const) != 0 ? " const" : "") +
        ((value & TypeQualifiers.Volatile) != 0 ? " volatile" : "");
}

internal sealed class NamedTypeInfo : TypeInfo
{
    public TemplateSpecializationInfo? Specialization;
    public AliasDescriptor? Alias;
    public string Name = "";
    public TypeQualifiers Qualifiers;
    public NativeTypeConverter? Converter;
    public TypeQualifiers EffectiveQualifiers => Qualifiers | (Converter?.NativeQualifiers ?? TypeQualifiers.None);
    public List<TemplateArgumentInfo> Arguments = [];
    public TypeResolution Resolution;
    public TypeSymbol? Symbol;
    public override bool IsSupported => Arguments.All(a => a.Type?.IsSupported ?? true);
    public override string Display => Name + (Arguments.Count == 0 ? "" : "<" + string.Join(", ", Arguments.Select(a => a.Display)) + ">") + Cv(Qualifiers);
}

internal sealed class PointerTypeInfo : TypeInfo
{
    public required TypeInfo Pointee;
    public TypeQualifiers Qualifiers;
    public override bool IsSupported => Pointee.IsSupported;
    public override string Display => Pointee.Display + "*" + Cv(Qualifiers);
}

internal sealed class ReferenceTypeInfo : TypeInfo
{
    public required TypeInfo Referent;
    public ReferenceKind ReferenceKind;
    public override bool IsSupported => Referent.IsSupported;
    public override string Display => Referent.Display + (ReferenceKind == ReferenceKind.LValue ? "&" : "&&");
}

internal sealed class ArrayTypeInfo : TypeInfo
{
    public required TypeInfo ElementType;
    public List<Token> ExtentTokens = [];
    public List<Token>? ExpandedExtentTokens;
    public IReadOnlyList<Token> EffectiveExtentTokens => ExpandedExtentTokens ?? ExtentTokens;
    public override bool IsSupported => ElementType.IsSupported;
    public override string Display
    {
        get
        {
            string suffix = "";
            TypeInfo element = this;
            while (element is ArrayTypeInfo array)
            {
                suffix += "[" + string.Concat(array.ExtentTokens.Select(t => t.text)) + "]";
                element = array.ElementType;
            }
            return element.Display + suffix;
        }
    }
}

internal sealed class UnsupportedTypeInfo : TypeInfo
{
    public required string Reason;
    public override bool IsSupported => false;
    public override string Display => "<unsupported: " + Reason + ">";
}

internal sealed class FunctionPointerTypeInfo : TypeInfo
{
    public required TypeInfo ReturnType;
    public List<TypeInfo> Parameters = [];
    public TypeQualifiers Qualifiers;
    public override bool IsSupported => ReturnType.IsSupported && Parameters.All(p => p.IsSupported);
    public override string Display => ReturnType.Display + " (*" + Cv(Qualifiers) + ")(" + string.Join(", ", Parameters.Select(p => p.Display)) + ")";
}

internal sealed class TemplateArgumentInfo
{
    public List<Token> Tokens = [];
    // Null means an unevaluated argument, not a guessed type/value classification.
    public TypeInfo? Type;
    public string Display => Type?.Display ?? string.Concat(Tokens.Select(t => t.text));
}

internal sealed class FunctionTypeInfo
{
    public TypeInfo? ReturnType; // Constructors/destructors have no return type.
    public List<TypeInfo> Parameters = [];
    public TypeQualifiers Qualifiers;
    public ReferenceKind? ReferenceQualifier;
    public bool HasNoexcept;
    public bool IsConstructor;
    public bool IsDestructor;
    public List<Token> SuffixTokens = [];
}

internal sealed class BaseTypeInfo
{
    public required TypeInfo Type;
    public string Access = "";
    public bool IsVirtual;
    public List<Token> SourceTokens = [];
}

internal sealed class TypeSymbol
{
    public required string QualifiedName;
    public List<DeclarationDescriptor> Declarations = [];
    public DeclarationDescriptor? Definition => Declarations.FirstOrDefault(d =>
        d is TypeDescriptor { IsForwardDeclaration: false } or EnumDescriptor { IsForwardDeclaration: false });
}

internal sealed class TemplateSpecializationInfo
{
    public TypeDescriptor? Concrete;
    public required TypeDescriptor Definition;
    public Dictionary<string, TypeInfo> TypeArguments = new(StringComparer.Ordinal);
}

internal sealed class TypeAnalysisResult
{
    public bool AutomaticFieldsReady;
    public bool StandardConvertersReady;
    public bool MarshalAsConvertersReady;
    public Dictionary<string, TypeDescriptor> TemplateInstances = new(StringComparer.Ordinal);
    public FileDescriptor? SourceFile;
    public Dictionary<string, List<AliasDescriptor>> Aliases = new(StringComparer.Ordinal);
    public HashSet<AliasDescriptor> ResolvingAliases = [];
    public TypeConverterRegistry Converters = new();
    public Dictionary<string, TypeSymbol> Symbols = new(StringComparer.Ordinal);
    public List<LogEntry> Diagnostics = [];
    public IEnumerable<LogEntry> Errors => Diagnostics.Where(entry => entry.IsError);
    public IEnumerable<LogEntry> Warnings => Diagnostics.Where(entry => entry.IsWarning && !entry.IsError);
}
