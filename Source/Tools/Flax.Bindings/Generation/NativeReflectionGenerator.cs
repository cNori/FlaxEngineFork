using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using VETools.Text;

namespace VETools.Generation;

internal static class NativeReflectionGenerator
{
    internal sealed record Result(string Header, string Registration, int TypeCount);
    static string Quote(string value) => JsonSerializer.Serialize(value);
    static string Id(DeclarationDescriptor value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value.NativeQualifiedName)))[..16];
    static bool IsType(DeclarationDescriptor d) => d is TypeDescriptor or EnumDescriptor;
    static IEnumerable<DeclarationDescriptor> Types(ScopeDescriptor scope)
    {
        foreach (var d in scope.Declarations)
        {
            if (IsType(d)) yield return d;
            if (d is ScopeDescriptor child) foreach (var nested in Types(child)) yield return nested;
        }
    }
    static bool CanName(DeclarationDescriptor value)
    {
        if (!Regex.IsMatch(value.Name.text ?? "", "^[A-Za-z_]\\w*$")) return false;
        for (var current = value; current != null; current = current.Parent)
        {
            if (current is TypeDescriptor type && (type.TemplateParameters.Count != 0 || type.IsForwardDeclaration)) return false;
            if (current.Parent is TypeDescriptor && current.Access != "public") return false;
        }
        return true;
    }
    static string NestedName(TypeInfo? info)
    {
        while (true)
        {
            if (info is NamedTypeInfo { Resolution: TypeResolution.Ambiguous }) return "";
            if (info is ReferenceTypeInfo reference) { info = reference.Referent; continue; }
            if (info is ArrayTypeInfo array) { info = array.ElementType; continue; }
            if (info is NamedTypeInfo { Name: "Array", Arguments.Count: > 0 } sequence) { info = sequence.Arguments[0].Type; continue; }
            return info is NamedTypeInfo { Specialization.Concrete: { } concrete } ? concrete.NativeQualifiedName : info is NamedTypeInfo { Symbol.Definition: DeclarationDescriptor type } ? type.NativeQualifiedName : "";
        }
    }
    static bool Constant(TypeInfo? info) => info switch
    {
        NamedTypeInfo n => (n.EffectiveQualifiers & TypeQualifiers.Const) != 0,
        PointerTypeInfo p => (p.Qualifiers & TypeQualifiers.Const) != 0,
        ReferenceTypeInfo r => Constant(r.Referent),
        ArrayTypeInfo a => Constant(a.ElementType),
        _ => false
    };
    static string TypeName(TypeInfo? info) => info switch
    {
        NamedTypeInfo n => (n.Symbol?.Definition?.NativeQualifiedName ?? n.Name) +
            (n.Arguments.Count == 0 ? n.Symbol?.Definition is TypeDescriptor { TemplateParameters.Count: > 0 } ? "<>" : "" : "<" + string.Join(", ", n.Arguments.Select(a => a.Type == null ? a.Display : TypeName(a.Type))) + ">") +
            ((n.Qualifiers & TypeQualifiers.Const) != 0 ? " const" : "") + ((n.Qualifiers & TypeQualifiers.Volatile) != 0 ? " volatile" : ""),
        PointerTypeInfo p => TypeName(p.Pointee) + "*" + ((p.Qualifiers & TypeQualifiers.Const) != 0 ? " const" : ""),
        ReferenceTypeInfo r => TypeName(r.Referent) + (r.ReferenceKind == ReferenceKind.RValue ? "&&" : "&"),
        ArrayTypeInfo a => TypeName(a.ElementType) + "[" + string.Concat(a.ExtentTokens.Select(t => t.text)) + "]",
        _ => info?.Display ?? ""
    };
    static FieldDescriptor[] Fields(TypeDescriptor type, HashSet<FieldDescriptor> unionMembers)
    {
        var fields = type.Declarations.OfType<FieldDescriptor>().Where(f => !f.Api.Event && !f.IsUnnamed && f.NativeArrayField == null).ToList();
        foreach (var raw in type.Declarations.OfType<RawDeclarationDescriptor>())
        {
            var tokens = raw.HeaderTokens;
            int keyword = tokens.FindIndex(t => t.text == "union");
            if (keyword < 0) continue;
            int open = tokens.FindIndex(keyword, t => t.text == "{");
            if (open < 0) continue;
            int end = open, depth = 0;
            do { if (tokens[end] == "{") depth++; if (tokens[end] == "}") depth--; end++; } while (end < tokens.Count && depth != 0);
            if (depth != 0) continue;
            // No active union member is inferred. Keep all leaf declarations as metadata.
            var parsed = Parshe.PreParse(new Tokenizer.Stream(tokens.GetRange(open + 1, end - open - 2)), new(type.NamespaceTokens), type.SourceFile?.CppStandard ?? 20, type.SourceFile?.Converters);
            string prefix = string.Concat(tokens.Skip(end).Where(t => t.kind == Tokenizer.Token.Kind.Identifier).Select(t => t.text));
            void Add(ScopeDescriptor scope)
            {
                foreach (var field in scope.Fields.Where(f => !f.Api.Event && !f.IsUnnamed))
                {
                    if (prefix.Length != 0) field.Name.text = prefix + "." + field.Name.text;
                    field.Parent = type; field.Access = raw.Access; field.Index = fields.Count;
                    fields.Add(field); unionMembers.Add(field);
                }
                foreach (var child in scope.Declarations.OfType<ScopeDescriptor>()) Add(child);
            }
            Add(parsed);
        }
        return fields.ToArray();
    }
    public static Result Generate(FileDescriptor file, string generatedHeaderPath)
    {
        string group = "Header_" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(file.Path ?? "")))[..16];
        var types = Types(file).Concat((file.TypeInfos?.TemplateInstances.Values ?? Enumerable.Empty<TypeDescriptor>())
            .Where(t => t.ConcreteTemplate?.Definition.SourceFile?.Path == file.Path)
            .SelectMany(t => new DeclarationDescriptor[] { t }.Concat(Types(t)))).Where(d => !string.IsNullOrEmpty(d.Name.text) && (d is not TypeDescriptor t || !t.IsForwardDeclaration) && (d is not EnumDescriptor e || !e.IsForwardDeclaration)).DistinctBy(d => d.NativeQualifiedName).ToArray();
        var code = new StringBuilder("// <auto-generated />\n#pragma once\n#include \"Engine/Scripting/NativeReflection.h\"\n");
        foreach (string include in file.Converters.ReflectionPreambleHeaders) code.AppendLine("#include " + Quote(include.Replace('\\', '/')));
        IEnumerable<string> ArgumentHeaders(TypeInfo? info)
        {
            if (info is NamedTypeInfo named)
            {
                if (named.Symbol?.Definition?.SourceFile?.Path is { } path && path != file.Path) yield return path;
                foreach (var argument in named.Arguments) foreach (string header in ArgumentHeaders(argument.Type)) yield return header;
            }
            else if (info is PointerTypeInfo pointer) { foreach (string header in ArgumentHeaders(pointer.Pointee)) yield return header; }
            else if (info is ReferenceTypeInfo reference) { foreach (string header in ArgumentHeaders(reference.Referent)) yield return header; }
        }
        foreach (string include in types.OfType<TypeDescriptor>().Where(t => t.ConcreteTemplate != null && t.Declarations.OfType<FieldDescriptor>().Any()).SelectMany(t => t.ConcreteTemplate!.TypeArguments.Values.SelectMany(ArgumentHeaders)).Distinct(StringComparer.OrdinalIgnoreCase))
            code.AppendLine("#include " + Quote(include.Replace('\\', '/')));
        code.AppendLine("#include " + Quote(Path.GetFullPath(file.Path!).Replace('\\', '/')));
        bool Friend(TypeDescriptor type) => CppGenerator.HasFriendAccess(type) || type.Declarations.OfType<RawDeclarationDescriptor>().Any(d => d.HeaderTokens.Any(t => t.text == "VETReflectionAccess"));
        var cppNames = new Dictionary<DeclarationDescriptor, string>();
        var nestedAliases = new Dictionary<TypeDescriptor, List<string>>();
        foreach (var type in types)
        {
            // Empty concrete templates need no native access; retain metadata without importing unrelated argument headers.
            if (type is TypeDescriptor { ConcreteTemplate: not null } empty && !empty.Declarations.OfType<FieldDescriptor>().Any()) continue;
            if (type is TypeDescriptor { TemplateParameters.Count: > 0 } || !Regex.IsMatch(type.Name.text, "^[A-Za-z_]\\w*$")) continue;
            if (type.Parent is TypeDescriptor owner)
            {
                if (!cppNames.TryGetValue(owner, out var parent)) continue;
                if (type.Access == "public") cppNames[type] = parent + "::" + type.Name.text;
                else if (Friend(owner))
                {
                    string alias = "NestedType_" + Id(type);
                    if (!nestedAliases.TryGetValue(owner, out var aliases)) nestedAliases[owner] = aliases = [];
                    aliases.Add("    using " + alias + " = " + parent + "::" + type.Name.text + ";");
                    cppNames[type] = "::VETReflectionAccess<" + parent + ">::" + alias;
                }
            }
            else if (CanName(type)) cppNames[type] = "::" + type.NativeQualifiedName;
        }
        bool CanAccess(DeclarationDescriptor type) => cppNames.ContainsKey(type);
        code.AppendLine("namespace VETGeneratedReflection { namespace " + group + " {");
        foreach (var type in types) code.AppendLine("inline const ::NativeReflection::Type* Type_" + Id(type) + "();");
        code.AppendLine("} }");
        var unionMembers = new HashSet<FieldDescriptor>();
        var fieldsByType = types.OfType<TypeDescriptor>().ToDictionary(t => t, t => Fields(t, unionMembers));
        var fieldIds = fieldsByType.Values.SelectMany(fields => fields.Select((field, index) => (field, index))).ToDictionary(p => p.field, p => p.index);
        var getters = new Dictionary<FieldDescriptor, string>();
        var setters = new Dictionary<FieldDescriptor, string>();
        var pointers = new Dictionary<FieldDescriptor, string>();
        var baseAddresses = new Dictionary<TypeDescriptor, Dictionary<int, string>>();
        var mutableBases = new Dictionary<TypeDescriptor, Dictionary<int, string>>();
        foreach (var type in types.OfType<TypeDescriptor>().Where(CanAccess))
        {
            bool friend = Friend(type);
            var fields = fieldsByType[type];
            var access = new StringBuilder();
            if (nestedAliases.TryGetValue(type, out var aliases)) foreach (string alias in aliases) access.AppendLine(alias);
            access.AppendLine("    static const ::NativeReflection::Type* GetType() { return ::VETGeneratedReflection::" + group + "::Type_" + Id(type) + "(); }");
            baseAddresses[type] = []; mutableBases[type] = [];
            for (int index = 0; index < type.BaseTypes.Count; index++)
            {
                var basis = type.BaseTypes[index];
                if (basis.Access != "public" && !friend) continue;
                if (basis.Type is not NamedTypeInfo { Symbol.Definition: TypeDescriptor }) continue;
                string baseType = "::" + TypeName(basis.Type).TrimStart(':');
                string owner = cppNames[type];
                string method = "Base_" + index;
                access.AppendLine("    static const void* " + method + "(const void* instance) { return instance ? static_cast<" + baseType + " const*>(static_cast<" + owner + " const*>(instance)) : nullptr; }");
                baseAddresses[type][index] = "&::VETReflectionAccess<" + owner + ">::" + method;
                if (basis.Access == "public")
                {
                    access.AppendLine("    static void* Mutable" + method + "(void* instance) { return instance ? static_cast<" + baseType + "*>(static_cast<" + owner + "*>(instance)) : nullptr; }");
                    mutableBases[type][index] = "&::VETReflectionAccess<" + owner + ">::Mutable" + method;
                }
            }

            for (int index = 0; index < fields.Length; ++index)
            {
                var field = fields[index];
                // An uninitialized static template declaration need not have a definition for every specialization.
                // Keep its descriptor, but do not create an ODR-use that would introduce a linker dependency.
                if (type.ConcreteTemplate != null && NativeLayout.IsStatic(field) && field.InitializerTokens.Count == 0 && !field.HeaderTokens.Any(t => t.text == "constexpr")) continue;
                if (unionMembers.Contains(field) || field.Access != "public" && !friend || field.TypeInfo is UnsupportedTypeInfo || field.TypeInfo == null) continue;
                string native = cppNames[type];
                string expression = NativeLayout.IsStatic(field) ? native + "::" + field.Name.text : "static_cast<" + native + " const*>(instance)->" + field.Name.text;
                string check = NativeLayout.IsStatic(field) ? "(void)instance; " : "if (!instance) return ::NativeReflection::Status::NullInstance; ";
                string prefix = "::VETReflectionAccess<" + native + ">::";
                string get = "Read_" + index, set = "Write_" + index, pointer = "Address_" + index;
                bool constexprField = field.HeaderTokens.Any(t => t.text == "constexpr") || NativeLayout.IsStatic(field) && Constant(field.TypeInfo) && field.InitializerTokens.Count > 0 && ScalarAbi.Resolve(field.TypeInfo) != null;
                string readExpression = constexprField ? "static_cast<typename std::remove_cv<decltype(" + expression + ")>::type>(" + expression + ")" : expression;
                string nested = NestedName(field.TypeInfo);
                bool sequence = field.TypeInfo is ArrayTypeInfo || field.TypeInfo is NamedTypeInfo { Name: "Array" };
                string read = sequence ? "return ::NativeReflection::ReadSequence(" + expression + ", " + Quote(nested) + ", output);" :
                    "auto status = ::NativeReflection::ReadValue(" + (field.IsBitField ? "static_cast<decltype(" + expression + ")>(" + expression + ")" : readExpression) + ", output); " +
                    (!field.IsBitField && field.TypeInfo is not PointerTypeInfo && nested.Length != 0 ? "if (status == ::NativeReflection::Status::UnsupportedValue) return ::NativeReflection::ReadSubtree(std::addressof(" + expression + "), " + Quote(nested) + ", output); " : "") + "return status;";
                access.AppendLine("    static ::NativeReflection::Status " + get + "(const void* instance, ::Variant& output) { " + check + read + " }");
                string directRead = sequence ? read : "return ::NativeReflection::ReadValue(" + (field.IsBitField ? "static_cast<decltype(" + expression + ")>(" + expression + ")" : readExpression) + ", output);";
                access.AppendLine("    static ::NativeReflection::Status Value_" + index + "(const void* instance, ::Variant& output) { " + check + directRead + " }");
                getters.Add(field, "&" + prefix + get);
                bool constant = Constant(field.TypeInfo);
                bool knownBitWidth = uint.TryParse(string.Concat(field.BitWidthTokens.Where(t => !t.IsTrivia).Select(t => t.text)), out uint bitWidth) && bitWidth > 0 && bitWidth <= 64;
                if (field.Access == "public" && !constant && (!field.IsBitField || knownBitWidth) && field.TypeInfo is not PointerTypeInfo && field.TypeInfo is not NamedTypeInfo { Name: "StringView" or "StringAnsiView" } && !field.Api.Binding.ReadOnly && !field.HeaderTokens.Any(t => t.text == "constexpr"))
                {
                    string mutable = expression.Replace(" const*>(instance)", "*>(instance)");
                    string write = "::NativeReflection::" + (sequence ? "WriteSequence" : "WriteValue") + "(" + mutable + ", value)";
                    if (field.IsBitField) write = "([&]() { auto temporary = " + mutable + "; auto result = ::NativeReflection::WriteValue(temporary, value); if (result == ::NativeReflection::Status::Ok && !::NativeReflection::FitsBits(temporary, " + bitWidth + ")) return ::NativeReflection::Status::OutOfRange; if (result == ::NativeReflection::Status::Ok) " + mutable + " = temporary; return result; })()";
                    access.AppendLine("    static ::NativeReflection::Status " + set + "(void* instance, const ::Variant& value) { " + check + "return " + write + "; }");
                    setters.Add(field, "&" + prefix + set);
                }
                if (!field.IsBitField && !constexprField && field.TypeInfo is not PointerTypeInfo && field.TypeInfo is not NamedTypeInfo { Qualifiers: TypeQualifiers.Volatile })
                {
                    access.AppendLine("    static const void* " + pointer + "(const void* instance) { " + (NativeLayout.IsStatic(field) ? "(void)instance; " : "if (!instance) return nullptr; ") + "return std::addressof(" + expression + "); }");
                    pointers.Add(field, "&" + prefix + pointer);
                }
            }
            code.AppendLine("template<> struct VETReflectionAccess<" + cppNames[type] + "> {\n" + access + "};");
        }
        foreach (var enumeration in types.OfType<EnumDescriptor>().Where(CanAccess)) code.AppendLine("template<> struct VETReflectionAccess<" + cppNames[enumeration] + "> { static const ::NativeReflection::Type* GetType() { return ::VETGeneratedReflection::" + group + "::Type_" + Id(enumeration) + "(); } };");
        code.AppendLine("namespace VETGeneratedReflection { namespace " + group + " {");
        foreach (var type in types)
        {
            string id = Id(type);
            var fields = type is TypeDescriptor record ? fieldsByType[record] : [];
            foreach (var field in fields)
            {
                string name = NestedName(field.TypeInfo);
                code.AppendLine("inline const ::NativeReflection::Type* Nested_" + id + "_" + fieldIds[field] + "() { return ::NativeReflection::FindType(" + Quote(name) + "); }");
            }
            if (type is EnumDescriptor enumeration)
            {
                for (int i = 0; i < enumeration.Members.Count; i++)
                    if (CanAccess(type)) code.AppendLine("inline ::NativeReflection::Status Enum_" + id + "_" + i + "(const void*, ::Variant& output) { return ::NativeReflection::ReadValue(" + cppNames[type] + "::" + enumeration.Members[i].Name.text + ", output); }");
                code.AppendLine("inline const ::NativeReflection::Type* Type_" + id + "() {");
                if (enumeration.Members.Count > 0)
                {
                    code.AppendLine("    static const ::NativeReflection::Field members[] = {");
                    for (int i = 0; i < enumeration.Members.Count; i++)
                        code.AppendLine("        { " + Quote(type.NativeQualifiedName) + ", " + Quote(enumeration.Members[i].Name.text) + ", ::NativeReflection::Access::Public, true, true, false, " + (CanAccess(type) ? "&Enum_" + id + "_" + i : "nullptr") + ", nullptr, nullptr, nullptr, nullptr },");
                    code.AppendLine("    };");
                }
                code.AppendLine("    static const ::NativeReflection::Type type = { " + Quote(type.NativeQualifiedName) + ", " + (enumeration.Members.Count > 0 ? "members" : "nullptr") + ", " + enumeration.Members.Count + ", nullptr, 0, nullptr, 0, ::NativeReflection::TypeKind::Enum }; return &type;\n}");
                continue;
            }
            var bases = type is TypeDescriptor derived ? derived.BaseTypes : [];
            for (int i = 0; i < bases.Count; ++i) code.AppendLine("inline const ::NativeReflection::Type* BaseType_" + id + "_" + i + "() { return ::NativeReflection::FindType(" + Quote(TypeName(bases[i].Type)) + "); }");
            code.AppendLine("inline const ::NativeReflection::Type* Type_" + id + "() {");
            if (fields.Length > 0)
            {
                code.AppendLine("    static const ::NativeReflection::Field fields[] = {");
                foreach (var field in fields)
                {
                    string spelling = field.TypeInfo == null ? string.Concat(field.TypeTokens.Select(t => t.text)) : TypeName(field.TypeInfo);
                    string visibility = field.Access switch { "private" => "Private", "protected" => "Protected", _ => "Public" };
                    string get = getters.GetValueOrDefault(field) ?? "nullptr", set = setters.GetValueOrDefault(field) ?? "nullptr";
                    bool serializable = !field.Api.ManagedAttributes.Any(a => Regex.IsMatch(a, @"(^|\W)NoSerialize(?:Attribute)?($|\W)")) && !field.AttributeTokens.SelectMany(a => a).Any(t => t.text == "NoSerialize");
                    code.AppendLine("        { " + Quote(spelling) + ", " + Quote(field.Name.text) + ", ::NativeReflection::Access::" + visibility + ", " + (NativeLayout.IsStatic(field) ? "true" : "false") + ", " + (set == "nullptr" ? "true" : "false") + ", " + (unionMembers.Contains(field) ? "true" : "false") + ", " + get + ", " + set + ", " + (pointers.GetValueOrDefault(field) ?? "nullptr") + ", &Nested_" + id + "_" + fieldIds[field] + ", " + (setters.ContainsKey(field) && pointers.ContainsKey(field) ? "+[](void* instance) -> void* { return const_cast<void*>(" + pointers[field][1..] + "(instance)); }" : "nullptr") + ", " + (serializable ? "true" : "false") + ", " + (field.TypeInfo is ArrayTypeInfo or NamedTypeInfo { Name: "Array" } ? "true" : "false") + ", " + (get == "nullptr" ? "nullptr" : get.Replace("::Read_", "::Value_")) + " },");
                }
                code.AppendLine("    };");
            }
            var children = type is ScopeDescriptor parent ? parent.Declarations.Where(IsType).Where(d => types.Contains(d)).ToArray() : [];
            if (children.Length > 0) code.AppendLine("    static const ::NativeReflection::Type* children[] = { " + string.Join(", ", children.Select(d => "Type_" + Id(d) + "()")) + " };");
            if (bases.Count > 0)
            {
                code.AppendLine("    static const ::NativeReflection::Base bases[] = {");
                for (int i = 0; i < bases.Count; i++)
                {
                    string visibility = bases[i].Access switch { "private" => "Private", "protected" => "Protected", _ => "Public" };
                    string address = type is TypeDescriptor owner && baseAddresses.TryGetValue(owner, out var addresses) ? addresses.GetValueOrDefault(i) ?? "nullptr" : "nullptr";
                    string mutable = type is TypeDescriptor mutableOwner && mutableBases.TryGetValue(mutableOwner, out var mutableAddresses) ? mutableAddresses.GetValueOrDefault(i) ?? "nullptr" : "nullptr";
                    code.AppendLine("        { " + Quote(TypeName(bases[i].Type)) + ", ::NativeReflection::Access::" + visibility + ", &BaseType_" + id + "_" + i + ", " + address + ", " + mutable + " },");
                }
                code.AppendLine("    };");
            }
            code.AppendLine("    static const ::NativeReflection::Type type = { " + Quote(type.NativeQualifiedName) + ", " + (fields.Length > 0 ? "fields" : "nullptr") + ", " + fields.Length + ", " + (children.Length > 0 ? "children" : "nullptr") + ", " + children.Length + ", " + (bases.Count > 0 ? "bases" : "nullptr") + ", " + bases.Count + ", ::NativeReflection::TypeKind::" + (type is TypeDescriptor { TemplateParameters.Count: > 0 } ? "Template" : type is InterfaceDescriptor ? "Interface" : type is ClassDescriptor ? "Class" : "Struct") + " }; return &type;\n}");
        }
        code.AppendLine("} }");
        var registration = new StringBuilder("// <auto-generated />\n#include " + Quote(Path.GetFullPath(generatedHeaderPath).Replace('\\', '/')) + "\nnamespace {\n");
        foreach (var type in types) registration.AppendLine("::NativeReflection::Registration Type_" + Id(type) + "(::VETGeneratedReflection::" + group + "::Type_" + Id(type) + "());");
        registration.AppendLine("}");
        return new(code.ToString(), registration.ToString(), types.Length);
    }
}
