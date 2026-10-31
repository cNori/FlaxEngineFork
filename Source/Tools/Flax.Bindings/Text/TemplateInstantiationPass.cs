using System.Security.Cryptography;
using System.Text;
using static VETools.Tokenizer;

namespace VETools.Text;

// Materialize resolved type-parameter templates. This does not establish an ABI
// for a union, dependent expression, partial specialization or arbitrary class.
internal sealed class TemplateInstantiationPass
{
    readonly Dictionary<string, TypeDescriptor> instances = new(StringComparer.Ordinal);
    readonly Dictionary<string, AliasDescriptor> aliases = new(StringComparer.Ordinal);
    readonly HashSet<TypeDescriptor> active = [];

    public static void Run(IReadOnlyList<FileDescriptor> files)
    {
        var pass = new TemplateInstantiationPass();
        // Prefer a named concrete typedef over a transparent alias; equivalent
        // aliases map to the same descriptor, independent of file traversal order.
        foreach (var alias in files.SelectMany(Declarations).OfType<AliasDescriptor>()
            .Where(a => a.TemplateParameters.Count == 0 && a.Api.Export && a.TypeInfo is NamedTypeInfo { Specialization: not null })
            .OrderBy(a => a.Api.Alias).ThenBy(a => a.QualifiedName, StringComparer.Ordinal))
            if (alias.TypeInfo is NamedTypeInfo named && Key(named) is { } key) pass.aliases.TryAdd(key, alias);
        foreach (var declaration in files.SelectMany(Declarations))
            foreach (var info in Types(declaration)) pass.Materialize(info);
        foreach (var file in files) file.TypeInfos!.TemplateInstances = pass.instances;
    }

    public static IEnumerable<DeclarationDescriptor> Declarations(ScopeDescriptor scope)
    {
        foreach (var declaration in scope.Declarations)
        {
            yield return declaration;
            if (declaration is ScopeDescriptor child)
                foreach (var nested in Declarations(child)) yield return nested;
        }
    }

    public static IEnumerable<TypeInfo> Types(DeclarationDescriptor declaration) => declaration switch
    {
        FieldDescriptor { TypeInfo: { } info } => [info],
        AliasDescriptor { TemplateParameters.Count: 0, TypeInfo: { } info } => [info],
        FunctionDescriptor { TypeInfo: { } function } => (function.ReturnType == null ? Enumerable.Empty<TypeInfo>() : [function.ReturnType]).Concat(function.Parameters),
        TypeDescriptor type => type.BaseTypes.Select(b => b.Type).Concat(type.MarshalAsType == null ? [] : new[] { type.MarshalAsType }),
        _ => [],
    };

    public static IEnumerable<TypeDescriptor> ConcreteReferences(TypeInfo? type)
    {
        if (type is NamedTypeInfo { Specialization.Concrete: { } concrete }) yield return concrete;
        foreach (var child in Children(type)) foreach (var nested in ConcreteReferences(child)) yield return nested;
    }

    static IEnumerable<TypeInfo> Children(TypeInfo? type) => type switch
    {
        NamedTypeInfo named => named.Arguments.Select(a => a.Type).OfType<TypeInfo>(),
        ReferenceTypeInfo reference => [reference.Referent],
        PointerTypeInfo pointer => [pointer.Pointee],
        ArrayTypeInfo array => [array.ElementType],
        FunctionPointerTypeInfo function => new[] { function.ReturnType }.Concat(function.Parameters),
        _ => [],
    };

    static string? Key(NamedTypeInfo named)
    {
        if (named.Specialization == null) return null;
        var arguments = named.Arguments.Select(a => BindingSignaturePass.Canonical(a.Type, false)).ToArray();
        return arguments.Any(a => a == null) ? null : named.Specialization.Definition.QualifiedName + "<" + string.Join(",", arguments) + ">";
    }

    void Materialize(TypeInfo? type)
    {
        foreach (var child in Children(type)) Materialize(child);
        if (type is not NamedTypeInfo { Converter: null, Specialization: { } specialization } named || Key(named) is not { } key) return;
        if (instances.TryGetValue(key, out var cached)) { specialization.Concrete = cached; return; }
        // Expanding dependent recursive templates without bounds can be infinite.
        if (active.Count >= 32 || instances.Count >= 2048) return;
        var prototype = specialization.Definition;
        aliases.TryGetValue(key, out var alias);
        var name = prototype.Name;
        name.text = alias?.Name.text ?? "__" + prototype.Name.text + "_" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key)))[..12];
        TypeDescriptor concrete = prototype switch { StructDescriptor => new StructDescriptor(), InterfaceDescriptor => new InterfaceDescriptor(), _ => new ClassDescriptor() };
        Common(prototype, concrete, specialization.TypeArguments);
        concrete.Name = name;
        concrete.Parent = alias?.Parent ?? prototype.Parent;
        concrete.NamespaceTokens = new(alias?.NamespaceTokens ?? prototype.NamespaceTokens);
        concrete.EnclosingTypeTokens = new(alias?.EnclosingTypeTokens ?? prototype.EnclosingTypeTokens);
        concrete.Api.Template = false;
        if (alias?.Api.InBuild == true) concrete.Api.InBuild = true;
        if (alias?.Api.ManagedNamespace != null) concrete.Api.ManagedNamespace = alias.Api.ManagedNamespace;
        concrete.ConcreteTemplate = specialization;
        concrete.NativeNameOverride = prototype.NativeQualifiedName + "<" + string.Join(", ", named.Arguments.Select(a => Native(a.Type!))) + ">";
        concrete.Keyword = prototype.Keyword; concrete.IsFinal = prototype.IsFinal;
        concrete.MarshalAsType = prototype.MarshalAsType == null ? null : Substitute(prototype.MarshalAsType, specialization.TypeArguments, prototype, concrete);
        concrete.RequiresNativeFieldAccess = prototype.RequiresNativeFieldAccess;
        concrete.TypeSymbol = new() { QualifiedName = concrete.QualifiedName, Declarations = [concrete] };
        instances.Add(key, concrete); specialization.Concrete = concrete; active.Add(concrete);
        foreach (var item in prototype.Declarations) concrete.Add(Clone(item, specialization.TypeArguments, prototype, concrete));
        foreach (var basis in prototype.BaseTypes) concrete.BaseTypes.Add(new() { Type = Substitute(basis.Type, specialization.TypeArguments, prototype, concrete), Access = basis.Access, IsVirtual = basis.IsVirtual, SourceTokens = Tokens(basis.SourceTokens, specialization.TypeArguments) });
        concrete.BaseTokens = Tokens(prototype.BaseTokens, specialization.TypeArguments);
        concrete.Body = Tokens(prototype.Body, specialization.TypeArguments);
        // Preserve all union storage as raw declarations. Extract only exported
        // anonymous members for metadata/accessor generation; never assign offsets.
        ExtractAnonymousFields(prototype, concrete, specialization.TypeArguments);
        foreach (var member in Declarations(concrete)) foreach (var info in Types(member)) Materialize(info);
        active.Remove(concrete);
    }

    DeclarationDescriptor Clone(DeclarationDescriptor original, IReadOnlyDictionary<string, TypeInfo> bindings, TypeDescriptor prototype, TypeDescriptor concrete)
    {
        DeclarationDescriptor copy = original switch
        {
            FieldDescriptor field => new FieldDescriptor { TypeTokens = Tokens(field.TypeTokens, bindings), DeclaratorTokens = Tokens(field.DeclaratorTokens, bindings), InitializerTokens = Tokens(field.InitializerTokens, bindings), ExpandedInitializerTokens = field.ExpandedInitializerTokens == null ? null : Tokens(field.ExpandedInitializerTokens, bindings), IsBitField = field.IsBitField, BitWidthTokens = Tokens(field.BitWidthTokens, bindings), TypeInfo = field.TypeInfo == null ? null : Substitute(field.TypeInfo, bindings, prototype, concrete), IsParameter = field.IsParameter, Index = field.Index },
            FunctionDescriptor function => new FunctionDescriptor { IsTemplate = function.IsTemplate, TemplateParameters = new(function.TemplateParameters), NameTokens = new(function.NameTokens), ParameterTokens = Tokens(function.ParameterTokens, bindings), Body = Tokens(function.Body, bindings), GeneratedStatic = function.GeneratedStatic,
                TypeInfo = function.TypeInfo == null ? null : new() { ReturnType = function.TypeInfo.ReturnType == null ? null : Substitute(function.TypeInfo.ReturnType, bindings, prototype, concrete), Parameters = function.TypeInfo.Parameters.Select(t => Substitute(t, bindings, prototype, concrete)).ToList(), Qualifiers = function.TypeInfo.Qualifiers, ReferenceQualifier = function.TypeInfo.ReferenceQualifier, HasNoexcept = function.TypeInfo.HasNoexcept, IsConstructor = function.TypeInfo.IsConstructor, IsDestructor = function.TypeInfo.IsDestructor, SuffixTokens = Tokens(function.TypeInfo.SuffixTokens, bindings) } },
            AliasDescriptor alias => new AliasDescriptor { TemplateParameters = new(alias.TemplateParameters), TargetTokens = Tokens(alias.TargetTokens, bindings), TypeInfo = alias.TypeInfo == null ? null : Substitute(alias.TypeInfo, bindings, prototype, concrete) },
            StructDescriptor => new StructDescriptor(), ClassDescriptor => new ClassDescriptor(), InterfaceDescriptor => new InterfaceDescriptor(),
            EnumDescriptor enumeration => new EnumDescriptor { IsClass = enumeration.IsClass, IsStruct = enumeration.IsStruct, IsForwardDeclaration = enumeration.IsForwardDeclaration, UnderlyingTypeTokens = Tokens(enumeration.UnderlyingTypeTokens, bindings), Body = Tokens(enumeration.Body, bindings), UnderlyingType = enumeration.UnderlyingType == null ? null : Substitute(enumeration.UnderlyingType, bindings, prototype, concrete) },
            EnumMemberDescriptor member => new EnumMemberDescriptor {ValueTokens=Tokens(member.ValueTokens,bindings), ExpandedValueTokens=member.ExpandedValueTokens == null ? null : Tokens(member.ExpandedValueTokens,bindings)},
            _ => new RawDeclarationDescriptor(),
        };
        Common(original, copy, bindings);
        copy.NamespaceTokens = new(concrete.NamespaceTokens);
        copy.EnclosingTypeTokens = new(concrete.EnclosingTypeTokens);
        if (copy.EnclosingTypeTokens.Count != 0) copy.EnclosingTypeTokens.AddRange(Tokenizer.Tokenize("::").Where(t => t.kind != Token.Kind.EndOfFile));
        copy.EnclosingTypeTokens.Add(concrete.Name);
        if (copy is FunctionDescriptor result && original is FunctionDescriptor source)
        {
            if (result.TypeInfo?.IsConstructor == true) result.Name.text = concrete.Name.text;
            foreach (var field in source.Fields) { var parameter = (FieldDescriptor)Clone(field, bindings, prototype, concrete); parameter.Parent = result; result.Fields.Add(parameter); }
        }
        if (copy is TypeDescriptor nested && original is TypeDescriptor sourceType)
        {
            var inner = bindings.Where(p => !sourceType.TemplateParameters.Any(t => t.Name.text == p.Key)).ToDictionary(p => p.Key, p => p.Value);
            nested.Keyword = sourceType.Keyword; nested.IsFinal = sourceType.IsFinal; nested.IsForwardDeclaration = sourceType.IsForwardDeclaration;
            nested.MarshalAsType = sourceType.MarshalAsType == null ? null : Substitute(sourceType.MarshalAsType, inner, prototype, concrete);
            nested.TemplateParameters = new(sourceType.TemplateParameters); nested.RequiresNativeFieldAccess = sourceType.RequiresNativeFieldAccess;
            nested.Body = Tokens(sourceType.Body, inner); nested.BaseTokens = Tokens(sourceType.BaseTokens, inner);
            nested.TypeSymbol = new() { QualifiedName = nested.QualifiedName, Declarations = [nested] };
            foreach (var child in sourceType.Declarations) nested.Add(Clone(child, inner, prototype, concrete));
        }
        if(copy is EnumDescriptor clonedEnum && original is EnumDescriptor sourceEnum)
        {
            foreach(var member in sourceEnum.Members)
            {
                var cloned=(EnumMemberDescriptor)Clone(member,bindings,prototype,concrete);
                cloned.Parent=clonedEnum;
                cloned.EnclosingTypeTokens=new(clonedEnum.EnclosingTypeTokens);
                if(cloned.EnclosingTypeTokens.Count>0)cloned.EnclosingTypeTokens.AddRange(Tokenizer.Tokenize("::").Where(t=>t.kind!=Token.Kind.EndOfFile));
                cloned.EnclosingTypeTokens.Add(clonedEnum.Name);
                clonedEnum.Members.Add(cloned);
            }
        }
        return copy;
    }

    TypeInfo Substitute(TypeInfo info, IReadOnlyDictionary<string, TypeInfo> bindings, TypeDescriptor prototype, TypeDescriptor concrete)
    {
        TypeInfo copy;
        switch (info)
        {
            case NamedTypeInfo named when bindings.TryGetValue(named.Name, out var argument) && named.Arguments.Count == 0:
                copy = Substitute(argument, new Dictionary<string, TypeInfo>(), prototype, concrete);
                Qualify(copy, named.Qualifiers); break;
            case NamedTypeInfo named:
                var value = new NamedTypeInfo { Name = named.Name, Qualifiers = named.Qualifiers, Converter = named.Converter, Alias = named.Alias, Resolution = named.Resolution, Symbol = named.Symbol,
                    Arguments = named.Arguments.Select((a, index) => new TemplateArgumentInfo { Tokens = Tokens(a.Tokens, bindings), Type = a.Type == null
                        ? named.Symbol?.Definition is TypeDescriptor definition && index < definition.TemplateParameters.Count && definition.TemplateParameters[index].IsType && !definition.TemplateParameters[index].IsPack
                            ? TypeInfoPass.Parse(Tokens(a.Tokens, bindings), prototype, prototype.SourceFile!.TypeInfos!) : null
                        : Substitute(a.Type, bindings, prototype, concrete) }).ToList() };
                if (value.Symbol?.Definition is TypeDescriptor { Api.Template: true } definition && definition.TemplateParameters.Count == value.Arguments.Count && definition.TemplateParameters.All(p => p.IsType && !p.IsPack) && value.Arguments.All(a => a.Type != null))
                    value.Specialization = new() { Definition = definition, TypeArguments = definition.TemplateParameters.Select((p, i) => (p.Name.text, value.Arguments[i].Type!)).ToDictionary(p => p.text, p => p.Item2) };
                else if (value.Symbol == prototype.TypeSymbol && value.Arguments.Count == 0) { value.Symbol = concrete.TypeSymbol; value.Name = concrete.Name.text; }
                copy = value; break;
            case PointerTypeInfo pointer: copy = new PointerTypeInfo { Pointee = Substitute(pointer.Pointee, bindings, prototype, concrete), Qualifiers = pointer.Qualifiers }; break;
            case ReferenceTypeInfo reference: copy = new ReferenceTypeInfo { Referent = Substitute(reference.Referent, bindings, prototype, concrete), ReferenceKind = reference.ReferenceKind }; break;
            case ArrayTypeInfo array: copy = new ArrayTypeInfo { ElementType = Substitute(array.ElementType, bindings, prototype, concrete), ExtentTokens = Tokens(array.ExtentTokens, bindings), ExpandedExtentTokens = array.ExpandedExtentTokens == null ? null : Tokens(array.ExpandedExtentTokens, bindings) }; break;
            case FunctionPointerTypeInfo function: copy = new FunctionPointerTypeInfo { ReturnType = Substitute(function.ReturnType, bindings, prototype, concrete), Parameters = function.Parameters.Select(t => Substitute(t, bindings, prototype, concrete)).ToList(), Qualifiers = function.Qualifiers }; break;
            default: copy = new UnsupportedTypeInfo { Reason = ((UnsupportedTypeInfo)info).Reason }; break;
        }
        copy.SourceTokens = Tokens(info.SourceTokens, bindings); copy.SpecifierTokens = new(info.SpecifierTokens);
        copy.NativeAliasSpelling = info.NativeAliasSpelling;
        return copy;
    }

    static void Qualify(TypeInfo info, TypeQualifiers qualifiers)
    {
        if (info is NamedTypeInfo named) named.Qualifiers |= qualifiers;
        else if (info is PointerTypeInfo pointer) pointer.Qualifiers |= qualifiers;
        else if (info is ArrayTypeInfo array) Qualify(array.ElementType, qualifiers);
    }

    static List<Token> Tokens(IEnumerable<Token> tokens, IReadOnlyDictionary<string, TypeInfo> bindings)
    {
        List<Token> output = [];
        foreach (var token in tokens)
            if (token.kind == Token.Kind.Identifier && bindings.TryGetValue(token.text, out var argument))
                foreach (var part in Tokenizer.Tokenize(Native(argument)).Where(t => t.kind != Token.Kind.EndOfFile))
                { var replacement = part; replacement.line = token.line; replacement.col = token.col; output.Add(replacement); }
            else output.Add(token);
        return output;
    }

    static void Common(DeclarationDescriptor source, DeclarationDescriptor target, IReadOnlyDictionary<string, TypeInfo> bindings)
    {
        target.Name = source.Name; target.Access = source.Access;
        target.DiagnosticOrigin = source.DiagnosticFile;
        target.HeaderTokens = Tokens(source.HeaderTokens, bindings); target.AttributeTokens = source.AttributeTokens.Select(t => Tokens(t, bindings)).ToList();
        target.NamespaceTokens = new(source.NamespaceTokens); target.EnclosingTypeTokens = new(source.EnclosingTypeTokens);
        var a = source.Api;
        target.Api = new() { Export = a.Export, Property = a.Property, Static = a.Static, Abstract = a.Abstract, Flags = a.Flags, Template = a.Template, Alias = a.Alias, Direction = a.Direction,
            Sealed = a.Sealed, NoDefault = a.NoDefault, NoSpawn = a.NoSpawn, NoProxy = a.NoProxy, NoConstructor = a.NoConstructor, InBuild = a.InBuild, NoArray = a.NoArray, Event = a.Event, MarshalAsTokens = Tokens(a.MarshalAsTokens, bindings), DefaultValueTokens = a.DefaultValueTokens == null ? null : Tokens(a.DefaultValueTokens, bindings), ManagedAccess = a.ManagedAccess, ManagedNamespace = a.ManagedNamespace, ManagedAttributes = new(a.ManagedAttributes),
            ManagedName = a.ManagedName, ExtensionTags = new(a.ExtensionTags, StringComparer.Ordinal), InjectedCode = new(a.InjectedCode), Hidden = a.Hidden,
            Invocations = new(a.Invocations), Errors = new(a.Errors), Warnings = new(a.Warnings) };
        target.Api.Binding.ReadOnly = a.Binding.ReadOnly;
    }

    void ExtractAnonymousFields(TypeDescriptor prototype, TypeDescriptor concrete, IReadOnlyDictionary<string, TypeInfo> bindings)
    {
        // Anonymous union/struct bodies are currently opaque in the first pass.
        // Keep their nested exported field declarations for later native accessors.
        foreach (var raw in prototype.Declarations.OfType<RawDeclarationDescriptor>())
        {
            var tokens = raw.HeaderTokens;
            for (int i = 0; i + 1 < tokens.Count; i++)
            {
                if (tokens[i] != "struct") continue;
                int start = i + 1; while (start < tokens.Count && tokens[start].IsTrivia) start++;
                if (start >= tokens.Count || tokens[start] != "{") continue;
                int end = start, depth = 0;
                do { if (tokens[end] == "{") depth++; if (tokens[end] == "}") depth--; end++; } while (end < tokens.Count && depth != 0);
                if (depth != 0) continue;
                var parsed = Parshe.PreParse(new Tokenizer.Stream(tokens.GetRange(start + 1, end - start - 2)), new(prototype.NamespaceTokens), prototype.SourceFile?.CppStandard ?? 20, prototype.SourceFile?.Converters);
                TypeInfoPass.ResolveTemplateMembers(parsed, prototype.SourceFile!.TypeInfos!);
                foreach (var field in parsed.Fields)
                {
                    field.Access = raw.Access;
                    // The opaque outer parse can leave an unexported unsupported
                    // declarator with the same final identifier. Preserve it as raw
                    // syntax instead of letting it hide a real anonymous member.
                    foreach (var placeholder in concrete.Fields.Where(f => f.Name.text == field.Name.text && !f.Api.Export && f.TypeInfo is UnsupportedTypeInfo).ToArray())
                    {
                        concrete.Fields.Remove(placeholder); concrete.Declarations.Remove(placeholder);
                        concrete.Add(new RawDeclarationDescriptor { HeaderTokens = new(placeholder.HeaderTokens), Parent = concrete, DiagnosticOrigin = placeholder.DiagnosticFile });
                    }
                    if (!concrete.Fields.Any(f => f.Name.text == field.Name.text)) concrete.Add(Clone(field, bindings, prototype, concrete));
                }
                i = end - 1;
            }
        }
        // These annotations belonged to the newly parsed anonymous fields, not
        // to the opaque union declaration. Preserve every other raw/API error.
        var handled = concrete.Fields.Where(f => f.Api.Export).SelectMany(f => f.Api.Invocations)
            .Where(i => i.ApiName == "API_FIELD").Select(i => (i.Name.line, i.Name.col)).ToHashSet();
        foreach (var raw in concrete.Declarations.OfType<RawDeclarationDescriptor>())
            raw.Api.Errors.RemoveAll(error => error.Message == "API_FIELD is unsupported at this declaration (RawDeclaration)." && handled.Contains((error.Where.line, error.Where.col)));
    }

    public static string Native(TypeInfo type) => type switch
    {
        NamedTypeInfo named => (named.Symbol?.Definition is TypeDescriptor { ConcreteTemplate: not null } concrete ? "::" + concrete.NativeQualifiedName : named.Symbol != null ? "::" + named.Symbol.QualifiedName : named.Name) +
            (named.Arguments.Count == 0 ? "" : "<" + string.Join(", ", named.Arguments.Select(a => a.Type == null ? a.Display : Native(a.Type))) + ">") + Cv(named.Qualifiers),
        PointerTypeInfo pointer => Native(pointer.Pointee) + "*" + Cv(pointer.Qualifiers),
        ReferenceTypeInfo reference => Native(reference.Referent) + (reference.ReferenceKind == ReferenceKind.LValue ? "&" : "&&"),
        ArrayTypeInfo array => Native(array.ElementType) + "[" + string.Concat(array.ExtentTokens.Select(t => t.text)) + "]",
        _ => type.Display,
    };
    static string Cv(TypeQualifiers qualifiers) => ((qualifiers & TypeQualifiers.Const) != 0 ? " const" : "") + ((qualifiers & TypeQualifiers.Volatile) != 0 ? " volatile" : "");
}
