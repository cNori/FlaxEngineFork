using System.Text;
using System.Text.RegularExpressions;
using VETools.Text;

namespace VETools.Generation;

// Native engine persistence uses typed overloads, independently of managed ABI converters.
internal static class AutoSerializationGenerator
{
    internal sealed record Result(string Source, IReadOnlyList<LogEntry> Diagnostics);
    sealed record Property(string Name, FunctionDescriptor Getter, FunctionDescriptor Setter);
    static bool Attribute(IEnumerable<DeclarationDescriptor> members, string name) => members.Any(m =>
        m.Api.ManagedAttributes.Any(a => Regex.IsMatch(a, @"(^|\W)" + name + @"(?:Attribute)?($|\W)")) ||
        m.AttributeTokens.SelectMany(t => t).Any(t => t.text == name));
    static bool Marked(DeclarationDescriptor member) => ExportAttributes.Read(member).Export;
    static IEnumerable<TypeDescriptor> Types(ScopeDescriptor scope)
    {
        foreach (var child in scope.Declarations.OfType<ScopeDescriptor>())
        {
            if (child is TypeDescriptor type) yield return type;
            foreach (var nested in Types(child)) yield return nested;
        }
    }
    static TypeInfo? ValueType(TypeInfo? type) => type is ReferenceTypeInfo reference ? reference.Referent : type;
    static bool Interface(TypeInfo? type) => ValueType(type) is NamedTypeInfo { Symbol.Definition: InterfaceDescriptor };
    static bool Eligible(IEnumerable<DeclarationDescriptor> members, bool isStatic, TypeInfo? type)
    {
        var array = members.ToArray();
        return !isStatic && (array.Any(m => m.Access == "public") || Attribute(array, "Serialize")) &&
            !Attribute(array, "NoSerialize") && !Attribute(array, "NonSerialized") && !Interface(type);
    }
    public static Result Generate(FileDescriptor file)
    {
        var output = new StringBuilder();
        List<LogEntry> errors = [];
        HashSet<string> includes = [];
        foreach (var type in Types(file).Where(t => t.Api.Binding.AutoSerialization && !t.IsForwardDeclaration))
        {
            if (type.TemplateParameters.Count != 0)
            { Error(type, "API_AUTO_SERIALIZATION on an open template requires a concrete serialization handler."); continue; }
            var fields = type.Fields.Where(f => Marked(f) && f.NativeArrayField == null && !f.Api.Event &&
                Eligible([f], NativeLayout.IsStatic(f), f.TypeInfo)).ToArray();
            List<Property> properties = [];
            if (type is ClassDescriptor)
                foreach (var group in type.Functions.Where(f => f.Api.Property).GroupBy(f =>
                    f.Name.text.StartsWith("Get", StringComparison.Ordinal) || f.Name.text.StartsWith("Set", StringComparison.Ordinal) ? f.Name.text[3..] : f.Name.text))
                {
                    var getters = group.Where(f => f.Fields.Count == 0 && f.TypeInfo?.ReturnType is not NamedTypeInfo { Name: "void" }).ToArray();
                    var setters = group.Where(f => f.Fields.Count == 1 && f.TypeInfo?.ReturnType is NamedTypeInfo { Name: "void" }).ToArray();
                    if (getters.Length == 0 || setters.Length == 0) continue;
                    if (getters.Length != 1 || setters.Length != 1)
                    { Error(group.First(), "API_AUTO_SERIALIZATION cannot select overloaded accessors for property '" + group.Key + "'."); continue; }
                    var getter = getters[0]; var setter = setters[0];
                    if (Eligible([getter, setter], BindingSignaturePass.IsStatic(getter) || BindingSignaturePass.IsStatic(setter), getter.TypeInfo?.ReturnType))
                        properties.Add(new(group.Key, getter, setter));
                }
            var basis = type.BaseTypes.FirstOrDefault(b => b.Type is NamedTypeInfo { Symbol.Definition: TypeDescriptor parent } && parent is not InterfaceDescriptor);
            string? baseName = basis?.Type is NamedTypeInfo { Symbol.Definition: TypeDescriptor parent } && basis.Access != "private" &&
                parent.Name.text is not ("PersistentScriptingObject" or "ScriptingObject" or "ManagedScriptingObject") ? parent.NativeQualifiedName : null;
            string name = type.NativeQualifiedName;
            foreach (var memberType in fields.Select(f => f.TypeInfo).Concat(properties.Select(p => p.Getter.TypeInfo?.ReturnType))) AddIncludes(memberType);
            output.AppendLine($"\nvoid {name}::Serialize(SerializeStream& stream, const void* otherObj)\n{{");
            Callback("OnSerializing", "{ nullptr }");
            if (baseName != null) output.AppendLine($"    {baseName}::Serialize(stream, otherObj);");
            output.AppendLine($"    SERIALIZE_GET_OTHER_OBJ({name});");
            foreach (var field in fields) output.AppendLine($"    SERIALIZE{Suffix(field)}({field.Name.text});");
            foreach (var property in properties.Where(p => !Attribute([p.Getter, p.Setter], "Obsolete")))
            {
                string reference = property.Getter.TypeInfo?.ReturnType is ReferenceTypeInfo ? "&" : "";
                output.AppendLine($"    {{ const auto{reference} value = {property.Getter.Name.text}();");
                output.AppendLine($"      if (other) {{ const auto{reference} otherValue = other->{property.Getter.Name.text}();");
                output.AppendLine($"        if (Serialization::ShouldSerialize(value, &otherValue)) {{ stream.JKEY(\"{property.Name}\"); Serialization::Serialize(stream, value, &otherValue); }}");
                output.AppendLine($"      }} else if (Serialization::ShouldSerialize(value, nullptr)) {{ stream.JKEY(\"{property.Name}\"); Serialization::Serialize(stream, value, nullptr); }} }}");
            }
            Callback("OnSerialized", "{ nullptr }");
            output.AppendLine("}");
            output.AppendLine($"\nvoid {name}::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)\n{{");
            Callback("OnDeserializing", "{ modifier }");
            if (baseName != null) output.AppendLine($"    {baseName}::Deserialize(stream, modifier);");
            foreach (var field in fields) output.AppendLine($"    DESERIALIZE{Suffix(field)}({field.Name.text});");
            foreach (var property in properties)
            {
                output.AppendLine($"    {{ const auto entry = SERIALIZE_FIND_MEMBER(stream, \"{property.Name}\");");
                output.AppendLine($"      if (entry != stream.MemberEnd()) {{ auto value = {property.Getter.Name.text}(); Serialization::Deserialize(entry->value, value, modifier); {property.Setter.Name.text}(value); }} }}");
            }
            Callback("OnDeserialized", "{ modifier }");
            output.AppendLine("}");
            output.AppendLine($"\nbool {name}::ShouldSerialize(const void* otherObj) const\n{{");
            if (HandleNETcpp.IsScriptingObject(type)) output.AppendLine("    return true;");
            else
            {
                output.AppendLine($"    if (!otherObj) return true;\n    SERIALIZE_GET_OTHER_OBJ({name});\n    bool result = false;");
                if (baseName != null) output.AppendLine($"    result |= {baseName}::ShouldSerialize(otherObj);");
                foreach (var field in fields)
                    output.AppendLine(field.IsBitField ? $"    result |= {field.Name.text} != other->{field.Name.text};" : $"    result |= Serialization::ShouldSerialize({field.Name.text}, &other->{field.Name.text});");
                foreach (var property in properties)
                {
                    string reference = property.Getter.TypeInfo?.ReturnType is ReferenceTypeInfo ? "&" : "";
                    output.AppendLine($"    {{ const auto{reference} value = {property.Getter.Name.text}(); const auto{reference} otherValue = other->{property.Getter.Name.text}(); result |= Serialization::ShouldSerialize(value, &otherValue); }}");
                }
                output.AppendLine("    return result;");
            }
            output.AppendLine("}");
            void Callback(string callback, string context)
            {
                foreach (var function in type.Functions.Where(f => Marked(f) && f.Name.text == callback))
                {
                    if (function.Fields.Count != 1 || ValueType(function.Fields[0].TypeInfo) is not NamedTypeInfo arg ||
                        arg.Name.Split("::", StringSplitOptions.RemoveEmptyEntries).LastOrDefault() != "CallbackContext" ||
                        function.TypeInfo?.ReturnType is not NamedTypeInfo { Name: "void" })
                    { Error(function, "Invalid serialization callback '" + function.QualifiedName + "': expected void " + callback + "(const CallbackContext& context)."); continue; }
                    output.AppendLine($"    {name}::{callback}({context});");
                }
            }
        }
        if (output.Length == 0) return new("", errors);
        var preamble = new StringBuilder("\n#include \"Engine/Serialization/Serialization.h\"\n");
        foreach (string include in includes.Order(StringComparer.Ordinal)) preamble.AppendLine("#include " + System.Text.Json.JsonSerializer.Serialize(include.Replace('\\', '/')));
        return new(preamble.Append(output).ToString(), errors);
        void Error(DeclarationDescriptor owner, string message) => errors.Add(new(message, owner.Name, LogEntryType.Error, "VETCPP002", owner.SourceFile?.Path));
        void AddIncludes(TypeInfo? info)
        {
            if (info is ReferenceTypeInfo r) AddIncludes(r.Referent);
            else if (info is PointerTypeInfo p) AddIncludes(p.Pointee);
            else if (info is ArrayTypeInfo a) AddIncludes(a.ElementType);
            else if (info is NamedTypeInfo n)
            {
                if (n.Symbol?.Definition?.SourceFile?.Path is { } path && path != file.Path) includes.Add(path);
                if (n.Name.EndsWith("Curve", StringComparison.Ordinal) && n.Arguments.Count > 0) includes.Add("Engine/Animations/CurveSerialization.h");
                foreach (var argument in n.Arguments) AddIncludes(argument.Type);
            }
        }
    }
    static string Suffix(FieldDescriptor field) => field.IsBitField ? "_BIT" :
        field.TypeInfo is PointerTypeInfo { Pointee: NamedTypeInfo { Symbol.Definition: TypeDescriptor type } } && HandleNETcpp.IsScriptingObject(type) ? "_OBJ" : "";
}
