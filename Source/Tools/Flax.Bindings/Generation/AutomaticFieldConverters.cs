using System.Text;
using VETools.Text;
using static VETools.Tokenizer;
using TypeInfo = VETools.Text.TypeInfo;

namespace VETools.Generation;

// Opt-in types, field-by-field value snapshots. No copying native object layouts,
// ownership graphs, alternative union members, callbacks or managed references.
internal sealed class AutomaticFieldConverters
{
    sealed record Scalar(string Cpp, string Managed, int Size, TypeInfo? ObjectType = null, NativeTypeConverter? Converter = null, bool RawPointer = false, string? ManagedValueType = null);
    sealed record Member(FieldDescriptor Field, Scalar? Scalar, Shape? Nested, int ArrayCount = 0);
    sealed record Leaf(string Path, Scalar Scalar, int Offset)
    {
        public string? ManagedOverride;
        public string ManagedPath => ManagedOverride ?? Path.Replace("[", ".Data[", StringComparison.Ordinal);
        public FieldDescriptor? PrivateBuffer;
        public int BufferIndex;
        public FieldDescriptor? AccessField;
    }
    sealed class Shape(TypeDescriptor type)
    {
        public TypeDescriptor Type = type;
        public List<Member> Members = [];
        public List<Leaf> Leaves = [];
        public int Size;
        public string Managed = "";
        public string DataManaged => Type.SourceFile?.Converters.ManagedValuePartial(Type.QualifiedName) ?? (Type.CanonicalManagedValue ? HandleNetCS.ManagedName(Type) : SnapshotName(Type));
        public bool Snapshot;
        public bool SelectedFieldsSnapshot;
        public bool FlattenedArrays;
        public bool HasInlineArrays => Leaves.Any(l => l.Path.Contains('['));
    }
    readonly Dictionary<TypeDescriptor, Shape?> shapes = [];
    readonly Dictionary<TypeDescriptor, NativeTypeConverter> converters = [];
    readonly HashSet<TypeDescriptor> active = [], bases = [];
    readonly NativeLayout layout = new();
    int attached;

    public static void Apply(FileDescriptor root)
    {
        if (root.TypeInfos!.AutomaticFieldsReady) return;
        var files = root.ProjectFiles.Count == 0 ? new[] { root } : root.ProjectFiles.ToArray();
        var types = files.SelectMany(TemplateInstantiationPass.Declarations).OfType<TypeDescriptor>()
            .Concat(root.TypeInfos.TemplateInstances.Values).Distinct().ToArray();
        foreach (var type in types) type.NativeFieldView = false;
        // Containers depend on record converters, and records can themselves
        // contain containers. Retry only while new record uses are resolved.
        // Attach never replaces a converter, so this finite graph converges.
        int attached;
        do
        {
            foreach (var file in files) file.TypeInfos!.StandardConvertersReady = false;
            StandardConverters.Apply(root);
            attached = ApplyRound(root, files, types);
        } while (attached != 0);
        // Exposing a native field through a typed accessor does not require a
        // complete object layout or a whole-value converter. Keep these views
        // borrowed; value/ref support is still checked independently per call.
        var nativeLayout = new NativeLayout();
        foreach (var type in types.OfType<StructDescriptor>().Where(t => !t.Api.InBuild && !t.Api.Template && !t.IsForwardDeclaration && t.MarshalAsType == null && ExportAttributes.Read(t).Export))
            if (nativeLayout.Struct(type) == null) type.NativeFieldView = true;
        foreach (var file in files) file.TypeInfos!.AutomaticFieldsReady = true;
    }

    static int ApplyRound(FileDescriptor root, FileDescriptor[] files, TypeDescriptor[] types)
    {
        var pass = new AutomaticFieldConverters();
        foreach (var type in types)
            foreach (var basis in type.BaseTypes)
                if ((basis.Type as NamedTypeInfo)?.Symbol?.Definition is TypeDescriptor definition) pass.bases.Add(definition);
        foreach (var type in types) pass.Build(type);
        foreach (var shape in pass.shapes.Values.OfType<Shape>())
            if (shape.Type is StructDescriptor structure && (pass.layout.Struct(structure) is not { } known || known.Storage.Alignment > IntPtr.Size)) shape.Type.NativeFieldView = true;
        foreach (var shape in pass.shapes.Values.OfType<Shape>())
        {
            var converter = pass.Create(shape, pass.Declarations(shape));
            pass.converters.Add(shape.Type, converter); shape.Type.AutomaticValueConverter = converter;
        }
        foreach (var declaration in files.SelectMany(TemplateInstantiationPass.Declarations).Concat(root.TypeInfos!.TemplateInstances.Values.SelectMany(TemplateInstantiationPass.Declarations)))
            foreach (var type in TemplateInstantiationPass.Types(declaration)) pass.Attach(type);
        return pass.attached;
    }

    Shape? Build(TypeDescriptor type)
    {
        if (shapes.TryGetValue(type, out var cached)) return cached;
        if (active.Count >= 32 || !active.Add(type)) return null;
        shapes[type] = null;
        try
        {
            if (type.Api.InBuild || type.Api.MarshalAsTokens.Count != 0 || HandleNETcpp.IsScriptingObject(type) || !ExportAttributes.Read(type).Export || type.Api.Template || type.Api.Abstract || type.Api.Static || type.IsForwardDeclaration || type is InterfaceDescriptor ||
                !NativeLayout.HasStatelessInterfaceBases(type) || bases.Contains(type) && type.SourceFile?.Converters.ManagedValuePartial(type.QualifiedName) == null || type.Functions.Any(f => f.HeaderTokens.Any(t => t == "virtual"))) return null;
            for (DeclarationDescriptor nested = type; nested.Parent is TypeDescriptor parent; nested = parent)
                if (nested.Access != "public") return null;
            if (type.TypeSymbol?.Declarations.OfType<TypeDescriptor>().Count(t => !t.IsForwardDeclaration) > 1) return null;
            if (type.SourceFile?.Errors.Any(d => d.BlocksGeneration) == true) return null;
            var constructors = type.Functions.Where(f => f.TypeInfo?.IsConstructor == true).ToArray();
            if (constructors.Any(f => f.Access != "public" || Deleted(f))) return null;
            if (constructors.Length != 0 && !constructors.Any(f => f.Fields.All(p => p.InitializerTokens.Any(t => !t.IsTrivia)) && f.Access == "public" && !Deleted(f))) return null;
            if (type.Functions.Any(f => f.TypeInfo?.IsDestructor == true && (f.Access != "public" || Deleted(f)))) return null;
            var fields = type.Fields.Where(f => !NativeLayout.IsStatic(f)).ToList();
            var anonymous = AnonymousFields(type);
            if (anonymous == null) return null;
            foreach (var field in anonymous)
            {
                fields.RemoveAll(f => f.Name.text == field.Name.text && f.TypeInfo is UnsupportedTypeInfo);
                if (!fields.Any(f => f.Name.text == field.Name.text)) fields.Add(field);
            }
            bool selectedSettings = type.SourceFile?.Converters.IsExportedSettingsSnapshot(type.NativeQualifiedName) == true;
            if (selectedSettings) fields.RemoveAll(f => !ExportAttributes.Read(f).Export);
            if (fields.Count == 0 || fields.Count > 1024 || fields.Any(f => f.IsBitField || f.Access != "public" || f.TypeInfo == null)) return null;
            Shape shape = new(type);
            foreach (var field in fields.OrderBy(f => f.Name.line).ThenBy(f => f.Name.col))
            {
                if (field.TypeInfo is ArrayTypeInfo array)
                {
                    if (field.Api.NoArray && field.Api.ManagedAccess is null or "public" or "internal" &&
                        array.ElementType is NamedTypeInfo { EffectiveQualifiers: TypeQualifiers.None } &&
                        int.TryParse(string.Concat(array.EffectiveExtentTokens.Where(t => !t.IsTrivia).Select(t => t.text)), out int recordCount) && recordCount > 0 && recordCount <= 4096 &&
                        Definition(array.ElementType) is { } elementDefinition && Build(elementDefinition) is { HasInlineArrays: false } record &&
                        ExportAttributes.Read(field).Export && !ExportAttributes.Read(field).Property && !ExportAttributes.Read(field).PassThrough)
                    {
                        if (shape.Leaves.Count + (long)recordCount * record.Leaves.Count > 4096 || shape.Size + (long)recordCount * record.Size > 65536) return null;
                        if (Enumerable.Range(0, recordCount).Any(i => fields.Any(other => other != field && other.Name.text == field.Name.text + i))) return null;
                        shape.FlattenedArrays = true;
                        shape.Members.Add(new(field, null, record, recordCount));
                        for (int i = 0; i < recordCount; i++)
                            foreach (var leaf in record.Leaves)
                            {
                                shape.Leaves.Add(leaf with { Path = Id(field.Name.text) + "[" + i + "]." + leaf.Path,
                                    ManagedOverride = Id(field.Name.text + i) + "." + leaf.ManagedPath, Offset = shape.Size });
                                shape.Size = checked(shape.Size + leaf.Scalar.Size);
                            }
                        continue;
                    }
                    // Only fixed primitive buffers with an emitted, accessible
                    // inline representation. No dynamic arrays or unknown layout.
                    if (type is not StructDescriptor || !ExportAttributes.Read(field).Export ||
                        field.Api.ManagedAccess is not (null or "public" or "internal" or "private") ||
                        ExportAttributes.Read(field).Property || ExportAttributes.Read(field).PassThrough ||
                        array.ElementType is NamedTypeInfo element && element.EffectiveQualifiers != TypeQualifiers.None ||
                        !int.TryParse(string.Concat(array.EffectiveExtentTokens.Where(t => !t.IsTrivia).Select(t => t.text)), out int count) || count <= 0 || count > 4096 ||
                        InlineScalar(array.ElementType) is not { Converter: null, ObjectType: null, ManagedValueType: null } scalarElement ||
                        scalarElement.Managed is not ("byte" or "sbyte" or "short" or "ushort" or "int" or "uint" or "long" or "ulong" or "float" or "double" or "char") ||
                        shape.Leaves.Count + count > 4096) return null;
                    if (Enumerable.Range(0, count).Any(i => fields.Any(other => other != field && other.Name.text == field.Name.text + i))) return null;
                    shape.Members.Add(new(field, scalarElement, null, count));
                    for (int i = 0; i < count; i++)
                    {
                        shape.Leaves.Add(new(Id(field.Name.text) + "[" + i + "]", scalarElement, shape.Size) {
                            PrivateBuffer = field.Api.ManagedAccess == "private" ? field : null, BufferIndex = i });
                        shape.Size = checked(shape.Size + scalarElement.Size);
                    }
                    continue;
                }
                if (field.TypeInfo is NamedTypeInfo cv && (cv.EffectiveQualifiers & (TypeQualifiers.Const | TypeQualifiers.Volatile)) != 0) return null;
                var scalar = ScalarType(field.TypeInfo!);
                // An explicitly exported pointer field is a borrowed address,
                // just like the existing direct field mapping. Never dereference,
                // adopt ownership, or infer a pointee layout from that address.
                if (scalar == null && field.TypeInfo is PointerTypeInfo { Qualifiers: TypeQualifiers.None } && ExportAttributes.Read(field).Export)
                    scalar = new("unsigned long long", "nint", 8, RawPointer: true);
                Shape? nested = scalar == null && field.TypeInfo is NamedTypeInfo { Converter: null or { AutomaticFields: true } }
                    ? Definition(field.TypeInfo!) is { } definition ? Build(definition) : null : null;
                if (scalar == null && nested == null) return null;
                if (nested != null && (shape.Size + nested.Size > 65536 ||
                    (shape.HasInlineArrays || nested.HasInlineArrays) && shape.Leaves.Count + nested.Leaves.Count > 4096)) return null;
                shape.Members.Add(new(field, scalar, nested));
                if (scalar != null) { shape.Leaves.Add(new(Id(field.Name.text), scalar, shape.Size) {
                    AccessField = field.Api.ManagedAccess == "private" ? field : null }); shape.Size = checked(shape.Size + scalar.Size); }
                else foreach (var leaf in nested!.Leaves)
                { shape.Leaves.Add(leaf with { Path = Id(field.Name.text) + "." + leaf.Path, ManagedOverride = leaf.ManagedOverride == null ? null : Id(field.Name.text) + "." + leaf.ManagedPath, Offset = shape.Size }); shape.Size = checked(shape.Size + leaf.Scalar.Size); }
            }
            if (shape.Size > 65536) return null;
            shape.SelectedFieldsSnapshot = selectedSettings || shape.Members.Any(m => m.Nested?.SelectedFieldsSnapshot == true || m.Scalar?.Converter?.Bodies?.SelectedFieldsSnapshot == true);
            shape.Snapshot = shape.SelectedFieldsSnapshot || shape.FlattenedArrays || type is not StructDescriptor structure || layout.Struct(structure) is not { } knownLayout || knownLayout.Storage.Alignment > IntPtr.Size ||
                shape.Members.Any(m => !ExportAttributes.Read(m.Field).Export || ExportAttributes.Read(m.Field).Property || ExportAttributes.Read(m.Field).PassThrough || m.Nested?.Snapshot == true || m.Scalar?.ObjectType != null || m.Scalar?.Converter != null);
            string? valuePartial = type.SourceFile?.Converters.ManagedValuePartial(type.QualifiedName);
            type.CanonicalManagedValue = type.SourceFile?.Converters.CanonicalValueRecords == true && type is StructDescriptor && !shape.Leaves.Any(l => l.AccessField != null || l.PrivateBuffer != null) &&
                !type.Functions.Any(f => ExportAttributes.Read(f).Export) && !type.Fields.Any(f => NativeLayout.IsStatic(f) && ExportAttributes.Read(f).Export) &&
                !type.Declarations.OfType<TypeDescriptor>().Any(t => ExportAttributes.Read(t).Export);
            shape.Snapshot |= valuePartial != null || type.CanonicalManagedValue;
            shape.Managed = valuePartial ?? (type.CanonicalManagedValue ? HandleNetCS.ManagedName(type) : shape.Snapshot ? SnapshotName(type) : HandleNetCS.ManagedName(type));
            if (shape.Snapshot && shape.Leaves.Any(l => l.AccessField != null)) return null; // Never flatten restricted fields into a public snapshot.
            // Snapshot DTOs flatten fixed buffers. Native paths keep their indexes;
            // direct shared-layout buffers retain the existing InlineArray contract.
            if (shape.Snapshot)
            {
                if (shape.Leaves.Any(l => l.PrivateBuffer != null)) return null;
                foreach (var leaf in shape.Leaves)
                    leaf.ManagedOverride = System.Text.RegularExpressions.Regex.Replace(leaf.Path, @"\[(\d+)\]", "$1");
            }
            if (shape.Leaves.Count > 4096) return null;
            shapes[type] = shape;
            return shape;
        }
        finally { if (shapes[type] == null) type.CanonicalManagedValue = false; active.Remove(type); }
    }

    static string AssignFields(Shape shape, string destination, string source)
    {
        List<string> assignments = [];
        foreach (var member in shape.Members)
            for (int i = 0; i < Math.Max(1, member.ArrayCount); ++i)
            {
                string suffix = "." + member.Field.Name.text + (member.ArrayCount == 0 ? "" : "[" + i + "]");
                string target = "(" + destination + ")" + suffix, value = "(" + source + ")" + suffix;
                assignments.Add(member.Nested != null ? AssignFields(member.Nested, target, value)
                    : member.Scalar?.Converter?.Bodies?.AssignNativeValue is { } assign ? assign(target, value) + ";"
                    : target + " = " + value + ";");
            }
        return string.Join(" ", assignments);
    }

    static string ArrayAssertions(Shape shape)
    {
        HashSet<Shape> seen = [];
        string Visit(Shape current) => !seen.Add(current) ? "" : string.Concat(current.Members.Where(m => m.ArrayCount != 0).Select(m =>
            $"\nstatic_assert(std::extent<decltype(std::declval<::{current.Type.NativeQualifiedName}>().{m.Field.Name.text})>::value == {m.ArrayCount}, \"VET fixed-array extent differs from the compiler\");\n")) +
            string.Concat(current.Members.Select(m => m.Nested).OfType<Shape>().Select(Visit));
        return Visit(shape);
    }
    static bool Deleted(FunctionDescriptor function) => function.HeaderTokens.Any(t => t == "delete");
    Scalar? InlineScalar(TypeInfo type)
    {
        // Char's scalar wire type is ushort, while its explicitly registered
        // inline representation is a UTF-16 C# char. No packet/pointer layout is inferred.
        if (type is NamedTypeInfo { Converter.Bodies: { ManagedType: "char", NativeInlineStorage: (2, 2) } })
            return new("unsigned short", "char", 2);
        return ScalarType(type);
    }
    static TypeDescriptor? Definition(TypeInfo type) => type is NamedTypeInfo { Resolution: TypeResolution.Resolved } named ? named.Specialization?.Concrete ?? named.Symbol?.Definition as TypeDescriptor : null;
    static string Id(string name) => CSharpGenerator.Qualified(name);
    static string SnapshotNamespace(TypeDescriptor type)
    {
        string enclosing = HandleNetCS.EnclosingNames(type).Replace("::", ".");
        if (type.Api.ManagedNamespace is { } configured) return string.Join(".", new[] { configured, "Values", enclosing }.Where(s=>s.Length!=0));
        string native = string.Concat(type.NamespaceTokens.Select(t => t.text)).Replace("::", ".");
        string prefix = (type.ConcreteTemplate?.Definition.SourceFile ?? type.SourceFile)?.IncludeNamespace ?? "VETools.Values";
        if (prefix.Length == 0) prefix = "Values"; // A root header still needs a namespace distinct from its native access view.
        return string.Join(".", new[] { prefix, native, enclosing }.Where(s => s.Length != 0));
    }
    static string SnapshotName(TypeDescriptor type) => "global::" +
        (SnapshotNamespace(type) is { Length: > 0 } ns ? ManagedNamespace(ns) + "." : "") + Id(HandleNetCS.SimpleName(type));
    static string ManagedNamespace(string value) => string.Join(".", value.Split('.').Select(Id));

    List<FieldDescriptor>? AnonymousFields(TypeDescriptor type)
    {
        List<FieldDescriptor> fields = [];
        foreach (var raw in type.Declarations.OfType<RawDeclarationDescriptor>())
        {
            var tokens = raw.HeaderTokens;
            if (!tokens.Any(t => t == "union")) continue;
            List<(int Start, int End)> groups = [];
            for (int i = 0; i + 1 < tokens.Count; ++i)
            {
                if (tokens[i] != "struct") continue;
                int start = i + 1; while (start < tokens.Count && tokens[start].IsTrivia) ++start;
                if (start == tokens.Count || tokens[start] != "{") continue;
                int end = start, depth = 0;
                do { if (tokens[end] == "{") ++depth; if (tokens[end] == "}") --depth; ++end; } while (end < tokens.Count && depth != 0);
                if (depth != 0) return null;
                groups.Add((start, end)); i = end - 1;
            }
            if (groups.Count != 1) return null; // Never select between alternative active members.
            var group = groups[0];
            var parsed = Parshe.PreParse(new Tokenizer.Stream(tokens.GetRange(group.Start + 1, group.End - group.Start - 2)), new(type.NamespaceTokens), type.SourceFile?.CppStandard ?? 20, type.SourceFile?.Converters);
            TypeInfoPass.ResolveTemplateMembers(parsed, type.SourceFile!.TypeInfos!);
            foreach (var field in parsed.Fields)
            {
                // Concrete template members already carry substituted types.
                var resolved = type.Fields.FirstOrDefault(f => f.Name.text == field.Name.text && f.TypeInfo is not UnsupportedTypeInfo) ?? field;
                resolved.Access = raw.Access;
                fields.Add(resolved);
            }
        }
        return fields;
    }

    Scalar? ScalarType(TypeInfo type)
    {
        if (HandleNETcpp.Use(type) is { Pointer: true, Slot: false } scripting && HandleNETcpp.CanCast(scripting.Type))
            return new("unsigned long long", "ulong", 8, type);
        if (type is not NamedTypeInfo named) return null;
        if (named.Converter?.FunctionValue==true || named.Converter?.Bodies?.BorrowedObjectElements == true) return null;
        if (named.Converter is { IsAlias: true } alias) return ScalarType(alias.AbiType(named.EffectiveQualifiers));
        // Enum converters carry no ownership. Keep their scalar field ABI even
        // when the standard converter pass has already registered them.
        if (named.Converter?.EnumValue == true && named.Symbol?.Definition is EnumDescriptor convertedEnum)
            return EnumScalar(convertedEnum);
        if (named.Converter is { AutomaticFields: false, Bodies: { ReleaseManagedAbi: not null, ReleaseNativeAbi: not null } } converter &&
            (named.Symbol?.QualifiedName ?? named.Name.TrimStart(':')) != "Span")
        {
            var wire = ScalarAbi.Require(converter);
            return new(wire.Cpp, wire.CS, wire.Size, Converter: converter);
        }
        if (named.Converter != null) return null; // Explicit non-scalar converters keep their own contract.
        if (named.Resolution == TypeResolution.Resolved && named.Symbol?.Definition is EnumDescriptor enumeration)
            return EnumScalar(enumeration);
        if (named.Resolution != TypeResolution.Builtin) return null;
        var words = named.Name.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        bool unsigned = words.Contains("unsigned"); int longs = words.Count(w => w == "long");
        if (words.Contains("void") || words.Contains("auto") || words.Contains("double") && longs != 0) return null;
        if (words.Contains("bool")) return new("bool", "bool", 1);
        if (words.Contains("float")) return new("float", "float", 4);
        if (words.Contains("double")) return new("double", "double", 8);
        if (words.Contains("char16_t") || words.Contains("wchar_t")) return new("unsigned short", "char", 2);
        if (words.Contains("char32_t")) return new("unsigned int", "uint", 4);
        if (words.Contains("char8_t")) return new("unsigned char", "byte", 1);
        if (words.Contains("char")) return new(unsigned ? "unsigned char" : "signed char", unsigned ? "byte" : "sbyte", 1);
        if (words.Contains("short")) return new(unsigned ? "unsigned short" : "short", unsigned ? "ushort" : "short", 2);
        return longs >= 2 ? new(unsigned ? "unsigned long long" : "long long", unsigned ? "ulong" : "long", 8) : new(unsigned ? "unsigned int" : "int", unsigned ? "uint" : "int", 4);
    }

    Scalar? EnumScalar(EnumDescriptor enumeration)
    {
        var scalar = enumeration.UnderlyingType != null ? ScalarType(enumeration.UnderlyingType) : enumeration.IsScoped ? new("int", "int", 4) : null;
        return scalar == null ? null : scalar with { ManagedValueType = HandleNetCS.ManagedName(enumeration) };
    }

    static string WrapValue(Shape shape, string body, string? managedType = null)
    {
        string full = (managedType ?? shape.Managed).Replace("global::", "");
        string ns; List<TypeDescriptor> parents = [];
        if (shape.Type.CanonicalManagedValue)
        {
            ns = HandleNetCS.NamespaceKey(shape.Type).Replace("::", ".");
            for (var owner = shape.Type.Parent as TypeDescriptor; owner != null; owner = owner.Parent as TypeDescriptor) parents.Insert(0, owner);
        }
        else { int split = full.LastIndexOf('.'); ns = split < 0 ? "" : full[..split]; }
        StringBuilder code = new();
        if (ns.Length != 0) {
            code.AppendLine("namespace " + ns + " {");
            foreach (string import in shape.Type.SourceFile?.Converters.ManagedUsings ?? []) code.AppendLine("using global::" + import + ";");
            if (shape.Type.SourceFile?.Converters.DefaultManagedNamespace != null && shape.Members.Any(m => m.Field.Api.ManagedAttributes.Any(a => a.Contains("JsonProperty(")))) code.AppendLine("using global::Newtonsoft.Json;");
        }
        foreach (var parent in parents) code.AppendLine((parent.Api.ManagedAccess ?? "public") + (parent.Api.Static ? " static" : "") + " partial " + (parent is StructDescriptor ? "struct " : "class ") + Id(HandleNetCS.SimpleName(parent)) + " {");
        code.AppendLine(body);
        foreach (var parent in parents) code.AppendLine("}");
        if (ns.Length != 0) code.AppendLine("}");
        return code.ToString();
    }

    IReadOnlyList<string> Declarations(Shape root)
    {
        if (!root.Snapshot) return [];
        HashSet<Shape> needed = [];
        void Collect(Shape shape) { if (!needed.Add(shape)) return; foreach (var nested in shape.Members.Select(m => m.Nested).OfType<Shape>()) Collect(nested); }
        Collect(root);
        List<string> declarations = [];
        foreach (var shape in needed.OrderBy(s => s.Type.QualifiedName, StringComparer.Ordinal))
        {
            if (shape.Type.SourceFile?.Converters.ManagedValueHasExistingFields(shape.Type.QualifiedName) == true) continue;
            StringBuilder code = new();
            string fullName=shape.DataManaged.Replace("global::", "");
            string name=Id(fullName[(fullName.LastIndexOf('.')+1)..]);
            code.AppendLine($"/// <summary>Field-by-field value snapshot; owns no native object.</summary>\npublic partial struct {name} {{");
            foreach (var member in shape.Members)
                for (int i = 0; i < Math.Max(1, member.ArrayCount); i++)
                {
                    foreach (string attribute in member.Field.Api.ManagedAttributes.Distinct(StringComparer.Ordinal)) code.AppendLine("    [" + attribute + "]");
                    code.AppendLine($"    {(member.Field.Api.ManagedAccess == "internal" ? "internal" : "public")} {(member.Scalar?.ObjectType is { } obj ? HandleNetCS.ObjectType(obj) : member.Scalar?.Converter?.Bodies?.ManagedType ?? member.Scalar?.ManagedValueType ?? member.Scalar?.Managed ?? member.Nested!.DataManaged)} {Id(member.Field.Name.text + (member.ArrayCount == 0 ? "" : i.ToString(System.Globalization.CultureInfo.InvariantCulture)))};");
                }
            code.AppendLine("}");
            declarations.Add(WrapValue(shape, code.ToString(), shape.DataManaged));
        }
        return declarations;
    }

    NativeTypeConverter Create(Shape shape, IReadOnlyList<string> declarations)
    {
        if (shape.Leaves.Any(l => l.Scalar.Converter != null)) return CreateComposed(shape, declarations);
        string native = "::" + shape.Type.NativeQualifiedName;
        bool shared = !shape.Snapshot && shape.Type is StructDescriptor structure && layout.Struct(structure)?.Storage.Alignment <= IntPtr.Size;
        var headers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Headers(Shape value) { if ((value.Type.ConcreteTemplate?.Definition.SourceFile ?? value.Type.SourceFile)?.Path is { } path) headers.Add(path); foreach (var nested in value.Members.Select(m => m.Nested).OfType<Shape>()) Headers(nested); }
        Headers(shape);
        foreach (var leaf in shape.Leaves)
            if (HandleNETcpp.Use(leaf.Scalar.ObjectType)?.Type.SourceFile?.Path is { } objectHeader) headers.Add(objectHeader);
        string ManagedWrite(Leaf leaf, string value) => leaf.Scalar.ObjectType == null ? $"({leaf.Scalar.Managed})({value})" : $"(ulong)(nuint){HandleNetCS.ToNative(value)}";
        string ManagedRead(Leaf leaf, string value) => leaf.Scalar.ObjectType != null ? HandleNetCS.FromNative(leaf.Scalar.ObjectType, $"(nint)(nuint)({value})") : leaf.Scalar.ManagedValueType is { } managed ? $"({managed})({value})" : value;
        string NativeWrite(Leaf leaf, string value) => leaf.Scalar.RawPointer ? $"reinterpret_cast<unsigned long long>({value})" : HandleNETcpp.Use(leaf.Scalar.ObjectType) is { } obj ? $"reinterpret_cast<unsigned long long>({HandleNETcpp.ToRoot(obj.Type, value)})" : $"static_cast<{leaf.Scalar.Cpp}>({value})";
        string NativeRead(Leaf leaf, string value, string field) => HandleNETcpp.Use(leaf.Scalar.ObjectType) is { } obj ? HandleNETcpp.ToNative(leaf.Scalar.ObjectType!, $"reinterpret_cast<{HandleNETcpp.RootName(obj.Type)}*>({value})") :
            leaf.Scalar.RawPointer ? $"reinterpret_cast<decltype({field})>({value})" : leaf.Path.Contains('[') ? $"static_cast<typename std::remove_reference<decltype({field})>::type>({value})" : $"static_cast<decltype({field})>({value})";
        string FieldReceiver(Leaf leaf, string value, FieldDescriptor field) =>
            value + (leaf.Path[..leaf.Path.LastIndexOf(Id(field.Name.text), StringComparison.Ordinal)] is { Length: > 0 } prefix ? "." + prefix.TrimEnd('.') : "");
        string ReadManaged(Leaf leaf, string value) => leaf.PrivateBuffer is { } field
            ? $"{HandleNetCS.ManagedName(field.Parent!)}.{BufferMethod(field, false)}({FieldReceiver(leaf, value, field)}, {leaf.BufferIndex})"
            : leaf.AccessField is { Api.ManagedAccess: "private" } hidden
            ? $"{HandleNetCS.ManagedName(hidden.Parent!)}.{FieldMethod(hidden, false)}({FieldReceiver(leaf, value, hidden)})"
            : $"({value}).{leaf.ManagedPath}";
        string WriteManaged(Leaf leaf, string value, string data) => leaf.PrivateBuffer is { } field
            ? $"{HandleNetCS.ManagedName(field.Parent!)}.{BufferMethod(field, true)}(ref {FieldReceiver(leaf, value, field)}, {leaf.BufferIndex}, {data});"
            : leaf.AccessField is { } restricted
            ? $"{HandleNetCS.ManagedName(restricted.Parent!)}.{FieldMethod(restricted, true)}(ref {FieldReceiver(leaf, value, restricted)}, {data});"
            : $"{value}.{leaf.ManagedPath} = {data};";
        return new()
        {
            NativeName = shape.Type.QualifiedName, NativeCppType = native, AbiCppType = "unsigned long long", NativeDefault = native + "{}", AutomaticFields = true,
            SharedReference = shared,
            ManagedDeclarations = declarations, DefinitionHeaders = headers.ToArray(), ToAbi = _ => throw new InvalidOperationException(), ToNative = _ => throw new InvalidOperationException(),
            Bodies = new()
            {
                ManagedType = shape.Managed, ManagedAbiType = "ulong", CopyReferenceReturns = !shared, CopyReferenceParameters = shape.Type is StructDescriptor && !shape.SelectedFieldsSnapshot,
                SelectedFieldsSnapshot = shape.SelectedFieldsSnapshot,
                AssignNativeValue = shape.SelectedFieldsSnapshot ? (destination, source) => "([&]() { " + AssignFields(shape, destination, source) + " }())" : null,
                CopiedCallbackValue = shape.Type is StructDescriptor && !shape.SelectedFieldsSnapshot && shape.Leaves.All(l => !l.Scalar.RawPointer),
                ReleaseManagedAbi = context => $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));",
                ReleaseNativeAbi = context => $"::CoTaskMemFree(reinterpret_cast<void*>({context.Value}));",
                ManagedSupport = ManagedHelpers, ManagedSupportOwner = shape.Managed, WrapManagedSupport = body => WrapValue(shape, "public partial struct " + shape.Managed.Replace("global::", "").Split('.').Last() + " {\n" + body + "\n}"), NativeSupport = NativeHelpers + ArrayAssertions(shape),
                NativeRequirements = shape.HasInlineArrays ? NativeSupportRequirements.Merge([HandleNETcpp.CoTaskMemorySupport, new() { Includes = ["<type_traits>"] }]) : HandleNETcpp.CoTaskMemorySupport,
                ManagedToAbi = context =>
                {
                    string buffer = context.Unique("__fields");
                    string input = shape.HasInlineArrays ? context.Unique("__inlineValue") : context.Value;
                    string setup = (shape.HasInlineArrays ? $"var {input} = {context.Value};\n" : "") + (shape.Leaves.Any(l => l.Scalar.ObjectType != null) ? context.RetainManaged?.Invoke(context.Value) ?? "" : "") + $"\n{buffer} = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem({shape.Size});\n" + string.Join("\n", shape.Leaves.Select(leaf => $"global::{context.Support}.Write<{leaf.Scalar.Managed}>({buffer}, {leaf.Offset}, {ManagedWrite(leaf, ReadManaged(leaf, input))});"));
                    return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;", setup, $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem({buffer});");
                },
                AbiToManaged = context =>
                {
                    string value = context.Unique("__fieldsValue");
                    return new(value, Setup: $"{shape.Managed} {value} = default;\n" + string.Join("\n", shape.Leaves.Select(leaf => WriteManaged(leaf, value, ManagedRead(leaf, $"global::{context.Support}.Read<{leaf.Scalar.Managed}>((nint)(nuint)({context.Value}), {leaf.Offset})")))),
                        Cleanup: $"global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)(nuint)({context.Value}));");
                },
                AbiToNative = context =>
                {
                    string value = context.Unique("fieldsValue");
                    return new(value, Setup: $"{native} {value}{{}};\n" + string.Join("\n", shape.Leaves.Select(leaf => $"{value}.{leaf.Path.Replace("@", "")} = {NativeRead(leaf, $"::{context.Support}::Read<{leaf.Scalar.Cpp}>({context.Value}, {leaf.Offset})", value + "." + leaf.Path.Replace("@", ""))};")));
                },
                NativeToAbi = context =>
                {
                    string buffer = context.Unique("fieldsBuffer");
                    return new(buffer, $"unsigned long long {buffer} = 0;", $"{buffer} = ::{context.Support}::Allocate({shape.Size});\n" + string.Join("\n", shape.Leaves.Select(leaf => $"::{context.Support}::Write<{leaf.Scalar.Cpp}>({buffer}, {leaf.Offset}, {NativeWrite(leaf, $"({context.Value}).{leaf.Path.Replace("@", "")}")});")),
                        $"if (!{context.Completed}) ::{context.Support}::Free({buffer});");
                }
            }
        };
    }

    NativeTypeConverter CreateComposed(Shape shape, IReadOnlyList<string> declarations)
    {
        string native = "::" + shape.Type.NativeQualifiedName;
        ConverterBodies Part(Leaf leaf)
        {
            if (leaf.Scalar.Converter?.Bodies is { } body) return body;
            var scalar = leaf.Scalar;
            string field = $"std::declval<{native}>().{leaf.Path.Replace("@", "")}";
            return new()
            {
                ManagedType = scalar.ObjectType == null ? scalar.Managed : HandleNetCS.ObjectType(scalar.ObjectType)!, ManagedAbiType = scalar.Managed,
                ManagedToAbi = c => new(scalar.ObjectType != null ? $"(ulong)(nuint){HandleNetCS.ToNative(c.Value)}" : scalar.ManagedValueType != null ? $"({scalar.Managed})({c.Value})" : c.Value),
                AbiToManaged = c => new(scalar.ObjectType != null ? HandleNetCS.FromNative(scalar.ObjectType, $"(nint)(nuint)({c.Value})") : scalar.ManagedValueType is { } managed ? $"({managed})({c.Value})" : c.Value),
                AbiToNative = c => new(scalar.RawPointer ? $"reinterpret_cast<decltype({field})>({c.Value})" : HandleNETcpp.Use(scalar.ObjectType) is { } obj ? HandleNETcpp.ToNative(scalar.ObjectType!, $"reinterpret_cast<{HandleNETcpp.RootName(obj.Type)}*>({c.Value})") : $"static_cast<decltype({field})>({c.Value})"),
                NativeToAbi = c => new(scalar.RawPointer ? $"reinterpret_cast<unsigned long long>({c.Value})" : HandleNETcpp.Use(scalar.ObjectType) is { } obj ? $"reinterpret_cast<unsigned long long>({HandleNETcpp.ToRoot(obj.Type, c.Value)})" : $"static_cast<{scalar.Cpp}>({c.Value})"),
                ReleaseManagedAbi = _ => "", ReleaseNativeAbi = _ => ""
            };
        }
        var parts = shape.Leaves.Select(Part).ToArray();
        string Support(int i, string root, bool cpp) => (cpp ? parts[i].NativeSupport : parts[i].ManagedSupport).Length == 0 ? "" : root + (cpp ? "::Part" : ".Part") + i;
        ConverterEmitContext Child(int i, ConverterEmitContext c, string value, bool cpp, string complete) => c with { Value = value, Support = Support(i, c.Support, cpp), Completed = complete };
        string Children(bool cpp) => string.Join("\n", parts.Select((p,i) => EmbedSupport(cpp ? p.NativeSupport : p.ManagedSupport, "Part" + i, cpp)));
        string Release(bool cpp)
        {
            List<string> lines = [];
            for(int i=0;i<parts.Length;++i)
            {
                var leaf = shape.Leaves[i];
                string value = cpp ? $"Read<{leaf.Scalar.Cpp}>(address, {leaf.Offset})" : $"Read<{leaf.Scalar.Managed}>(address, {leaf.Offset})";
                var c = new ConverterEmitContext(value, null, s=>s, "false", "false", Support(i,"{Support}",cpp));
                lines.Add((cpp ? parts[i].ReleaseNativeAbi : parts[i].ReleaseManagedAbi)?.Invoke(c) ?? "");
            }
            return string.Join("\n",lines);
        }
        string managedSupport = ManagedHelpers;
        // Insert nested helper bodies and aggregate release in the helper class.
        int managedClose = managedSupport.LastIndexOf('}');
        managedSupport = managedSupport[..managedClose] + Children(false) + "\ninternal static void Free(nint address) { if(address == 0) return; try { " + Release(false) + " } finally { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(address); } }\n}";
        string nativeSupport = "namespace {Support} {\n" + Children(true) + "\n" +
            "inline unsigned long long Allocate(int size) { void* value = ::CoTaskMemAlloc(size); if(!value) throw std::bad_alloc(); std::memset(value,0,size); return reinterpret_cast<unsigned long long>(value); }\n" +
            "template<typename T> T Read(unsigned long long address, int offset) { if(!address) throw std::invalid_argument(\"Null field buffer\"); T value; std::memcpy(&value,reinterpret_cast<const unsigned char*>(address)+offset,sizeof(T)); return value; }\n" +
            "template<typename T> void Write(unsigned long long address, int offset, T value) { std::memcpy(reinterpret_cast<unsigned char*>(address)+offset,&value,sizeof(T)); }\n" +
            "inline void Free(unsigned long long address) { if(!address) return; " + Release(true) + " ::CoTaskMemFree(reinterpret_cast<void*>(address)); }\n}" + ArrayAssertions(shape);
        var bodies = new ConverterBodies
        {
            ManagedType = shape.Managed, ManagedAbiType = "ulong", CopyReferenceReturns = true, CopyReferenceParameters = shape.Type is StructDescriptor && !shape.SelectedFieldsSnapshot,
                SelectedFieldsSnapshot = shape.SelectedFieldsSnapshot,
                AssignNativeValue = shape.SelectedFieldsSnapshot ? (destination, source) => "([&]() { " + AssignFields(shape, destination, source) + " }())" : null,
            ManagedSupportOwner = shape.Managed, WrapManagedSupport = body => WrapValue(shape, "public partial struct " + shape.Managed.Replace("global::", "").Split('.').Last() + " {\n" + body + "\n}"), ManagedSupport = managedSupport, NativeSupport = nativeSupport,
            NativeRequirements = NativeSupportRequirements.Merge(parts.Select(p=>p.NativeRequirements).Append(HandleNETcpp.CoTaskMemorySupport).Append(new() { Includes = ["<utility>"] })),
            ReleaseManagedAbi = c => $"global::{c.Support}.Free((nint)(nuint)({c.Value}));",
            ReleaseNativeAbi = c => $"::{c.Support}::Free({c.Value});",
            ManagedToAbi = c =>
            {
                string buffer=c.Unique("__record"), setup=(c.RetainManaged?.Invoke(c.Value)??"")+ $"\n{buffer} = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem({shape.Size});\nnew global::System.Span<byte>((void*){buffer}, {shape.Size}).Clear();\n";
                for(int i=0;i<parts.Length;++i)
                {
                    var leaf=shape.Leaves[i]; string done=c.Unique("__fieldStored");
                    var child=parts[i].ManagedToAbi(Child(i,c,$"({c.Value}).{leaf.ManagedPath}",false,done));
                    setup += child.Declarations + $"\nbool {done}=false; try {{\n" + child.Setup + $"\nglobal::{c.Support}.Write<{leaf.Scalar.Managed}>({buffer}, {leaf.Offset}, {child.Expression}); {done}=true;\n}} finally {{ if(!{done}) {{ {child.Cleanup} }} }}\n";
                }
                return new($"(ulong)(nuint){buffer}",$"nint {buffer}=0;",setup,$"global::{c.Support}.Free({buffer});");
            },
            AbiToManaged = c =>
            {
                string result=c.Unique("__recordValue"), setup=$"{shape.Managed} {result}=default;\n";
                for(int i=0;i<parts.Length;++i)
                {
                    var leaf=shape.Leaves[i]; var child=parts[i].AbiToManaged(Child(i,c,$"global::{c.Support}.Read<{leaf.Scalar.Managed}>((nint)(nuint)({c.Value}), {leaf.Offset})",false,"false"));
                    setup += child.Declarations+"\n"+child.Setup+$"\n{result}.{leaf.ManagedPath}={child.Expression};\n";
                }
                return new(result,Setup:setup,Cleanup:$"global::{c.Support}.Free((nint)(nuint)({c.Value}));");
            },
            AbiToNative = c =>
            {
                string result=c.Unique("recordValue"), setup=$"{native} {result}{{}};\n";
                for(int i=0;i<parts.Length;++i)
                {
                    var leaf=shape.Leaves[i]; var child=parts[i].AbiToNative(Child(i,c,$"::{c.Support}::Read<{leaf.Scalar.Cpp}>({c.Value}, {leaf.Offset})",true,"false"));
                    setup += "{\n"+child.Declarations+"\n"+child.Setup+$"\n{result}.{leaf.Path.Replace("@", "")}={child.Expression};\n}}\n";
                }
                return new(result,Setup:setup);
            },
            NativeToAbi = c =>
            {
                string buffer=c.Unique("recordBuffer"), setup=$"{buffer}=::{c.Support}::Allocate({shape.Size});\n";
                for(int i=0;i<parts.Length;++i)
                {
                    var leaf=shape.Leaves[i]; string done=c.Unique("fieldStored");
                    var child=parts[i].NativeToAbi(Child(i,c,$"({c.Value}).{leaf.Path.Replace("@", "")}",true,done));
                    setup += $"{{ bool {done}=false;\n"+child.Declarations+$"\nauto {c.Unique("fieldCleanup")} = VETConverterFinally([&]() {{ if(!{done}) {{ {child.Cleanup} }} }});\n"+child.Setup+$"\n::{c.Support}::Write<{leaf.Scalar.Cpp}>({buffer}, {leaf.Offset}, {child.Expression}); {done}=true; }}\n";
                }
                return new(buffer,$"unsigned long long {buffer}=0;",setup,$"if(!{c.Completed}) ::{c.Support}::Free({buffer});");
            }
        };
        var headers=shape.Leaves.SelectMany(l=>l.Scalar.Converter?.DefinitionHeaders??[]).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if((shape.Type.ConcreteTemplate?.Definition.SourceFile??shape.Type.SourceFile)?.Path is { } path) headers.Add(path);
        foreach(var leaf in shape.Leaves) if(HandleNETcpp.Use(leaf.Scalar.ObjectType)?.Type.SourceFile?.Path is { } objPath) headers.Add(objPath);
        return new() { NativeName=shape.Type.QualifiedName,NativeCppType=native,AbiCppType="unsigned long long",NativeDefault=native+"{}",AutomaticFields=true,
            Bodies=bodies,ManagedDeclarations=declarations.Concat(shape.Leaves.SelectMany(l=>l.Scalar.Converter?.ManagedDeclarations??[])).Distinct().ToArray(),DefinitionHeaders=headers.ToArray(),
            ToAbi=_=>throw new InvalidOperationException(),ToNative=_=>throw new InvalidOperationException() };
    }

    internal static string BufferMethod(FieldDescriptor field, bool write) => "__VET" + (write ? "Write" : "Read") + "Buffer_" + field.Name.text;
    internal static string FieldMethod(FieldDescriptor field, bool write) => "__VET" + (write ? "Write" : "Read") + "Field_" + field.Name.text;

    internal static string EmbedSupport(string source, string name, bool cpp) =>
        source.Replace(cpp ? "namespace {Support}" : "class {Support}", (cpp ? "namespace " : "class ") + name)
            .Replace("{Support}", "{Support}" + (cpp ? "::" : ".") + name);

    void Attach(TypeInfo type)
    {
        if (type is NamedTypeInfo named)
        {
            foreach (var argument in named.Arguments.Select(a => a.Type).OfType<TypeInfo>()) Attach(argument);
            if (named.Converter == null && Definition(named) is { } definition && converters.TryGetValue(definition, out var converter))
            {
                named.Converter = converter;
                attached++;
            }
        }
        else if (type is ReferenceTypeInfo reference) Attach(reference.Referent);
        else if (type is PointerTypeInfo pointer) Attach(pointer.Pointee);
        else if (type is ArrayTypeInfo array) Attach(array.ElementType);
    }

    public static IEnumerable<NativeTypeConverter> Converters(TypeInfo type)
    {
        if (type is NamedTypeInfo { Converter: { } converter }) yield return converter;
        if (type is ReferenceTypeInfo reference) foreach (var c in Converters(reference.Referent)) yield return c;
        if (type is PointerTypeInfo pointer) foreach (var c in Converters(pointer.Pointee)) yield return c;
    }

    const string ManagedHelpers = """
        internal static unsafe class {Support}
        {
            internal static void Write<T>(nint address, int offset, T value) where T : unmanaged
            { if (address == 0) throw new global::System.InvalidOperationException("Null field buffer."); global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)(address + offset), value); }
            internal static T Read<T>(nint address, int offset) where T : unmanaged
            { if (address == 0) throw new global::System.InvalidOperationException("Null field buffer."); return global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<T>((void*)(address + offset)); }
        }
        """;
    const string NativeHelpers = """
        namespace {Support} {
        inline unsigned long long Allocate(int size) { void* value = ::CoTaskMemAlloc(size); if (!value) throw std::bad_alloc(); std::memset(value, 0, size); return reinterpret_cast<unsigned long long>(value); }
        inline void Free(unsigned long long address) { ::CoTaskMemFree(reinterpret_cast<void*>(address)); }
        template<typename T> T Read(unsigned long long address, int offset) { if (!address) throw std::invalid_argument("Null field buffer."); T value; std::memcpy(&value, reinterpret_cast<const unsigned char*>(address) + offset, sizeof(T)); return value; }
        template<typename T> void Write(unsigned long long address, int offset, T value) { std::memcpy(reinterpret_cast<unsigned char*>(address) + offset, &value, sizeof(T)); }
        }
        """;
}
