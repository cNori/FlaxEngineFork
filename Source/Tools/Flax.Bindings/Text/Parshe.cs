using static VETools.Tokenizer;
using static VETools.Tokenizer.Token;

namespace VETools.Text
{
    internal class Parshe
    {
        public static FileDescriptor PreParse(Tokenizer.Stream stream, List<Token> NamespaceTokens, int cppStandard = 20, TypeConverterRegistry? converters = null, IEnumerable<string>? knownFunctionMacros = null)
        {
            FileDescriptor file = new() { Tokens = stream.Tokens, Errors = stream.Errors, CppStandard = cppStandard, MacroDefinitions = MacroMetadata.Definitions(stream.Tokens), Converters = converters ?? new() };
            ReadScope(stream, file, NamespaceTokens, [], file.MacroDefinitions.Select(d => d.Name.text).Concat(knownFunctionMacros ?? []).Concat(file.Converters.FunctionTypeMacros).Concat(file.Converters.DeclarationMacros.Keys).ToHashSet(StringComparer.Ordinal),
                file.Converters.DeclarationMacros.Where(p=>p.Value==DeclarationMacroKind.PublicDeclarations).Select(p=>p.Key).ToHashSet(StringComparer.Ordinal));
            PromoteAnonymousFields(file, cppStandard, file.Converters);
            EnumMembers.Parse(file);
            MacroMetadata.Apply(file);
            return file;
        }

        // Preserve the union as opaque storage, but model the lifted members of
        // its single anonymous struct. No offsets or active union arm are guessed.
        static void PromoteAnonymousFields(ScopeDescriptor scope, int cppStandard, TypeConverterRegistry converters)
        {
            foreach (var child in scope.Declarations.OfType<ScopeDescriptor>().ToArray()) PromoteAnonymousFields(child, cppStandard, converters);
            if (scope is not TypeDescriptor owner) return;
            foreach (var declaration in owner.Declarations.ToArray())
            {
                var tokens = declaration.HeaderTokens.Where(t => !t.IsTrivia).ToList();
                if (tokens.Count < 4 || tokens[0] != "union" || tokens[1] != "{") continue;
                int depth = 1, end = 2;
                for (; end < tokens.Count && depth > 0; ++end) { if (tokens[end] == "{") ++depth; else if (tokens[end] == "}") --depth; }
                if (depth != 0 || !(end == tokens.Count || end == tokens.Count - 1 && tokens[end] == ";")) continue;
                List<(int Start, int End)> groups = [];
                for (int i = 2; i < end - 1; ++i)
                {
                    if (tokens[i] != "struct" || i + 1 >= tokens.Count || tokens[i + 1] != "{") continue;
                    int begin = i + 2, close = begin, nesting = 1;
                    for (; close < end && nesting > 0; ++close) { if (tokens[close] == "{") ++nesting; else if (tokens[close] == "}") --nesting; }
                    if (nesting != 0 || close >= end || tokens[close] != ";") { groups.Clear(); break; }
                    groups.Add((begin, close - 1)); i = close;
                }
                if (groups.Count != 1) continue;
                var group = groups[0];
                var opening = tokens[group.Start - 1]; var closing = tokens[group.End];
                int originalStart = declaration.HeaderTokens.FindIndex(t => t.line == opening.line && t.col == opening.col && t == "{") + 1;
                int originalEnd = declaration.HeaderTokens.FindIndex(t => t.line == closing.line && t.col == closing.col && t == "}");
                var parsed = PreParse(new Tokenizer.Stream(declaration.HeaderTokens.GetRange(originalStart, originalEnd - originalStart)), new(owner.NamespaceTokens), cppStandard, converters);
                if (parsed.Errors.Any(e => e.BlocksGeneration) || parsed.Declarations.Any(d => d is not FieldDescriptor)) continue;
                if (declaration is FieldDescriptor placeholder)
                {
                    owner.Fields.Remove(placeholder); owner.Declarations.Remove(placeholder);
                    owner.Add(new RawDeclarationDescriptor { HeaderTokens = new(declaration.HeaderTokens), Name = tokens[0], Access = declaration.Access,
                        NamespaceTokens = new(owner.NamespaceTokens), EnclosingTypeTokens = new(owner.EnclosingTypeTokens) });
                }
                foreach (var field in parsed.Fields)
                {
                    field.Api = new(); // Metadata is applied once, in the actual enclosing file.
                    SetContext(field, owner.NamespaceTokens, AppendScope(owner.EnclosingTypeTokens, [owner.Name]), declaration.Access);
                    owner.Add(field);
                }
            }
        }

        static void ReadScope(Tokenizer.Stream stream, ScopeDescriptor scope,
            List<Token> namespaceTokens, List<Token> enclosingTypeTokens, HashSet<string> macros, HashSet<string> publicMacros)
        {
            List<Token> declarationTokens = [];
            string access = scope is TypeDescriptor owner ? owner.DefaultAccess : "public";
            while (!stream.IsEOF)
            {
                ReadHeaderTrivia(stream, declarationTokens);
                if (stream.IsEOF)
                    break;
                var tok = stream.Peek();
                if (tok == "#")
                {
                    declarationTokens.Clear();
                    SkipDirective(stream);
                    continue;
                }
                if (tok.kind == Kind.Identifier)
                {
                    int next = 1;
                    while (stream.Peek(next).IsTrivia) ++next;
                    if ((tok.text.StartsWith("API_", StringComparison.Ordinal) || tok.text.StartsWith("DECLARE_SCRIPTING_", StringComparison.Ordinal) || macros.Contains(tok.text)) && stream.Peek(next) == "(")
                    {
                        declarationTokens.Add(stream.Next());
                        ReadHeaderTrivia(stream, declarationTokens);
                        declarationTokens.AddRange(ReadGroup(stream, "(", ")"));
                        if(scope is TypeDescriptor && publicMacros.Contains(tok.text))
                        {
                            RawDeclarationDescriptor declaration=new() { Name=tok,HeaderTokens=new(declarationTokens) };
                            SetContext(declaration,namespaceTokens,enclosingTypeTokens,access);scope.Add(declaration);
                            declarationTokens.Clear();access="public";
                            int end=0;while(stream.Peek(end).IsTrivia)++end;
                            if(stream.Peek(end)==";")stream.Advance(end+1);
                        }
                        continue;
                    }
                    switch (tok.text)
                    {
                        case "template":
                            declarationTokens.Add(stream.Next()); ReadHeaderTrivia(stream, declarationTokens);
                            if (stream.Peek() == "<") declarationTokens.AddRange(ReadGroup(stream, "<", ">"));
                            continue;
                        case "typedef":
                        case "using":
                            // Alias declarations own their parentheses/braces; never classify them as functions.
                            declarationTokens.Add(stream.Next());
                            while (!stream.IsEOF && stream.Peek() != ";")
                            {
                                var token = stream.Peek();
                                if (token.text is "(" or "[" or "{") declarationTokens.AddRange(ReadGroup(stream, token.text, token == "(" ? ")" : token == "[" ? "]" : "}"));
                                else declarationTokens.Add(stream.Next());
                            }
                            if (stream.Peek() == ";") declarationTokens.Add(stream.Next());
                            AddFields(scope, declarationTokens, namespaceTokens, enclosingTypeTokens, access);
                            declarationTokens.Clear();
                            continue;
                        case "namespace":
                            declarationTokens.Clear();
                            ReadNamespace(stream, namespaceTokens, enclosingTypeTokens, scope, macros, publicMacros);
                            continue;
                        case "inline":
                            declarationTokens.Add(stream.Next());
                            ReadHeaderTrivia(stream, declarationTokens);
                            if (stream.Peek() == "namespace")
                            {
                                declarationTokens.Clear();
                                stream.Next();
                                SkipUnsupportedNamespace(stream);
                            }
                            continue;
                        case "class":
                        case "struct":
                        case "interface":
                        case "__interface":
                            // A friend declaration refers to another type; it does not
                            // declare a nested member that can shadow the real type.
                            if (declarationTokens.FirstOrDefault(t => !t.IsTrivia) == "friend") { declarationTokens.Add(stream.Next()); continue; }
                            if (InTemplateArgument(declarationTokens)) { declarationTokens.Add(stream.Next()); continue; }
                            int anonymousOffset = 1;
                            while (stream.Peek(anonymousOffset).IsTrivia) ++anonymousOffset;
                            if (stream.Peek(anonymousOffset) == "{")
                            {
                                var record = ReadAnonymousRecord(stream, declarationTokens);
                                SetContext(record, namespaceTokens, enclosingTypeTokens, access);
                                scope.Add(record);
                                declarationTokens.Clear();
                                continue;
                            }
                            if (IsElaboratedTypeUse(stream, declarationTokens))
                            {
                                // e.g. static class TaskGraphSystem* System;
                                // Leave the class-key in the field/return-type tokens.
                                declarationTokens.Add(stream.Next());
                                continue;
                            }
                            if (TryReadTemplateSpecialization(stream, declarationTokens, out var specialization))
                            {
                                SetContext(specialization!, namespaceTokens, enclosingTypeTokens, access);
                                scope.Add(specialization!); declarationTokens.Clear(); continue;
                            }
                            var type = ReadClass(stream, declarationTokens.Any(t => t == "API_INTERFACE"));
                            type.HeaderTokens.InsertRange(0, declarationTokens);
                            ReadTemplateParameters(type, type.TemplateParameters);
                            type.RequiresNativeFieldAccess = type.Body.Any(token => !token.IsTrivia && token == "union");
                            declarationTokens.Clear();
                            SetContext(type, namespaceTokens, enclosingTypeTokens, access);
                            if (type.Body.Count >= 2 && type.Body[^1] == "}")
                            {
                                var body = new Tokenizer.Stream(type.Body.GetRange(1, type.Body.Count - 2));
                                ReadScope(body, type, namespaceTokens, AppendScope(enclosingTypeTokens, [type.Name]), macros, publicMacros);
                                type.RequiresNativeFieldAccess |= type.Fields.Any(f => f.IsBitField);
                                stream.Errors.AddRange(body.Errors);
                            }
                            scope.Add(type);
                            continue;
                        case "enum":
                            if (InTemplateArgument(declarationTokens)) { declarationTokens.Add(stream.Next()); continue; }
                            var enumeration = ReadEnum(stream);
                            enumeration.HeaderTokens.InsertRange(0, declarationTokens);
                            declarationTokens.Clear();
                            SetContext(enumeration, namespaceTokens, enclosingTypeTokens, access);
                            scope.Add(enumeration);
                            continue;
                    }
                }
                if (tok.text is "public" or "protected" or "private")
                {
                    int offset = 1;
                    while (stream.Peek(offset).IsTrivia)
                        offset++;
                    if (stream.Peek(offset) == ":")
                    {
                        access = tok.text;
                        stream.Advance(offset + 1);
                        declarationTokens.Clear();
                        continue;
                    }
                }
                if (tok == ";")
                {
                    declarationTokens.Add(stream.Next());
                    var significant = declarationTokens.Where(t => !t.IsTrivia).ToList();
                    if (IsStandaloneInvocation(significant) &&
                        (macros.Contains(significant[0].text) || significant[0].text.StartsWith("API_", StringComparison.Ordinal) || significant[0].text.StartsWith("DECLARE_SCRIPTING_", StringComparison.Ordinal)))
                    {
                        RawDeclarationDescriptor macro = new() { HeaderTokens = new(declarationTokens) };
                        SetContext(macro, namespaceTokens, enclosingTypeTokens, access); scope.Add(macro);
                    }
                    else AddFields(scope, declarationTokens, namespaceTokens, enclosingTypeTokens, access);
                    declarationTokens.Clear();
                    continue;
                }
                if (tok == "(" && !InTemplateArgument(declarationTokens) && TryReadFunction(stream, declarationTokens, out var function))
                {
                    SetContext(function!, namespaceTokens, enclosingTypeTokens, access);
                    function!.IsTemplate = function.HeaderTokens.Any(t => t == "template");
                    ReadTemplateParameters(function, function.TemplateParameters);
                    ReadParameters(function!);
                    scope.Add(function!);
                    declarationTokens.Clear();
                    continue;
                }
                if (tok == "{" || tok == "(" || tok == "[")
                {
                    declarationTokens.AddRange(ReadGroup(stream, tok.text, tok == "{" ? "}" : tok == "(" ? ")" : "]"));
                    continue;
                }
                declarationTokens.Add(stream.Next());
            }
            if (declarationTokens.Exists(token => !token.IsTrivia))
            {
                RawDeclarationDescriptor raw = new() { HeaderTokens = new(declarationTokens) };
                SetContext(raw, namespaceTokens, enclosingTypeTokens, access);
                scope.Add(raw);
            }
        }

        // Function signature parentheses inside Function<R(A...)> are part of the
        // return/field type, not the enclosing function's parameter declaration.
        static bool IsStandaloneInvocation(List<Token> tokens)
        {
            if(tokens.Count<4 || tokens[1]!="(" || tokens[^1]!=";")return false;
            int depth=0;
            for(int i=1;i<tokens.Count-1;++i)
            {
                if(tokens[i]=="(")++depth;
                else if(tokens[i]==")" && --depth==0)return i==tokens.Count-2;
            }
            return false;
        }

        static bool InTemplateArgument(List<Token> tokens)
        {
            int angle=0,round=0,square=0;
            foreach(var token in tokens)
            {
                if(token=="(")round++;else if(token==")")round--;
                if(token=="[")square++;else if(token=="]")square--;
                if(round!=0 || square!=0)continue;
                if(token=="<")angle++;else if(token==">" && angle>0)angle--;
            }
            return angle>0;
        }

        static void SetContext(DeclarationDescriptor declaration, List<Token> namespaces,
            List<Token> types, string access)
        {
            declaration.NamespaceTokens = new(namespaces);
            declaration.EnclosingTypeTokens = new(types);
            declaration.Access = access;
        }


        static void ReadParameters(FunctionDescriptor function)
        {
            if (function.ParameterTokens.Count < 2 || function.ParameterTokens[^1] != ")")
                return;
            var parameters = function.ParameterTokens.GetRange(1, function.ParameterTokens.Count - 2);
            var meaningful = parameters.FindAll(token => !token.IsTrivia);
            if (meaningful.Count == 0 || (meaningful.Count == 1 && meaningful[0] == "void"))
                return;
            foreach (var part in SplitDeclarators(parameters))
            {
                var field = ReadField(part, true, []);
                field ??= new FieldDescriptor { HeaderTokens = new(part), TypeTokens = new(part), IsParameter = true };
                field.Index = function.Fields.Count;
                field.Parent = function;
                SetContext(field, function.NamespaceTokens, function.EnclosingTypeTokens, "");
                function.Fields.Add(field);
            }
        }

        static void AddFields(ScopeDescriptor scope, List<Token> declaration, List<Token> namespaces,
            List<Token> enclosingTypes, string access)
        {
            var tokens = new List<Token>(declaration);
            if (tokens.Count != 0 && tokens[^1] == ";")
                tokens.RemoveAt(tokens.Count - 1);
            var first = tokens.Find(token => !token.IsTrivia);
            if (first.text == null)
                return;
            var prefix = new Tokenizer.Stream(new(tokens));
            SkipHeaderTrivia(prefix);
            while (prefix.Peek() == "template" || prefix.Peek().text?.StartsWith("API_", StringComparison.Ordinal) == true)
            {
                if (prefix.Peek() == "template")
                {
                    prefix.Next(); SkipHeaderTrivia(prefix);
                    if (prefix.Peek() != "<") break;
                    ReadGroup(prefix, "<", ">"); SkipHeaderTrivia(prefix);
                    continue;
                }
                prefix.Next(); SkipHeaderTrivia(prefix);
                if (prefix.Peek() == "(") ReadGroup(prefix, "(", ")");
                SkipHeaderTrivia(prefix);
            }
            var aliasTokens = tokens.Skip(prefix.Index).ToList();
            if (aliasTokens.FirstOrDefault(t => !t.IsTrivia).text is "typedef" or "using" && ReadAliases(scope, aliasTokens, declaration, namespaces, enclosingTypes, access)) return;
            if (first.text is "typedef" or "using" && ReadAliases(scope, tokens, declaration, namespaces, enclosingTypes, access)) return;
            if (first.text is "using" or "typedef" or "static_assert" or "friend")
            {
                RawDeclarationDescriptor raw = new() { HeaderTokens = new(declaration) };
                SetContext(raw, namespaces, enclosingTypes, access);
                scope.Add(raw);
                return;
            }
            List<Token> commonType = [];
            List<List<Token>> commonAttributes = [];
            foreach (var part in SplitDeclarators(tokens))
            {
                var field = ReadField(part, false, commonType);
                if (field == null)
                {
                    RawDeclarationDescriptor raw = new() { HeaderTokens = new(part) };
                    SetContext(raw, namespaces, enclosingTypes, access);
                    scope.Add(raw);
                    continue;
                }
                if (commonAttributes.Count == 0 && field.AttributeTokens.Count > 0)
                    commonAttributes = field.AttributeTokens.Select(group => new List<Token>(group)).ToList();
                else if (field.AttributeTokens.Count == 0)
                    field.AttributeTokens.AddRange(commonAttributes.Select(group => new List<Token>(group)));
                field.Index = scope.Fields.Count;
                SetContext(field, namespaces, enclosingTypes, access);
                scope.Add(field);
                if (commonType.Count == 0)
                {
                    // Pointer/reference declarators belong to this field, not later comma-separated fields.
                    int pointer = field.TypeTokens.FindIndex(token => token == "*" || token == "&" || token == "(");
                    commonType = pointer >= 0 ? field.TypeTokens.GetRange(0, pointer) : new(field.TypeTokens);
                }
            }
        }

        static bool ReadAliases(ScopeDescriptor scope, List<Token> tokens, List<Token> original, List<Token> namespaces, List<Token> enclosingTypes, string access)
        {
            var clean = tokens.Where(t => !t.IsTrivia).ToList();
            if (clean.Count == 0) return false;
            if (clean[0] == "using")
            {
                if (clean.Count < 4 || clean[1].kind != Kind.Identifier || clean[2] != "=") return false;
                Add(clean[1], clean.Skip(3).ToList()); return true;
            }
            // Inline type definitions require their own type declarations; do not guess their layout.
            if (clean.Any(t => t == "{")) return false;
            List<Token> common = [];
            foreach (var part in SplitDeclarators(clean.Skip(1).ToList()))
            {
                var field = ReadField(common.Concat(part).ToList(), false, []);
                if (field == null || string.IsNullOrEmpty(field.Name.text)) return false;
                var target = field.TypeTokens.Concat(field.DeclaratorTokens.Where(t => !(t.line == field.Name.line && t.col == field.Name.col))).ToList();
                Add(field.Name, target);
                if (common.Count == 0)
                {
                    int end = field.TypeTokens.FindIndex(t => t.text is "*" or "&" or "(");
                    common = end < 0 ? new(field.TypeTokens) : field.TypeTokens.Take(end).ToList();
                }
            }
            return true;
            void Add(Token name, List<Token> target)
            {
                AliasDescriptor alias = new() { Name = name, HeaderTokens = new(original), TargetTokens = target };
                ReadTemplateParameters(alias, alias.TemplateParameters);
                SetContext(alias, namespaces, enclosingTypes, access); scope.Add(alias);
            }
        }

        static bool TryReadTemplateSpecialization(Tokenizer.Stream stream, List<Token> prefix, out RawDeclarationDescriptor? declaration)
        {
            declaration = null;
            if (!prefix.Any(token => token == "template")) return false;
            bool specialized = false; int square = 0, round = 0;
            for (int i = 1; !stream.IsEOF && stream.Peek(i).kind != Kind.EndOfFile; i++)
            {
                var token = stream.Peek(i);
                if (round == 0 && square == 0 && (token == "{" || token == ";")) break;
                if (round == 0 && square == 0 && token == ":" && stream.Peek(i - 1) != ":" && stream.Peek(i + 1) != ":") break;
                if (token == "[") square++; if (token == "]") square--;
                if (token == "(") round++; if (token == ")") round--;
                if (round == 0 && square == 0 && token == "<") { specialized = true; break; }
            }
            if (!specialized) return false;
            List<Token> tokens = new(prefix);
            while (!stream.IsEOF)
            {
                var token = stream.Peek();
                if (token == "{") { tokens.AddRange(ReadGroup(stream, "{", "}")); SkipHeaderTrivia(stream); if (stream.Peek() == ";") tokens.Add(stream.Next()); break; }
                tokens.Add(stream.Next()); if (token == ";") break;
            }
            declaration = new() { HeaderTokens = tokens };
            return true;
        }

        static void ReadTemplateParameters(DeclarationDescriptor type, List<TemplateParameterDescriptor> target)
        {
            var tokens = type.HeaderTokens.Where(t => !t.IsTrivia).ToList();
            int begin = tokens.FindIndex(t => t == "template");
            if (begin < 0 || begin + 1 >= tokens.Count || tokens[begin + 1] != "<") return;
            int depth = 1, end = begin + 2;
            while (end < tokens.Count && depth != 0) { if (tokens[end] == "<") depth++; if (tokens[end] == ">") depth--; if (depth != 0) end++; }
            List<List<Token>> parameters = []; List<Token> current = []; int angle = 0, round = 0, square = 0;
            foreach (var token in tokens.Skip(begin + 2).Take(end - begin - 2))
            {
                if (token == "," && angle == 0 && round == 0 && square == 0) { parameters.Add(current); current = []; continue; }
                if (token == "<") angle++; if (token == ">") angle--;
                if (token == "(") round++; if (token == ")") round--;
                if (token == "[") square++; if (token == "]") square--;
                current.Add(token);
            }
            if (current.Count > 0) parameters.Add(current);
            foreach (var part in parameters)
            {
                int equals = part.FindIndex(t => t == "=");
                var declaration = equals < 0 ? part : part.Take(equals).ToList();
                var name = declaration.LastOrDefault(t => t.kind == Kind.Identifier);
                target.Add(new() { Name = name, IsType = declaration.FirstOrDefault().text is "typename" or "class", IsPack = declaration.Any(t => t == "."), Tokens = new(part), DefaultTokens = equals < 0 ? [] : part.Skip(equals + 1).ToList() });
            }
        }

        static List<List<Token>> SplitDeclarators(List<Token> tokens)
        {
            List<List<Token>> parts = [];
            List<Token> current = [];
            int round = 0, square = 0, curly = 0, angle = 0;
            bool initializer = false;
            foreach (var token in tokens)
            {
                if (token == "," && round == 0 && square == 0 && curly == 0 && angle == 0)
                {
                    parts.Add(current);
                    current = [];
                    initializer = false;
                    continue;
                }
                current.Add(token);
                if (token == "(") round++;
                else if (token == ")") round--;
                else if (token == "[") square++;
                else if (token == "]") square--;
                else if (token == "{") curly++;
                else if (token == "}") curly--;
                else if (round == 0 && square == 0 && curly == 0)
                {
                    if (token == "=") initializer = true;
                    if (!initializer && token == "<") angle++;
                    else if (!initializer && token == ">" && angle > 0) angle--;
                }
            }
            if (current.Exists(token => !token.IsTrivia))
                parts.Add(current);
            return parts;
        }

        static FieldDescriptor? ReadField(List<Token> raw, bool parameter, List<Token> commonType)
        {
            FieldDescriptor field = new() { HeaderTokens = new(raw), IsParameter = parameter };
            var reader = new Tokenizer.Stream(new(raw));
            SkipHeaderTrivia(reader);
            while (reader.Peek().text?.StartsWith("API_", StringComparison.Ordinal) == true)
            {
                reader.Next(); SkipHeaderTrivia(reader);
                if (reader.Peek() != "(") break;
                ReadGroup(reader, "(", ")"); SkipHeaderTrivia(reader);
            }
            while (reader.Peek() == "[")
            {
                field.AttributeTokens.Add(ReadGroup(reader, "[", "]"));
                SkipHeaderTrivia(reader);
            }
            var tokens = raw.GetRange(reader.Index, raw.Count - reader.Index);
            int end = tokens.Count;
            int round = 0, square = 0, curly = 0, angle = 0;
            int nameIndex = -1;
            for (int i = 0; i < tokens.Count; i++)
            {
                var token = tokens[i];
                if (!parameter && round == 0 && square == 0 && curly == 0 && angle == 0 && token == ":" &&
                    !(i > 0 && tokens[i - 1] == ":") && !(i + 1 < tokens.Count && tokens[i + 1] == ":") &&
                    !tokens.Take(i).Any(t => t == "("))
                {
                    field.IsBitField = true;
                    end = i;
                    int widthEnd = tokens.FindIndex(i + 1, t => t == "=" || t == "{");
                    if (widthEnd < 0) widthEnd = tokens.Count;
                    field.BitWidthTokens = tokens.GetRange(i + 1, widthEnd - i - 1);
                    if (widthEnd < tokens.Count)
                        field.InitializerTokens = tokens.GetRange(tokens[widthEnd] == "=" ? widthEnd + 1 : widthEnd, tokens.Count - widthEnd - (tokens[widthEnd] == "=" ? 1 : 0));
                    break;
                }
                if (round == 0 && square == 0 && curly == 0 && angle == 0 && (token == "=" || token == "{"))
                {
                    end = i;
                    break;
                }
                if (round == 0 && square == 0 && curly == 0 && angle == 0 && token.kind == Kind.Identifier)
                    nameIndex = i;
                if (token == "(") round++;
                else if (token == ")") round--;
                else if (token == "[") square++;
                else if (token == "]") square--;
                else if (token == "{") curly++;
                else if (token == "}") curly--;
                else if (round == 0 && square == 0 && curly == 0)
                {
                    if (token == "<") angle++;
                    else if (token == ">" && angle > 0) angle--;
                }
            }
            // A named function-pointer/reference field has its name inside (*name) / (&name).
            for (int i = 0; i < end; i++)
            {
                if (tokens[i] != "(") continue;
                int pointer = i + 1;
                while (pointer < end && tokens[pointer].IsTrivia) pointer++;
                if (pointer == end || (tokens[pointer] != "*" && tokens[pointer] != "&")) continue;
                int candidate = pointer + 1;
                while (candidate < end && (tokens[candidate].IsTrivia || tokens[candidate].text is "const" or "volatile")) candidate++;
                nameIndex = candidate < end && tokens[candidate].kind == Kind.Identifier ? candidate : -1;
                break;
            }
            bool named = nameIndex >= 0;
            if (field.IsBitField && named &&
                (tokens[nameIndex].text is "bool" or "char" or "wchar_t" or "signed" or "unsigned" or "short" or "int" or "long" ||
                 !tokens.Take(nameIndex).Any(t => t.kind == Kind.Identifier))) named = false;
            if (named && parameter)
            {
                var candidate = tokens[nameIndex];
                int previous = nameIndex - 1;
                while (previous >= 0 && tokens[previous].IsTrivia) previous--;
                bool typeBefore = tokens.Take(nameIndex).Any(token => token.kind == Kind.Identifier &&
                    token.text is not ("const" or "volatile" or "typename" or "class" or "struct" or "enum"));
                if (candidate.text is "void" or "bool" or "char" or "char8_t" or "char16_t" or "char32_t" or "wchar_t"
                    or "signed" or "unsigned" or "short" or "int" or "long" or "float" or "double"
                    || !typeBefore || (previous >= 0 && tokens[previous] == ":"))
                    named = false;
            }
            if (named)
            {
                field.Name = tokens[nameIndex];
                field.TypeTokens = tokens.GetRange(0, nameIndex);
                if (!field.TypeTokens.Exists(token => !token.IsTrivia))
                    field.TypeTokens.InsertRange(0, commonType);
                if (!parameter && !field.TypeTokens.Exists(token => !token.IsTrivia))
                    return null;
                field.DeclaratorTokens = tokens.GetRange(nameIndex, end - nameIndex);
            }
            else
            {
                if (!parameter && !field.IsBitField) return null;
                field.TypeTokens = tokens.GetRange(0, end);
            }
            if (end < tokens.Count && !field.IsBitField)
                field.InitializerTokens = tokens.GetRange(tokens[end] == "=" ? end + 1 : end,
                    tokens.Count - end - (tokens[end] == "=" ? 1 : 0));
            return field;
        }


        static List<Token> AppendScope(List<Token> parent, List<Token> child)
        {
            List<Token> result = new(parent);
            if (child.Count == 0 || string.IsNullOrEmpty(child[0].text))
                return result;
            if (result.Count != 0)
            {
                // Separator between nested scope blocks; original name tokens keep their locations.
                var separator = child[0];
                separator.kind = Kind.ControlChar;
                separator.text = ":";
                result.Add(separator);
                result.Add(separator);
            }
            result.AddRange(child);
            return result;
        }

        static void ReadNamespace(Tokenizer.Stream stream, List<Token> namespaceTokens,
            List<Token> enclosingTypeTokens, ScopeDescriptor pack, HashSet<string> macros, HashSet<string> publicMacros)
        {
            stream.Next(); // namespace
            SkipHeaderTrivia(stream);
            if (stream.Peek() == "{" || stream.Peek() == "[")
            {
                // Anonymous namespaces and namespace attributes are unsupported: skip their bodies.
                SkipUnsupportedNamespace(stream);
                return;
            }

            List<Token> names = [];
            if (!stream.TryExpect(Kind.Identifier, out var name))
                return;
            names.Add(name);
            SkipHeaderTrivia(stream);
            while (stream.Peek() == ":")
            {
                names.Add(stream.Next());
                if (!stream.TryExpect(Kind.ControlChar, out var colon, ":"))
                    return;
                names.Add(colon);
                SkipHeaderTrivia(stream);
                if (!stream.TryExpect(Kind.Identifier, out name))
                    return;
                if (name == "inline")
                {
                    SkipUnsupportedNamespace(stream);
                    return;
                }
                names.Add(name);
                SkipHeaderTrivia(stream);
            }
            if (stream.Peek() == "=")
            {
                SkipUnsupportedNamespace(stream); // namespace alias, no new body to visit.
                return;
            }
            if (stream.Peek() != "{")
            {
                stream.Errors.Add(new LogEntry("Expected '{' after namespace name.", stream.Peek()));
                return;
            }

            var tokens = ReadGroup(stream, "{", "}");
            if (tokens.Count < 2 || tokens[^1] != "}")
                return;
            var body = new Tokenizer.Stream(tokens.GetRange(1, tokens.Count - 2));
            ReadScope(body, pack, AppendScope(namespaceTokens, names), enclosingTypeTokens, macros, publicMacros);
            stream.Errors.AddRange(body.Errors);
        }

        static void SkipUnsupportedNamespace(Tokenizer.Stream stream)
        {
            while (!stream.IsEOF)
            {
                var token = stream.Peek();
                if (token == "{")
                {
                    ReadGroup(stream, "{", "}");
                    return;
                }
                if (token == ";")
                {
                    stream.Next();
                    return;
                }
                if (token == "[" || token == "(")
                    ReadGroup(stream, token.text, token == "[" ? "]" : ")");
                else
                    stream.Next();
            }
        }


        static bool TryReadFunction(Tokenizer.Stream stream, List<Token> headerTokens,
            out FunctionDescriptor? source)
        {
            source = null;
            if (stream.Peek() != "(")
                return false;

            int nameIndex = headerTokens.Count - 1;
            while (nameIndex >= 0 && headerTokens[nameIndex].IsTrivia)
                nameIndex--;
            int operatorIndex = headerTokens.FindLastIndex(token => token == "operator");
            if (operatorIndex >= 0) nameIndex = operatorIndex;
            if (nameIndex < 0 || headerTokens[nameIndex].kind != Kind.Identifier)
                return false;
            var name = headerTokens[nameIndex];
            if (name.text is "alignas" or "decltype" or "sizeof" or "alignof" or "noexcept"
                or "requires" or "static_assert" or "__declspec" or "__attribute__")
                return false;
            int firstParameter = 1;
            while (stream.Peek(firstParameter).IsTrivia)
                firstParameter++;
            if (stream.Peek(firstParameter) == "*" || stream.Peek(firstParameter) == "&")
                return false; // e.g. void (*callback)(int), not a function named void.
            // A call in a field initializer is not a function declaration.
            int initializerRound = 0, initializerSquare = 0, initializerAngle = 0;
            foreach (var token in headerTokens.Take(operatorIndex >= 0 ? operatorIndex : headerTokens.Count))
            {
                if (token == "=" && initializerRound == 0 && initializerSquare == 0 && initializerAngle == 0) return false;
                if (token == "(") ++initializerRound; else if (token == ")") --initializerRound;
                if (token == "[") ++initializerSquare; else if (token == "]") --initializerSquare;
                if (token == "<") ++initializerAngle; else if (token == ">" && initializerAngle > 0) --initializerAngle;
            }

            source = new FunctionDescriptor { Name = name, HeaderTokens = new(headerTokens) };
            if (operatorIndex >= 0)
            {
                source.NameTokens.AddRange(headerTokens.Skip(operatorIndex).Where(token => !token.IsTrivia));
                source.Name.text = string.Concat(source.NameTokens.Select(token => token.text));
            }
            else source.NameTokens.Add(name);
            int previous = nameIndex - 1;
            while (previous >= 0 && headerTokens[previous].IsTrivia)
                previous--;
            if (previous >= 0 && headerTokens[previous] == "~")
            {
                source.NameTokens.Insert(0, headerTokens[previous]);
                source.Name.text = "~" + name.text;
            }

            source.ParameterTokens = ReadGroup(stream, "(", ")");
            source.HeaderTokens.AddRange(source.ParameterTokens);
            while (!stream.IsEOF)
            {
                var token = stream.Peek();
                if (token == "{")
                {
                    source.Body = ReadGroup(stream, "{", "}");
                    return true;
                }
                if (token == ";")
                {
                    source.HeaderTokens.Add(stream.Next());
                    return true;
                }
                if (token == "(")
                    source.HeaderTokens.AddRange(ReadGroup(stream, "(", ")"));
                else if (token == "[")
                    source.HeaderTokens.AddRange(ReadGroup(stream, "[", "]"));
                else if (token == ":")
                {
                    // Constructor initializer list. Its braced member initializers are
                    // part of the header, not the function body.
                    source.HeaderTokens.Add(stream.Next());
                    ReadConstructorInitializers(stream, source.HeaderTokens);
                }
                else
                    source.HeaderTokens.Add(stream.Next());
            }
            stream.Errors.Add(new LogEntry("Expected a function body or ';' after the function header.", source.Name));
            return true;
        }

        static void ReadConstructorInitializers(Tokenizer.Stream stream, List<Token> header)
        {
            while (!stream.IsEOF)
            {
                ReadHeaderTrivia(stream, header);
                if (!stream.TryExpect(Kind.Identifier, out var name))
                    return;
                header.Add(name);

                // Keep a qualified/template base or member name until its initializer group.
                while (!stream.IsEOF)
                {
                    var token = stream.Peek();
                    if (token == "(" || token == "{")
                        break;
                    if (token == ";" || token == "}")
                    {
                        stream.Errors.Add(new LogEntry("Expected a constructor initializer group.", token));
                        return;
                    }
                    if (token == "<")
                        header.AddRange(ReadGroup(stream, "<", ">"));
                    else
                        header.Add(stream.Next());
                }
                if (stream.IsEOF)
                    return;
                var opening = stream.Peek();
                header.AddRange(ReadGroup(stream, opening.text, opening == "(" ? ")" : "}"));
                ReadHeaderTrivia(stream, header);
                if (stream.Peek() != ",")
                    return;
                header.Add(stream.Next());
            }
        }


        // Anonymous records have no addressable type name. Preserve them intact for
        // later handlers without interpreting their members as enclosing members.
        static RawDeclarationDescriptor ReadAnonymousRecord(Tokenizer.Stream stream, List<Token> prefix)
        {
            RawDeclarationDescriptor record = new() { HeaderTokens = new(prefix) };
            var keyword = stream.Next();
            record.HeaderTokens.Add(keyword);
            record.Name = keyword;
            while (stream.Peek().IsTrivia) record.HeaderTokens.Add(stream.Next());
            record.HeaderTokens.AddRange(ReadGroup(stream, "{", "}"));
            bool named = false;
            while (!stream.IsEOF && stream.Peek() != ";" && stream.Peek() != "}")
            {
                var token = stream.Peek();
                if (!named && token.kind == Token.Kind.Identifier) { record.Name = token; named = true; }
                if (token.text is "(" or "[" or "{")
                    record.HeaderTokens.AddRange(ReadGroup(stream, token.text, token == "(" ? ")" : token == "[" ? "]" : "}"));
                else record.HeaderTokens.Add(stream.Next());
            }
            if (stream.Peek() == ";") record.HeaderTokens.Add(stream.Next());
            else stream.TryExpect(Token.Kind.ControlChar, out _, ";");
            return record;
        }

        static bool IsElaboratedTypeUse(Tokenizer.Stream stream, List<Token> prefix)
        {
            int prefixRound = 0, prefixSquare = 0;
            foreach (var token in prefix)
            {
                if (token == "(") { ++prefixRound; continue; }
                if (token == ")") { if (prefixRound > 0) --prefixRound; continue; }
                if (token == "[") { ++prefixSquare; continue; }
                if (token == "]") { if (prefixSquare > 0) --prefixSquare; continue; }
                if (prefixRound == 0 && prefixSquare == 0 && token.text is "static" or "extern" or "mutable" or "const" or "volatile" or "API_FIELD" or "API_FUNCTION" or "API_PROPERTY") return true;
            }
            int round = 0, square = 0, angle = 0;
            for (int offset = 1; ; ++offset)
            {
                var token = stream.Peek(offset);
                if (token.kind == Kind.EndOfFile) return false;
                if (token.IsTrivia) continue;
                if (round == 0 && square == 0 && angle == 0)
                {
                    if (token == "*" || token == "&") return true;
                    if (token == ";" || token == "{") return false;
                    if (token == ":" && stream.Peek(offset - 1) != ":" && stream.Peek(offset + 1) != ":") return false;
                }
                if (token == "(") ++round; else if (token == ")" && round > 0) --round;
                if (token == "[") ++square; else if (token == "]" && square > 0) --square;
                if (round == 0 && square == 0) { if (token == "<") ++angle; else if (token == ">" && angle > 0) --angle; }
            }
        }

        static TypeDescriptor ReadClass(Tokenizer.Stream stream, bool apiInterface = false)
        {
            // class|struct|interface [attributes/macros] Name [final] [: bases] {body};
            var keyword = stream.Next();
            TypeDescriptor source = apiInterface ? new InterfaceDescriptor() : keyword.text switch
            {
                "struct" => new StructDescriptor(),
                "interface" or "__interface" => new InterfaceDescriptor(),
                _ => new ClassDescriptor(),
            };
            source.Keyword = keyword;
            source.HeaderTokens.Add(source.Keyword);

            List<Token> chains = [];
            while (!stream.IsEOF)
            {
                var token = stream.Peek();
                if (token == ":" || token == "{" || token == ";")
                    break;
                if (token == "[" || token == "(")
                    chains.AddRange(ReadGroup(stream, token.text, token == "[" ? "]" : ")"));
                else
                    chains.Add(stream.Next());
            }
            source.HeaderTokens.AddRange(chains);

            // Name and optional final are at the end; attribute/macro chains precede them.
            chains.Reverse();
            Tokenizer.Stream header = new(chains);
            SkipHeaderTrivia(header);
            if (header.Peek() == "final")
            {
                // 'final' can itself be a class name: class final {};
                int nameIndex = header.Index + 1;
                while (nameIndex < header.Tokens.Count && header.Tokens[nameIndex].IsTrivia)
                    nameIndex++;
                if (nameIndex < header.Tokens.Count && header.Tokens[nameIndex].kind == Kind.Identifier)
                {
                    source.IsFinal = true;
                    header.Next();
                    SkipHeaderTrivia(header);
                }
            }

            header.TryExpect(Kind.Identifier, out source.Name);
            while (!header.IsEOF)
            {
                SkipHeaderTrivia(header);
                if (header.IsEOF)
                    break;
                var token = header.Peek();
                if (token == "]")
                {
                    var group = ReadGroup(header, "]", "[");
                    group.Reverse();
                    source.AttributeTokens.Add(group);
                }
                else if (token == ")")
                {
                    var group = ReadGroup(header, ")", "(");
                    SkipHeaderTrivia(header);
                    if (!header.TryExpect(Kind.Identifier, out var macroName))
                        break;
                    group.Add(macroName);
                    group.Reverse();
                    source.AttributeTokens.Add(group);
                }
                else if (token.kind == Kind.Identifier)
                    source.AttributeTokens.Add([header.Next()]);
                else
                {
                    header.Errors.Add(new LogEntry("Unexpected token in class attribute/macro chain.", token));
                    header.Next();
                }
            }
            source.AttributeTokens.Reverse();
            stream.Errors.AddRange(header.Errors);

            if (stream.Peek() == ":")
            {
                var colon = stream.Next();
                source.HeaderTokens.Add(colon);
                while (!stream.IsEOF && stream.Peek() != "{" && stream.Peek() != ";")
                {
                    var token = stream.Peek();
                    if (token == "(" || token == "[" || token == "<")
                    {
                        string closing = token == "(" ? ")" : token == "[" ? "]" : ">";
                        source.BaseTokens.AddRange(ReadGroup(stream, token.text, closing));
                    }
                    else
                        source.BaseTokens.Add(stream.Next());
                }
                source.HeaderTokens.AddRange(source.BaseTokens);
                if (!source.BaseTokens.Exists(token => !token.IsTrivia))
                    stream.Errors.Add(new LogEntry("Expected a base type after ':'.", colon));
                if (stream.Peek() == ";")
                    stream.Errors.Add(new LogEntry("Expected a class body after the base list.", stream.Peek()));
            }

            if (stream.Peek() == ";")
            {
                source.IsForwardDeclaration = true;
                source.HeaderTokens.Add(stream.Next());
                return source;
            }
            if (stream.Peek() != "{")
            {
                stream.Errors.Add(new LogEntry("Expected '{' or ';' after the class header.", stream.Peek()));
                return source;
            }

            source.Body = ReadGroup(stream, "{", "}");
            // ReadScope recursively fills the type descriptor; interface rules are validated later.
            stream.SkipNoise();
            if (stream.Peek() == ";")
                stream.Next();
            return source;
        }


        static EnumDescriptor ReadEnum(Tokenizer.Stream stream)
        {
            // enum [class|struct] [attributes/macros] Name [: underlying type] {body};
            // Retain the header tokens for the later attribute/type interpretation pass.
            EnumDescriptor e = new EnumDescriptor();
            var enumToken = stream.Next(); // Consume enum before checking class/struct.
            e.HeaderTokens.Add(enumToken);
            ReadHeaderTrivia(stream, e.HeaderTokens);
            var tok = stream.Peek();
            e.IsClass = tok == "class";
            e.IsStruct = tok == "struct";
            if (e.IsClass || e.IsStruct)
                e.HeaderTokens.Add(stream.Next());

            // Read chains without stopping on ':' or '{' inside attribute/macro arguments.
            List<Token> chainsTokkens = [];
            while (!stream.IsEOF)
            {
                tok = stream.Peek();
                if (tok == "{" || tok == ":" || tok == ";")
                    break;
                if (tok == "[" || tok == "(")
                    chainsTokkens.AddRange(ReadGroup(stream, tok.text, tok == "[" ? "]" : ")"));
                else
                    chainsTokkens.Add(stream.Next());
            }
            e.HeaderTokens.AddRange(chainsTokkens);

            // Last non-trivia token before ':', '{' or ';' is the enum name.
            chainsTokkens.Reverse();
            Tokenizer.Stream chainstream = new(chainsTokkens);
            SkipHeaderTrivia(chainstream);
            if (chainstream.IsEOF)
            {
                if (e.IsClass || e.IsStruct)
                    stream.Errors.Add(new LogEntry("Expected a name for the scoped enum.", enumToken));
            }
            else
            {
                if (chainstream.Peek().kind == Kind.Identifier)
                    e.Name = chainstream.Next();
                else if (e.IsClass || e.IsStruct)
                    stream.Errors.Add(new LogEntry("Expected a name for the scoped enum.", enumToken));

                // Chains are read backwards, then restored to their original order.
                while (!chainstream.IsEOF)
                {
                    SkipHeaderTrivia(chainstream);
                    if (chainstream.IsEOF)
                        break;
                    var ctk = chainstream.Peek();
                    if (ctk == "]")
                    {
                        var group = ReadGroup(chainstream, "]", "[");
                        group.Reverse();
                        e.AttributeTokens.Add(group);
                    }
                    else if (ctk.kind == Token.Kind.Identifier)
                    {
                        e.AttributeTokens.Add([chainstream.Next()]);
                    }
                    else if (ctk == ")")
                    {
                        var group = ReadGroup(chainstream, ")", "(");
                        SkipHeaderTrivia(chainstream);
                        if (!chainstream.TryExpect(Token.Kind.Identifier, out var macroName))
                            break;
                        group.Add(macroName);
                        group.Reverse();
                        e.AttributeTokens.Add(group);
                    }
                    else
                    {
                        chainstream.Errors.Add(new LogEntry("Unexpected token in enum attribute/macro chain.", ctk));
                        chainstream.Next();
                    }
                }
                e.AttributeTokens.Reverse();
            }
            stream.Errors.AddRange(chainstream.Errors);

            tok = stream.Peek();
            if (tok == ":")
            {
                e.HeaderTokens.Add(stream.Next());
                while (!stream.IsEOF && stream.Peek() != "{" && stream.Peek() != ";")
                {
                    var typeToken = stream.Next();
                    e.UnderlyingTypeTokens.Add(typeToken);
                    e.HeaderTokens.Add(typeToken);
                }
                if (!e.UnderlyingTypeTokens.Exists(token => !token.IsTrivia))
                    stream.Errors.Add(new LogEntry("Expected an enum underlying type after ':'.", tok));
            }

            tok = stream.Peek();
            if (tok == ";")
            {
                e.IsForwardDeclaration = true;
                e.HeaderTokens.Add(stream.Next());
                if (string.IsNullOrEmpty(e.Name.text))
                    stream.Errors.Add(new LogEntry("Expected a name for the enum forward declaration.", enumToken));
                return e;
            }
            if (tok != "{")
            {
                stream.Errors.Add(new LogEntry("Expected '{' or ';' after the enum header.", tok));
                return e;
            }

            // Leave '{' unconsumed until ReadGroup, and read from the original stream.
            e.Body = ReadGroup(stream, "{", "}");
            stream.SkipNoise();
            if (stream.Peek() == ";")
                stream.Next();
            return e;
        }

        static void ReadHeaderTrivia(Tokenizer.Stream stream, List<Token> tokens)
        {
            while (!stream.IsEOF && stream.Peek().IsTrivia)
                tokens.Add(stream.Next());
        }

        static void SkipHeaderTrivia(Tokenizer.Stream stream)
        {
            while (!stream.IsEOF && stream.Peek().IsTrivia)
                stream.Next();
        }

        static void SkipDirective(Tokenizer.Stream stream)
        {
            bool continued;
            do
            {
                int line = stream.Peek().line;
                Token last = default;
                while (!stream.IsEOF && stream.Peek().line == line)
                {
                    var token = stream.Next();
                    if (!token.IsTrivia)
                        last = token;
                }
                continued = last == "\\";
            } while (continued && !stream.IsEOF);
        }

        static List<Token> ReadGroup(Tokenizer.Stream stream, string A, string B)
        {
            List<Token> result = [];
            var start = stream.Peek();
            if (start != A)
            {
                stream.Errors.Add(new LogEntry($"Expected '{A}'.", start));
                return result;
            }

            int depth = 0;
            while (!stream.IsEOF)
            {
                var tok = stream.Next();
                result.Add(tok);
                if (tok == A)
                    depth++;
                else if (tok == B && --depth == 0)
                    return result;
            }
            stream.Errors.Add(new LogEntry($"Expected '{B}' to close '{A}'.", start));
            return result;
        }
    }
}
