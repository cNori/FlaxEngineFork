using System.Security.Cryptography;
using System.Text;
using VETools.Text;
using TypeInfo = VETools.Text.TypeInfo;
using Scalar = VETools.Text.ScalarAbi;

namespace VETools.Generation;

// Enum and explicitly enabled Flax collection ABIs. Containers compose existing
// element converters and carry their ownership/release hooks, not object layouts.
internal sealed class StandardConverters
{
    internal static NativeTypeConverter? ValueConverter(TypeInfo type)
    {
        if (type is NamedTypeInfo { Converter.Bodies.BorrowedObjectElements: true }) return null;
        if (type is NamedTypeInfo { Converter: { Bodies: not null } converter }) return converter;
        var element = new StandardConverters().MakeElement(type);
        if (element == null) return null;
        var body = element.Bodies;
        // Primitive/pointer elements own no wire allocation. Explicit converters
        // retain their own release contract rather than inventing one.
        return new() { NativeName = type.Display, NativeCppType = TemplateInstantiationPass.Native(type), AbiCppType = element.WireCpp,
            NativeDefault = TemplateInstantiationPass.Native(type) + "{}", Bodies = body,
            ToAbi = _ => throw new InvalidOperationException(), ToNative = _ => throw new InvalidOperationException(), DefinitionHeaders = element.Headers };
    }
    sealed record Element(string Managed, string WireCS, string WireCpp, int Size, ConverterBodies Bodies,
        IReadOnlyList<string> Declarations, IReadOnlyList<string> Headers);
    readonly Dictionary<EnumDescriptor, NativeTypeConverter> enums = [];
    readonly Dictionary<string, NativeTypeConverter> collections = new(StringComparer.Ordinal);
    readonly HashSet<string> active = new(StringComparer.Ordinal);

    public static void Apply(FileDescriptor root)
    {
        if (root.TypeInfos!.StandardConvertersReady) return;
        var files = root.ProjectFiles.Count == 0 ? new[] { root } : root.ProjectFiles.ToArray();
        var pass = new StandardConverters();
        var declarations = files.SelectMany(TemplateInstantiationPass.Declarations)
            .Concat(root.TypeInfos.TemplateInstances.Values.SelectMany(TemplateInstantiationPass.Declarations)).ToArray();
        foreach (var declaration in declarations) foreach (var type in TemplateInstantiationPass.Types(declaration)) pass.Visit(type, false, declaration);
        foreach (var declaration in declarations) foreach (var type in TemplateInstantiationPass.Types(declaration)) pass.Visit(type, declaration.SourceFile?.Converters.FlaxContainers == true, declaration);
        foreach (var file in files) file.TypeInfos!.StandardConvertersReady = true;
    }

    void Visit(TypeInfo type, bool containers, DeclarationDescriptor context)
    {
        if (type is ReferenceTypeInfo r) { Visit(r.Referent, containers, context); return; }
        if (type is PointerTypeInfo p) { Visit(p.Pointee, containers, context); return; }
        if (type is not NamedTypeInfo named) return;
        foreach (var argument in named.Arguments.Select(a => a.Type).OfType<TypeInfo>()) Visit(argument, containers, context);
        if (named.Converter != null)
        {
            if (named.Converter.Bodies != null) ScalarAbi.Require(named.Converter);
            return;
        }
        if (FlaxAssetReferenceConverter.Create(named) is { } assetReference)
        {
            named.Converter = assetReference;
            return;
        }
        if (FlaxFunctionConverter.Create(named, context) is { } function)
        {
            named.Converter = function;
            return;
        }
        if (named.Resolution == TypeResolution.Resolved && named.Symbol?.Definition is EnumDescriptor enumeration)
        {
            if (!ExportAttributes.Read(enumeration).Export) return;
            if (!enums.TryGetValue(enumeration, out var converter))
            {
                var scalar = enumeration.UnderlyingType == null ? enumeration.IsScoped ? new Scalar("int", "int", 4) : null : Primitive(enumeration.UnderlyingType);
                if (scalar == null || scalar.CS is "bool" or "char") return;
                string native = "::" + enumeration.QualifiedName, managed = HandleNetCS.ManagedName(enumeration);
                converter = new() { NativeName = enumeration.QualifiedName, NativeCppType = native, AbiCppType = scalar.Cpp, EnumValue = true, SharedReference = true,
                    NativeDefault = native + "{}", DefinitionHeaders = enumeration.SourceFile?.Path is { } path ? [path] : [],
                    ToAbi = value => $"static_cast<{scalar.Cpp}>({value})", ToNative = value => $"static_cast<{native}>({value})",
                    Bodies = new() { ManagedType = managed, ManagedAbiType = scalar.CS,
                        ManagedToAbi = c => new($"({scalar.CS})({c.Value})"), AbiToManaged = c => new($"({managed})({c.Value})"),
                        AbiToNative = c => new($"static_cast<{native}>({c.Value})"), NativeToAbi = c => new($"static_cast<{scalar.Cpp}>({c.Value})") } };
                enums.Add(enumeration, converter);
            }
            named.Converter = converter; return;
        }
        if (!containers) return;
        string name = named.Symbol?.QualifiedName ?? named.Name.TrimStart(':');
        if (name == "BitArray")
        {
            if(named.Arguments.Count > 1 || named.Arguments.Count == 1 && named.Arguments[0].Type is not NamedTypeInfo { Name: "HeapAllocation" or "::HeapAllocation" }) return;
            string bitNative = TemplateInstantiationPass.Native(named);
            while(bitNative.EndsWith(" const",StringComparison.Ordinal)||bitNative.EndsWith(" volatile",StringComparison.Ordinal)) bitNative=bitNative[..bitNative.LastIndexOf(' ')];
            if(named.Arguments.Count == 0) bitNative += "<>";
            named.Converter=CreateCollection(named,bitNative,[MakeElement(new NamedTypeInfo { Name="bool",Resolution=TypeResolution.Builtin })!],false,false,false,null,true);
            return;
        }
        if (named.Arguments.Count == 0) return;
        bool dictionary = name == "Dictionary", span = name == "Span", data = name == "DataContainer";
        int arity = dictionary ? 2 : 1;
        if (name is not ("Array" or "Span" or "Dictionary" or "DataContainer") || named.Arguments.Count < arity || named.Arguments.Count > arity + (span || data ? 0 : 1)) return;
        string? capacity = null;
        if (named.Arguments.Count > arity)
        {
            if (named.Arguments[arity].Type is not NamedTypeInfo allocator) return;
            string allocation = allocator.Symbol?.QualifiedName ?? allocator.Name.TrimStart(':');
            if (name == "Array" && allocation == "FixedAllocation" && allocator.Arguments.Count == 1)
                capacity = allocator.Arguments[0].Display;
            else if (name == "Array" && allocation == "InlinedAllocation" && allocator.Arguments.Count == 1) { }
            else if (allocation != "HeapAllocation" && !(name == "Array" && allocator.Arguments.Count == 0 &&
                context.SourceFile?.Converters.ContainerAllocators.Contains(allocation.TrimStart(':')) == true)) return;
        }
        if (named.Arguments.Take(arity).Any(a => a.Type == null)) return;
        // DataContainer supports only values requiring no construction/destruction.
        // Restrict automatic conversion to known primitive/enum values.
        if (data && Primitive(named.Arguments[0].Type!) == null && named.Arguments[0].Type is not NamedTypeInfo { Converter.EnumValue: true }) return;
        string key = TemplateInstantiationPass.Native(named);
        // Converter storage is the unqualified value; the use site retains its cv/ref qualifiers.
        while (key.EndsWith(" const", StringComparison.Ordinal) || key.EndsWith(" volatile", StringComparison.Ordinal))
            key = key[..key.LastIndexOf(' ')];
        if (collections.TryGetValue(key, out var cached)) { named.Converter = cached; return; }
        if (active.Count >= 16 || !active.Add(key)) return;
        try
        {
            bool inlineObjects = name is "Array" or "Span" && named.Arguments[0].Type is NamedTypeInfo { Symbol.Definition: ClassDescriptor elementClass } && HandleNETcpp.IsScriptingObject(elementClass);
            var elements = named.Arguments.Take(arity).Select(a => MakeElement(a.Type!, allowConst: span, allowInlineObjects: inlineObjects)).ToArray();
            if (elements.Any(e => e == null)) return;
            if (dictionary && named.Arguments[0].Type is NamedTypeInfo { Converter.AutomaticFields: true } keyType && !(keyType.Specialization?.Concrete?.Name.text ?? keyType.Symbol?.Definition?.Name.text ?? keyType.Name).EndsWith("Guid", StringComparison.Ordinal)) return;
            var elementArray = elements.OfType<Element>().ToArray();
            var created = CreateCollection(named, key, elementArray, dictionary, span, data, capacity, borrowedObjects: inlineObjects);
            collections.Add(key, created); named.Converter = created;
        }
        finally { active.Remove(key); }
    }

    Element? MakeElement(TypeInfo type, bool allowConst = false, bool allowInlineObjects = false)
    {
        if (type is NamedTypeInfo { Converter.Bodies.BorrowedObjectElements: true }) return null;
        if (allowInlineObjects && type is NamedTypeInfo { Symbol.Definition: ClassDescriptor cls } && HandleNETcpp.IsScriptingObject(cls))
        {
            if (!HandleNETcpp.CanCast(cls) || type is NamedTypeInfo { EffectiveQualifiers: not TypeQualifiers.None }) return null;
            var pointer = new PointerTypeInfo { Pointee = type };
            return new(HandleNetCS.ManagedName(cls), "ulong", "unsigned long long", 8, new() {
                ManagedType = HandleNetCS.ManagedName(cls), ManagedAbiType = "ulong",
                ManagedToAbi = _ => throw new NotSupportedException("Inline engine object collections cannot be copied into native storage."),
                AbiToNative = _ => throw new NotSupportedException("Inline engine objects cannot be constructed/copied by the glue."),
                AbiToManaged = c => new(HandleNetCS.FromNative(pointer, $"(nint)(nuint)({c.Value})")),
                NativeToAbi = c => new($"reinterpret_cast<unsigned long long>({HandleNETcpp.ToRoot(cls, "std::addressof(" + c.Value + ")")})")
            }, [], cls.SourceFile?.Path is { } path ? [path] : []);
        }
        if(type is NamedTypeInfo { Converter.FunctionValue:true }) return null;
        if (type is NamedTypeInfo cv && cv.EffectiveQualifiers != TypeQualifiers.None)
        {
            if (!allowConst || cv.EffectiveQualifiers != TypeQualifiers.Const || cv.Converter?.NativeQualifiers is not (null or TypeQualifiers.None)) return null;
            // Only the copied backing element is mutable. The Span<T const>
            // signature keeps its const qualification and cannot write to it.
            type = new NamedTypeInfo { Name = cv.Name, Resolution = cv.Resolution, Symbol = cv.Symbol, Converter = cv.Converter,
                Specialization = cv.Specialization, Alias = cv.Alias, Arguments = new(cv.Arguments) };
        }
        if (type is PointerTypeInfo pointerCV && pointerCV.Qualifiers != TypeQualifiers.None) return null;
        if(type is NamedTypeInfo { Name:"bool", Resolution:TypeResolution.Builtin })
            return new("bool","byte","unsigned char",1,new() {
                ManagedType="bool",ManagedAbiType="byte",ManagedToAbi=c=>new($"(byte)({c.Value} ? 1 : 0)"),
                AbiToManaged=c=>new($"{c.Value} != 0",Setup:$"if ({c.Value} > 1) throw new global::System.InvalidOperationException(\"Invalid boolean element\");"),
                AbiToNative=c=>new($"{c.Value} != 0",Setup:$"if ({c.Value} > 1) throw std::invalid_argument(\"Invalid boolean element\");"),
                NativeToAbi=c=>new($"static_cast<unsigned char>({c.Value} ? 1 : 0)"),ReleaseManagedAbi=_=>"",ReleaseNativeAbi=_=>""
            },[],[]);
        if (HandleNETcpp.SupportedUse(type) && HandleNETcpp.Use(type) is { Pointer: true, Slot: false } script)
        {
            if (!HandleNETcpp.CanCast(script.Type)) return null;
            return new(HandleNetCS.ObjectType(type)!, "ulong", "unsigned long long", 8, new() {
                ManagedType = HandleNetCS.ObjectType(type)!, ManagedAbiType = "ulong",
                ManagedToAbi = c => new($"(ulong)(nuint){HandleNetCS.ToNative(c.Value)}"), AbiToManaged = c => new(HandleNetCS.FromNative(type, $"(nint)(nuint)({c.Value})")),
                AbiToNative = c => new(HandleNETcpp.ToNative(type, $"reinterpret_cast<{HandleNETcpp.RootName(script.Type)}*>({c.Value})")),
                NativeToAbi = c => new($"reinterpret_cast<unsigned long long>({HandleNETcpp.ToRoot(script.Type, c.Value)})") }, [], script.Type.SourceFile?.Path is { } path ? [path] : []);
        }
        if (type is PointerTypeInfo)
            return new("nint", "ulong", "unsigned long long", 8, new() { ManagedType = "nint", ManagedAbiType = "ulong",
                ManagedToAbi = c => new($"(ulong)(nuint)({c.Value})"), AbiToManaged = c => new($"(nint)(nuint)({c.Value})"),
                AbiToNative = c => new($"reinterpret_cast<{TemplateInstantiationPass.Native(type)}>({c.Value})"), NativeToAbi = c => new($"reinterpret_cast<unsigned long long>({c.Value})") }, [], []);
        if (type is not NamedTypeInfo named) return null;
        // A nested Span would retain backing storage owned by a per-element temporary.
        // Only top-level Span arguments/results have a supported call-scoped lifetime.
        if ((named.Symbol?.QualifiedName ?? named.Name.TrimStart(':')) == "Span") return null;
        if (named.Converter is { Bodies: { } body } converter)
        {
            if (!converter.EnumValue && (body.ReleaseManagedAbi == null || body.ReleaseNativeAbi == null)) return null;
            var wire = ScalarAbi.Require(converter);
            var headers = converter.DefinitionHeaders.Concat(named.Symbol?.Definition is TypeDescriptor { IsForwardDeclaration: false } definition &&
                definition.SourceFile?.Path is { } definitionPath ? new[] { definitionPath } : Array.Empty<string>()).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            return new(body.ManagedType, wire.CS, wire.Cpp, wire.Size, body, converter.ManagedDeclarations, headers);
        }
        var scalar = Primitive(type); if (scalar == null) return null;
        var simple = named.Converter;
        return new(scalar.CS, scalar.CS, scalar.Cpp, scalar.Size, new() { ManagedType = scalar.CS, ManagedAbiType = scalar.CS,
            ManagedToAbi = c => new(c.Value), AbiToManaged = c => new(c.Value),
            AbiToNative = c => new(simple?.ToNative(c.Value) ?? $"static_cast<{TemplateInstantiationPass.Native(type)}>({c.Value})"),
            NativeToAbi = c => new(simple?.ToAbi(c.Value) ?? c.Value) }, [], []);
    }

    NativeTypeConverter CreateCollection(NamedTypeInfo named, string native, Element[] elements, bool dictionary, bool span, bool data, string? capacity, bool bits = false, bool borrowedObjects = false)
    {
        int stride = elements.Sum(e => e.Size);
        string managed = dictionary ? $"global::System.Collections.Generic.Dictionary<{elements[0].Managed}, {elements[1].Managed}>" : elements[0].Managed + "[]";
        string[] managedNames = elements.Select((e, i) => "Part" + i + "_" + Hash(e.Bodies.ManagedSupport)).ToArray();
        string[] nativeNames = elements.Select((e, i) => "Part" + i + "_" + Hash(e.Bodies.NativeSupport)).ToArray();
        string MS(int i, string root) => elements[i].Bodies.ManagedSupport.Length == 0 ? "" : root + "." + managedNames[i];
        string NS(int i, string root) => elements[i].Bodies.NativeSupport.Length == 0 ? "" : root + "::" + nativeNames[i];
        string ChildSupport(bool cpp) => string.Join("\n", elements.Select((e, i) => AutomaticFieldConverters.EmbedSupport(cpp ? e.Bodies.NativeSupport : e.Bodies.ManagedSupport, cpp ? nativeNames[i] : managedNames[i], cpp)));
        int Offset(int i) => elements.Take(i).Sum(e => e.Size);
        ConverterEmitContext Child(ConverterEmitContext parent, int i, string value, bool cpp, string complete) => new(value, parent.Direction, parent.Unique, complete, parent.CallSucceeded, cpp ? NS(i, parent.Support) : MS(i, parent.Support), parent.RetainManaged);
        string ReleaseManaged(string root, string address)
        {
            List<string> lines = [];
            for (int i = 0; i < elements.Length; i++) if (elements[i].Bodies.ReleaseManagedAbi is { } release)
                lines.Add(release(new($"Read<{elements[i].WireCS}>(address, 8 + index * {stride} + {Offset(i)})", null, s => s, "false", "false", MS(i, root))));
            return string.Join("\n", lines);
        }
        string ReleaseNative(string root)
        {
            List<string> lines = [];
            for (int i = 0; i < elements.Length; i++) if (elements[i].Bodies.ReleaseNativeAbi is { } release)
                lines.Add(release(new($"Read<{elements[i].WireCpp}>(address, 8 + index * {stride} + {Offset(i)})", null, s => s, "false", "false", NS(i, root))));
            return string.Join("\n", lines);
        }
        string managedSupport = ManagedCommon.Replace("{Stride}", stride.ToString()).Replace("{Release}", ReleaseManaged("{Support}", "address")).Replace("{Children}", ChildSupport(false));
        string nativeSupport = NativeCommon.Replace("{Stride}", stride.ToString()).Replace("{Release}", ReleaseNative("{Support}")).Replace("{Children}", ChildSupport(true));
        var bodies = new ConverterBodies {
            ManagedType = managed, ManagedAbiType = "ulong", CopyReferenceReturns = true, BorrowedObjectElements = borrowedObjects, BorrowedObjectViewReturn = borrowedObjects && span, CopyReferenceParameters = !span && !borrowedObjects,
            ManagedOptionalDefault = expression => expression == "null" ? "null" : null,
            ManagedSupport = managedSupport, NativeSupport = nativeSupport,
            NativeRequirements = NativeSupportRequirements.Merge(elements.Select(e => e.Bodies.NativeRequirements).Prepend(HandleNETcpp.CoTaskMemorySupport).Append(new() { Includes = span ? ["<type_traits>"] : [] })),
            ReleaseManagedAbi = c => $"global::{c.Support}.Free((nint)(nuint)({c.Value}));",
            ReleaseNativeAbi = c => $"::{c.Support}::Free({c.Value});",
            ManagedToAbi = context =>
            {
                string buffer = context.Unique("__arrayBuffer"), source = context.Unique("__arraySource"), index = context.Unique("__arrayIndex");
                string empty = dictionary ? $"new {managed}()" : $"global::System.Array.Empty<{elements[0].Managed}>()";
                StringBuilder setup = new($"var {source} = {context.Value} ?? {empty};\n{buffer} = global::{context.Support}.Allocate({source}.{(dictionary ? "Count" : "Length")});\nint {index} = 0;\n");
                string item = context.Unique("__arrayItem");
                setup.AppendLine($"foreach (var {item} in {source}) {{");
                for (int i = 0; i < elements.Length; ++i)
                {
                    string complete = context.Unique("__elementStored");
                    string value = dictionary ? item + (i == 0 ? ".Key" : ".Value") : item;
                    var child = elements[i].Bodies.ManagedToAbi(Child(context, i, value, false, complete));
                    setup.AppendLine(child.Declarations).AppendLine($"bool {complete} = false; try {{").AppendLine(child.Setup)
                        .AppendLine($"global::{context.Support}.Write<{elements[i].WireCS}>({buffer}, 8 + {index} * {stride} + {Offset(i)}, {child.Expression});\n{complete} = true;")
                        .AppendLine($"}} finally {{ if (!{complete}) {{ {child.Cleanup} }} }}");
                }
                setup.AppendLine($"++{index}; }}");
                return new($"(ulong)(nuint){buffer}", $"nint {buffer} = 0;", setup.ToString(), $"try {{ global::{context.Support}.Free({buffer}); }} finally {{ global::System.GC.KeepAlive({context.Value}); }}");
            },
            AbiToManaged = context =>
            {
                string address = context.Unique("__arrayAddress"), count = context.Unique("__arrayCount"), result = context.Unique("__arrayResult"), index = context.Unique("__arrayIndex");
                StringBuilder setup = new($"nint {address} = (nint)(nuint)({context.Value});\nint {count} = global::{context.Support}.Count({address});\n");
                string elementName = elements[0].Managed;
                int arrayStart = elementName.Length;
                while (arrayStart >= 2 && elementName[(arrayStart - 2)..arrayStart] == "[]") arrayStart -= 2;
                string arrayCreation = $"new {elementName[..arrayStart]}[{count}]{elementName[arrayStart..]}";
                setup.AppendLine(dictionary ? $"var {result} = new {managed}({count});" : $"var {result} = {arrayCreation};");
                setup.AppendLine($"for (int {index} = 0; {index} < {count}; ++{index}) {{");
                List<string> values = [];
                for (int i = 0; i < elements.Length; ++i)
                {
                    string complete = context.Unique("__elementRead");
                    var child = elements[i].Bodies.AbiToManaged(Child(context, i, $"global::{context.Support}.Read<{elements[i].WireCS}>({address}, 8 + {index} * {stride} + {Offset(i)})", false, complete));
                    string value = context.Unique("__element");
                    setup.AppendLine($"bool {complete} = false; {child.Declarations}").AppendLine(child.Setup).AppendLine($"var {value} = {child.Expression}; {complete} = true; _ = {complete};");
                    values.Add(value);
                }
                setup.AppendLine(dictionary ? $"{result}.Add({values[0]}, {values[1]});" : $"{result}[{index}] = {values[0]};").AppendLine("}");
                return new(result, Setup: setup.ToString(), Cleanup: $"global::{context.Support}.Free((nint)(nuint)({context.Value}));");
            },
            AbiToNative = context =>
            {
                string count = context.Unique("count"), result = context.Unique("arrayValue"), index = context.Unique("index"), backing = context.Unique("spanStorage");
                string nativeElement = bits ? "bool" : TemplateInstantiationPass.Native(named.Arguments[0].Type!);
                if (span) nativeElement = $"typename std::remove_const<{nativeElement}>::type";
                StringBuilder setup = new($"int {count} = ::{context.Support}::Count({context.Value});\n");
                if (span) setup.AppendLine($"\n#if !defined(__cpp_aligned_new) || __cpp_aligned_new < 201606L\nstatic_assert(alignof({nativeElement}) <= alignof(std::max_align_t), \"VET over-aligned Span storage requires aligned new or an explicit allocator\");\n#endif\nstd::unique_ptr<{nativeElement}[]> {backing}({count} ? new {nativeElement}[{count}]{{}} : nullptr);");
                else
                {
                    setup.AppendLine($"{native} {result};");
                    if (capacity != null) setup.AppendLine($"if ({count} > static_cast<int>({capacity})) throw std::invalid_argument(\"FixedAllocation input exceeds native capacity\");");
                    if (data) setup.AppendLine($"{result}.Allocate({count});");
                    else if (!dictionary) setup.AppendLine($"{result}.Resize({count}, false);");
                }
                setup.AppendLine($"for (int {index} = 0; {index} < {count}; ++{index}) {{");
                List<string> values = [];
                for (int i = 0; i < elements.Length; ++i)
                {
                    string complete = context.Unique("elementRead");
                    var child = elements[i].Bodies.AbiToNative(Child(context, i, $"::{context.Support}::Read<{elements[i].WireCpp}>({context.Value}, 8 + {index} * {stride} + {Offset(i)})", true, complete));
                    string value = context.Unique("element");
                    setup.AppendLine($"bool {complete} = false; {child.Declarations}").AppendLine(child.Setup).AppendLine($"auto {value} = {child.Expression}; {complete} = true; (void){complete};");
                    values.Add(value);
                }
                setup.AppendLine(dictionary ? $"{result}.Add({values[0]}, {values[1]});" : bits ? $"{result}.Set({index}, {values[0]});" : $"{(span ? backing : result)}[{index}] = {values[0]};").AppendLine("}");
                return new(span ? $"{native}({backing}.get(), {count})" : result, Setup: setup.ToString());
            },
            NativeToAbi = context =>
            {
                string buffer = context.Unique("arrayBuffer"), index = context.Unique("index");
                StringBuilder setup = new($"{buffer} = ::{context.Support}::Allocate(({context.Value}).{(span || data ? "Length" : "Count")}());\nint {index} = 0;\n");
                string item = context.Unique("item");
                if (dictionary) setup.AppendLine($"for (auto {item} = ({context.Value}).Begin(); {item} != ({context.Value}).End(); ++{item}) {{");
                else setup.AppendLine($"for (; {index} < ({context.Value}).{(span || data ? "Length" : "Count")}(); ++{index}) {{");
                for (int i = 0; i < elements.Length; ++i)
                {
                    string value = dictionary ? item + (i == 0 ? "->Key" : "->Value") : $"({context.Value})[{index}]";
                    string complete = context.Unique("elementStored");
                    var child = elements[i].Bodies.NativeToAbi(Child(context, i, value, true, complete));
                    setup.AppendLine($"bool {complete} = false; {child.Declarations}")
                        .AppendLine($"auto {context.Unique("elementCleanup")} = VETConverterFinally([&]() {{ if (!{complete}) {{ {child.Cleanup} }} }});")
                        .AppendLine(child.Setup).AppendLine($"::{context.Support}::Write<{elements[i].WireCpp}>({buffer}, 8 + {index} * {stride} + {Offset(i)}, {child.Expression});\n{complete} = true;");
                }
                if (dictionary) setup.AppendLine($"++{index};");
                setup.AppendLine("}");
                return new(buffer, $"unsigned long long {buffer} = 0;", setup.ToString(), $"if (!{context.Completed}) ::{context.Support}::Free({buffer});");
            }
        };
        var headers = elements.SelectMany(e => e.Headers).Concat(named.Symbol?.Definition?.SourceFile?.Path is { } path ? [path] : []).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        return new() { NativeName = native, NativeCppType = native, AbiCppType = "unsigned long long", NativeDefault = native + "{}", Bodies = bodies,
            ToAbi = _ => throw new InvalidOperationException(), ToNative = _ => throw new InvalidOperationException(),
            ManagedDeclarations = elements.SelectMany(e => e.Declarations).Distinct(StringComparer.Ordinal).ToArray(), DefinitionHeaders = headers };
    }

    static string Hash(string text) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)))[..12];
    internal static Scalar? Primitive(TypeInfo type) => ScalarAbi.Resolve(type);

    const string ManagedCommon = """
        internal static unsafe class {Support} {
        internal static int Count(nint address) { if (address == 0) return 0; int count = global::System.Runtime.InteropServices.Marshal.ReadInt32(address); if (count < 0 || count > (int.MaxValue - 8) / {Stride}) throw new global::System.InvalidOperationException("Invalid collection count."); return count; }
        internal static nint Allocate(int count) { int size = checked(8 + count * {Stride}); nint address = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(size); new global::System.Span<byte>((void*)address, size).Clear(); global::System.Runtime.InteropServices.Marshal.WriteInt32(address, count); return address; }
        internal static T Read<T>(nint address, int offset) where T : unmanaged => global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<T>((void*)(address + offset));
        internal static void Write<T>(nint address, int offset, T value) where T : unmanaged => global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)(address + offset), value);
        internal static void Free(nint address) { if (address == 0) return; try { int count = Count(address); for (int index = 0; index < count; ++index) { {Release} } } finally { global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(address); } }
        {Children}
        }
        """;
    const string NativeCommon = """
        namespace {Support} {
        template<typename T> T Read(unsigned long long address, int offset) { T value; std::memcpy(&value, reinterpret_cast<const unsigned char*>(address) + offset, sizeof(T)); return value; }
        template<typename T> void Write(unsigned long long address, int offset, T value) { std::memcpy(reinterpret_cast<unsigned char*>(address) + offset, &value, sizeof(T)); }
        inline int Count(unsigned long long address) { if (!address) return 0; int count = Read<int>(address, 0); if (count < 0 || count > (0x7fffffff - 8) / {Stride}) throw std::invalid_argument("Invalid collection count."); return count; }
        inline unsigned long long Allocate(int count) { if (count < 0 || count > (0x7fffffff - 8) / {Stride}) throw std::invalid_argument("Invalid collection count."); int size = 8 + count * {Stride}; void* buffer = ::CoTaskMemAlloc(size); if (!buffer) throw std::bad_alloc(); std::memset(buffer, 0, size); auto address = reinterpret_cast<unsigned long long>(buffer); Write<int>(address, 0, count); return address; }
        {Children}
        inline void Free(unsigned long long address) { if (!address) return; try { int count = Count(address); for (int index = 0; index < count; ++index) { {Release} } } catch (...) { ::CoTaskMemFree(reinterpret_cast<void*>(address)); throw; } ::CoTaskMemFree(reinterpret_cast<void*>(address)); }
        }
        """;
}
