using VETools.Text;
using static VETools.Tokenizer;

namespace VETools.Generation;

internal sealed class ExportAttributes
{
    public bool Export, Getter, Setter, PassThrough;
    public bool Property => Getter || Setter;
    public static ExportAttributes Read(DeclarationDescriptor declaration)
    {
        ExportAttributes result = new() { Export = declaration.Api.Export };
        var header = declaration.HeaderTokens;
        if (declaration is RawDeclarationDescriptor && header.FindIndex(token => token == "{") is >= 0 and var bodyStart)
            header = header.Take(bodyStart).ToList(); // Nested attributes do not export the opaque union itself.
        if (!string.IsNullOrEmpty(declaration.Name.text))
            header = header.TakeWhile(t => t.line < declaration.Name.line || t.line == declaration.Name.line && t.col < declaration.Name.col).ToList();
        ReadTokens(header);
        foreach (var group in declaration.AttributeTokens) ReadTokens(group);
        return result;

        void ReadTokens(IEnumerable<Token> source)
        {
            var tokens = source.Where(t => !t.IsTrivia).ToList();
            for (int i = 0; i + 1 < tokens.Count; i++)
            {
                if (tokens[i] != "[" || tokens[i + 1] != "[") continue;
                i += 2; int round = 0; bool name = true;
                while (i + 1 < tokens.Count && !(tokens[i] == "]" && tokens[i + 1] == "]" && round == 0))
                {
                    if (name && tokens[i].kind == Token.Kind.Identifier && tokens[i + 1] != ":")
                    {
                        switch (tokens[i].text)
                        {
                            case "Export": result.Export = true; break;
                            case "Getter": result.Getter = true; break;
                            case "Setter": result.Setter = true; break;
                            case "PassThrough": result.PassThrough = true; break;
                        }
                        name = false;
                    }
                    else if (name) name = false;
                    if (tokens[i] == "(") round++;
                    if (tokens[i] == ")") round--;
                    if (tokens[i] == "," && round == 0) name = true;
                    i++;
                }
            }
        }
    }
}

internal sealed class PropertyProjection
{
    public required FieldDescriptor Field;
    public FunctionDescriptor? Getter, Setter, PassThrough;
    public bool IsStatic;
    public string Access => Field.Api.ManagedAccess ?? (new[] { Getter?.Access, Setter?.Access, PassThrough?.Access }.Contains("public") ? "public"
        : new[] { Getter?.Access, Setter?.Access, PassThrough?.Access }.Contains("protected") ? "protected" : "private");
}

internal sealed class ExportPlan
{
    readonly HashSet<DeclarationDescriptor> selected = [];
    readonly HashSet<FunctionDescriptor> consumed = [];
    readonly HashSet<FunctionDescriptor> nativeCalls = [];
    readonly HashSet<TypeSymbol> types = [];
    readonly HashSet<TypeSymbol> referencedTypes = [];
    readonly Dictionary<FunctionDescriptor, string> managedNames = [];
    readonly bool all;
    public List<LogEntry> Errors = [], Warnings = [];
    public Dictionary<FieldDescriptor, PropertyProjection> Properties = [];
    readonly Dictionary<ScopeDescriptor, List<DeclarationDescriptor>> additional = [];
    ExportPlan(bool all) { this.all = all; }
    public static ExportPlan Build(FileDescriptor file) => Build(file, false);
    // Internal fixture plan only; there is no CLI switch bypassing [[Export]].
    public static ExportPlan ForDeclarationTests(FileDescriptor file) => Build(file, true);
    static ExportPlan Build(FileDescriptor file, bool all)
    {
        if (file.TypeInfos != null) { AutomaticFieldConverters.Apply(file); StandardConverters.Apply(file); MarshalAsConverters.Apply(file); }
        ExportPlan plan = new(all);
        if (file.TypeInfos != null)
            foreach (var symbol in file.TypeInfos.Symbols.Values)
                if (symbol.Definition is { } definition && definition.SourceFile != file && ExportedReference(definition)) plan.referencedTypes.Add(symbol);
        plan.Select(file);
        // Concrete templates are owned by their typedef/prototype file. Referenced
        // instances remain external; do not emit duplicate types in consumers.
        var pending = new Queue<TypeDescriptor>(plan.selected.SelectMany(TemplateInstantiationPass.Types).SelectMany(TemplateInstantiationPass.ConcreteReferences));
        var seen = new HashSet<TypeDescriptor>();
        while (pending.TryDequeue(out var instance))
        {
            if (!seen.Add(instance)) continue;
            if (instance.SourceFile != file) { if (instance.TypeSymbol != null) plan.referencedTypes.Add(instance.TypeSymbol); continue; }
            if (instance.Api.InBuild) { if (instance.TypeSymbol != null) plan.referencedTypes.Add(instance.TypeSymbol); continue; }
            if (instance.Parent is not ScopeDescriptor owner) continue;
            if (!plan.additional.TryGetValue(owner, out var extra)) plan.additional.Add(owner, extra = []);
            extra.Add(instance); plan.selected.Add(instance);
            if (instance.TypeSymbol != null) plan.types.Add(instance.TypeSymbol);
            plan.Select(instance);
            foreach (var child in TemplateInstantiationPass.Declarations(instance).SelectMany(TemplateInstantiationPass.Types).SelectMany(TemplateInstantiationPass.ConcreteReferences)) pending.Enqueue(child);
        }
        foreach (var field in plan.selected.OfType<FieldDescriptor>().ToList())
        {
            if (field.Api.Event) continue;
            plan.Project(field);
            if (!all && plan.selected.Contains(field) && !plan.Properties.ContainsKey(field) && field.Parent is not StructDescriptor { MarshalAsType: not null } && (field.Parent is ClassDescriptor or FileDescriptor or StructDescriptor { RequiresNativeFieldAccess: true } or StructDescriptor { NativeFieldView: true } || NativeLayout.IsStatic(field)))
                plan.ProjectDirectField(field);
        }
        plan.ProjectInterfaceCalls();
        plan.ProjectApiProperties();
        plan.AssignAccessorNames();
        plan.AssignNoProxyNames();
        return plan;
    }
    void ProjectInterfaceCalls()
    {
        foreach (var owner in selected.OfType<ClassDescriptor>().ToArray())
        {
            HashSet<InterfaceDescriptor> interfaces = [];
            void Find(TypeDescriptor type, HashSet<TypeDescriptor> path)
            {
                if (!path.Add(type)) return;
                foreach (var basis in type.BaseTypes.Where(b => b.Access == "public"))
                    if (basis.Type is NamedTypeInfo { Symbol.Definition: InterfaceDescriptor iface }) { if (basis.IsVirtual) { Error(owner, "Virtual native interface receiver adjustment requires an explicit adapter."); continue; } if (!interfaces.Add(iface)) { Error(owner, "Ambiguous native interface receiver base requires an explicit adapter."); continue; } Find(iface, path); }
                path.Remove(type);
            }
            Find(owner, []);
            foreach (var iface in interfaces)
            {
                if (!HasType(iface.TypeSymbol!)) continue;
                if (HandleNETcpp.IsScriptingObject(owner) && !HandleNETcpp.CanCast(owner)) { Error(owner, "Native interface dispatch requires a public non-virtual scripting root."); continue; }
                foreach (var method in iface.Functions.Where(f => Marked(f) && !f.Api.Hidden && !BindingSignaturePass.IsStatic(f)))
                {
                    if (method.Api.Property) { Error(owner, "Native interface property projection needs an explicit receiver adapter: " + iface.QualifiedName); continue; }
                    if (method.Access != "public" || method.TypeInfo == null) continue;
                    string Parameters(FunctionDescriptor f) => string.Join(",", f.TypeInfo!.Parameters.Select(t => BindingSignaturePass.Canonical(t, true)));
                    var existing = owner.Functions.FirstOrDefault(f => Emits(f) && f.Name.text == method.Name.text && f.TypeInfo != null && Parameters(f) == Parameters(method));
                    if (existing != null)
                    {
                        if (BindingSignaturePass.Canonical(existing.TypeInfo!.ReturnType, false) != BindingSignaturePass.Canonical(method.TypeInfo.ReturnType, false) ||
                            existing.Fields.Select(f => (f.Binding?.Direction, f.Binding?.ReadOnlyReference)).SequenceEqual(method.Fields.Select(f => (f.Binding?.Direction, f.Binding?.ReadOnlyReference))) == false)
                            Error(existing, "Exported method conflicts with its native interface binding contract: " + method.QualifiedName);
                        continue;
                    }
                    var name = owner.Name; name.text = method.Name.text;
                    var enclosing = new List<Token>(owner.EnclosingTypeTokens);
                    if (enclosing.Count != 0) { enclosing.Add(new() { kind = Token.Kind.ControlChar, text = ":" }); enclosing.Add(new() { kind = Token.Kind.ControlChar, text = ":" }); }
                    enclosing.Add(owner.Name);
                    FunctionDescriptor projection = new() {
                        Name = name, Parent = owner, Access = "public", NativeDispatchInterface = iface,
                        NamespaceTokens = new(owner.NamespaceTokens), EnclosingTypeTokens = enclosing,
                        HeaderTokens = new(method.HeaderTokens), AttributeTokens = new(method.AttributeTokens), DiagnosticOrigin = owner.DiagnosticFile,
                        TypeInfo = method.TypeInfo, Binding = new() { NativeSignature = owner.QualifiedName + "::" + method.Name.text + "(" + Parameters(method) + ")" },
                        Api = new() { Export = true, Sealed = method.Api.Sealed, ManagedAccess = "public" }
                    };
                    projection.Api.ManagedAttributes.AddRange(method.Api.ManagedAttributes);
                    foreach (var parameter in method.Fields) {
                        var field = new FieldDescriptor { Name = parameter.Name, Parent = projection, IsParameter = true,
                            HeaderTokens = new(parameter.HeaderTokens), TypeInfo = parameter.TypeInfo, TypeTokens = new(parameter.TypeTokens),
                            InitializerTokens = new(parameter.InitializerTokens), Binding = parameter.Binding, Api = parameter.Api };
                        projection.Fields.Add(field); if (field.Binding != null) projection.Binding.Parameters.Add(field.Binding);
                    }
                    if (!additional.TryGetValue(owner, out var extra)) additional.Add(owner, extra = []);
                    extra.Add(projection); selected.Add(projection);
                }
            }
        }
    }

    public bool Emits(DeclarationDescriptor declaration) => selected.Contains(declaration) && (declaration is not FunctionDescriptor function || !consumed.Contains(function));
    public bool HasType(TypeSymbol symbol) => types.Contains(symbol) || referencedTypes.Contains(symbol);
    public string ManagedName(FunctionDescriptor function) => managedNames.GetValueOrDefault(function, function.Name.text);
    void AssignNoProxyNames()
    {
        foreach (var owner in selected.Concat(nativeCalls).OfType<FunctionDescriptor>().Where(f => f.Api.NoProxy).Distinct().GroupBy(f => f.Parent))
        {
            var scope = owner.Key as ScopeDescriptor;
            var reserved = (scope?.Declarations ?? []).Select(d => d.Name.text).Concat(managedNames.Where(p => p.Key.Parent == scope).Select(p => p.Value)).ToHashSet(StringComparer.Ordinal);
            foreach (var overloads in owner.GroupBy(f => f.Name.text))
            {
                string stem = "Internal_" + overloads.Key, name = stem;
                for (int suffix = 1; !reserved.Add(name); ++suffix) name = stem + "_" + suffix;
                foreach (var function in overloads) managedNames[function] = name;
            }
        }
    }
    void AssignAccessorNames()
    {
        foreach (var group in Properties.Values.GroupBy(property => property.Field.Parent))
        {
            var scope = group.Key as ScopeDescriptor;
            var reserved = (scope?.Declarations ?? []).Concat(scope == null ? [] : additional.GetValueOrDefault(scope) ?? [])
                .Select(declaration => declaration.Name.text).Where(name => !string.IsNullOrEmpty(name)).ToHashSet(StringComparer.Ordinal);
            if (scope != null && !string.IsNullOrEmpty(scope.Name.text)) reserved.Add(scope.Name.text);
            foreach (var property in group)
                foreach (var accessor in new[] { property.Getter, property.Setter, property.PassThrough }.OfType<FunctionDescriptor>())
                {
                    if (accessor.Name.text != property.Field.Name.text || managedNames.ContainsKey(accessor)) continue;
                    string stem = "Internal_" + accessor.Name.text, candidate = stem;
                    for (int suffix = 1; !reserved.Add(candidate); suffix++) candidate = stem + "_" + suffix;
                    managedNames.Add(accessor, candidate);
                }
        }
    }
    static bool ExportedReference(DeclarationDescriptor declaration)
    {
        if (!ExportAttributes.Read(declaration).Export) return false;
        for (var parent = declaration.Parent; parent is TypeDescriptor; parent = parent.Parent)
            if (!ExportAttributes.Read(parent).Export) return false;
        return true;
    }
    public bool Validates(FunctionDescriptor function) => selected.Contains(function) || nativeCalls.Contains(function);
    // Remove the complete binding dependency group, then regenerate both languages.
    // Failed output and its manifest must never be reused after an exclusion.
    public bool Exclude(DeclarationDescriptor declaration)
    {
        if (declaration.Parent is FunctionDescriptor parent) declaration = parent;
        var removed = new HashSet<DeclarationDescriptor> { declaration };
        if (declaration is ScopeDescriptor scope) Collect(scope);
        bool changed;
        do
        {
            changed = false;
            foreach (var property in Properties.Values)
            {
                DeclarationDescriptor[] group = new DeclarationDescriptor?[] { property.Field, property.Getter, property.Setter, property.PassThrough }.OfType<DeclarationDescriptor>().ToArray();
                if (group.Any(removed.Contains))
                    foreach (var member in group) changed |= removed.Add(member);
            }
            var symbols = removed.OfType<TypeDescriptor>().Select(t => t.TypeSymbol).OfType<TypeSymbol>().ToHashSet();
            foreach (var candidate in Candidates.ToArray())
            {
                bool depends = candidate switch
                {
                    TypeDescriptor type => type.BaseTypes.Any(b => Uses(b.Type, symbols)),
                    FunctionDescriptor function => Uses(function.TypeInfo?.ReturnType, symbols) || function.Fields.Any(p => Uses(p.TypeInfo, symbols)),
                    FieldDescriptor field => Uses(field.TypeInfo, symbols),
                    AliasDescriptor alias => Uses(alias.TypeInfo, symbols),
                    _ => false,
                };
                if (depends && removed.Add(candidate)) { changed = true; if (candidate is ScopeDescriptor child) Collect(child); }
            }
        } while (changed);
        bool progress = false;
        foreach (var member in removed)
        {
            progress |= selected.Remove(member);
            if (member is TypeDescriptor { TypeSymbol: { } symbol }) types.Remove(symbol);
            if (member is FunctionDescriptor function) { progress |= nativeCalls.Remove(function); consumed.Add(function); }
            if (member is FieldDescriptor field) Properties.Remove(field);
        }
        return progress;
        void Collect(ScopeDescriptor owner)
        {
            foreach (var child in owner.Declarations) { removed.Add(child); if (child is ScopeDescriptor nested) Collect(nested); }
            foreach (var child in additional.GetValueOrDefault(owner) ?? []) removed.Add(child);
        }
        static bool Uses(TypeInfo? info, HashSet<TypeSymbol> symbols) => info switch
        {
            NamedTypeInfo named => named.Symbol != null && symbols.Contains(named.Symbol) || named.Arguments.Any(a => Uses(a.Type, symbols)),
            PointerTypeInfo pointer => Uses(pointer.Pointee, symbols),
            ReferenceTypeInfo reference => Uses(reference.Referent, symbols),
            ArrayTypeInfo array => Uses(array.ElementType, symbols),
            FunctionPointerTypeInfo callback => Uses(callback.ReturnType, symbols) || callback.Parameters.Any(p => Uses(p, symbols)),
            _ => false,
        };
    }
    public IEnumerable<DeclarationDescriptor> Candidates => selected.Concat(nativeCalls).Distinct();
    public IEnumerable<DeclarationDescriptor> Members(ScopeDescriptor scope) => scope.Declarations.Concat(additional.GetValueOrDefault(scope) ?? []).Where(Emits);

    bool Marked(DeclarationDescriptor declaration) => all || ExportAttributes.Read(declaration).Export ||
        declaration is TypeDescriptor type && type.TypeSymbol?.Declarations.Any(d => ExportAttributes.Read(d).Export) == true ||
        declaration is EnumDescriptor enumeration && enumeration.TypeSymbol?.Declarations.Any(d => ExportAttributes.Read(d).Export) == true;

    void Select(ScopeDescriptor scope)
    {
        foreach (var declaration in scope.Declarations)
        {
            // Explicit converters may reuse a framework-owned managed type.
            // Its native members must not generate a competing managed declaration.
            if (declaration is TypeDescriptor && declaration.SourceFile?.Converters.TryGet(declaration.QualifiedName, out var existing) == true && existing.ExistingManagedType) continue;
            Errors.AddRange(declaration.Api.Errors); Warnings.AddRange(declaration.Api.Warnings);
            // Flax Hidden members are native-only, not C# members decorated
            // with a UI visibility attribute. Do not demand an interop ABI.
            if (declaration is FunctionDescriptor { Api.Hidden: true }) continue;
            if (declaration.Api.InBuild)
            {
                if (declaration is TypeDescriptor { TypeSymbol: { } symbol }) referencedTypes.Add(symbol);
                if (declaration is EnumDescriptor { TypeSymbol: { } enumeration }) referencedTypes.Add(enumeration);
                continue;
            }
            if (declaration is FunctionDescriptor function)
                foreach (var parameter in function.Fields) { Errors.AddRange(parameter.Api.Errors); Warnings.AddRange(parameter.Api.Warnings); }
            // Templates remain in the source model, but have no concrete callable ABI.
            if (declaration is FunctionDescriptor { IsTemplate: true }) continue;
            if (!Marked(declaration))
            {
                if (declaration is TypeDescriptor ignored) WarnChildren(ignored, ignored);
                continue;
            }
            selected.Add(declaration);
            if (scope is TypeDescriptor { Api.Static: true } && declaration is FunctionDescriptor method && !BindingSignaturePass.IsStatic(method))
                Error(method, "API_CLASS(Static) contains a non-static exported function.");
            if (declaration is TypeDescriptor type)
            {
                if (type.TypeSymbol != null) types.Add(type.TypeSymbol);
                if (type.Api.Template) continue; // Prototype fields are selected only after concrete specialization.
                Select(type);
            }
            else if (declaration is EnumDescriptor enumeration && enumeration.TypeSymbol != null) types.Add(enumeration.TypeSymbol);
            var flags = ExportAttributes.Read(declaration);
            if ((flags.Property || flags.PassThrough) && declaration is not FieldDescriptor)
                Error(declaration, "Getter, Setter and PassThrough apply to exported fields only.");
        }
    }

    void ProjectApiProperties()
    {
        foreach (var group in selected.OfType<FunctionDescriptor>().Where(f => f.Api.Property).GroupBy(f => (Owner: f.Parent, Name: f.Name.text.StartsWith("Get", StringComparison.Ordinal) || f.Name.text.StartsWith("Set", StringComparison.Ordinal) ? f.Name.text[3..] : f.Name.text)))
        {
            if (group.Key.Owner is not TypeDescriptor owner) { Error(group.First(), "API_PROPERTY requires a class, struct or interface owner."); continue; }
            var getters = group.Where(f => !Void(f.TypeInfo?.ReturnType)).ToList();
            var setters = group.Where(f => Void(f.TypeInfo?.ReturnType)).ToList();
            if (getters.Count > 1 || setters.Count > 1 || getters.Any(f => f.Fields.Count != 0) || setters.Any(f => f.Fields.Count != 1))
            { Error(group.First(), "API_PROPERTY requires at most one zero-parameter getter and one single-parameter void setter; indexed/overloaded properties need a converter."); continue; }
            var getter = getters.SingleOrDefault(); var setter = setters.SingleOrDefault();
            var type = getter?.TypeInfo?.ReturnType ?? setter?.Fields[0].TypeInfo;
            if (type is ReferenceTypeInfo reference) type = reference.Referent;
            var setterType = setter?.Fields[0].TypeInfo;
            if (setterType is ReferenceTypeInfo input) setterType = input.Referent;
            if (type is NamedTypeInfo unknown && unknown.Converter == null && unknown.Resolution is not (TypeResolution.Builtin or TypeResolution.Resolved))
            { Error(group.First(), $"No converter/declaration info for native type '{unknown.Display}' in property '{owner.QualifiedName}::{group.Key.Name}'. Register a converter or provide its typedef declaration; includes/macros are not expanded."); continue; }
            string propertyName = owner.QualifiedName + "::" + group.Key.Name;
            bool opaqueHandle = (getter ?? setter)?.SourceFile?.Converters.IsOpaqueHandleProperty(propertyName) == true;
            if (opaqueHandle && type is not PointerTypeInfo { Qualifiers: TypeQualifiers.None, Pointee: NamedTypeInfo { Name: "void", EffectiveQualifiers: TypeQualifiers.None } })
            { Error(getter ?? setter!, "Configured opaque-handle property requires an unqualified void* value: " + propertyName); continue; }
            if (opaqueHandle && (getter?.TypeInfo?.ReturnType is ReferenceTypeInfo || setter != null && setter.Fields[0].Binding?.Passing != ParameterPassing.PointerValue))
            { Error(getter ?? setter!, "Configured opaque-handle property must copy a single void* value; reference returns and pointer-slot setters require a separate lifetime contract: " + propertyName); continue; }
            string signatures = "\n  Getter: " + (getter == null ? "<none>" : PropertySignature(getter)) + "\n  Setter: " + (setter == null ? "<none>" : PropertySignature(setter));
            if (type == null)
            { Error(getter ?? setter ?? group.First(), $"Cannot determine the type of API_PROPERTY '{propertyName}'." + signatures); continue; }
            if (setter != null && !PropertyCompatible(type, setterType))
            { Error(setter, $"API_PROPERTY '{propertyName}' type mismatch: getter/property type is '{type.Display}', but setter parameter '{setter.Fields[0].Name.text}' has type '{setterType?.Display ?? "unknown"}'." + signatures); continue; }
            if (getter != null && setter != null && BindingSignaturePass.IsStatic(getter) != BindingSignaturePass.IsStatic(setter))
            { Error(setter, $"API_PROPERTY '{propertyName}' static mismatch: getter is {(BindingSignaturePass.IsStatic(getter) ? "static" : "instance")}, setter is {(BindingSignaturePass.IsStatic(setter) ? "static" : "instance")}." + signatures); continue; }
            if (setter != null && !opaqueHandle && setter.Fields[0].Binding?.Direction != ParameterDirection.In &&
                setter.Fields[0].Binding is not { Passing: ParameterPassing.LValueReference, Direction: ParameterDirection.Ref, EffectiveType: ReferenceTypeInfo { Referent: NamedTypeInfo } } && HandleNETcpp.Use(setter.Fields[0].TypeInfo) is not { Pointer: true, Slot: false })
            {
                var parameter = setter.Fields[0]; var binding = parameter.Binding;
                if (binding?.Passing == ParameterPassing.PointerValue)
                {
                    SkipInvalidHandle(group, $"Skipped API_PROPERTY '{propertyName}' and both accessors. Parameter '{parameter.Name.text}' is a copied pointer ('{parameter.TypeInfo?.Display}') with writable pointee access, not a C++ reference. This binding has no supported property/handle contract. This does not prove the native declaration is invalid." + signatures, setter);
                    continue;
                }
                string reason = binding == null ? $"setter parameter '{parameter.Name.text}' has no access/direction analysis."
                    : $"setter parameter '{parameter.Name.text}' uses {binding.Passing} with direction {binding.Direction}; property setters currently require an In parameter.";
                Error(setter, $"Cannot emit API_PROPERTY '{propertyName}': " + reason + signatures); continue;
            }
            var origin = getter ?? setter!;
            Token name = origin.Name; name.text = group.Key.Name;
            FieldDescriptor property = new() { Name = name, TypeInfo = type, HeaderTokens = new(origin.HeaderTokens), Parent = owner,
                NamespaceTokens = new(origin.NamespaceTokens), EnclosingTypeTokens = new(origin.EnclosingTypeTokens), Access = origin.Access };
            property.Api.ManagedAttributes.AddRange(group.SelectMany(f => f.Api.ManagedAttributes).Distinct(StringComparer.Ordinal));
            selected.Add(property);
            if (!additional.TryGetValue(owner, out var members)) additional.Add(owner, members = []);
            members.Add(property);
            Properties.Add(property, new() { Field = property, Getter = getter, Setter = setter, IsStatic = BindingSignaturePass.IsStatic(origin) });
            foreach (var accessor in group) { consumed.Add(accessor); nativeCalls.Add(accessor); }
        }
    }

    static string PropertySignature(FunctionDescriptor function)
    {
        var signature = function.TypeInfo;
        string parameters = string.Join(", ", function.Fields.Select(parameter => (parameter.TypeInfo?.Display ?? "<unknown>") + (parameter.IsUnnamed ? "" : " " + parameter.Name.text)));
        string suffix = signature == null ? "" : ((signature.Qualifiers & TypeQualifiers.Const) != 0 ? " const" : "") + ((signature.Qualifiers & TypeQualifiers.Volatile) != 0 ? " volatile" : "");
        return (BindingSignaturePass.IsStatic(function) ? "static " : "") + (signature?.ReturnType?.Display ?? "<unknown>") + " " + function.Name.text + "(" + parameters + ")" + suffix;
    }

    void SkipInvalidHandle(IEnumerable<FunctionDescriptor> functions, string reason, FunctionDescriptor? location = null)
    {
        var excluded = functions.ToArray();
        foreach (var function in excluded) { selected.Remove(function); consumed.Add(function); nativeCalls.Remove(function); }
        var origin = location ?? excluded[0];
        Errors.Add(new LogEntry("InvalidHandleError: " + reason + " No C# member, native wrapper or interop import is emitted for this binding.",
            origin.Name, LogEntryType.InvalidHandleError, "VETCS005", origin.SourceFile?.Path));
    }

    void WarnChildren(TypeDescriptor scope, TypeDescriptor ignored)
    {
        foreach (var child in scope.Declarations)
        {
            if (ExportAttributes.Read(child).Export)
                Warnings.Add(new LogEntry($"Ignoring exported '{child.QualifiedName}': enclosing type '{ignored.QualifiedName}' is not marked [[Export]].", child.Name, LogEntryType.Warning, "VETCS002"));
            if (child is TypeDescriptor nested) WarnChildren(nested, ignored);
        }
    }

    void Project(FieldDescriptor field)
    {
        var flags = ExportAttributes.Read(field);
        if (!flags.Property && !flags.PassThrough) return;
        if (field.Parent is not ScopeDescriptor owner) { Error(field, "Property field has no declaring scope."); return; }
        if (field.TypeInfo == null || !field.TypeInfo.IsSupported || field.TypeInfo is ArrayTypeInfo or ReferenceTypeInfo)
        { Error(field, "Property/accessor storage type requires a supported non-array, non-reference field type."); return; }
        if ((flags.Setter || flags.PassThrough) && (IsConst(field.TypeInfo) || field.TypeInfo.SpecifierTokens.Any(t => t == "constexpr")))
        { Error(field, "Setter/PassThrough cannot assign a const field."); return; }
        PropertyProjection property = new() { Field = field, IsStatic = NativeLayout.IsStatic(field) || owner is FileDescriptor };
        Properties.Add(field, property);
        bool isClass = field.TypeInfo is NamedTypeInfo { Symbol.Definition: ClassDescriptor };
        if (flags.PassThrough)
        {
            property.PassThrough = Find("GetSet" + field.Name.text, f => GetterReturn(f, true) && SetterInput(f),
                $"{TypeName()}& GetSet{field.Name.text}({ConstReference()} InValue)");
            Consume(property.PassThrough);
        }
        else
        {
            if (flags.Getter)
            {
                property.Getter = Find("Get" + field.Name.text, f => f.Fields.Count == 0 && GetterReturn(f, isClass),
                    $"{TypeName()}{(isClass ? "&" : "")} Get{field.Name.text}()");
                Consume(property.Getter);
            }
            if (flags.Setter)
            {
                property.Setter = Find("Set" + field.Name.text, f => Void(f.TypeInfo?.ReturnType) && SetterInput(f),
                    $"void Set{field.Name.text}({(isClass ? "const " : "")}{TypeName()}{(isClass ? "&" : "")} InValue)");
                Consume(property.Setter);
            }
        }

        string TypeName() => field.TypeInfo.Display;
        string ConstReference() => field.TypeInfo is PointerTypeInfo or FunctionPointerTypeInfo ? TypeName() + " const&" : "const " + TypeName() + "&";
        bool GetterReturn(FunctionDescriptor function, bool reference)
        {
            var type = function.TypeInfo?.ReturnType;
            if (!reference) return type is not ReferenceTypeInfo && Same(type, field.TypeInfo);
            return type is ReferenceTypeInfo { ReferenceKind: ReferenceKind.LValue } returned && !IsConst(returned.Referent) && Same(returned.Referent, field.TypeInfo);
        }
        bool SetterInput(FunctionDescriptor function)
        {
            if (function.Fields.Count != 1) return false;
            var parameter = function.Fields[0];
            if (parameter.Binding?.Direction != ParameterDirection.In) return false;
            if (flags.PassThrough || isClass)
                return parameter.TypeInfo is ReferenceTypeInfo { ReferenceKind: ReferenceKind.LValue } reference && IsConst(reference.Referent) && Same(reference.Referent, field.TypeInfo);
            return parameter.TypeInfo is ReferenceTypeInfo { ReferenceKind: ReferenceKind.LValue } input
                ? Same(input.Referent, field.TypeInfo) : Same(parameter.TypeInfo, field.TypeInfo);
        }
        FunctionDescriptor? Find(string name, Func<FunctionDescriptor, bool> matches, string expected)
        {
            var candidates = owner.Functions.Where(f => f.Name.text == name &&
                f.NamespaceTokens.Select(t => t.text).SequenceEqual(field.NamespaceTokens.Select(t => t.text)) &&
                (owner is FileDescriptor || BindingSignaturePass.IsStatic(f) == property.IsStatic) &&
                f.Binding?.RedeclarationOf == null &&
                f.TypeInfo?.SuffixTokens.SkipWhile(t => t != "=").Skip(1).FirstOrDefault(t => !t.IsTrivia).text != "delete" && matches(f)).ToList();
            if (candidates.Count == 1) return candidates[0];
            Error(field, candidates.Count == 0 ? $"Missing compatible native accessor for '{field.QualifiedName}'. Expected existing declaration: {expected}."
                : $"Ambiguous native accessor '{name}' for '{field.QualifiedName}': {candidates.Count} matching declarations.");
            return null;
        }
        void Consume(FunctionDescriptor? function)
        {
            if (function == null) return;
            consumed.Add(function); nativeCalls.Add(function);
        }
    }
    void ProjectDirectField(FieldDescriptor field)
    {
        if (field.TypeInfo is ArrayTypeInfo array && field.Api.NoArray && array.ElementType is NamedTypeInfo &&
            int.TryParse(string.Concat(array.EffectiveExtentTokens.Where(t => !t.IsTrivia).Select(t => t.text)), out int count) && count > 0 && count <= 4096 &&
            field.Parent is ScopeDescriptor owner)
        {
            var names = Enumerable.Range(0, count).Select(i => field.Name.text + i).ToArray();
            if (owner.Declarations.Any(d => d != field && names.Contains(d.Name.text, StringComparer.Ordinal)))
            { Error(field, $"Numbered NoArray fields for '{field.QualifiedName}' collide with an existing declaration."); return; }
            if (!additional.TryGetValue(owner, out var extra)) additional.Add(owner, extra = []);
            selected.Remove(field);
            for (int i = 0; i < count; i++)
            {
                var name = field.Name; name.text = names[i];
                FieldDescriptor item = new() { Name = name, Parent = owner, TypeInfo = array.ElementType, Api = field.Api, Access = field.Access,
                    NamespaceTokens = new(field.NamespaceTokens), EnclosingTypeTokens = new(field.EnclosingTypeTokens), HeaderTokens = new(field.HeaderTokens),
                    DiagnosticOrigin = field.DiagnosticFile, NativeArrayField = field, NativeArrayIndex = i, NativeArrayCount = count };
                extra.Add(item); selected.Add(item); ProjectDirectField(item);
            }
            return;
        }
        if (field.TypeInfo is null or ArrayTypeInfo or ReferenceTypeInfo || !field.TypeInfo.IsSupported)
        { Error(field, $"Field '{field.QualifiedName}' requires an array/reference/type converter before it can be exposed as a native-backed property."); return; }
        bool isStatic = field.Parent is FileDescriptor || NativeLayout.IsStatic(field.NativeArrayField ?? field);
        if (field.Parent is TypeDescriptor { Api.Static: true } && !isStatic)
        { Error(field, "A static managed class cannot expose a non-static native field property."); return; }
        TypeInfo valueType = field.TypeInfo switch
        {
            NamedTypeInfo named => new NamedTypeInfo { Name = named.Name, Resolution = named.Resolution, Symbol = named.Symbol, Arguments = new(named.Arguments), Converter = named.Converter, Specialization = named.Specialization },
            PointerTypeInfo pointer => new PointerTypeInfo { Pointee = pointer.Pointee },
            _ => field.TypeInfo
        };
        bool readOnly = field.Api.Binding.ReadOnly || IsConst(field.TypeInfo) || field.TypeInfo.SpecifierTokens.Any(t => t == "constexpr");
        TypeInfo getterType = valueType;
        if (readOnly && valueType is NamedTypeInfo { Converter.Bodies.BorrowedObjectElements: true } borrowed)
            getterType = new ReferenceTypeInfo { ReferenceKind = ReferenceKind.LValue, Referent = new NamedTypeInfo {
                Name = borrowed.Name, Symbol = borrowed.Symbol, Resolution = borrowed.Resolution, Arguments = new(borrowed.Arguments),
                Converter = borrowed.Converter, Specialization = borrowed.Specialization, Qualifiers = TypeQualifiers.Const } };
        var getter = Accessor(false);
        var setter = readOnly ? null : Accessor(true);
        Properties.Add(field, new() { Field = field, Getter = getter, Setter = setter, IsStatic = isStatic });
        nativeCalls.Add(getter); if (setter != null) nativeCalls.Add(setter);

        FunctionDescriptor Accessor(bool set)
        {
            Token name = field.Name; name.text = (set ? "Set" : "Get") + field.Name.text;
            FunctionDescriptor function = new() { Name = name, Parent = field.Parent, Access = field.Access,
                DiagnosticOrigin = field.DiagnosticFile,
                NamespaceTokens = new(field.NamespaceTokens), EnclosingTypeTokens = new(field.EnclosingTypeTokens), HeaderTokens = new(field.HeaderTokens),
                AccessedField = field, IsFieldSetter = set, GeneratedStatic = isStatic,
                TypeInfo = new() { ReturnType = set ? new NamedTypeInfo { Name = "void", Resolution = TypeResolution.Builtin } : getterType },
                Binding = new() { NativeSignature = "field:" + field.QualifiedName + (set ? ":set" : ":get") } };
            if (set)
            {
                Token parameterName = field.Name; parameterName.text = "value";
                var binding = new ParameterBindingInfo { EffectiveType = valueType, Passing = valueType is PointerTypeInfo or FunctionPointerTypeInfo ? ParameterPassing.PointerValue : ParameterPassing.Value, Direction = ParameterDirection.In };
                function.Fields.Add(new() { Name = parameterName, Parent = function, TypeInfo = valueType, IsParameter = true, Binding = binding });
                function.TypeInfo.Parameters.Add(valueType); function.Binding.Parameters.Add(binding);
            }
            return function;
        }
    }
    static bool Void(TypeInfo? type) => type is NamedTypeInfo n && (n.Converter is { IsAlias: true } c ? c.AbiCppType == "void" : n.Resolution == TypeResolution.Builtin && n.Name == "void");
    static bool Same(TypeInfo? a, TypeInfo? b) => a != null && b != null && BindingSignaturePass.Canonical(a, true) is { } key && key == BindingSignaturePass.Canonical(b, true);
    static bool PropertyCompatible(TypeInfo? a, TypeInfo? b) => Same(a, b) ||
        a is NamedTypeInfo { Converter.Bodies: { } getter } && b is NamedTypeInfo { Converter.Bodies: { } setter } && getter.ManagedType == setter.ManagedType;
    static bool IsConst(TypeInfo type) => type switch
    {
        NamedTypeInfo named => (named.EffectiveQualifiers & TypeQualifiers.Const) != 0,
        PointerTypeInfo pointer => (pointer.Qualifiers & TypeQualifiers.Const) != 0,
        FunctionPointerTypeInfo pointer => (pointer.Qualifiers & TypeQualifiers.Const) != 0,
        _ => false
    };
    void Error(DeclarationDescriptor declaration, string message) => Errors.Add(new LogEntry(message, declaration.Name, LogEntryType.Error, "VETCS001"));
}
