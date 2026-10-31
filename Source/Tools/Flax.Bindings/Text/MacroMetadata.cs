using static VETools.Tokenizer;

namespace VETools.Text;

internal sealed record MacroDefinition(Token Name, bool FunctionLike, IReadOnlyList<Token> Parameters, IReadOnlyList<Token> Replacement, IReadOnlyList<Token> Tokens);
internal sealed record MacroInvocation(Token Name, IReadOnlyList<List<Token>> Arguments, IReadOnlyList<Token> Tokens, string? ApiName = null);
// API_* parentheses combine several independent metadata domains. None of
// these binding options changes the native type's cv/access qualifiers.
internal sealed class ApiBindingOptions
{
    public bool Export, Property, ReadOnly, Template, Alias, NoDefault, NoPod, NoSpawn, NoProxy, NoConstructor, InBuild, NoArray, Event, Hidden;
    public List<Token> MarshalAsTokens = [];
    public List<Token>? DefaultValueTokens;
    public ParameterDirection? Direction;
}
internal sealed class ApiManagedDeclarationOptions
{
    public bool Static, Abstract, Flags, Sealed;
    public string? Name, Access, Namespace;
}
internal sealed class ApiManagedAnnotations
{
    public List<string> Attributes = [];
}
internal sealed class ApiMetadata
{
    public ApiBindingOptions Binding = new();
    public ApiManagedDeclarationOptions Managed = new();
    public ApiManagedAnnotations Annotations = new();
    public Dictionary<string, string> ExtensionTags = new(StringComparer.Ordinal);
    public bool NativeInvokeUseName => ExtensionTags.ContainsKey("NativeInvokeUseName");
    public List<(string Language, string Code)> InjectedCode = [];
    // Compatibility-facing accessors keep consumers on the same metadata object
    // while the typed domains make their different responsibilities explicit.
    public bool Export { get => Binding.Export; set => Binding.Export = value; }
    public bool Property { get => Binding.Property; set => Binding.Property = value; }
    public bool Template { get => Binding.Template; set => Binding.Template = value; }
    public bool Alias { get => Binding.Alias; set => Binding.Alias = value; }
    public bool NoPod { get => Binding.NoPod; set => Binding.NoPod = value; }
    public bool NoDefault { get => Binding.NoDefault; set => Binding.NoDefault = value; }
    public bool NoSpawn { get => Binding.NoSpawn; set => Binding.NoSpawn = value; }
    public bool NoProxy { get => Binding.NoProxy; set => Binding.NoProxy = value; }
    public bool NoConstructor { get => Binding.NoConstructor; set => Binding.NoConstructor = value; }
    public bool InBuild { get => Binding.InBuild; set => Binding.InBuild = value; }
    public bool NoArray { get => Binding.NoArray; set => Binding.NoArray = value; }
    public bool Event { get => Binding.Event; set => Binding.Event = value; }
    public bool Hidden { get => Binding.Hidden; set => Binding.Hidden = value; }
    public bool Static { get => Managed.Static; set => Managed.Static = value; }
    public bool Abstract { get => Managed.Abstract; set => Managed.Abstract = value; }
    public bool Flags { get => Managed.Flags; set => Managed.Flags = value; }
    public bool Sealed { get => Managed.Sealed; set => Managed.Sealed = value; }
    public string? ManagedName { get => Managed.Name; set => Managed.Name = value; }
    public string? ManagedAccess { get => Managed.Access; set => Managed.Access = value; }
    public string? ManagedNamespace { get => Managed.Namespace; set => Managed.Namespace = value; }
    public List<Token> MarshalAsTokens { get => Binding.MarshalAsTokens; set => Binding.MarshalAsTokens = value; }
    public List<Token>? DefaultValueTokens { get => Binding.DefaultValueTokens; set => Binding.DefaultValueTokens = value; }
    public ParameterDirection? Direction { get => Binding.Direction; set => Binding.Direction = value; }
    public List<string> ManagedAttributes { get => Annotations.Attributes; set => Annotations.Attributes = value; }
    public List<MacroInvocation> Invocations = [];
    public List<LogEntry> Errors = [], Warnings = [];
}

internal static class MacroMetadata
{
    public static readonly HashSet<string> ApiNames = new(StringComparer.Ordinal)
    { "API_CLASS", "API_STRUCT", "API_INTERFACE", "API_ENUM", "API_FUNCTION", "API_FIELD", "API_PROPERTY", "API_PARAM", "API_EVENT", "API_TYPEDEF", "API_INJECT_CODE", "API_AUTO_SERIALIZATION" };

    public static List<MacroDefinition> Definitions(List<Token> source)
    {
        List<MacroDefinition> result = [];
        for (int i = 0; i < source.Count; ++i)
        {
            if (source[i] != "#") continue;
            int start = i, line = source[i].line;
            Token last = default;
            do
            {
                last = default;
                while (i < source.Count && source[i].line == line)
                { if (!source[i].IsTrivia) last = source[i]; ++i; }
                if (last == "\\" && i < source.Count) line = source[i].line; else break;
            } while (true);
            var tokens = source.GetRange(start, i - start);
            --i;
            var meaningful = tokens.Where(t => !t.IsTrivia).ToList();
            if (meaningful.Count < 3 || meaningful[1] != "define") continue;
            Token name = meaningful[2];
            int nameIndex = tokens.FindIndex(t => t.line == name.line && t.col == name.col);
            int next = nameIndex + 1;
            bool function = next < tokens.Count && tokens[next] == "("; // whitespace makes an object-like macro
            List<Token> parameters = [];
            if (function)
            {
                int depth = 0, begin = next;
                do { if (tokens[next] == "(") ++depth; else if (tokens[next] == ")") --depth; ++next; }
                while (next < tokens.Count && depth > 0);
                parameters = tokens.GetRange(begin, next - begin);
            }
            result.Add(new(name, function, parameters, tokens.Skip(next).ToList(), tokens));
        }
        return result;
    }

    public static void Apply(FileDescriptor file)
    {
        foreach (var declaration in Declarations(file))
        {
            var metadata = declaration.Api;
            var header = declaration.HeaderTokens;
            if (declaration is RawDeclarationDescriptor && header.FindIndex(t => t == "{") is >= 0 and var bodyStart)
            {
                var lifted = declaration.Parent is ScopeDescriptor scope ? scope.Fields.SelectMany(f => f.HeaderTokens).Where(t => t == "API_FIELD").Select(t => (t.line, t.col)).ToHashSet() : [];
                foreach (var annotation in header.Skip(bodyStart).Where(t => t == "API_FIELD" && !lifted.Contains((t.line, t.col))))
                    metadata.Errors.Add(new("Cannot expose annotated fields in this anonymous record: a single lifted anonymous struct is required; named instances and alternative union structs need an explicit converter.", annotation));
                header = header.Take(bodyStart).ToList();
            }
            var leading = header.Where(t => string.IsNullOrEmpty(declaration.Name.text) ||
                t.line < declaration.Name.line || t.line == declaration.Name.line && t.col < declaration.Name.col).ToList();
            var tokens = leading.Where(t => !t.IsTrivia).ToList();
            for (int i = 0; i < tokens.Count; ++i)
            {
                if (tokens[i].kind != Token.Kind.Identifier) continue;
                var directDefinition = file.MacroDefinitions.LastOrDefault(d => d.Name.text == tokens[i].text && d.Name.line <= tokens[i].line);
                if (i + 1 >= tokens.Count || tokens[i + 1] != "(")
                {
                    if (directDefinition is { FunctionLike: false })
                    {
                        metadata.Invocations.Add(new(tokens[i], [], [tokens[i]]));
                        if (directDefinition.Replacement.Any(t => !t.IsTrivia && t != "\\") && !CompilerMacro(tokens[i].text) && !file.Converters.DeclarationMacros.ContainsKey(tokens[i].text))
                            metadata.Warnings.Add(new($"Object-like macro '{tokens[i].text}' is preserved without expansion. If it supplies a type, register its converter explicitly.", tokens[i], LogEntryType.Warning, "VETP004"));
                    }
                    continue;
                }
                string original = tokens[i].text;
                string macro = original;
                HashSet<string> aliases = [];
                while (!macro.StartsWith("API_", StringComparison.Ordinal) && aliases.Add(macro))
                {
                    var definition = file.MacroDefinitions.LastOrDefault(d => d.Name.text == macro && d.Name.line <= tokens[i].line);
                    var replacement = definition?.Replacement.Where(t => !t.IsTrivia && t != "\\").ToArray();
                    if (replacement?.Length != 1 || replacement[0].kind != Token.Kind.Identifier) break;
                    macro = replacement[0].text;
                }
                bool known = macro.StartsWith("API_", StringComparison.Ordinal);
                bool declared = file.MacroDefinitions.Any(d => d.Name.text == original);
                bool declarationMacro = file.Converters.DeclarationMacros.ContainsKey(original);
                if (!known && !declared && !declarationMacro && !original.StartsWith("DECLARE_SCRIPTING_", StringComparison.Ordinal)) continue;
                int start = i, round = 0, angle = 0;
                List<List<Token>> arguments = [[]];
                i += 1;
                for (; i < tokens.Count; ++i)
                {
                    if (tokens[i] == "(") { if (round++ == 0) continue; }
                    if (tokens[i] == ")" && --round == 0) break;
                    if (round == 1 && tokens[i] == "<") ++angle;
                    if (round == 1 && tokens[i] == ">" && angle > 0) --angle;
                    if (tokens[i] == "," && round == 1 && angle == 0) arguments.Add([]); else arguments[^1].Add(tokens[i]);
                }
                var invocation = new MacroInvocation(tokens[start], arguments, tokens.Skip(start).Take(i - start + 1).ToList(), known ? macro : null);
                metadata.Invocations.Add(invocation);
                if (declarationMacro) continue;
                if (!known)
                {
                    if (directDefinition != null && directDefinition.Replacement.Any(t => !t.IsTrivia && t != "\\") && !CompilerMacro(original))
                        metadata.Warnings.Add(new($"Function-like macro '{original}' is preserved without expansion. If it supplies a type, register its converter explicitly.", invocation.Name, LogEntryType.Warning, "VETP004"));
                    continue;
                }
                if (!ApiNames.Contains(macro)) { metadata.Errors.Add(new("Unknown API annotation macro '" + macro + "'.", invocation.Name)); continue; }
                if (macro == "API_AUTO_SERIALIZATION")
                {
                    metadata.Warnings.Add(new("API_AUTO_SERIALIZATION is preserved but skipped by V2; reflection-based serialization generation is not implemented yet.",
                        invocation.Name, LogEntryType.Warning, "VETP004"));
                    continue;
                }
                if (macro == "API_INJECT_CODE")
                {
                    if (arguments.Count != 2 || arguments[0].Count != 1 || arguments[0][0].text is not ("cpp" or "csharp") ||
                        arguments[1].Count == 0 || arguments[1].Any(t => t.kind != Token.Kind.String))
                        metadata.Errors.Add(new("API_INJECT_CODE expects (cpp or csharp, a quoted code string); adjacent string literals are supported.", invocation.Name));
                    else
                    {
                        try { metadata.InjectedCode.Add((arguments[0][0].text, string.Concat(arguments[1].Select(t => System.Text.Json.JsonSerializer.Deserialize<string>(t.text))))); }
                        catch (System.Text.Json.JsonException) { metadata.Errors.Add(new("Invalid API_INJECT_CODE string literal.", invocation.Name)); }
                    }
                    continue;
                }
                bool location = macro switch
                {
                    "API_CLASS" => declaration is ClassDescriptor,
                    "API_STRUCT" => declaration is StructDescriptor,
                    "API_INTERFACE" => declaration is InterfaceDescriptor,
                    "API_ENUM" => declaration is EnumDescriptor or EnumMemberDescriptor,
                    "API_TYPEDEF" => declaration is AliasDescriptor,
                    "API_FUNCTION" or "API_PROPERTY" => declaration is FunctionDescriptor,
                    "API_PARAM" => declaration is FieldDescriptor { IsParameter: true },
                    "API_FIELD" => declaration is FieldDescriptor { IsParameter: false },
                    "API_EVENT" => declaration is FieldDescriptor { IsParameter: false },
                    _ => false
                };
                if (!location) { metadata.Errors.Add(new($"{macro} is unsupported at this declaration ({declaration.Kind}).", invocation.Name)); continue; }
                if (macro != "API_PARAM") metadata.Export = true;
                metadata.Property |= macro == "API_PROPERTY";
                metadata.Event |= macro == "API_EVENT";
                foreach (var argument in arguments.Where(a => a.Count > 0))
                {
                    string tag = argument[0].text.ToLowerInvariant();
                    switch (tag)
                    {
                        case "networkrpc" when macro == "API_FUNCTION":
                            if (argument.Count < 3 || argument[1] != "=")
                            { metadata.Errors.Add(new("NetworkRpc expects a Server or Client target.", argument[0])); break; }
                            metadata.ExtensionTags["NetworkRpc"] = string.Concat(argument.Skip(2).Select(t => t.text));
                            break;
                        case "name" when macro == "API_CLASS":
                            if (argument.Count != 3 || argument[1] != "=" || argument[2].kind != Token.Kind.String)
                            { metadata.Errors.Add(new("API_CLASS Name expects a quoted managed type name.", argument[0])); break; }
                            if (metadata.ManagedName != null)
                            { metadata.Errors.Add(new("Duplicate API_CLASS Name override.", argument[0])); break; }
                            try
                            {
                                string value = System.Text.Json.JsonSerializer.Deserialize<string>(argument[2].text)!;
                                if (!System.Text.RegularExpressions.Regex.IsMatch(value, @"^[A-Za-z_][A-Za-z0-9_]*$"))
                                    metadata.Errors.Add(new("API_CLASS Name must be a single C# identifier, not a qualified name.", argument[0]));
                                else metadata.ManagedName = value;
                            }
                            catch (System.Text.Json.JsonException) { metadata.Errors.Add(new("Invalid API_CLASS Name string.", argument[0])); }
                            break;
                        case "defaultvalue" when macro == "API_PARAM":
                            if (argument.Count < 3 || argument[1] != "=")
                            { metadata.Errors.Add(new("API_PARAM DefaultValue expects an expression after '='.", argument[0])); break; }
                            if (metadata.DefaultValueTokens != null)
                            { metadata.Errors.Add(new("Duplicate API_PARAM DefaultValue.", argument[0])); break; }
                            var defaultTokens = argument.Skip(2).ToList();
                            if (defaultTokens.Count == 1 && defaultTokens[0].kind == Token.Kind.String)
                            {
                                try
                                {
                                    var where = defaultTokens[0];
                                    defaultTokens = Tokenizer.Tokenize(System.Text.Json.JsonSerializer.Deserialize<string>(where.text)!)
                                        .Where(t => t.kind != Token.Kind.EndOfFile).Select(t => { t.line = where.line; t.col = where.col; return t; }).ToList();
                                }
                                catch (System.Text.Json.JsonException)
                                { metadata.Errors.Add(new("Invalid API_PARAM DefaultValue expression string.", argument[0])); break; }
                            }
                            if (defaultTokens.All(t => t.IsTrivia))
                            { metadata.Errors.Add(new("API_PARAM DefaultValue must not be empty.", argument[0])); break; }
                            metadata.DefaultValueTokens = defaultTokens;
                            break;
                        case "inbuild" when declaration is TypeDescriptor or EnumDescriptor or AliasDescriptor: metadata.InBuild = true; break;
                        case "marshalas" when macro is "API_STRUCT" or "API_CLASS":
                            if (argument.Count < 3 || argument[1] != "=") metadata.Errors.Add(new("MarshalAs expects a native type after '='.", argument[0]));
                            else if (metadata.MarshalAsTokens.Count != 0) metadata.Errors.Add(new("Duplicate MarshalAs target.", argument[0]));
                            else if (argument.Count == 3 && argument[2].kind == Token.Kind.String)
                            {
                                try { metadata.MarshalAsTokens = Tokenizer.Tokenize(System.Text.Json.JsonSerializer.Deserialize<string>(argument[2].text)!).Where(t => t.kind != Token.Kind.EndOfFile).ToList(); }
                                catch (System.Text.Json.JsonException) { metadata.Errors.Add(new("Invalid MarshalAs type string.", argument[0])); }
                                if (metadata.MarshalAsTokens.All(t => t.IsTrivia)) metadata.Errors.Add(new("MarshalAs target must not be empty.", argument[0]));
                            }
                            else metadata.MarshalAsTokens = argument.Skip(2).ToList();
                            break;
                        case "internal" when macro is "API_CLASS" or "API_FUNCTION" or "API_FIELD" or "API_PROPERTY" or "API_STRUCT" or "API_EVENT" or "API_ENUM": metadata.ManagedAccess = "internal"; break;
                        case "private" when macro is "API_FIELD" or "API_FUNCTION" or "API_FIELD" or "API_PROPERTY" or "API_STRUCT": metadata.ManagedAccess = "private"; break;
                        case "noarray" when macro == "API_FIELD": metadata.NoArray = true; break;
                        case "sealed" when macro is "API_CLASS" or "API_FUNCTION" or "API_PROPERTY": metadata.Sealed = true; break;
                        case "noconstructor" when macro == "API_CLASS": metadata.NoConstructor = true; break;
                        case "nospawn" when macro == "API_CLASS": metadata.NoSpawn = true; break;
                        case "noproxy" when macro == "API_FUNCTION": metadata.NoProxy = true; break;
                        case "hidden" when macro is "API_FUNCTION" or "API_FUNCTION" or "API_FIELD" or "API_PROPERTY" or "API_STRUCT": metadata.Hidden = true; break;
                        // Disables only a synthetic Default member; native constructors
                        // and user-declared Default members retain their own semantics.
                        case "nopod" when macro == "API_STRUCT": metadata.NoPod = true; break;
                        case "nodefault" when macro == "API_STRUCT": metadata.NoDefault = true; break;
                        case "namespace" when declaration is TypeDescriptor or EnumDescriptor or AliasDescriptor:
                            if (argument.Count != 3 || argument[1] != "=" || argument[2].kind != Token.Kind.String)
                            { metadata.Errors.Add(new($"{macro} Namespace expects a quoted managed namespace.", argument[0])); break; }
                            try
                            {
                                string value = System.Text.Json.JsonSerializer.Deserialize<string>(argument[2].text)!;
                                if (value.Length != 0 && !System.Text.RegularExpressions.Regex.IsMatch(value, @"^[A-Za-z_]\w*(\.[A-Za-z_]\w*)*$"))
                                    metadata.Errors.Add(new("Invalid managed namespace override.", argument[0]));
                                else metadata.ManagedNamespace = value;
                            }
                            catch (System.Text.Json.JsonException) { metadata.Errors.Add(new("Invalid managed namespace string.", argument[0])); }
                            break;
                        case "static" when macro == "API_CLASS": metadata.Static = true; break;
                        case "abstract" when macro == "API_CLASS": metadata.Abstract = true; break;
                        case "template" when macro is "API_STRUCT" or "API_CLASS": metadata.Template = true; break;
                        case "alias" when macro == "API_TYPEDEF": metadata.Alias = true; break;
                        case "readonly" when macro == "API_FIELD": metadata.Binding.ReadOnly = true; break;
                        case "flags" when macro == "API_ENUM" && declaration is EnumDescriptor: metadata.Flags = true; break;
                        case "out" or "ref" or "in" when macro == "API_PARAM":
                            var direction = Enum.Parse<ParameterDirection>(tag, true);
                            if (metadata.Direction != null && metadata.Direction != direction) metadata.Errors.Add(new("Conflicting API_PARAM directions.", argument[0]));
                            metadata.Direction = direction; break;
                        case "attributes":
                            if (argument.Count < 3 || argument[1] != "=" || argument.Skip(2).Any(t => t.kind != Token.Kind.String))
                            { metadata.Errors.Add(new($"{macro} Attributes expects a quoted C# attribute list.", argument[0])); break; }
                            try
                            {
                                string value = string.Concat(argument.Skip(2).Select(t => System.Text.Json.JsonSerializer.Deserialize<string>(t.text)));
                                if (!string.IsNullOrWhiteSpace(value)) metadata.ManagedAttributes.Add(value);
                            }
                            catch (System.Text.Json.JsonException)
                            { metadata.Errors.Add(new($"{macro} Attributes contains an unsupported string literal or escape sequence.", argument[0])); }
                            break;
                        case "tag":
                            if (argument.Count != 3 || argument[1] != "=" || argument[2].kind != Token.Kind.String)
                            { metadata.Errors.Add(new($"{macro} Tag expects a quoted extension tag ('name' or 'name=value').", argument[0])); break; }
                            try
                            {
                                string value = System.Text.Json.JsonSerializer.Deserialize<string>(argument[2].text)!;
                                int separator = value.IndexOf('=');
                                string key = separator < 0 ? value : value[..separator];
                                if (key.Length == 0) metadata.Errors.Add(new("Extension tag name must not be empty.", argument[0]));
                                else metadata.ExtensionTags[key] = separator < 0 ? "" : value[(separator + 1)..];
                            }
                            catch (System.Text.Json.JsonException) { metadata.Errors.Add(new("Invalid extension tag string.", argument[0])); }
                            break;
                        default: metadata.Errors.Add(new($"{macro} metadata '{tag}' is detected but not supported yet; it will not be silently ignored.", argument[0])); break;
                    }
                }
            }
            if (metadata.ExtensionTags.TryGetValue("NetworkRpc", out var rpc))
            {
                bool server = rpc.Contains("Server", StringComparison.OrdinalIgnoreCase), client = rpc.Contains("Client", StringComparison.OrdinalIgnoreCase);
                if (server == client) metadata.Errors.Add(new("NetworkRpc requires exactly one Server or Client target.", declaration.Name));
                if (!metadata.Hidden || !file.Converters.RetainedLegacyNetworkRpc)
                    metadata.Errors.Add(new("NetworkRpc requires the retained legacy registration backend and a Hidden native method. V2 does not emit ordinary local wrappers for RPC declarations.", declaration.Name));
            }
            if (file.CppStandard < 20)
            {
                var attributes = declaration.AttributeTokens.SelectMany(a => a).Concat(leading);
                if (attributes.Any(t => t.text is "Export" or "Getter" or "Setter" or "PassThrough") && attributes.Any(t => t == "["))
                    metadata.Errors.Add(new("Binding [[attributes]] require the C++20 annotation profile. Use API_* macros below C++20.", declaration.Name));
            }
        }
    }

    static bool CompilerMacro(string name) => name is "FORCE_INLINE" or "FORCE_NOINLINE" or "INLINE" or "FLAXENGINE_API" or "FLAXEDITOR_API" or "interface";

    static IEnumerable<DeclarationDescriptor> Declarations(ScopeDescriptor scope)
    {
        foreach (var declaration in scope.Declarations)
        {
            yield return declaration;
            if(declaration is EnumDescriptor enumeration) foreach(var member in enumeration.Members) yield return member;
            if (declaration is ScopeDescriptor child) foreach (var nested in Declarations(child)) yield return nested;
            if (declaration is FunctionDescriptor function) foreach (var parameter in function.Fields) yield return parameter;
        }
    }
}
