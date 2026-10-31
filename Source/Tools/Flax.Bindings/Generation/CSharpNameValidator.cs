using System.Text;
using VETools.Text;
using static VETools.Tokenizer;

namespace VETools.Generation;

// Names are validated in the scope where they will be emitted, not in the
// flattened file list. Inherited members deliberately do not reserve names.
internal sealed class CSharpNameValidator
{
    sealed record Entry(string Name, string Kind, DeclarationDescriptor Origin, Token Where, TypeSymbol? Symbol = null, bool Forward = false);
    readonly Dictionary<string, Dictionary<string, Entry>> scopes = new(StringComparer.Ordinal);
    readonly List<LogEntry> errors = [];
    readonly HashSet<TypeSymbol> checkedOpaqueTypes = [];
    ExportPlan plan = null!;

    public static List<LogEntry> Validate(FileDescriptor file, ExportPlan? emissionPlan = null)
    {
        var validator = new CSharpNameValidator();
        validator.plan = emissionPlan ?? ExportPlan.Build(file);
        foreach (var declaration in validator.plan.Members(file))
            validator.Namespaces(declaration);
        foreach (var group in validator.plan.Members(file).GroupBy(HandleNetCS.NamespaceKey))
        {
            string scope = "namespace " + group.Key;
            foreach (var declaration in group.Where(d => d is TypeDescriptor or EnumDescriptor)) validator.Type(scope, declaration, null);
            var globals = group.Where(d => d is FieldDescriptor or FunctionDescriptor).ToList();
            if (globals.Count > 0)
            {
                validator.Add(scope, new("NativeGlobals", "generated globals container", globals[0], globals[0].Name));
                validator.Members("globals " + group.Key, "NativeGlobals", globals);
            }
        }
        return validator.errors;
    }

    void Namespaces(DeclarationDescriptor declaration)
    {
        var names = declaration.NamespaceTokens.Where(t => t.kind == Token.Kind.Identifier).ToList();
        if (HandleNetCS.EngineNamespace(declaration)) { var engine = declaration.Name; engine.text = "FlaxEngine"; names.Insert(0, engine); }
        string parent = "";
        foreach (var name in names)
        {
            Add("namespace " + parent, new(name.text, "namespace", declaration, name));
            parent = parent.Length == 0 ? name.text : parent + "::" + name.text;
        }
    }

    void Type(string parentScope, DeclarationDescriptor declaration, string? ownerName)
    {
        TypeSymbol? symbol = declaration is TypeDescriptor type ? type.TypeSymbol : ((EnumDescriptor)declaration).TypeSymbol;
        bool forward = declaration is TypeDescriptor { IsForwardDeclaration: true } or EnumDescriptor { IsForwardDeclaration: true };
        string name = declaration is EnumDescriptor enumeration ? EnumName(enumeration) : HandleNetCS.SimpleName(declaration);
        if (string.IsNullOrEmpty(name)) return;
        Entry entry = new(name, declaration.Kind.ToLowerInvariant(), declaration, Where(declaration), symbol, forward);
        Add(parentScope, entry, ownerName);
        if (forward && symbol?.Definition != null) return;
        if (forward && symbol != null && !checkedOpaqueTypes.Add(symbol)) return;
        if (declaration is TypeDescriptor scope)
        {
            if (forward)
            {
                // Forward-only records contain an Address field in the preview.
                Add("opaque " + scope.QualifiedName, new("Address", "generated opaque-address field", declaration, Where(declaration)), name);
                return;
            }
            Members("type " + scope.QualifiedName, name, plan.Members(scope));
        }
        else if (declaration is EnumDescriptor enumType)
            Enum(enumType, name);
    }

    void Members(string scope, string ownerName, IEnumerable<DeclarationDescriptor> declarations)
    {
        foreach (var declaration in declarations)
        {
            if (declaration is TypeDescriptor or EnumDescriptor) Type(scope, declaration, ownerName);
            else if (declaration is FieldDescriptor field)
            {
                if (field.Api.Event)
                {
                    Add(scope, new(field.Name.text, "event", field, field.Name), ownerName);
                    foreach (string name in new[] { "Pump" + field.Name.text + "Events", field.Name.text + "DispatchError", "Shutdown" + field.Name.text + "Events" })
                        Add(scope, new(name, "generated event control", field, field.Name), ownerName);
                    continue;
                }
                if (plan.Properties.TryGetValue(field, out var property))
                {
                    Add(scope, new(field.Name.text, "property", field, field.Name), ownerName);
                    foreach (var call in new[] { property.Getter, property.Setter, property.PassThrough }.OfType<FunctionDescriptor>())
                    {
                        Add(scope, new(plan.ManagedName(call), "method", call, call.Name), ownerName);
                        Parameters(call);
                    }
                    if (property.PassThrough is { } combined)
                        Add(scope, new(combined.Name.text + "_" + field.Name.text, "generated property cache", field, field.Name), ownerName);
                    continue;
                }
                Add(scope, new(field.Name.text, "field", field, field.Name), ownerName);
                TypeInfo? storage = field.TypeInfo;
                string helper = "__NativeArray_" + field.Name.text;
                while (storage is ArrayTypeInfo array)
                {
                    Add(scope, new(helper, "generated array storage type", field, field.Name), ownerName);
                    helper += "_Element";
                    storage = array.ElementType;
                }
            }
            else if (declaration is FunctionDescriptor function)
            {
                if (Deleted(function)) continue;
                if (function.TypeInfo is not { IsConstructor: true })
                {
                    string name = function.TypeInfo is { IsDestructor: true } ? "__NativeDestroy_" + ownerName : function.Name.text;
                    Add(scope, new(name, "method", function, function.Name), ownerName);
                }
                Parameters(function);
            }
        }
    }

    void Parameters(FunctionDescriptor function)
    {
        var names = new Dictionary<string, Entry>(StringComparer.Ordinal);
        foreach (var parameter in function.Fields.Where(p => !p.IsUnnamed))
        {
            Entry entry = new(parameter.Name.text, "parameter", parameter, parameter.Name);
            if (!names.TryAdd(entry.Name, entry)) Collision(names[entry.Name], entry, "parameter names must be unique within a function");
        }
    }

    void Enum(EnumDescriptor enumeration, string ownerName)
    {
        var names = new Dictionary<string, Entry>(StringComparer.Ordinal);
        foreach (var member in enumeration.Members)
        {
                var token=member.Name;
                Entry entry = new(token.text, "enum member", enumeration, token);
                if (!names.TryAdd(token.text, entry)) Collision(names[token.text], entry, "enum member names must be unique");
                if (token == "value__")
                    errors.Add(new LogEntry("Enum member 'value__' conflicts with the reserved CLR enum storage field.", token, LogEntryType.Error, "VETCS001"));
        }
    }

    void Add(string scope, Entry current, string? ownerName = null)
    {
        if (string.IsNullOrEmpty(current.Name)) return;
        if (ownerName == current.Name)
            errors.Add(new LogEntry($"{current.Kind} '{current.Name}' has the same name as its containing C# type; only constructors may use that name.", current.Where, LogEntryType.Error, "VETCS001"));
        if (!scopes.TryGetValue(scope, out var names)) scopes.Add(scope, names = new(StringComparer.Ordinal));
        if (!names.TryGetValue(current.Name, out var previous)) { names.Add(current.Name, current); return; }
        if (previous.Kind == "namespace" && current.Kind == "namespace") return;
        if (previous.Kind == "method" && current.Kind == "method") return; // Signatures are checked separately.
        if (previous.Symbol != null && ReferenceEquals(previous.Symbol, current.Symbol) && (previous.Forward || current.Forward))
        {
            if (!current.Forward) names[current.Name] = current;
            return;
        }
        Collision(previous, current, $"these declarations share the same emitted C# name in {scope}");
    }

    void Collision(Entry previous, Entry current, string reason)
    {
        string Describe(Entry entry) => entry.Kind + " '" + (entry.Origin is FieldDescriptor field && entry.Kind is "field" or "parameter"
            ? (field.TypeInfo?.Display ?? "<unknown>") + " " + entry.Name : entry.Name) + "'";
        StringBuilder text = new();
        text.Append("Defined ").Append(Describe(previous)).Append(" has collided with ").Append(Describe(current)).AppendLine(".");
        text.Append("Unable to emit '").Append(current.Name).Append("': ").Append(reason).AppendLine(".");
        text.AppendLine("In:");
        Location(previous); Location(current);
        errors.Add(new LogEntry(text.ToString().TrimEnd(), current.Where, LogEntryType.Error, "VETCS001"));
        void Location(Entry entry)
        {
            DeclarationDescriptor? root = entry.Origin;
            while (root != null && root is not FileDescriptor) root = root.Parent;
            text.Append("  ").Append((root as FileDescriptor)?.Path ?? "<source>").Append('(')
                .Append(entry.Where.line + 1).Append(',').Append(entry.Where.col + 1).AppendLine(")");
        }
    }

    public static string EnumName(EnumDescriptor enumeration) => string.IsNullOrEmpty(enumeration.Name.text)
        ? "__AnonymousEnum_L" + (enumeration.HeaderTokens.FirstOrDefault(t => t == "enum").line + 1) : enumeration.Name.text;
    static Token Where(DeclarationDescriptor declaration) => string.IsNullOrEmpty(declaration.Name.text) ? declaration.HeaderTokens.FirstOrDefault(t => !t.IsTrivia) : declaration.Name;
    static bool Deleted(FunctionDescriptor function) => function.TypeInfo?.SuffixTokens is { } suffix && suffix.SkipWhile(t => t != "=").Skip(1).FirstOrDefault(t => !t.IsTrivia) == "delete";
}
