using System.Diagnostics;
using static VETools.Tokenizer;

namespace VETools.Text;

[DebuggerDisplay("{Kind,nq} {QualifiedName,nq}")]
internal abstract class DeclarationDescriptor
{
    public Token Name;
    public List<Token> NamespaceTokens = [];
    public List<Token> EnclosingTypeTokens = [];
    public List<Token> HeaderTokens = [];
    public List<List<Token>> AttributeTokens = [];
    public DeclarationDescriptor? Parent;
    public string Access = "public";
    public FileDescriptor? DiagnosticOrigin;
    public FileDescriptor? DiagnosticFile => DiagnosticOrigin ?? SourceFile;
    public string? NativeNameOverride;
    public string NativeQualifiedName => NativeNameOverride ?? (Parent is TypeDescriptor parent ? parent.NativeQualifiedName + "::" + Name.text : QualifiedName);
    public ApiMetadata Api = new();
    public string Kind => GetType().Name.Replace("Descriptor", "");
    public string QualifiedName
    {
        get
        {
            string prefix = string.Concat(NamespaceTokens.Select(token => token.text));
            string types = string.Concat(EnclosingTypeTokens.Select(token => token.text));
            if (types.Length != 0)
                prefix = prefix.Length == 0 ? types : prefix + "::" + types;
            return prefix.Length == 0 ? Name.text ?? "" : prefix + "::" + Name.text;
        }
    }
    public FileDescriptor? SourceFile
    {
        get { DeclarationDescriptor? current = this; while (current?.Parent != null) current = current.Parent; return current as FileDescriptor; }
    }
}

internal abstract class ScopeDescriptor : DeclarationDescriptor
{
    // The ordered list preserves declaration order; typed lists make inspection and generation easy.
    public List<DeclarationDescriptor> Declarations = [];
    public List<ClassDescriptor> Classes = [];
    public List<StructDescriptor> Structs = [];
    public List<InterfaceDescriptor> Interfaces = [];
    public List<FunctionDescriptor> Functions = [];
    public List<FieldDescriptor> Fields = [];
    public List<EnumDescriptor> Enums = [];
    public List<AliasDescriptor> Aliases = [];

    public void Add(DeclarationDescriptor declaration)
    {
        declaration.Parent = this;
        Declarations.Add(declaration);
        switch (declaration)
        {
            case ClassDescriptor value: Classes.Add(value); break;
            case StructDescriptor value: Structs.Add(value); break;
            case InterfaceDescriptor value: Interfaces.Add(value); break;
            case FunctionDescriptor value: Functions.Add(value); break;
            case FieldDescriptor value: Fields.Add(value); break;
            case EnumDescriptor value: Enums.Add(value); break;
            case AliasDescriptor value: Aliases.Add(value); break;
        }
    }
}

internal sealed class FileDescriptor : ScopeDescriptor
{
    public int CppStandard = 20;
    public TypeConverterRegistry Converters = new();
    public List<MacroDefinition> MacroDefinitions = [];
    public string? Path;
    public string? IncludeNamespace;
    public string SourceText = "";
    public string OriginalSourceText = "";
    public TypeAnalysisResult? TypeInfos;
    public BindingAnalysisResult? Bindings;
    public List<Token> Tokens = [];
    public List<LogEntry> Errors = [];
    public IReadOnlyList<FileDescriptor> ProjectFiles = [];
    public IReadOnlySet<ClassDescriptor> ProjectClassLifetimes = new HashSet<ClassDescriptor>();
}

internal abstract class TypeDescriptor : ScopeDescriptor
{
    public NativeTypeConverter? AutomaticValueConverter;
    public bool CanonicalManagedValue;
    public TypeInfo? MarshalAsType;
    public string? MarshalAsError;
    public bool NativeFieldView;
    public TemplateSpecializationInfo? ConcreteTemplate;
    public List<TemplateParameterDescriptor> TemplateParameters = [];
    public bool RequiresNativeFieldAccess;
    public Token Keyword;
    public TypeSymbol? TypeSymbol;
    public bool IsFinal;
    public bool IsForwardDeclaration;
    public List<Token> BaseTokens = [];
    public List<BaseTypeInfo> BaseTypes = [];
    public List<Token> Body = [];
    public string DefaultAccess => this is ClassDescriptor ? "private" : "public";
}

internal sealed class ClassDescriptor : TypeDescriptor { }
internal sealed class StructDescriptor : TypeDescriptor { }
internal sealed class InterfaceDescriptor : TypeDescriptor { }

internal sealed class FunctionDescriptor : DeclarationDescriptor
{
    public bool IsTemplate;
    public List<TemplateParameterDescriptor> TemplateParameters = [];
    public FieldDescriptor? AccessedField;
    public bool IsFieldSetter;
    public bool GeneratedStatic;
    public bool NativeDefaultFactory;
    public InterfaceDescriptor? NativeDispatchInterface;
    public List<Token> NameTokens = [];
    public FunctionTypeInfo? TypeInfo;
    public FunctionBindingInfo? Binding;
    public List<Token> ParameterTokens = [];
    public List<Token> Body = [];
    // Function fields are its parameter declarations. Types are kept as raw tokens for now.
    public List<FieldDescriptor> Fields = [];
    public bool IsDeclaration => Body.Count == 0;
}

internal sealed class FieldDescriptor : DeclarationDescriptor
{
    public List<Token> TypeTokens = [];
    public TypeInfo? TypeInfo;
    public ParameterBindingInfo? Binding;
    public List<Token> DeclaratorTokens = [];
    public List<Token> InitializerTokens = [];
    // Expanded at the declaration's preprocessing location; the original remains intact.
    public List<Token>? ExpandedInitializerTokens;
    public List<List<Token>?> ExpandedArrayExtents = [];
    // Synthetic NoArray projections retain the original native array declaration.
    public FieldDescriptor? NativeArrayField;
    public int NativeArrayIndex;
    public int NativeArrayCount;
    public bool IsBitField;
    public List<Token> BitWidthTokens = [];
    public bool IsParameter;
    public bool IsUnnamed => string.IsNullOrEmpty(Name.text);
    public int Index;
}

internal sealed class EnumDescriptor : DeclarationDescriptor
{
    public List<EnumMemberDescriptor> Members = [];
    public bool IsClass;
    public TypeSymbol? TypeSymbol;
    public TypeInfo? UnderlyingType;
    public bool IsStruct;
    public bool IsScoped => IsClass || IsStruct;
    public bool IsForwardDeclaration;
    public List<Token> UnderlyingTypeTokens = [];
    public List<Token> Body = [];
}

internal sealed class EnumMemberDescriptor : DeclarationDescriptor
{
    public List<Token> ValueTokens = [];
    public List<Token>? ExpandedValueTokens;
}

// Keep declarations that this pass doesn't interpret, rather than manufacturing a wrong field type.
internal sealed class RawDeclarationDescriptor : DeclarationDescriptor { }
internal sealed class AliasDescriptor : DeclarationDescriptor
{
    public List<TemplateParameterDescriptor> TemplateParameters = [];
    public List<Token> TargetTokens = [];
    public TypeInfo? TypeInfo;
}

internal sealed class TemplateParameterDescriptor
{
    public Token Name;
    public bool IsType, IsPack;
    public List<Token> Tokens = [], DefaultTokens = [];
}


