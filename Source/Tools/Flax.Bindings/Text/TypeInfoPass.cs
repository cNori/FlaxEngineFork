using static VETools.Tokenizer;

namespace VETools.Text;

internal static class TypeInfoPass
{
    static readonly HashSet<string> Specifiers = new(StringComparer.Ordinal)
    { "static", "inline", "constexpr", "consteval", "constinit", "extern", "mutable", "virtual", "explicit", "friend", "register", "thread_local" };
    static readonly HashSet<string> Builtins = new(StringComparer.Ordinal)
    { "void", "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "short int", "signed short", "signed short int", "unsigned short", "unsigned short int", "int", "signed", "signed int", "unsigned", "unsigned int", "long", "long int", "signed long", "signed long int", "unsigned long", "unsigned long int", "long long", "long long int", "signed long long", "signed long long int", "unsigned long long", "unsigned long long int", "float", "double", "long double" };
    static string BuiltinKey(string name) => string.Join(" ", name.Split(' ', StringSplitOptions.RemoveEmptyEntries).OrderBy(x => x, StringComparer.Ordinal));
    static readonly HashSet<string> BuiltinKeys = new(Builtins.Select(BuiltinKey), StringComparer.Ordinal);
    internal static bool IsBuiltin(string name) => BuiltinKeys.Contains(BuiltinKey(name));

    public static TypeAnalysisResult Run(FileDescriptor file)
    {
        RunProject(file.ProjectFiles.Count == 0 ? [file] : file.ProjectFiles);
        return file.TypeInfos!;
    }

    // Parse all files first, collect symbols once, then resolve each file against that shared index.
    public static void RunProject(IEnumerable<FileDescriptor> inputs)
    {
        var files = inputs.Distinct().ToArray();
        TypeAnalysisResult index = new();
        foreach (var file in files) Collect(file, index);
        HashSet<AliasDescriptor> resolving = [];
        foreach (var file in files)
        {
            file.ProjectFiles = files;
            file.TypeInfos = new() { SourceFile = file, Symbols = index.Symbols, Aliases = index.Aliases, Converters = file.Converters, ResolvingAliases = resolving };
            file.Bindings = null;
        }
        // Inherited type names need the complete base graph before field parsing,
        // regardless of source/project enumeration order. The normal visit emits
        // diagnostics once; this header-only prepass just establishes links.
        foreach (var file in files) PrepareBases(file, file.TypeInfos!);
        foreach (var file in files) Visit(file, file.TypeInfos!);
        TemplateInstantiationPass.Run(files);
        foreach (var file in files) BindingSignaturePass.Run(file);
    }

    static void Collect(ScopeDescriptor scope, TypeAnalysisResult result)
    {
        foreach (var declaration in scope.Declarations)
        {
            if (declaration is AliasDescriptor alias)
            {
                if (!result.Aliases.TryGetValue(alias.QualifiedName, out var aliases)) result.Aliases.Add(alias.QualifiedName, aliases = []);
                aliases.Add(alias);
            }
            if (declaration is TypeDescriptor or EnumDescriptor && !string.IsNullOrEmpty(declaration.Name.text))
            {
                if (!result.Symbols.TryGetValue(declaration.QualifiedName, out var symbol))
                    result.Symbols.Add(declaration.QualifiedName, symbol = new() { QualifiedName = declaration.QualifiedName });
                symbol.Declarations.Add(declaration);
                if (declaration is TypeDescriptor type) type.TypeSymbol = symbol;
                else ((EnumDescriptor)declaration).TypeSymbol = symbol;
            }
            if (declaration is ScopeDescriptor child) Collect(child, result);
        }
    }

    static void PrepareBases(ScopeDescriptor scope, TypeAnalysisResult result)
    {
        int diagnostics = result.Diagnostics.Count;
        foreach (var declaration in scope.Declarations)
        {
            if (declaration is TypeDescriptor type) Bases(type, result);
            if (declaration is ScopeDescriptor child) PrepareBases(child, result);
        }
        if (result.Diagnostics.Count > diagnostics) result.Diagnostics.RemoveRange(diagnostics, result.Diagnostics.Count - diagnostics);
    }

    static void Visit(ScopeDescriptor scope, TypeAnalysisResult result)
    {
        foreach (var declaration in scope.Declarations)
        {
            if (declaration is AliasDescriptor alias) alias.TypeInfo = Alias(alias, result);
            if (declaration is TypeDescriptor type)
            {
                Bases(type, result);
                if (type.Api.MarshalAsTokens.Count != 0) type.MarshalAsType = Parse(type.Api.MarshalAsTokens, type, result);
            }
            if (declaration is FieldDescriptor field)
                field.TypeInfo = Field(field, result);
            else if (declaration is FunctionDescriptor function)
                Function(function, result);
            else if (declaration is EnumDescriptor enumeration)
                enumeration.UnderlyingType = enumeration.UnderlyingTypeTokens.Any(t => !t.IsTrivia)
                    ? Parse(enumeration.UnderlyingTypeTokens, enumeration, result) : null;
            if (declaration is ScopeDescriptor child) Visit(child, result);
        }
    }

    internal static void ResolveTemplateMembers(ScopeDescriptor scope, TypeAnalysisResult result) => Visit(scope, result);

    static void Bases(TypeDescriptor type, TypeAnalysisResult result)
    {
        type.BaseTypes.Clear();
        List<Token> part = [];
        int angle = 0;
        foreach (var token in type.BaseTokens.Append(new Token { text = "," }))
        {
            if (token == "<") angle++;
            if (token == ">") angle--;
            if (token == "," && angle == 0)
            {
                if (part.Any(t => !t.IsTrivia))
                {
                    string access = part.FirstOrDefault(t => t.text is "public" or "protected" or "private").text ?? type.DefaultAccess;
                    var baseTokens = part.Where(t => t.text is not ("public" or "protected" or "private" or "virtual")).ToList();
                    type.BaseTypes.Add(new() { Type = Parse(baseTokens, type, result), Access = access,
                        IsVirtual = part.Any(t => t == "virtual"), SourceTokens = new(part) });
                }
                part.Clear();
            }
            else part.Add(token);
        }
    }

    static TypeInfo Field(FieldDescriptor field, TypeAnalysisResult result)
    {
        if (field.Name == "operator")
        {
            const string reason = "First-pass operator declaration needs an operator handler; it is not a field.";
            result.Diagnostics.Add(new LogEntry(reason, field.Name, LogEntryType.Warning, "VETP002"));
            return new UnsupportedTypeInfo { Reason = reason, SourceTokens = new(field.HeaderTokens) };
        }
        List<Token> tokens = new(field.TypeTokens);
        // The first pass keeps the name and any array suffix in DeclaratorTokens.
        bool removedName = false;
        foreach (var token in field.DeclaratorTokens)
        {
            if (!removedName && token.line == field.Name.line && token.col == field.Name.col && token.text == field.Name.text)
            { removedName = true; continue; }
            tokens.Add(token);
        }
        var type = Parse(tokens, field, result);
        TypeInfo? element = type;
        for (int i = 0; element is ArrayTypeInfo array && i < field.ExpandedArrayExtents.Count; i++, element = array.ElementType)
            array.ExpandedExtentTokens = field.ExpandedArrayExtents[i];
        return type;
    }

    static void Function(FunctionDescriptor function, TypeAnalysisResult result)
    {
        FunctionTypeInfo info = new();
        function.TypeInfo = info;
        string owner = (function.Parent as TypeDescriptor)?.Name.text ?? "";
        info.IsConstructor = owner.Length != 0 && function.Name.text == owner;
        info.IsDestructor = owner.Length != 0 && function.Name.text == "~" + owner;
        int name = function.HeaderTokens.FindIndex(t => function.NameTokens.Count > 0 &&
            t.line == function.NameTokens[0].line && t.col == function.NameTokens[0].col);
        if (!info.IsConstructor && !info.IsDestructor)
            info.ReturnType = Parse(name >= 0 ? function.HeaderTokens.Take(name).ToList() : [], function, result);
        foreach (var parameter in function.Fields)
        {
            parameter.TypeInfo = Field(parameter, result);
            info.Parameters.Add(parameter.TypeInfo);
        }
        if (function.ParameterTokens.Count > 0)
        {
            var last = function.ParameterTokens[^1];
            int end = function.HeaderTokens.FindIndex(t => t.line == last.line && t.col == last.col && t.text == last.text);
            if (end >= 0) info.SuffixTokens = function.HeaderTokens.Skip(end + 1).ToList();
        }
        var suffix = Clean(info.SuffixTokens);
        for (int i = 0; i < suffix.Count; i++)
        {
            var token = suffix[i];
            if (token.text is "=" or ":" or "-" or "requires" or ";") break;
            if (token == "(")
            {
                int depth = 1;
                while (++i < suffix.Count && depth > 0)
                { if (suffix[i] == "(") depth++; if (suffix[i] == ")") depth--; }
                i--;
                continue;
            }
            if (token == "noexcept") info.HasNoexcept = true;
            if (token.text == "const") info.Qualifiers |= TypeQualifiers.Const;
            if (token.text == "volatile") info.Qualifiers |= TypeQualifiers.Volatile;
            if (token == "&")
            {
                info.ReferenceQualifier = i + 1 < suffix.Count && suffix[i + 1] == "&" ? ReferenceKind.RValue : ReferenceKind.LValue;
                if (info.ReferenceQualifier == ReferenceKind.RValue) i++;
            }
        }
        int arrow = suffix.FindIndex(t => t == "-");
        if (arrow >= 0 && arrow + 1 < suffix.Count && suffix[arrow + 1] == ">")
        {
            var trailing = suffix.Skip(arrow + 2).TakeWhile(t => t.text is not (";" or "=" or "requires" or "override" or "final")).ToList();
            info.ReturnType = Parse(trailing, function, result);
        }
    }

    static List<Token> Clean(IEnumerable<Token> tokens) => tokens.Where(t => !t.IsTrivia && t.kind != Token.Kind.EndOfFile).ToList();

    public static TypeInfo Parse(List<Token> original, DeclarationDescriptor context, TypeAnalysisResult result)
    {
        var tokens = Clean(original);
        var specifiers = new List<Token>();
        // Strip metadata from type interpretation, preserving it in SourceTokens.
        for (int p = 0; p < tokens.Count;)
        {
            if (tokens[p] == "template" && p + 1 < tokens.Count && tokens[p + 1] == "<")
            {
                tokens.RemoveAt(p);
                SkipGroup(tokens, p, "<", ">");
                continue;
            }
            if (result.Converters.DeclarationMacros.ContainsKey(tokens[p].text))
            {
                specifiers.Add(tokens[p]); tokens.RemoveAt(p);
                if (p < tokens.Count && tokens[p] == "(") SkipGroup(tokens, p, "(", ")");
                continue;
            }
            if (tokens[p].text.StartsWith("API_", StringComparison.Ordinal) || context.Api.Invocations.Any(m => m.ApiName != null && m.Name.text == tokens[p].text))
            {
                tokens.RemoveAt(p);
                if (p < tokens.Count && tokens[p] == "(") SkipGroup(tokens, p, "(", ")");
                continue;
            }
            if (tokens[p].text is "FORCE_INLINE" or "FORCE_NOINLINE" or "INLINE" or "FLAXENGINE_API" or "FLAXEDITOR_API")
            { specifiers.Add(tokens[p]); tokens.RemoveAt(p); continue; }
            if (Specifiers.Contains(tokens[p].text))
            {
                specifiers.Add(tokens[p]); tokens.RemoveAt(p);
                if (p < tokens.Count && tokens[p] == "(") SkipGroup(tokens, p, "(", ")");
                continue;
            }
            if (tokens[p].text is "alignas" or "__declspec")
            {
                specifiers.Add(tokens[p]); tokens.RemoveAt(p);
                if (p < tokens.Count && tokens[p] == "(") SkipGroup(tokens, p, "(", ")");
                continue;
            }
            if (tokens[p] == "[" && p + 1 < tokens.Count && tokens[p + 1] == "[")
            { SkipGroup(tokens, p, "[", "]"); continue; }
            // Specifiers/attributes only precede a type in this supported subset.
            break;
        }
        TypeInfo Fail(string reason)
        {
            bool templateContext = false;
            for (DeclarationDescriptor? scope = context; scope != null; scope = scope.Parent)
                templateContext |= scope is FunctionDescriptor { IsTemplate: true };
            if (!templateContext)
                result.Diagnostics.Add(new LogEntry(tokens.Count == 0 ? reason + $" In '{context.QualifiedName}' ({context.Kind})." : reason,
                    original.FirstOrDefault(t => !t.IsTrivia).text != null ? original.First(t => !t.IsTrivia) : context.Name,
                    LogEntryType.Warning, "VETP002"));
            return new UnsupportedTypeInfo { Reason = reason, SourceTokens = new(original), SpecifierTokens = specifiers };
        }
        if (tokens.Count == 0) return Fail("Missing type declaration.");
        // A registered opaque type spelling (macro/template) is a contract, not an expansion.
        int opaqueIndex = 0;
        var opaqueCv = ReadCv(tokens, ref opaqueIndex);
        if (result.Converters.TryMatchOpaque(tokens, opaqueIndex, out var opaqueConverter, out int opaqueLength))
        {
            int suffix = opaqueIndex + opaqueLength;
            string placeholder = "__RegisteredOpaqueType";
            var substituted = tokens.Take(opaqueIndex).ToList();
            var token = tokens[opaqueIndex]; token.text = placeholder; token.kind = Token.Kind.Identifier;
            substituted.Add(token); substituted.AddRange(tokens.Skip(suffix));
            var parsed = Parse(substituted, context, result);
            TypeInfo root = parsed;
            while (root is PointerTypeInfo or ReferenceTypeInfo or ArrayTypeInfo)
                root = root switch { PointerTypeInfo p => p.Pointee, ReferenceTypeInfo r => r.Referent, ArrayTypeInfo a => a.ElementType, _ => root };
            if (root is NamedTypeInfo namedOpaque)
            { namedOpaque.Name = opaqueConverter!.NativeName; namedOpaque.Symbol = null; namedOpaque.Converter = opaqueConverter; namedOpaque.Qualifiers |= opaqueCv; }
            parsed.SourceTokens = new(original); parsed.SpecifierTokens = specifiers;
            return parsed;
        }
        int functionOpen = tokens.FindIndex(t => t == "(");
        if (functionOpen > 0 && functionOpen + 3 < tokens.Count && tokens[functionOpen + 1] == "*")
        {
            int cursor = functionOpen + 2;
            var pointerCv = ReadCv(tokens, ref cursor);
            if (cursor + 1 < tokens.Count && tokens[cursor] == ")" && tokens[cursor + 1] == "(" && tokens[^1] == ")")
            {
                var functionPointer = new FunctionPointerTypeInfo
                {
                    ReturnType = Parse(tokens.Take(functionOpen).ToList(), context, result),
                    Qualifiers = pointerCv, SourceTokens = new(original), SpecifierTokens = specifiers
                };
                var parameterTokens = tokens.Skip(cursor + 2).SkipLast(1).ToList();
                if (parameterTokens.Count == 1 && parameterTokens[0] == "void") parameterTokens.Clear();
                List<Token> parameter = [];
                int depth = 0;
                foreach (var token in parameterTokens.Append(new Token { text = "," }))
                {
                    if (token == "," && depth == 0)
                    {
                        if (parameter.Count > 0) functionPointer.Parameters.Add(Parse(parameter, context, result));
                        parameter = []; continue;
                    }
                    if (token.text is "(" or "<" or "[") depth++;
                    if (token.text is ")" or ">" or "]") depth--;
                    parameter.Add(token);
                }
                return functionPointer;
            }
        }
        int index = 0;
        TypeQualifiers qualifiers = ReadCv(tokens, ref index);
        if (index < tokens.Count && tokens[index].text is "class" or "struct" or "enum" or "typename") index++;
        List<Token> names = [];
        List<TemplateArgumentInfo> arguments = [];
        while (index < tokens.Count)
        {
            var token = tokens[index];
            if (token == "<")
            {
                ++index;
                int depth = 1;
                int round = 0, square = 0, curly = 0;
                List<Token> part = [];
                while (index < tokens.Count && depth > 0)
                {
                    token = tokens[index++];
                    if(token=="(")round++; else if(token==")")round--;
                    if(token=="[")square++; else if(token=="]")square--;
                    if(token=="{")curly++; else if(token=="}")curly--;
                    if (token == "<") depth++;
                    if (token == ">") depth--;
                    if (depth == 0 || (depth == 1 && round == 0 && square == 0 && curly == 0 && token == ","))
                    {
                        if (part.Count == 0) { if (depth != 0 || arguments.Count > 0) return Fail("Empty template argument."); }
                        else arguments.Add(Argument(part, context, result));
                        part = [];
                    }
                    else part.Add(token);
                }
                if (depth != 0) return Fail("Unclosed template argument list.");
                if (index < tokens.Count && tokens[index] == ":") return Fail("Qualified members of template types are not interpreted yet.");
                break;
            }
            if (token.text is "const" or "volatile") break;
            if (token.kind == Token.Kind.Identifier || token == ":") { names.Add(token); index++; }
            else break;
        }
        if (names.Count == 0) return Fail("Expected a named or builtin type.");
        string nameText = FormatName(names);
        if (!ValidName(names)) return Fail("Unknown native type spelling '" + nameText + "'. Register a converter or provide a typedef declaration; includes and macros are not expanded.");
        qualifiers |= ReadCv(tokens, ref index);
        var named = new NamedTypeInfo { Name = nameText, Qualifiers = qualifiers, Arguments = arguments };
        Resolve(named, context, result);
        if (named.Symbol?.Definition is TypeDescriptor { Api.Template: true } template && named.Arguments.Count > 0 && template.TemplateParameters.Count == named.Arguments.Count && template.TemplateParameters.All(parameter => parameter.IsType && !parameter.IsPack))
        {
            TemplateSpecializationInfo specialization = new() { Definition = template };
            for (int i = 0; i < named.Arguments.Count; i++)
                if (named.Arguments[i].Type is { } argument) specialization.TypeArguments.Add(template.TemplateParameters[i].Name.text, argument);
            if (specialization.TypeArguments.Count == named.Arguments.Count) named.Specialization = specialization;
        }
        TypeInfo type = named.Alias == null ? named : Alias(named.Alias, result, named.Arguments);
        if (named.Alias != null)
        {
            Qualify(type, qualifiers);
            type.NativeAliasSpelling = "::" + named.Alias.QualifiedName + (arguments.Count == 0 ? "" : "<" + string.Join(", ", arguments.Select(a => a.Type == null ? a.Display : TemplateInstantiationPass.Native(a.Type))) + ">") + ((qualifiers & TypeQualifiers.Const) != 0 ? " const" : "") + ((qualifiers & TypeQualifiers.Volatile) != 0 ? " volatile" : "");
        }
        bool referenced = false;
        while (index < tokens.Count && tokens[index].text is "*" or "&")
        {
            if (referenced) return Fail("Unexpected pointer/reference after a reference declarator.");
            if (tokens[index++] == "*")
            {
                if (type is ReferenceTypeInfo) return Fail("A pointer cannot target a reference typedef.");
                type = new PointerTypeInfo { Pointee = type, Qualifiers = ReadCv(tokens, ref index) };
            }
            else
            {
                var kind = ReferenceKind.LValue;
                if (index < tokens.Count && tokens[index] == "&") { index++; kind = ReferenceKind.RValue; }
                // Reference aliases obey C++ reference collapsing.
                type = type is ReferenceTypeInfo existing ? new ReferenceTypeInfo { Referent = existing.Referent, ReferenceKind = existing.ReferenceKind == ReferenceKind.LValue || kind == ReferenceKind.LValue ? ReferenceKind.LValue : ReferenceKind.RValue } : new ReferenceTypeInfo { Referent = type, ReferenceKind = kind };
                referenced = true;
            }
        }
        List<List<Token>> extents = [];
        while (index < tokens.Count && tokens[index] == "[")
        {
            index++; List<Token> extent = [];
            int depth = 1;
            while (index < tokens.Count && depth > 0)
            {
                var t = tokens[index++];
                if (t == "[") depth++;
                if (t == "]") depth--;
                if (depth != 0) extent.Add(t);
            }
            if (depth != 0) return Fail("Unclosed array extent.");
            extents.Add(extent);
        }
        if (index != tokens.Count) return Fail("Declarator requires a later handler (function/member pointer, expression type, or unsupported syntax).");
        if (referenced && extents.Count > 0) return Fail("Array/reference declarator requires explicit grouping analysis.");
        for (int i = extents.Count - 1; i >= 0; i--) type = new ArrayTypeInfo { ElementType = type, ExtentTokens = extents[i] };
        type.SourceTokens = new(original);
        type.SpecifierTokens = specifiers;
        return type;
    }

    static TemplateArgumentInfo Argument(List<Token> tokens, DeclarationDescriptor context, TypeAnalysisResult result)
    {
        TemplateArgumentInfo argument = new() { Tokens = new(tokens) };
        // Only definitely builtin or locally resolved type arguments are classified.
        // Unknown identifiers could be value parameters, so keep their raw tokens.
        TypeAnalysisResult scratch = new() { Symbols = result.Symbols, Aliases = result.Aliases, Converters = result.Converters, ResolvingAliases = result.ResolvingAliases };
        var candidate = Parse(tokens, context, scratch);
        TypeInfo root = candidate;
        while (root is PointerTypeInfo or ReferenceTypeInfo)
            root = root is PointerTypeInfo pointer ? pointer.Pointee : ((ReferenceTypeInfo)root).Referent;
        if (candidate.IsSupported && root is NamedTypeInfo namedRoot && (namedRoot.Converter != null || namedRoot.Resolution is TypeResolution.Builtin or TypeResolution.Resolved or TypeResolution.Dependent)) argument.Type = candidate;
        return argument;
    }

    static void Resolve(NamedTypeInfo type, DeclarationDescriptor context, TypeAnalysisResult result)
    {
        if (IsBuiltin(type.Name)) { type.Resolution = TypeResolution.Builtin; return; }
        if (type.Name == "auto") { type.Resolution = TypeResolution.Deduced; return; }
        for (DeclarationDescriptor? scope = context; scope != null; scope = scope.Parent)
        {
            var parameters = scope is FunctionDescriptor f ? f.TemplateParameters : scope is TypeDescriptor t ? t.TemplateParameters : scope is AliasDescriptor a ? a.TemplateParameters : [];
            if (parameters.Any(p => p.Name.text == type.Name || type.Name.StartsWith(p.Name.text + "::", StringComparison.Ordinal)))
            { type.Resolution = TypeResolution.Dependent; return; }
        }
        string name = type.Name;
        if (name.StartsWith("::")) { Match(name[2..]); return; }
        string prefix = string.Concat(context.NamespaceTokens.Select(t => t.text));
        string enclosing = string.Concat(context.EnclosingTypeTokens.Select(t => t.text));
        if (enclosing.Length != 0) prefix = prefix.Length == 0 ? enclosing : prefix + "::" + enclosing;
        while (prefix.Length != 0)
        {
            if (Match(prefix + "::" + name)) return;
            if (result.Symbols.TryGetValue(prefix, out var enclosingSymbol) && enclosingSymbol.Definition is TypeDescriptor enclosingType)
            {
                HashSet<TypeDescriptor> path = [];
                var inherited = FindInherited(enclosingType).Distinct(StringComparer.Ordinal).ToArray();
                if (inherited.Length > 1) { type.Resolution = TypeResolution.Ambiguous; return; }
                if (inherited.Length == 1 && Match(inherited[0])) return;
                IEnumerable<string> FindInherited(TypeDescriptor owner)
                {
                    if (path.Count >= 64 || !path.Add(owner)) yield break;
                    try {
                        foreach (var basis in owner.BaseTypes)
                        {
                            if (basis.Type is not NamedTypeInfo namedBase || namedBase.Arguments.Count != 0 && namedBase.Specialization?.Concrete == null) continue;
                            var parent = namedBase.Specialization?.Concrete ?? namedBase.Symbol?.Definition as TypeDescriptor;
                            if (parent == null) continue;
                            string candidate = parent.QualifiedName + "::" + name;
                            if (result.Symbols.ContainsKey(candidate) || result.Aliases.ContainsKey(candidate)) yield return candidate;
                            else foreach (string value in FindInherited(parent)) yield return value;
                        }
                    } finally { path.Remove(owner); }
                }
            }
            int split = prefix.LastIndexOf("::", StringComparison.Ordinal);
            prefix = split >= 0 ? prefix[..split] : "";
        }
        Match(name);
        bool Match(string qualified)
        {
            if (result.Converters.TryGet(qualified, out var converter))
            { type.Converter = converter; if (result.Symbols.TryGetValue(qualified, out var registered)) type.Symbol = registered; return true; }
            if (result.Aliases.TryGetValue(qualified, out var aliases))
            {
                if (aliases.Select(a => string.Join(" ", a.TargetTokens.Where(t => !t.IsTrivia).Select(t => t.text))).Distinct().Skip(1).Any())
                { type.Resolution = TypeResolution.Ambiguous; return true; }
                type.Alias = aliases[0]; return true;
            }
            if (!result.Symbols.TryGetValue(qualified, out var symbol)) return false;
            type.Symbol = symbol;
            int definitions = symbol.Declarations.Count(d => d is TypeDescriptor { IsForwardDeclaration: false } or EnumDescriptor { IsForwardDeclaration: false });
            type.Resolution = definitions > 1 ? TypeResolution.Ambiguous : TypeResolution.Resolved;
            return true;
        }
    }

    static TypeInfo Alias(AliasDescriptor alias, TypeAnalysisResult result, List<TemplateArgumentInfo>? arguments = null)
    {
        if (alias.SourceFile?.TypeInfos is { } owner && owner != result) return Alias(alias, owner, arguments);
        if (!result.ResolvingAliases.Add(alias)) return new UnsupportedTypeInfo { Reason = $"Cyclic typedef/using alias '{alias.QualifiedName}'.", SourceTokens = new(alias.TargetTokens) };
        try
        {
            if (arguments == null) return Parse(alias.TargetTokens, alias, result);
            if (alias.TemplateParameters.Count != arguments.Count || alias.TemplateParameters.Any(p => !p.IsType || p.IsPack) || arguments.Any(a => a.Type == null))
                return new UnsupportedTypeInfo { Reason = $"Alias template '{alias.QualifiedName}' requires exactly {alias.TemplateParameters.Count} explicit type arguments; defaults, non-type parameters and packs require a handler.", SourceTokens = new(alias.TargetTokens) };
            var substitutions = alias.TemplateParameters.Select((p, i) => (p.Name.text, Tokens: Tokenizer.Tokenize(TemplateInstantiationPass.Native(arguments[i].Type!)).Where(t => t.kind != Token.Kind.EndOfFile).ToList())).ToDictionary(p => p.text, p => p.Tokens);
            var target = alias.TargetTokens.SelectMany(t => t.kind == Token.Kind.Identifier && substitutions.TryGetValue(t.text, out var replacement) ? replacement : new List<Token> { t }).ToList();
            return Parse(target, alias, result);
        }
        finally { result.ResolvingAliases.Remove(alias); }
    }
    static void Qualify(TypeInfo type, TypeQualifiers qualifiers)
    {
        switch (type)
        {
            case NamedTypeInfo n: n.Qualifiers |= qualifiers; break;
            case PointerTypeInfo p: p.Qualifiers |= qualifiers; break;
            case FunctionPointerTypeInfo f: f.Qualifiers |= qualifiers; break;
            case ArrayTypeInfo a: Qualify(a.ElementType, qualifiers); break;
            // Top-level cv on reference aliases is ignored by C++.
        }
    }

    static string FormatName(List<Token> tokens)
    {
        string name = "";
        foreach (var token in tokens)
        {
            if (token == ":") name += ":";
            else { if (name.Length > 0 && !name.EndsWith(':')) name += " "; name += token.text; }
        }
        return name;
    }

    static bool ValidName(List<Token> tokens)
    {
        if (tokens.All(t => t.kind == Token.Kind.Identifier)) return tokens.Count == 1 || IsBuiltin(FormatName(tokens));
        string text = string.Concat(tokens.Select(t => t.text));
        if (text.StartsWith("::")) text = text[2..];
        return text.Split("::", StringSplitOptions.None).All(p => p.Length > 0 && (char.IsLetter(p[0]) || p[0] == '_') && p.All(c => char.IsLetterOrDigit(c) || c == '_'));
    }

    static TypeQualifiers ReadCv(List<Token> tokens, ref int index)
    {
        TypeQualifiers value = TypeQualifiers.None;
        while (index < tokens.Count && tokens[index].text is "const" or "volatile")
            value |= tokens[index++].text == "const" ? TypeQualifiers.Const : TypeQualifiers.Volatile;
        return value;
    }

    static void SkipGroup(List<Token> tokens, int start, string open, string close)
    {
        int depth = 0, end = start;
        do { var token = tokens[end++]; if (token == open) depth++; if (token == close) depth--; } while (end < tokens.Count && depth > 0);
        tokens.RemoveRange(start, end - start);
    }
}
